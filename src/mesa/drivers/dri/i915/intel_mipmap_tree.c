/**************************************************************************
 * 
 * Copyright 2006 Tungsten Graphics, Inc., Cedar Park, Texas.
 * All Rights Reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 **************************************************************************/

#include <GL/gl.h>
#include <GL/internal/dri_interface.h>

#include "intel_batchbuffer.h"
#include "intel_chipset.h"
#include "intel_context.h"
#include "intel_mipmap_tree.h"
#include "intel_regions.h"
#include "intel_tex_layout.h"
#include "intel_tex.h"
#include "intel_blit.h"

#include "main/enums.h"
#include "main/formats.h"
#include "main/glformats.h"
#include "main/texcompress_etc.h"
#include "main/teximage.h"

#define FILE_DEBUG_FLAG DEBUG_MIPTREE

static GLenum
target_to_target(GLenum target)
{
   switch (target) {
   case GL_TEXTURE_CUBE_MAP_POSITIVE_X_ARB:
   case GL_TEXTURE_CUBE_MAP_NEGATIVE_X_ARB:
   case GL_TEXTURE_CUBE_MAP_POSITIVE_Y_ARB:
   case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y_ARB:
   case GL_TEXTURE_CUBE_MAP_POSITIVE_Z_ARB:
   case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z_ARB:
      return GL_TEXTURE_CUBE_MAP_ARB;
   default:
      return target;
   }
}

/**
 * @param for_bo Indicates that the caller is
 *        intel_miptree_create_for_bo(). If true, then do not create
 *        \c stencil_mt.
 */
struct intel_mipmap_tree *
intel_miptree_create_layout(struct intel_context *intel,
                            GLenum target,
                            gl_format format,
                            GLuint first_level,
                            GLuint last_level,
                            GLuint width0,
                            GLuint height0,
                            GLuint depth0,
                            bool for_bo)
{
   struct intel_mipmap_tree *mt = calloc(sizeof(*mt), 1);
   if (!mt)
      return NULL;

   DBG("%s target %s format %s level %d..%d <-- %p\n", __FUNCTION__,
       _mesa_lookup_enum_by_nr(target),
       _mesa_get_format_name(format),
       first_level, last_level, mt);

   mt->target = target_to_target(target);
   mt->format = format;
   mt->first_level = first_level;
   mt->last_level = last_level;
   mt->logical_width0 = width0;
   mt->logical_height0 = height0;
   mt->logical_depth0 = depth0;

   /* The cpp is bytes per (1, blockheight)-sized block for compressed
    * textures.  This is why you'll see divides by blockheight all over
    */
   unsigned bw, bh;
   _mesa_get_format_block_size(format, &bw, &bh);
   assert(_mesa_get_format_bytes(mt->format) % bw == 0);
   mt->cpp = _mesa_get_format_bytes(mt->format) / bw;

   mt->compressed = _mesa_is_format_compressed(format);
   mt->refcount = 1; 

   if (target == GL_TEXTURE_CUBE_MAP) {
      assert(depth0 == 1);
      depth0 = 6;
   }

   mt->physical_width0 = width0;
   mt->physical_height0 = height0;
   mt->physical_depth0 = depth0;

   if (!for_bo &&
       _mesa_get_format_base_format(format) == GL_DEPTH_STENCIL &&
       (intel->must_use_separate_stencil)) {
      mt->stencil_mt = intel_miptree_create(intel,
                                            mt->target,
                                            MESA_FORMAT_S8,
                                            mt->first_level,
                                            mt->last_level,
                                            mt->logical_width0,
                                            mt->logical_height0,
                                            mt->logical_depth0,
                                            true,
                                            INTEL_MIPTREE_TILING_ANY);
      if (!mt->stencil_mt) {
	 intel_miptree_release(&mt);
	 return NULL;
      }

      /* Fix up the Z miptree format for how we're splitting out separate
       * stencil.  Gen7 expects there to be no stencil bits in its depth buffer.
       */
      if (mt->format == MESA_FORMAT_S8_Z24) {
	 mt->format = MESA_FORMAT_X8_Z24;
      } else if (mt->format == MESA_FORMAT_Z32_FLOAT_X24S8) {
	 mt->format = MESA_FORMAT_Z32_FLOAT;
	 mt->cpp = 4;
      } else {
	 _mesa_problem(NULL, "Unknown format %s in separate stencil mt\n",
		       _mesa_get_format_name(mt->format));
      }
   }

   intel_get_texture_alignment_unit(intel, mt->format,
				    &mt->align_w, &mt->align_h);

   (void) intel;
   if (intel->is_945)
      i945_miptree_layout(mt);
   else
      i915_miptree_layout(mt);

   return mt;
}

/**
 * \brief Helper function for intel_miptree_create().
 */
static uint32_t
intel_miptree_choose_tiling(struct intel_context *intel,
                            gl_format format,
                            uint32_t width0,
                            enum intel_miptree_tiling_mode requested,
                            struct intel_mipmap_tree *mt)
{

   if (format == MESA_FORMAT_S8) {
      /* The stencil buffer is W tiled. However, we request from the kernel a
       * non-tiled buffer because the GTT is incapable of W fencing.
       */
      return I915_TILING_NONE;
   }

   /* Some usages may want only one type of tiling, like depth miptrees (Y
    * tiled), or temporary BOs for uploading data once (linear).
    */
   switch (requested) {
   case INTEL_MIPTREE_TILING_ANY:
      break;
   case INTEL_MIPTREE_TILING_Y:
      return I915_TILING_Y;
   case INTEL_MIPTREE_TILING_NONE:
      return I915_TILING_NONE;
   }

   GLenum base_format = _mesa_get_format_base_format(format);
   if (intel->gen >= 4 &&
       (base_format == GL_DEPTH_COMPONENT ||
        base_format == GL_DEPTH_STENCIL_EXT))
      return I915_TILING_Y;

