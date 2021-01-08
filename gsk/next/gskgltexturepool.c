/* gskgltexturepool.c
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

#include "config.h"

#include <string.h>

#include "gskgltexturepoolprivate.h"

static void
gsk_gl_texture_free (GskGLTexture *texture)
{
  if (texture->texture_id != 0)
    {
      glDeleteTextures (1, &texture->texture_id);
      texture->texture_id = 0;
    }

  g_slice_free (GskGLTexture, texture);
}

void
gsk_gl_texture_pool_init (GskGLTexturePool *self)
{
  memset (self, 0, sizeof *self);
}

void
gsk_gl_texture_pool_clear (GskGLTexturePool *self)
{
  while (self->by_width.length > 0)
    {
      GskGLTexture *head = g_queue_peek_head (&self->by_width);

      g_queue_unlink (&self->by_width, &head->width_link);
      g_queue_unlink (&self->by_height, &head->height_link);

      gsk_gl_texture_free (head);
    }

  g_assert (self->by_width.length == 0);
  g_assert (self->by_height.length == 0);
}

void
gsk_gl_texture_pool_put (GskGLTexturePool *self,
                         GskGLTexture     *texture)
{
  GList *sibling = NULL;

  g_return_if_fail (self != NULL);
  g_return_if_fail (texture != NULL);

  for (GList *iter = self->by_width.head;
       iter != NULL;
       iter = iter->next)
    {
      GskGLTexture *other = iter->data;

      if (other->width > texture->width ||
          (other->width == texture->width &&
           other->height > texture->height))
        break;

      sibling = iter;
    }

  g_queue_insert_after_link (&self->by_width, sibling, &texture->width_link);

  for (GList *iter = self->by_width.head;
       iter != NULL;
       iter = iter->next)
    {
      GskGLTexture *other = iter->data;

      if (other->height > texture->height ||
          (other->height == texture->height &&
           other->width > texture->width))
        break;

      sibling = iter;
    }

  g_queue_insert_after_link (&self->by_width, sibling, &texture->width_link);
}

GskGLTexture *
gsk_gl_texture_pool_get (GskGLTexturePool *self,
                         float             width,
                         float             height,
                         int               min_filter,
                         int               mag_filter,
                         gboolean          always_create)
{
  GskGLTexture *texture;

  g_return_val_if_fail (self != NULL, NULL);

  if (always_create)
    goto create_texture;

  if (width >= height)
    {
      for (GList *iter = self->by_width.head;
           iter != NULL;
           iter = iter->next)
        {
          texture = iter->data;

          if (texture->width >= width &&
              texture->height >= height &&
              texture->min_filter == min_filter &&
              texture->mag_filter == mag_filter)
            {
              g_queue_unlink (&self->by_width, &texture->width_link);
              g_queue_unlink (&self->by_height, &texture->height_link);

              return texture;
            }
        }
    }
  else
    {
      for (GList *iter = self->by_height.head;
           iter != NULL;
           iter = iter->next)
        {
          texture = iter->data;

          if (texture->width >= width &&
              texture->height >= height &&
              texture->min_filter == min_filter &&
              texture->mag_filter == mag_filter)
            {
              g_queue_unlink (&self->by_width, &texture->width_link);
              g_queue_unlink (&self->by_height, &texture->height_link);

              return texture;
            }
        }
    }

create_texture:

  texture = g_slice_new0 (GskGLTexture);
  texture->width_link.data = texture;
  texture->height_link.data = texture;
  texture->min_filter = min_filter;
  texture->mag_filter = mag_filter;

  glGenTextures (1, &texture->texture_id);

  glActiveTexture (GL_TEXTURE0);
  glBindTexture (GL_TEXTURE_2D, texture->texture_id);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_filter);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  if (gdk_gl_context_get_use_es (gdk_gl_context_get_current ()))
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  else
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);

  glBindTexture (GL_TEXTURE_2D, 0);

  return texture;
}
