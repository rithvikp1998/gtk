/* gskgltexturepoolprivate.h
 *
 * Copyright 2020 Christian Hergert <chergert@redhat.com>
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version.
 *
 * This file is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef _GSK_GL_TEXTURE_POOL_PRIVATE_H__
#define _GSK_GL_TEXTURE_POOL_PRIVATE_H__

#include "gskgltypesprivate.h"

G_BEGIN_DECLS

typedef struct _GskGLTexturePool
{
  GQueue by_width;
  GQueue by_height;
} GskGLTexturePool;

struct _GskGLTextureSlice
{
  cairo_rectangle_int_t rect;
  guint texture_id;
};

struct _GskGLTexture
{
  /* Used to sort by width/height in pool */
  GList              width_link;
  GList              height_link;

  /* Identifier of the frame that created it */
  gint64             last_used_in_frame;

  /* Backpointer to texture (can be cleared asynchronously) */
  GdkTexture        *user;

  /* Only used by sliced textures */
  GskGLTextureSlice *slices;
  guint              n_slices;

  /* The actual GL texture identifier in some shared context */
  guint              texture_id;

  float              width;
  float              height;
  int                min_filter;
  int                mag_filter;

  /* Set when used by an atlas so we don't drop the texture */
  guint              permanent : 1;
};

void          gsk_gl_texture_pool_init  (GskGLTexturePool *self);
void          gsk_gl_texture_pool_clear (GskGLTexturePool *self);
GskGLTexture *gsk_gl_texture_pool_get   (GskGLTexturePool *self,
                                         float             width,
                                         float             height,
                                         int               min_filter,
                                         int               mag_filter,
                                         gboolean          always_create);
void          gsk_gl_texture_pool_put   (GskGLTexturePool *self,
                                         GskGLTexture     *texture);
GskGLTexture *gsk_gl_texture_new        (guint             texture_id,
                                         int               width,
                                         int               height,
                                         int               min_filter,
                                         int               mag_filter,
                                         gint64            frame_id);
void          gsk_gl_texture_free       (GskGLTexture     *texture);

G_END_DECLS

#endif /* _GSK_GL_TEXTURE_POOL_PRIVATE_H__ */