   int minimum_pitch = mt->total_width * mt->cpp;

   /* If the width is much smaller than a tile, don't bother tiling. */
   if (minimum_pitch < 64)
      return I915_TILING_NONE;

   if (ALIGN(minimum_pitch, 512) >= 32768) {
      perf_debug("%dx%d miptree too large to blit, falling back to untiled",
                 mt->total_width, mt->total_height);
      return I915_TILING_NONE;
   }

   /* Pre-gen6 doesn't have BLORP to handle Y-tiling, so use X-tiling. */
   if (intel->gen < 6)
      return I915_TILING_X;

   return I915_TILING_Y | I915_TILING_X;
}

struct intel_mipmap_tree *
intel_miptree_create(struct intel_context *intel,
		     GLenum target,
		     gl_format format,
		     GLuint first_level,
		     GLuint last_level,
		     GLuint width0,
		     GLuint height0,
		     GLuint depth0,
		     bool expect_accelerated_upload,
                     enum intel_miptree_tiling_mode requested_tiling)
{
   struct intel_mipmap_tree *mt;
   gl_format tex_format = format;
   gl_format etc_format = MESA_FORMAT_NONE;
   GLuint total_width, total_height;

   if (!intel->is_baytrail) {
      switch (format) {
      case MESA_FORMAT_ETC1_RGB8:
         format = MESA_FORMAT_RGBX8888_REV;
         break;
      case MESA_FORMAT_ETC2_RGB8:
         format = MESA_FORMAT_RGBX8888_REV;
         break;
      case MESA_FORMAT_ETC2_SRGB8:
      case MESA_FORMAT_ETC2_SRGB8_ALPHA8_EAC:
      case MESA_FORMAT_ETC2_SRGB8_PUNCHTHROUGH_ALPHA1:
         format = MESA_FORMAT_SARGB8;
         break;
      case MESA_FORMAT_ETC2_RGBA8_EAC:
      case MESA_FORMAT_ETC2_RGB8_PUNCHTHROUGH_ALPHA1:
         format = MESA_FORMAT_RGBA8888_REV;
         break;
      case MESA_FORMAT_ETC2_R11_EAC:
         format = MESA_FORMAT_R16;
         break;
      case MESA_FORMAT_ETC2_SIGNED_R11_EAC:
         format = MESA_FORMAT_SIGNED_R16;
         break;
      case MESA_FORMAT_ETC2_RG11_EAC:
         format = MESA_FORMAT_GR1616;
         break;
      case MESA_FORMAT_ETC2_SIGNED_RG11_EAC:
         format = MESA_FORMAT_SIGNED_GR1616;
         break;
      default:
         /* Non ETC1 / ETC2 format */
         break;
      }
   }

   etc_format = (format != tex_format) ? tex_format : MESA_FORMAT_NONE;

   mt = intel_miptree_create_layout(intel, target, format,
				      first_level, last_level, width0,
				      height0, depth0,
				      false);
   /*
    * pitch == 0 || height == 0  indicates the null texture
    */
   if (!mt || !mt->total_width || !mt->total_height) {
      intel_miptree_release(&mt);
      return NULL;
   }

   total_width = mt->total_width;
   total_height = mt->total_height;

   if (format == MESA_FORMAT_S8) {
      /* Align to size of W tile, 64x64. */
      total_width = ALIGN(total_width, 64);
      total_height = ALIGN(total_height, 64);
   }

   uint32_t tiling = intel_miptree_choose_tiling(intel, format, width0,
                                                 requested_tiling,
                                                 mt);
   bool y_or_x = tiling == (I915_TILING_Y | I915_TILING_X);

   mt->etc_format = etc_format;
   mt->region = intel_region_alloc(intel->intelScreen,
				   y_or_x ? I915_TILING_Y : tiling,
				   mt->cpp,
				   total_width,
				   total_height,
				   expect_accelerated_upload);

   /* If the region is too large to fit in the aperture, we need to use the
    * BLT engine to support it.  The BLT paths can't currently handle Y-tiling,
    * so we need to fall back to X.
    */
   if (y_or_x && mt->region->bo->size >= intel->max_gtt_map_object_size) {
      perf_debug("%dx%d miptree larger than aperture; falling back to X-tiled\n",
                 mt->total_width, mt->total_height);
      intel_region_release(&mt->region);

      mt->region = intel_region_alloc(intel->intelScreen,
                                      I915_TILING_X,
                                      mt->cpp,
                                      total_width,
                                      total_height,
                                      expect_accelerated_upload);
   }

   mt->offset = 0;

   if (!mt->region) {
       intel_miptree_release(&mt);
       return NULL;
   }

   return mt;
}

struct intel_mipmap_tree *
intel_miptree_create_for_bo(struct intel_context *intel,
                            drm_intel_bo *bo,
                            gl_format format,
                            uint32_t offset,
                            uint32_t width,
                            uint32_t height,
                            int pitch,
                            uint32_t tiling)
{
   struct intel_mipmap_tree *mt;

   struct intel_region *region = calloc(1, sizeof(*region));
   if (!region)
      return NULL;

   /* Nothing will be able to use this miptree with the BO if the offset isn't
    * aligned.
    */
   if (tiling != I915_TILING_NONE)
      assert(offset % 4096 == 0);

   /* miptrees can't handle negative pitch.  If you need flipping of images,
    * that's outside of the scope of the mt.
    */
   assert(pitch >= 0);

   mt = intel_miptree_create_layout(intel, GL_TEXTURE_2D, format,
                                    0, 0,
                                    width, height, 1,
                                    true);
   if (!mt)
      return mt;

   region->cpp = mt->cpp;
   region->width = width;
   region->height = height;
   region->pitch = pitch;
   region->refcount = 1;
   drm_intel_bo_reference(bo);
   region->bo = bo;
   region->tiling = tiling;

   mt->region = region;
   mt->offset = offset;

   return mt;
}


