/* gskgldriverprivate.h
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

#ifndef __GSK_GL_DRIVER_PRIVATE_H__
#define __GSK_GL_DRIVER_PRIVATE_H__

#include "gskgltypesprivate.h"

#include "gskgltexturepoolprivate.h"

G_BEGIN_DECLS

enum {
  UNIFORM_SHARED_ALPHA,
  UNIFORM_SHARED_SOURCE,
  UNIFORM_SHARED_CLIP_RECT,
  UNIFORM_SHARED_VIEWPORT,
  UNIFORM_SHARED_PROJECTION,
  UNIFORM_SHARED_MODELVIEW,

  UNIFORM_SHARED_LAST
};

enum {
  UNIFORM_CUSTOM_SIZE = UNIFORM_SHARED_LAST,
  UNIFORM_CUSTOM_TEXTURE1,
  UNIFORM_CUSTOM_TEXTURE2,
  UNIFORM_CUSTOM_TEXTURE3,
  UNIFORM_CUSTOM_TEXTURE4,

  UNIFORM_CUSTOM_LAST
};

typedef struct {
  gpointer        pointer;
  float           scale_x;
  float           scale_y;
  int             filter;
  int             pointer_is_child;
  graphene_rect_t parent_rect; /* Valid when pointer_is_child */
} GskTextureKey;

#define GSL_GK_NO_UNIFORMS UNIFORM_INVALID_##__COUNTER__
#define GSK_GL_ADD_UNIFORM(pos, KEY, name) UNIFORM_##KEY = UNIFORM_SHARED_LAST + pos,
#define GSK_GL_DEFINE_PROGRAM(name, resource, uniforms) enum { uniforms };
# include "gskglprograms.defs"
#undef GSK_GL_DEFINE_PROGRAM
#undef GSK_GL_ADD_UNIFORM
#undef GSL_GK_NO_UNIFORMS

#define GSK_TYPE_NEXT_DRIVER (gsk_next_driver_get_type())

G_DECLARE_FINAL_TYPE (GskNextDriver, gsk_next_driver, GSK, NEXT_DRIVER, GObject)

struct _GskGLRenderTarget
{
  guint framebuffer_id;
  guint texture_id;
  int min_filter;
  int mag_filter;
  int width;
  int height;
};

struct _GskNextDriver
{
  GObject parent_instance;

  GskGLCommandQueue *command_queue;

  GskGLTexturePool texture_pool;

  GskGLGlyphLibrary *glyphs;
  GskGLIconLibrary *icons;
  GskGLShadowLibrary *shadows;

  GHashTable *textures;
  GHashTable *key_to_texture_id;
  GHashTable *texture_id_to_key;

  GHashTable *shader_cache;

  GArray *autorelease_framebuffers;
  GPtrArray *render_targets;

#define GSK_GL_NO_UNIFORMS
#define GSK_GL_ADD_UNIFORM(pos, KEY, name)
#define GSK_GL_DEFINE_PROGRAM(name, resource, uniforms) GskGLProgram *name;
# include "gskglprograms.defs"
#undef GSK_GL_NO_UNIFORMS
#undef GSK_GL_ADD_UNIFORM
#undef GSK_GL_DEFINE_PROGRAM

  gint64 current_frame_id;

  guint debug : 1;
  guint in_frame : 1;
};

GskNextDriver *gsk_next_driver_new                   (GskGLCommandQueue    *command_queue,
                                                      gboolean              debug,
                                                      GError              **error);
GdkGLContext  *gsk_next_driver_get_context           (GskNextDriver        *self);
gboolean       gsk_next_driver_create_render_target  (GskNextDriver        *self,
                                                      int                   width,
                                                      int                   height,
                                                      int                   min_filter,
                                                      int                   mag_filter,
                                                      GskGLRenderTarget   **render_target);
guint          gsk_next_driver_release_render_target (GskNextDriver        *self,
                                                      GskGLRenderTarget    *render_target,
                                                      gboolean              release_texture);
void           gsk_next_driver_begin_frame           (GskNextDriver        *self);
void           gsk_next_driver_end_frame             (GskNextDriver        *self);
guint          gsk_next_driver_lookup_texture        (GskNextDriver        *self,
                                                      const GskTextureKey  *key);
void           gsk_next_driver_cache_texture         (GskNextDriver        *self,
                                                      const GskTextureKey  *key,
                                                      guint                 texture_id);
guint          gsk_next_driver_load_texture          (GskNextDriver        *self,
                                                      GdkTexture           *texture,
                                                      int                   min_filter,
                                                      int                   mag_filter);
GskGLTexture  *gsk_next_driver_create_texture        (GskNextDriver        *self,
                                                      float                 width,
                                                      float                 height,
                                                      int                   min_filter,
                                                      int                   mag_filter);
GskGLTexture  *gsk_next_driver_acquire_texture       (GskNextDriver        *self,
                                                      float                 width,
                                                      float                 height,
                                                      int                   min_filter,
                                                      int                   mag_filter);
void           gsk_next_driver_release_texture       (GskNextDriver        *self,
                                                      GskGLTexture         *texture);
GskGLProgram  *gsk_next_driver_lookup_shader         (GskNextDriver        *self,
                                                      GskGLShader          *shader,
                                                      GError              **error);

G_END_DECLS

#endif /* __GSK_GL_DRIVER_PRIVATE_H__ */
