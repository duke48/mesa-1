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

#ifndef INTEL_FBO_H
#define INTEL_FBO_H

#include <stdbool.h>
#include <assert.h>
#include "main/formats.h"
#include "main/macros.h"
#include "brw_context.h"
#include "intel_mipmap_tree.h"
#include "intel_screen.h"

#ifdef __cplusplus
extern "C" {
#endif

struct intel_mipmap_tree;
struct intel_texture_image;

/**
 * Intel renderbuffer, derived from gl_renderbuffer.
 */
struct intel_renderbuffer
{
   struct swrast_renderbuffer Base;
   struct intel_mipmap_tree *mt; /**< The renderbuffer storage. */

   /**
    * \name Miptree view
    * \{
    *
    * Multiple renderbuffers may simultaneously wrap a single texture and each
    * provide a different view into that texture. The fields below indicate
    * which miptree slice is wrapped by this renderbuffer.  The fields' values
    * are consistent with the 'level' and 'layer' parameters of
    * glFramebufferTextureLayer().
    *
    * For renderbuffers not created with glFramebufferTexture*(), mt_level and
    * mt_layer are 0.
    */
   unsigned int mt_level;
   unsigned int mt_layer;
   /** \} */

   GLuint draw_x, draw_y; /**< Offset of drawing within the region */
};


/**
 * gl_renderbuffer is a base class which we subclass.  The Class field
 * is used for simple run-time type checking.
 */
#define INTEL_RB_CLASS 0x12345678


/**
 * Return a gl_renderbuffer ptr casted to intel_renderbuffer.
 * NULL will be returned if the rb isn't really an intel_renderbuffer.
 * This is determined by checking the ClassID.
 */
static INLINE struct intel_renderbuffer *
intel_renderbuffer(struct gl_renderbuffer *rb)
{
   struct intel_renderbuffer *irb = (struct intel_renderbuffer *) rb;
   if (irb && irb->Base.Base.ClassID == INTEL_RB_CLASS) {
      /*_mesa_warning(NULL, "Returning non-intel Rb\n");*/
      return irb;
   }
   else
      return NULL;
}


/**
 * \brief Return the framebuffer attachment specified by attIndex.
 *
 * If the framebuffer lacks the specified attachment, then return null.
 *
 * If the attached renderbuffer is a wrapper, then return wrapped
 * renderbuffer.
 */
static INLINE struct intel_renderbuffer *
intel_get_renderbuffer(struct gl_framebuffer *fb, gl_buffer_index attIndex)
{
   struct gl_renderbuffer *rb;

   assert((unsigned)attIndex < ARRAY_SIZE(fb->Attachment));

   rb = fb->Attachment[attIndex].Renderbuffer;
   if (!rb)
      return NULL;

   return intel_renderbuffer(rb);
}


static INLINE gl_format
intel_rb_format(const struct intel_renderbuffer *rb)
{
   return rb->Base.Base.Format;
}

extern struct intel_renderbuffer *
intel_create_renderbuffer(gl_format format, unsigned num_samples);

struct intel_renderbuffer *
intel_create_private_renderbuffer(gl_format format, unsigned num_samples);

struct gl_renderbuffer*
intel_create_wrapped_renderbuffer(struct gl_context * ctx,
				  int width, int height,
				  gl_format format);

extern void
intel_fbo_init(struct brw_context *brw);

void
intel_renderbuffer_set_draw_offset(struct intel_renderbuffer *irb);

static inline uint32_t
intel_renderbuffer_get_tile_offsets(struct intel_renderbuffer *irb,
                                    uint32_t *tile_x,
                                    uint32_t *tile_y)
{
   return intel_miptree_get_tile_offsets(irb->mt, irb->mt_level, irb->mt_layer,
                                         tile_x, tile_y);
}

void
intel_renderbuffer_set_needs_downsample(struct intel_renderbuffer *irb);

bool
intel_renderbuffer_has_hiz(struct intel_renderbuffer *irb);

void
intel_renderbuffer_att_set_needs_depth_resolve(struct gl_renderbuffer_attachment *att);


/**
 * \brief Perform a HiZ resolve on the renderbuffer.
 *
 * It is safe to call this function on a renderbuffer without HiZ. In that
 * case, the function is a no-op.
 *
 * \return false if no resolve was needed
 */
bool
intel_renderbuffer_resolve_hiz(struct brw_context *brw,
			       struct intel_renderbuffer *irb);

/**
 * \brief Perform a depth resolve on the renderbuffer.
 *
 * It is safe to call this function on a renderbuffer without HiZ. In that
 * case, the function is a no-op.
 *
 * \return false if no resolve was needed
 */
bool
intel_renderbuffer_resolve_depth(struct brw_context *brw,
				 struct intel_renderbuffer *irb);

void intel_renderbuffer_move_to_temp(struct brw_context *brw,
                                     struct intel_renderbuffer *irb,
                                     bool invalidate);

unsigned
intel_quantize_num_samples(struct intel_screen *intel, unsigned num_samples);

#ifdef __cplusplus
}
#endif

#endif /* INTEL_FBO_H */