/**
 * For a singlesample DRI2 buffer, this simply wraps the given region with a miptree.
 *
 * For a multisample DRI2 buffer, this wraps the given region with
 * a singlesample miptree, then creates a multisample miptree into which the
 * singlesample miptree is embedded as a child.
 */
struct intel_mipmap_tree*
intel_miptree_create_for_dri2_buffer(struct intel_context *intel,
                                     unsigned dri_attachment,
                                     gl_format format,
                                     struct intel_region *region)
{
   struct intel_mipmap_tree *mt = NULL;

   /* Only the front and back buffers, which are color buffers, are shared
    * through DRI2.
    */
   assert(dri_attachment == __DRI_BUFFER_BACK_LEFT ||
          dri_attachment == __DRI_BUFFER_FRONT_LEFT ||
          dri_attachment == __DRI_BUFFER_FAKE_FRONT_LEFT);
   assert(_mesa_get_format_base_format(format) == GL_RGB ||
          _mesa_get_format_base_format(format) == GL_RGBA);

   mt = intel_miptree_create_for_bo(intel,
                                    region->bo,
                                    format,
                                    0,
                                    region->width,
                                    region->height,
                                    region->pitch,
                                    region->tiling);
   if (!mt)
      return NULL;
   mt->region->name = region->name;

   return mt;
}

struct intel_mipmap_tree*
intel_miptree_create_for_renderbuffer(struct intel_context *intel,
                                      gl_format format,
                                      uint32_t width,
                                      uint32_t height)
{
   uint32_t depth = 1;

   return intel_miptree_create(intel, GL_TEXTURE_2D, format, 0, 0,
                               width, height, depth, true,
                               INTEL_MIPTREE_TILING_ANY);
}

void
intel_miptree_reference(struct intel_mipmap_tree **dst,
                        struct intel_mipmap_tree *src)
{
   if (*dst == src)
      return;

   intel_miptree_release(dst);

   if (src) {
      src->refcount++;
      DBG("%s %p refcount now %d\n", __FUNCTION__, src, src->refcount);
   }

   *dst = src;
}


void
intel_miptree_release(struct intel_mipmap_tree **mt)
{
   if (!*mt)
      return;

   DBG("%s %p refcount will be %d\n", __FUNCTION__, *mt, (*mt)->refcount - 1);
   if (--(*mt)->refcount <= 0) {
      GLuint i;

      DBG("%s deleting %p\n", __FUNCTION__, *mt);

      intel_region_release(&((*mt)->region));
      intel_miptree_release(&(*mt)->stencil_mt);

      for (i = 0; i < MAX_TEXTURE_LEVELS; i++) {
	 free((*mt)->level[i].slice);
      }

      free(*mt);
   }
   *mt = NULL;
}

void
intel_miptree_get_dimensions_for_image(struct gl_texture_image *image,
                                       int *width, int *height, int *depth)
{
   switch (image->TexObject->Target) {
   case GL_TEXTURE_1D_ARRAY:
      *width = image->Width;
      *height = 1;
      *depth = image->Height;
      break;
   default:
      *width = image->Width;
      *height = image->Height;
      *depth = image->Depth;
      break;
   }
}

/**
 * Can the image be pulled into a unified mipmap tree?  This mirrors
 * the completeness test in a lot of ways.
 *
 * Not sure whether I want to pass gl_texture_image here.
 */
bool
intel_miptree_match_image(struct intel_mipmap_tree *mt,
                          struct gl_texture_image *image)
{
   struct intel_texture_image *intelImage = intel_texture_image(image);
   GLuint level = intelImage->base.Base.Level;
   int width, height, depth;

   /* glTexImage* choose the texture object based on the target passed in, and
    * objects can't change targets over their lifetimes, so this should be
    * true.
    */
   assert(target_to_target(image->TexObject->Target) == mt->target);

   gl_format mt_format = mt->format;
   if (mt->format == MESA_FORMAT_X8_Z24 && mt->stencil_mt)
      mt_format = MESA_FORMAT_S8_Z24;
   if (mt->format == MESA_FORMAT_Z32_FLOAT && mt->stencil_mt)
      mt_format = MESA_FORMAT_Z32_FLOAT_X24S8;
   if (mt->etc_format != MESA_FORMAT_NONE)
      mt_format = mt->etc_format;

   if (image->TexFormat != mt_format)
      return false;

   intel_miptree_get_dimensions_for_image(image, &width, &height, &depth);

   if (mt->target == GL_TEXTURE_CUBE_MAP)
      depth = 6;

   /* Test image dimensions against the base level image adjusted for
    * minification.  This will also catch images not present in the
    * tree, changed targets, etc.
    */
   if (mt->target == GL_TEXTURE_2D_MULTISAMPLE ||
         mt->target == GL_TEXTURE_2D_MULTISAMPLE_ARRAY) {
      /* nonzero level here is always bogus */
      assert(level == 0);

      if (width != mt->logical_width0 ||
            height != mt->logical_height0 ||
            depth != mt->logical_depth0) {
         return false;
      }
   }
   else {
      /* all normal textures, renderbuffers, etc */
      if (width != mt->level[level].width ||
          height != mt->level[level].height ||
          depth != mt->level[level].depth) {
         return false;
      }
   }

   return true;
}


void
intel_miptree_set_level_info(struct intel_mipmap_tree *mt,
			     GLuint level,
			     GLuint x, GLuint y,
			     GLuint w, GLuint h, GLuint d)
{
   mt->level[level].width = w;
   mt->level[level].height = h;
   mt->level[level].depth = d;
   mt->level[level].level_x = x;
   mt->level[level].level_y = y;

   DBG("%s level %d size: %d,%d,%d offset %d,%d\n", __FUNCTION__,
       level, w, h, d, x, y);

   assert(mt->level[level].slice == NULL);

   mt->level[level].slice = calloc(d, sizeof(*mt->level[0].slice));
   mt->level[level].slice[0].x_offset = mt->level[level].level_x;
   mt->level[level].slice[0].y_offset = mt->level[level].level_y;
}


void
intel_miptree_set_image_offset(struct intel_mipmap_tree *mt,
			       GLuint level, GLuint img,
			       GLuint x, GLuint y)
{
   if (img == 0 && level == 0)
      assert(x == 0 && y == 0);

   assert(img < mt->level[level].depth);

   mt->level[level].slice[img].x_offset = mt->level[level].level_x + x;
   mt->level[level].slice[img].y_offset = mt->level[level].level_y + y;

   DBG("%s level %d img %d pos %d,%d\n",
       __FUNCTION__, level, img,
       mt->level[level].slice[img].x_offset,
       mt->level[level].slice[img].y_offset);
}

void
intel_miptree_get_image_offset(struct intel_mipmap_tree *mt,
			       GLuint level, GLuint slice,
			       GLuint *x, GLuint *y)
{
   assert(slice < mt->level[level].depth);

   *x = mt->level[level].slice[slice].x_offset;
   *y = mt->level[level].slice[slice].y_offset;
}

/**
 * Rendering with tiled buffers requires that the base address of the buffer
 * be aligned to a page boundary.  For renderbuffers, and sometimes with
 * textures, we may want the surface to point at a texture image level that
 * isn't at a page boundary.
 *
 * This function returns an appropriately-aligned base offset
 * according to the tiling restrictions, plus any required x/y offset
 * from there.
 */
uint32_t
intel_miptree_get_tile_offsets(struct intel_mipmap_tree *mt,
                               GLuint level, GLuint slice,
                               uint32_t *tile_x,
                               uint32_t *tile_y)
{
   struct intel_region *region = mt->region;
   uint32_t x, y;
   uint32_t mask_x, mask_y;

   intel_region_get_tile_masks(region, &mask_x, &mask_y, false);
   intel_miptree_get_image_offset(mt, level, slice, &x, &y);

   *tile_x = x & mask_x;
   *tile_y = y & mask_y;

   return intel_region_get_aligned_offset(region, x & ~mask_x, y & ~mask_y,
                                          false);
}

static void
intel_miptree_copy_slice_sw(struct intel_context *intel,
                            struct intel_mipmap_tree *dst_mt,
                            struct intel_mipmap_tree *src_mt,
                            int level,
                            int slice,
                            int width,
                            int height)
{
   void *src, *dst;
   int src_stride, dst_stride;
   int cpp = dst_mt->cpp;

   intel_miptree_map(intel, src_mt,
                     level, slice,
                     0, 0,
                     width, height,
                     GL_MAP_READ_BIT | BRW_MAP_DIRECT_BIT,
                     &src, &src_stride);

   intel_miptree_map(intel, dst_mt,
                     level, slice,
                     0, 0,
                     width, height,
                     GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT |
                     BRW_MAP_DIRECT_BIT,
                     &dst, &dst_stride);

   DBG("sw blit %s mt %p %p/%d -> %s mt %p %p/%d (%dx%d)\n",
       _mesa_get_format_name(src_mt->format),
       src_mt, src, src_stride,
       _mesa_get_format_name(dst_mt->format),
       dst_mt, dst, dst_stride,
       width, height);

   int row_size = cpp * width;
   if (src_stride == row_size &&
       dst_stride == row_size) {
      memcpy(dst, src, row_size * height);
   } else {
      for (int i = 0; i < height; i++) {
         memcpy(dst, src, row_size);
         dst += dst_stride;
         src += src_stride;
      }
   }

   intel_miptree_unmap(intel, dst_mt, level, slice);
   intel_miptree_unmap(intel, src_mt, level, slice);

   /* Don't forget to copy the stencil data over, too.  We could have skipped
    * passing BRW_MAP_DIRECT_BIT, but that would have meant intel_miptree_map
    * shuffling the two data sources in/out of temporary storage instead of
    * the direct mapping we get this way.
    */
   if (dst_mt->stencil_mt) {
      assert(src_mt->stencil_mt);
      intel_miptree_copy_slice_sw(intel, dst_mt->stencil_mt, src_mt->stencil_mt,
                                  level, slice, width, height);
   }
}

static void
intel_miptree_copy_slice(struct intel_context *intel,
			 struct intel_mipmap_tree *dst_mt,
			 struct intel_mipmap_tree *src_mt,
			 int level,
			 int face,
			 int depth)

{
   gl_format format = src_mt->format;
   uint32_t width = src_mt->level[level].width;
   uint32_t height = src_mt->level[level].height;
   int slice;

   if (face > 0)
      slice = face;
   else
      slice = depth;

   assert(depth < src_mt->level[level].depth);
   assert(src_mt->format == dst_mt->format);

   if (dst_mt->compressed) {
      height = ALIGN(height, dst_mt->align_h) / dst_mt->align_h;
      width = ALIGN(width, dst_mt->align_w);
   }

   /* If it's a packed depth/stencil buffer with separate stencil, the blit
    * below won't apply since we can't do the depth's Y tiling or the
    * stencil's W tiling in the blitter.
    */
   if (src_mt->stencil_mt) {
      intel_miptree_copy_slice_sw(intel,
                                  dst_mt, src_mt,
                                  level, slice,
                                  width, height);
      return;
   }

   uint32_t dst_x, dst_y, src_x, src_y;
   intel_miptree_get_image_offset(dst_mt, level, slice, &dst_x, &dst_y);
   intel_miptree_get_image_offset(src_mt, level, slice, &src_x, &src_y);

   DBG("validate blit mt %s %p %d,%d/%d -> mt %s %p %d,%d/%d (%dx%d)\n",
       _mesa_get_format_name(src_mt->format),
       src_mt, src_x, src_y, src_mt->region->pitch,
       _mesa_get_format_name(dst_mt->format),
       dst_mt, dst_x, dst_y, dst_mt->region->pitch,
       width, height);

   if (!intel_miptree_blit(intel,
                           src_mt, level, slice, 0, 0, false,
                           dst_mt, level, slice, 0, 0, false,
                           width, height, GL_COPY)) {
      perf_debug("miptree validate blit for %s failed\n",
                 _mesa_get_format_name(format));

      intel_miptree_copy_slice_sw(intel, dst_mt, src_mt, level, slice,
                                  width, height);
   }
}

/**
 * Copies the image's current data to the given miptree, and associates that
 * miptree with the image.
 *
 * If \c invalidate is true, then the actual image data does not need to be
 * copied, but the image still needs to be associated to the new miptree (this
 * is set to true if we're about to clear the image).
 */
void
intel_miptree_copy_teximage(struct intel_context *intel,
			    struct intel_texture_image *intelImage,
			    struct intel_mipmap_tree *dst_mt,
                            bool invalidate)
{
   struct intel_mipmap_tree *src_mt = intelImage->mt;
   struct intel_texture_object *intel_obj =
      intel_texture_object(intelImage->base.Base.TexObject);
   int level = intelImage->base.Base.Level;
   int face = intelImage->base.Base.Face;
   GLuint depth = intelImage->base.Base.Depth;

   if (!invalidate) {
      for (int slice = 0; slice < depth; slice++) {
         intel_miptree_copy_slice(intel, dst_mt, src_mt, level, face, slice);
      }
   }

   intel_miptree_reference(&intelImage->mt, dst_mt);
   intel_obj->needs_validate = true;
}

/**
 * \brief Get pointer offset into stencil buffer.
 *
 * The stencil buffer is W tiled. Since the GTT is incapable of W fencing, we
 * must decode the tile's layout in software.
 *
 * See
 *   - PRM, 2011 Sandy Bridge, Volume 1, Part 2, Section 4.5.2.1 W-Major Tile
 *     Format.
 *   - PRM, 2011 Sandy Bridge, Volume 1, Part 2, Section 4.5.3 Tiling Algorithm
 *
 * Even though the returned offset is always positive, the return type is
 * signed due to
 *    commit e8b1c6d6f55f5be3bef25084fdd8b6127517e137
 *    mesa: Fix return type of  _mesa_get_format_bytes() (#37351)
 */
static intptr_t
intel_offset_S8(uint32_t stride, uint32_t x, uint32_t y, bool swizzled)
{
   uint32_t tile_size = 4096;
   uint32_t tile_width = 64;
   uint32_t tile_height = 64;
   uint32_t row_size = 64 * stride;

   uint32_t tile_x = x / tile_width;
   uint32_t tile_y = y / tile_height;

   /* The byte's address relative to the tile's base addres. */
   uint32_t byte_x = x % tile_width;
   uint32_t byte_y = y % tile_height;

   uintptr_t u = tile_y * row_size
               + tile_x * tile_size
               + 512 * (byte_x / 8)
               +  64 * (byte_y / 8)
               +  32 * ((byte_y / 4) % 2)
               +  16 * ((byte_x / 4) % 2)
               +   8 * ((byte_y / 2) % 2)
               +   4 * ((byte_x / 2) % 2)
               +   2 * (byte_y % 2)
               +   1 * (byte_x % 2);

   if (swizzled) {
      /* adjust for bit6 swizzling */
      if (((byte_x / 8) % 2) == 1) {
	 if (((byte_y / 8) % 2) == 0) {
	    u += 64;
	 } else {
	    u -= 64;
	 }
      }
   }

   return u;
}

void *
intel_miptree_map_raw(struct intel_context *intel, struct intel_mipmap_tree *mt)
{
   drm_intel_bo *bo = mt->region->bo;

   if (unlikely(INTEL_DEBUG & DEBUG_PERF)) {
      if (drm_intel_bo_busy(bo)) {
         perf_debug("Mapping a busy BO, causing a stall on the GPU.\n");
      }
   }

   intel_flush(&intel->ctx);

   if (mt->region->tiling != I915_TILING_NONE)
      drm_intel_gem_bo_map_gtt(bo);
   else
      drm_intel_bo_map(bo, true);

   return bo->virtual;
}

void
intel_miptree_unmap_raw(struct intel_context *intel,
                        struct intel_mipmap_tree *mt)
{
   drm_intel_bo_unmap(mt->region->bo);
}

static void
intel_miptree_map_gtt(struct intel_context *intel,
		      struct intel_mipmap_tree *mt,
		      struct intel_miptree_map *map,
		      unsigned int level, unsigned int slice)
{
   unsigned int bw, bh;
   void *base;
   unsigned int image_x, image_y;
   int x = map->x;
   int y = map->y;

   /* For compressed formats, the stride is the number of bytes per
    * row of blocks.  intel_miptree_get_image_offset() already does
    * the divide.
    */
   _mesa_get_format_block_size(mt->format, &bw, &bh);
   assert(y % bh == 0);
   y /= bh;

   base = intel_miptree_map_raw(intel, mt) + mt->offset;

   if (base == NULL)
      map->ptr = NULL;
   else {
      /* Note that in the case of cube maps, the caller must have passed the
       * slice number referencing the face.
      */
      intel_miptree_get_image_offset(mt, level, slice, &image_x, &image_y);
      x += image_x;
      y += image_y;

      map->stride = mt->region->pitch;
      map->ptr = base + y * map->stride + x * mt->cpp;
   }

   DBG("%s: %d,%d %dx%d from mt %p (%s) %d,%d = %p/%d\n", __FUNCTION__,
       map->x, map->y, map->w, map->h,
       mt, _mesa_get_format_name(mt->format),
       x, y, map->ptr, map->stride);
}

static void
intel_miptree_unmap_gtt(struct intel_context *intel,
			struct intel_mipmap_tree *mt,
			struct intel_miptree_map *map,
			unsigned int level,
			unsigned int slice)
{
   intel_miptree_unmap_raw(intel, mt);
}

static void
intel_miptree_map_blit(struct intel_context *intel,
		       struct intel_mipmap_tree *mt,
		       struct intel_miptree_map *map,
		       unsigned int level, unsigned int slice)
{
   map->mt = intel_miptree_create(intel, GL_TEXTURE_2D, mt->format,
                                  0, 0,
                                  map->w, map->h, 1,
                                  false,
                                  INTEL_MIPTREE_TILING_NONE);
   if (!map->mt) {
      fprintf(stderr, "Failed to allocate blit temporary\n");
      goto fail;
   }
   map->stride = map->mt->region->pitch;

   if (!intel_miptree_blit(intel,
                           mt, level, slice,
                           map->x, map->y, false,
                           map->mt, 0, 0,
                           0, 0, false,
                           map->w, map->h, GL_COPY)) {
      fprintf(stderr, "Failed to blit\n");
      goto fail;
   }

   intel_batchbuffer_flush(intel);
   map->ptr = intel_miptree_map_raw(intel, map->mt);

   DBG("%s: %d,%d %dx%d from mt %p (%s) %d,%d = %p/%d\n", __FUNCTION__,
       map->x, map->y, map->w, map->h,
       mt, _mesa_get_format_name(mt->format),
       level, slice, map->ptr, map->stride);

   return;

fail:
   intel_miptree_release(&map->mt);
   map->ptr = NULL;
   map->stride = 0;
}

static void
intel_miptree_unmap_blit(struct intel_context *intel,
			 struct intel_mipmap_tree *mt,
			 struct intel_miptree_map *map,
			 unsigned int level,
			 unsigned int slice)
{
   struct gl_context *ctx = &intel->ctx;

   intel_miptree_unmap_raw(intel, map->mt);

   if (map->mode & GL_MAP_WRITE_BIT) {
      bool ok = intel_miptree_blit(intel,
                                   map->mt, 0, 0,
                                   0, 0, false,
                                   mt, level, slice,
                                   map->x, map->y, false,
                                   map->w, map->h, GL_COPY);
      WARN_ONCE(!ok, "Failed to blit from linear temporary mapping");
   }

   intel_miptree_release(&map->mt);
}

static void
intel_miptree_map_s8(struct intel_context *intel,
		     struct intel_mipmap_tree *mt,
		     struct intel_miptree_map *map,
		     unsigned int level, unsigned int slice)
{
   map->stride = map->w;
   map->buffer = map->ptr = malloc(map->stride * map->h);
   if (!map->buffer)
      return;

   /* One of either READ_BIT or WRITE_BIT or both is set.  READ_BIT implies no
    * INVALIDATE_RANGE_BIT.  WRITE_BIT needs the original values read in unless
    * invalidate is set, since we'll be writing the whole rectangle from our
    * temporary buffer back out.
    */
   if (!(map->mode & GL_MAP_INVALIDATE_RANGE_BIT)) {
      uint8_t *untiled_s8_map = map->ptr;
      uint8_t *tiled_s8_map = intel_miptree_map_raw(intel, mt);
      unsigned int image_x, image_y;

      intel_miptree_get_image_offset(mt, level, slice, &image_x, &image_y);

      for (uint32_t y = 0; y < map->h; y++) {
	 for (uint32_t x = 0; x < map->w; x++) {
	    ptrdiff_t offset = intel_offset_S8(mt->region->pitch,
	                                       x + image_x + map->x,
	                                       y + image_y + map->y,
					       intel->has_swizzling);
	    untiled_s8_map[y * map->w + x] = tiled_s8_map[offset];
	 }
      }

      intel_miptree_unmap_raw(intel, mt);

      DBG("%s: %d,%d %dx%d from mt %p %d,%d = %p/%d\n", __FUNCTION__,
	  map->x, map->y, map->w, map->h,
	  mt, map->x + image_x, map->y + image_y, map->ptr, map->stride);
   } else {
      DBG("%s: %d,%d %dx%d from mt %p = %p/%d\n", __FUNCTION__,
	  map->x, map->y, map->w, map->h,
	  mt, map->ptr, map->stride);
   }
}

static void
intel_miptree_unmap_s8(struct intel_context *intel,
		       struct intel_mipmap_tree *mt,
		       struct intel_miptree_map *map,
		       unsigned int level,
		       unsigned int slice)
{
   if (map->mode & GL_MAP_WRITE_BIT) {
      unsigned int image_x, image_y;
      uint8_t *untiled_s8_map = map->ptr;
      uint8_t *tiled_s8_map = intel_miptree_map_raw(intel, mt);

      intel_miptree_get_image_offset(mt, level, slice, &image_x, &image_y);

      for (uint32_t y = 0; y < map->h; y++) {
	 for (uint32_t x = 0; x < map->w; x++) {
	    ptrdiff_t offset = intel_offset_S8(mt->region->pitch,
	                                       x + map->x,
	                                       y + map->y,
					       intel->has_swizzling);
	    tiled_s8_map[offset] = untiled_s8_map[y * map->w + x];
	 }
      }

      intel_miptree_unmap_raw(intel, mt);
   }

   free(map->buffer);
}

static void
intel_miptree_map_etc(struct intel_context *intel,
                      struct intel_mipmap_tree *mt,
                      struct intel_miptree_map *map,
                      unsigned int level,
                      unsigned int slice)
{
   assert(mt->etc_format != MESA_FORMAT_NONE);
   if (mt->etc_format == MESA_FORMAT_ETC1_RGB8) {
      assert(mt->format == MESA_FORMAT_RGBX8888_REV);
   }

   assert(map->mode & GL_MAP_WRITE_BIT);
   assert(map->mode & GL_MAP_INVALIDATE_RANGE_BIT);

   map->stride = _mesa_format_row_stride(mt->etc_format, map->w);
   map->buffer = malloc(_mesa_format_image_size(mt->etc_format,
                                                map->w, map->h, 1));
   map->ptr = map->buffer;
}

static void
intel_miptree_unmap_etc(struct intel_context *intel,
                        struct intel_mipmap_tree *mt,
                        struct intel_miptree_map *map,
                        unsigned int level,
                        unsigned int slice)
{
   uint32_t image_x;
   uint32_t image_y;
   intel_miptree_get_image_offset(mt, level, slice, &image_x, &image_y);

   image_x += map->x;
   image_y += map->y;

   uint8_t *dst = intel_miptree_map_raw(intel, mt)
                + image_y * mt->region->pitch
                + image_x * mt->region->cpp;

   if (mt->etc_format == MESA_FORMAT_ETC1_RGB8)
      _mesa_etc1_unpack_rgba8888(dst, mt->region->pitch,
                                 map->ptr, map->stride,
                                 map->w, map->h);
   else
      _mesa_unpack_etc2_format(dst, mt->region->pitch,
                               map->ptr, map->stride,
                               map->w, map->h, mt->etc_format);

   intel_miptree_unmap_raw(intel, mt);
   free(map->buffer);
}

/**
 * Mapping function for packed depth/stencil miptrees backed by real separate
 * miptrees for depth and stencil.
 *
 * On gen7, and to support HiZ pre-gen7, we have to have the stencil buffer
 * separate from the depth buffer.  Yet at the GL API level, we have to expose
 * packed depth/stencil textures and FBO attachments, and Mesa core expects to
 * be able to map that memory for texture storage and glReadPixels-type
 * operations.  We give Mesa core that access by mallocing a temporary and
 * copying the data between the actual backing store and the temporary.
 */
static void
intel_miptree_map_depthstencil(struct intel_context *intel,
			       struct intel_mipmap_tree *mt,
			       struct intel_miptree_map *map,
			       unsigned int level, unsigned int slice)
{
   struct intel_mipmap_tree *z_mt = mt;
   struct intel_mipmap_tree *s_mt = mt->stencil_mt;
   bool map_z32f_x24s8 = mt->format == MESA_FORMAT_Z32_FLOAT;
   int packed_bpp = map_z32f_x24s8 ? 8 : 4;

   map->stride = map->w * packed_bpp;
   map->buffer = map->ptr = malloc(map->stride * map->h);
   if (!map->buffer)
      return;

   /* One of either READ_BIT or WRITE_BIT or both is set.  READ_BIT implies no
    * INVALIDATE_RANGE_BIT.  WRITE_BIT needs the original values read in unless
    * invalidate is set, since we'll be writing the whole rectangle from our
    * temporary buffer back out.
    */
   if (!(map->mode & GL_MAP_INVALIDATE_RANGE_BIT)) {
      uint32_t *packed_map = map->ptr;
      uint8_t *s_map = intel_miptree_map_raw(intel, s_mt);
      uint32_t *z_map = intel_miptree_map_raw(intel, z_mt);
      unsigned int s_image_x, s_image_y;
      unsigned int z_image_x, z_image_y;

      intel_miptree_get_image_offset(s_mt, level, slice,
				     &s_image_x, &s_image_y);
      intel_miptree_get_image_offset(z_mt, level, slice,
				     &z_image_x, &z_image_y);

      for (uint32_t y = 0; y < map->h; y++) {
	 for (uint32_t x = 0; x < map->w; x++) {
	    int map_x = map->x + x, map_y = map->y + y;
	    ptrdiff_t s_offset = intel_offset_S8(s_mt->region->pitch,
						 map_x + s_image_x,
						 map_y + s_image_y,
						 intel->has_swizzling);
	    ptrdiff_t z_offset = ((map_y + z_image_y) *
                                  (z_mt->region->pitch / 4) +
				  (map_x + z_image_x));
	    uint8_t s = s_map[s_offset];
	    uint32_t z = z_map[z_offset];

	    if (map_z32f_x24s8) {
	       packed_map[(y * map->w + x) * 2 + 0] = z;
	       packed_map[(y * map->w + x) * 2 + 1] = s;
	    } else {
	       packed_map[y * map->w + x] = (s << 24) | (z & 0x00ffffff);
	    }
	 }
      }

      intel_miptree_unmap_raw(intel, s_mt);
      intel_miptree_unmap_raw(intel, z_mt);

      DBG("%s: %d,%d %dx%d from z mt %p %d,%d, s mt %p %d,%d = %p/%d\n",
	  __FUNCTION__,
	  map->x, map->y, map->w, map->h,
	  z_mt, map->x + z_image_x, map->y + z_image_y,
	  s_mt, map->x + s_image_x, map->y + s_image_y,
	  map->ptr, map->stride);
   } else {
      DBG("%s: %d,%d %dx%d from mt %p = %p/%d\n", __FUNCTION__,
	  map->x, map->y, map->w, map->h,
	  mt, map->ptr, map->stride);
   }
}

static void
intel_miptree_unmap_depthstencil(struct intel_context *intel,
				 struct intel_mipmap_tree *mt,
				 struct intel_miptree_map *map,
				 unsigned int level,
				 unsigned int slice)
{
   struct intel_mipmap_tree *z_mt = mt;
   struct intel_mipmap_tree *s_mt = mt->stencil_mt;
   bool map_z32f_x24s8 = mt->format == MESA_FORMAT_Z32_FLOAT;

   if (map->mode & GL_MAP_WRITE_BIT) {
      uint32_t *packed_map = map->ptr;
      uint8_t *s_map = intel_miptree_map_raw(intel, s_mt);
      uint32_t *z_map = intel_miptree_map_raw(intel, z_mt);
      unsigned int s_image_x, s_image_y;
      unsigned int z_image_x, z_image_y;

      intel_miptree_get_image_offset(s_mt, level, slice,
				     &s_image_x, &s_image_y);
      intel_miptree_get_image_offset(z_mt, level, slice,
				     &z_image_x, &z_image_y);

      for (uint32_t y = 0; y < map->h; y++) {
	 for (uint32_t x = 0; x < map->w; x++) {
	    ptrdiff_t s_offset = intel_offset_S8(s_mt->region->pitch,
						 x + s_image_x + map->x,
						 y + s_image_y + map->y,
						 intel->has_swizzling);
	    ptrdiff_t z_offset = ((y + z_image_y) *
                                  (z_mt->region->pitch / 4) +
				  (x + z_image_x));

	    if (map_z32f_x24s8) {
	       z_map[z_offset] = packed_map[(y * map->w + x) * 2 + 0];
	       s_map[s_offset] = packed_map[(y * map->w + x) * 2 + 1];
	    } else {
	       uint32_t packed = packed_map[y * map->w + x];
	       s_map[s_offset] = packed >> 24;
	       z_map[z_offset] = packed;
	    }
	 }
      }

      intel_miptree_unmap_raw(intel, s_mt);
      intel_miptree_unmap_raw(intel, z_mt);

      DBG("%s: %d,%d %dx%d from z mt %p (%s) %d,%d, s mt %p %d,%d = %p/%d\n",
	  __FUNCTION__,
	  map->x, map->y, map->w, map->h,
	  z_mt, _mesa_get_format_name(z_mt->format),
	  map->x + z_image_x, map->y + z_image_y,
	  s_mt, map->x + s_image_x, map->y + s_image_y,
	  map->ptr, map->stride);
   }

   free(map->buffer);
}

/**
 * Create and attach a map to the miptree at (level, slice). Return the
 * attached map.
 */
static struct intel_miptree_map*
intel_miptree_attach_map(struct intel_mipmap_tree *mt,
                         unsigned int level,
                         unsigned int slice,
                         unsigned int x,
                         unsigned int y,
                         unsigned int w,
                         unsigned int h,
                         GLbitfield mode)
{
   struct intel_miptree_map *map = calloc(1, sizeof(*map));

   if (!map)
      return NULL;

   assert(mt->level[level].slice[slice].map == NULL);
   mt->level[level].slice[slice].map = map;

   map->mode = mode;
   map->x = x;
   map->y = y;
   map->w = w;
   map->h = h;

   return map;
}

/**
 * Release the map at (level, slice).
 */
static void
intel_miptree_release_map(struct intel_mipmap_tree *mt,
                         unsigned int level,
                         unsigned int slice)
{
   struct intel_miptree_map **map;

   map = &mt->level[level].slice[slice].map;
   free(*map);
   *map = NULL;
}

void
intel_miptree_map(struct intel_context *intel,
                  struct intel_mipmap_tree *mt,
                  unsigned int level,
                  unsigned int slice,
                  unsigned int x,
                  unsigned int y,
                  unsigned int w,
                  unsigned int h,
                  GLbitfield mode,
                  void **out_ptr,
                  int *out_stride)
{
   struct intel_miptree_map *map;

   map = intel_miptree_attach_map(mt, level, slice, x, y, w, h, mode);
   if (!map){
      *out_ptr = NULL;
      *out_stride = 0;
      return;
   }

   if (mt->format == MESA_FORMAT_S8) {
      intel_miptree_map_s8(intel, mt, map, level, slice);
   } else if (mt->etc_format != MESA_FORMAT_NONE &&
              !(mode & BRW_MAP_DIRECT_BIT)) {
      intel_miptree_map_etc(intel, mt, map, level, slice);
   } else if (mt->stencil_mt && !(mode & BRW_MAP_DIRECT_BIT)) {
      intel_miptree_map_depthstencil(intel, mt, map, level, slice);
   }
   /* See intel_miptree_blit() for details on the 32k pitch limit. */
   else if (intel->has_llc &&
            !(mode & GL_MAP_WRITE_BIT) &&
            !mt->compressed &&
            (mt->region->tiling == I915_TILING_X ||
             (intel->gen >= 6 && mt->region->tiling == I915_TILING_Y)) &&
            mt->region->pitch < 32768) {
      intel_miptree_map_blit(intel, mt, map, level, slice);
   } else if (mt->region->tiling != I915_TILING_NONE &&
              mt->region->bo->size >= intel->max_gtt_map_object_size) {
      assert(mt->region->pitch < 32768);
      intel_miptree_map_blit(intel, mt, map, level, slice);
   } else {
      intel_miptree_map_gtt(intel, mt, map, level, slice);
   }

   *out_ptr = map->ptr;
   *out_stride = map->stride;

   if (map->ptr == NULL)
      intel_miptree_release_map(mt, level, slice);
}

void
intel_miptree_unmap(struct intel_context *intel,
                    struct intel_mipmap_tree *mt,
                    unsigned int level,
                    unsigned int slice)
{
   struct intel_miptree_map *map = mt->level[level].slice[slice].map;

   if (!map)
      return;

   DBG("%s: mt %p (%s) level %d slice %d\n", __FUNCTION__,
       mt, _mesa_get_format_name(mt->format), level, slice);

   if (mt->format == MESA_FORMAT_S8) {
      intel_miptree_unmap_s8(intel, mt, map, level, slice);
   } else if (mt->etc_format != MESA_FORMAT_NONE &&
              !(map->mode & BRW_MAP_DIRECT_BIT)) {
      intel_miptree_unmap_etc(intel, mt, map, level, slice);
   } else if (mt->stencil_mt && !(map->mode & BRW_MAP_DIRECT_BIT)) {
      intel_miptree_unmap_depthstencil(intel, mt, map, level, slice);
   } else if (map->mt) {
      intel_miptree_unmap_blit(intel, mt, map, level, slice);
   } else {
      intel_miptree_unmap_gtt(intel, mt, map, level, slice);
   }

   intel_miptree_release_map(mt, level, slice);
}