/* gskgldriver.c
 *
 * Copyright 2017 Timm Bäder <mail@baedert.org>
 * Copyright 2018 Matthias Clasen <mclasen@redhat.com>
 * Copyright 2018 Alexander Larsson <alexl@redhat.com>
 * Copyright 2020 Christian Hergert <chergert@redhat.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <gdk/gdkglcontextprivate.h>
#include <gdk/gdktextureprivate.h>
#include <gdk/gdkgltextureprivate.h>
#include <gsk/gskdebugprivate.h>
#include <gsk/gskrendererprivate.h>

#include "gskglcommandqueueprivate.h"
#include "gskglcompilerprivate.h"
#include "gskgldriverprivate.h"
#include "gskglglyphlibraryprivate.h"
#include "gskgliconlibraryprivate.h"
#include "gskglprogramprivate.h"
#include "gskglshadowlibraryprivate.h"
#include "gskgltexturepoolprivate.h"

#define TEXTURES_CACHED_FOR_N_FRAMES 5

G_DEFINE_TYPE (GskNextDriver, gsk_next_driver, G_TYPE_OBJECT)

static guint
texture_key_hash (gconstpointer v)
{
  const GskTextureKey *k = (const GskTextureKey *)v;

  return GPOINTER_TO_UINT (k->pointer)
         + (guint)(k->scale_x * 100)
         + (guint)(k->scale_y * 100)
         + (guint)k->filter * 2 +
         + (guint)k->pointer_is_child;
}

static gboolean
texture_key_equal (gconstpointer v1, gconstpointer v2)
{
  const GskTextureKey *k1 = (const GskTextureKey *)v1;
  const GskTextureKey *k2 = (const GskTextureKey *)v2;

  return k1->pointer == k2->pointer &&
         k1->scale_x == k2->scale_x &&
         k1->scale_y == k2->scale_y &&
         k1->filter == k2->filter &&
         k1->pointer_is_child == k2->pointer_is_child &&
         (!k1->pointer_is_child || graphene_rect_equal (&k1->parent_rect, &k2->parent_rect));
}

static void
gsk_gl_texture_free (gpointer data)
{
  GskGLTexture *texture = data;

  if (texture != NULL)
    {
      g_slice_free (GskGLTexture, texture);
    }
}

static void
remove_texture_key_for_id (GskNextDriver *self,
                           guint          texture_id)
{
  GskTextureKey *key;

  g_assert (GSK_IS_NEXT_DRIVER (self));
  g_assert (texture_id > 0);

  if (g_hash_table_steal_extended (self->texture_id_to_key,
                                   GUINT_TO_POINTER (texture_id),
                                   NULL,
                                   (gpointer *)&key))
    g_hash_table_remove (self->key_to_texture_id, key);
}

static void
gsk_gl_texture_destroyed (gpointer data)
{
  ((GskGLTexture *)data)->user = NULL;
}

static guint
gsk_next_driver_collect_unused_textures (GskNextDriver *self,
                                         gint64         watermark)
{
  GHashTableIter iter;
  gpointer k, v;
  guint old_size;

  g_assert (GSK_IS_NEXT_DRIVER (self));

  old_size = g_hash_table_size (self->textures);

  g_hash_table_iter_init (&iter, self->textures);
  while (g_hash_table_iter_next (&iter, &k, &v))
    {
      GskGLTexture *t = v;

      if (t->user || t->permanent)
        continue;

      if (t->last_used_in_frame <= watermark)
        {
          remove_texture_key_for_id (self, t->texture_id);
          g_hash_table_iter_remove (&iter);
        }
    }

  return old_size - g_hash_table_size (self->textures);
}

static void
gsk_next_driver_dispose (GObject *object)
{
  GskNextDriver *self = (GskNextDriver *)object;

#define GSK_GL_NO_UNIFORMS
#define GSK_GL_ADD_UNIFORM(pos, KEY, name)
#define GSK_GL_DEFINE_PROGRAM(name, resource, uniforms) \
  G_STMT_START {                                        \
    if (self->name)                                     \
      gsk_gl_program_delete (self->name);               \
    g_clear_object (&self->name);                       \
  } G_STMT_END;
# include "gskglprograms.defs"
#undef GSK_GL_NO_UNIFORMS
#undef GSK_GL_ADD_UNIFORM
#undef GSK_GL_DEFINE_PROGRAM

  if (self->command_queue != NULL)
    {
      gsk_gl_command_queue_make_current (self->command_queue);
      gsk_next_driver_collect_unused_textures (self, 0);
      g_clear_object (&self->command_queue);
    }

  g_assert (self->autorelease_framebuffers->len == 0);

  g_clear_pointer (&self->autorelease_framebuffers, g_array_unref);
  g_clear_pointer (&self->key_to_texture_id, g_hash_table_unref);
  g_clear_pointer (&self->textures, g_hash_table_unref);
  g_clear_pointer (&self->key_to_texture_id, g_hash_table_unref);
  g_clear_pointer (&self->texture_id_to_key, g_hash_table_unref);

  G_OBJECT_CLASS (gsk_next_driver_parent_class)->dispose (object);
}

static void
gsk_next_driver_class_init (GskNextDriverClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gsk_next_driver_dispose;
}

static void
gsk_next_driver_init (GskNextDriver *self)
{
  self->autorelease_framebuffers = g_array_new (FALSE, FALSE, sizeof (guint));
  self->textures = g_hash_table_new_full (NULL, NULL, NULL, gsk_gl_texture_free);
  self->texture_id_to_key = g_hash_table_new (NULL, NULL);
  self->key_to_texture_id = g_hash_table_new_full (texture_key_hash,
                                                   texture_key_equal,
                                                   g_free,
                                                   NULL);
  gsk_gl_texture_pool_init (&self->texture_pool);
}

static gboolean
gsk_next_driver_load_programs (GskNextDriver  *self,
                               GError        **error)
{
  GskGLCompiler *compiler;
  gboolean ret = FALSE;

  g_assert (GSK_IS_NEXT_DRIVER (self));
  g_assert (GSK_IS_GL_COMMAND_QUEUE (self->command_queue));

  compiler = gsk_gl_compiler_new (self->command_queue, self->debug);

  /* Setup preambles that are shared by all shaders */
  gsk_gl_compiler_set_preamble_from_resource (compiler,
                                              GSK_GL_COMPILER_ALL,
                                              "/org/gtk/libgsk/glsl/preamble.glsl");
  gsk_gl_compiler_set_preamble_from_resource (compiler,
                                              GSK_GL_COMPILER_VERTEX,
                                              "/org/gtk/libgsk/glsl/preamble.vs.glsl");
  gsk_gl_compiler_set_preamble_from_resource (compiler,
                                              GSK_GL_COMPILER_FRAGMENT,
                                              "/org/gtk/libgsk/glsl/preamble.fs.glsl");

  /* Setup attributes that are provided via VBO */
  gsk_gl_compiler_bind_attribute (compiler, "aPosition", 0);
  gsk_gl_compiler_bind_attribute (compiler, "aUv", 1);

  /* Use XMacros to register all of our programs and their uniforms */
#define GSK_GL_NO_UNIFORMS
#define GSK_GL_ADD_UNIFORM(pos, KEY, name)                                                      \
  gsk_gl_program_add_uniform (program, #name, UNIFORM_##KEY);
#define GSK_GL_DEFINE_PROGRAM(name, resource, uniforms)                                         \
  G_STMT_START {                                                                                \
    GskGLProgram *program;                                                                      \
    gboolean have_alpha;                                                                        \
                                                                                                \
    gsk_gl_compiler_set_source_from_resource (compiler, GSK_GL_COMPILER_ALL, resource);         \
                                                                                                \
    if (!(program = gsk_gl_compiler_compile (compiler, #name, error)))                          \
      goto failure;                                                                             \
                                                                                                \
    have_alpha = gsk_gl_program_add_uniform (program, "u_alpha", UNIFORM_SHARED_ALPHA);         \
    gsk_gl_program_add_uniform (program, "u_source", UNIFORM_SHARED_SOURCE);                    \
    gsk_gl_program_add_uniform (program, "u_clip_rect", UNIFORM_SHARED_CLIP_RECT);              \
    gsk_gl_program_add_uniform (program, "u_viewport", UNIFORM_SHARED_VIEWPORT);                \
    gsk_gl_program_add_uniform (program, "u_projection", UNIFORM_SHARED_PROJECTION);            \
    gsk_gl_program_add_uniform (program, "u_modelview", UNIFORM_SHARED_MODELVIEW);              \
                                                                                                \
    uniforms                                                                                    \
                                                                                                \
    if (have_alpha)                                                                             \
      gsk_gl_program_set_uniform1f (program, UNIFORM_SHARED_ALPHA, 1.0f);                       \
                                                                                                \
    *(GskGLProgram **)(((guint8 *)self) + G_STRUCT_OFFSET (GskNextDriver, name)) =              \
        g_steal_pointer (&program);                                                             \
  } G_STMT_END;
# include "gskglprograms.defs"
#undef GSK_GL_DEFINE_PROGRAM
#undef GSK_GL_ADD_UNIFORM

  ret = TRUE;

failure:
  g_clear_object (&compiler);

  return ret;
}

/**
 * gsk_next_driver_autorelease_framebuffer:
 * @self: a #GskNextDriver
 * @framebuffer_id: the id of the OpenGL framebuffer
 *
 * Marks @framebuffer_id to be deleted when the current frame
 * has completed.
 */
void
gsk_next_driver_autorelease_framebuffer (GskNextDriver *self,
                                         guint          framebuffer_id)
{
  g_return_if_fail (GSK_IS_NEXT_DRIVER (self));

  g_array_append_val (self->autorelease_framebuffers, framebuffer_id);
}

GskNextDriver *
gsk_next_driver_new (GskGLCommandQueue  *command_queue,
                     gboolean            debug,
                     GError            **error)
{
  GskNextDriver *self;
  GdkGLContext *context;

  g_return_val_if_fail (GSK_IS_GL_COMMAND_QUEUE (command_queue), NULL);

  context = gsk_gl_command_queue_get_context (command_queue);

  gdk_gl_context_make_current (context);

  self = g_object_new (GSK_TYPE_NEXT_DRIVER, NULL);
  self->command_queue = g_object_ref (command_queue);
  self->debug = !!debug;

  if (!gsk_next_driver_load_programs (self, error))
    {
      g_object_unref (self);
      return NULL;
    }

  self->glyphs = gsk_gl_glyph_library_new (context);
  self->icons = gsk_gl_icon_library_new (context);
  self->shadows = gsk_gl_shadow_library_new (context);

  return g_steal_pointer (&self);
}

/**
 * gsk_next_driver_begin_frame:
 * @self: a #GskNextDriver
 *
 * Begin a new frame.
 *
 * Texture atlases, pools, and other resources will be prepared to
 * draw the next frame.
 */
void
gsk_next_driver_begin_frame (GskNextDriver *self)
{
  g_return_if_fail (GSK_IS_NEXT_DRIVER (self));
  g_return_if_fail (self->in_frame == FALSE);

  self->in_frame = TRUE;
  self->current_frame_id++;

  gsk_gl_command_queue_make_current (self->command_queue);
  gsk_gl_command_queue_begin_frame (self->command_queue);

  gsk_gl_texture_library_begin_frame (GSK_GL_TEXTURE_LIBRARY (self->icons));
  gsk_gl_texture_library_begin_frame (GSK_GL_TEXTURE_LIBRARY (self->glyphs));
  gsk_gl_texture_library_begin_frame (GSK_GL_TEXTURE_LIBRARY (self->shadows));

  /* We avoid collecting on every frame. To do so would accidentally
   * drop some textures we want cached but fell out of the damage clip
   * on this cycle through the rendering.
   */
  gsk_next_driver_collect_unused_textures (self,
                                           self->current_frame_id - TEXTURES_CACHED_FOR_N_FRAMES);
}

/**
 * gsk_next_driver_end_frame:
 * @self: a #GskNextDriver
 *
 * Clean up resources from drawing the current frame.
 *
 * Temporary resources used while drawing will be released.
 */
void
gsk_next_driver_end_frame (GskNextDriver *self)
{
  g_return_if_fail (GSK_IS_NEXT_DRIVER (self));
  g_return_if_fail (self->in_frame == TRUE);

  gsk_gl_command_queue_end_frame (self->command_queue);

  gsk_gl_texture_library_end_frame (GSK_GL_TEXTURE_LIBRARY (self->icons));
  gsk_gl_texture_library_end_frame (GSK_GL_TEXTURE_LIBRARY (self->glyphs));
  gsk_gl_texture_library_end_frame (GSK_GL_TEXTURE_LIBRARY (self->shadows));

  if (self->autorelease_framebuffers->len > 0)
    {
      glDeleteFramebuffers (self->autorelease_framebuffers->len,
                            (GLuint *)(gpointer)self->autorelease_framebuffers->data);
      self->autorelease_framebuffers->len = 0;
    }

  gsk_gl_texture_pool_clear (&self->texture_pool);

  self->in_frame = FALSE;
}

GdkGLContext *
gsk_next_driver_get_context (GskNextDriver *self)
{
  g_return_val_if_fail (GSK_IS_NEXT_DRIVER (self), NULL);
  g_return_val_if_fail (GSK_IS_GL_COMMAND_QUEUE (self->command_queue), NULL);

  return gsk_gl_command_queue_get_context (self->command_queue);
}

/**
 * gsk_next_driver_create_render_target:
 * @self: a #GskNextDriver
 * @width: the width for the render target
 * @height: the height for the render target
 * @out_fbo_id: (out): location for framebuffer id
 * @out_texture_id: (out): location for texture id
 *
 * Creates a new render target where @out_texture_id is bound
 * to the framebuffer @out_fbo_id using glFramebufferTexture2D().
 *
 * Returns: %TRUE if successful; otherwise %FALSE and @out_fbo_id and
 *   @out_texture_id are undefined.
 */
gboolean
gsk_next_driver_create_render_target (GskNextDriver *self,
                                      int            width,
                                      int            height,
                                      guint         *out_fbo_id,
                                      guint         *out_texture_id)
{
  g_return_val_if_fail (GSK_IS_NEXT_DRIVER (self), FALSE);
  g_return_val_if_fail (GSK_IS_GL_COMMAND_QUEUE (self->command_queue), FALSE);

  return gsk_gl_command_queue_create_render_target (self->command_queue,
                                                    width,
                                                    height,
                                                    out_fbo_id,
                                                    out_texture_id);
}

/**
 * gsk_next_driver_lookup_texture:
 * @self: a #GskNextDriver
 * @key: the key for the texture
 *
 * Looks up a texture in the texture cache by @key.
 *
 * If the texture could not be found, then zero is returned.
 *
 * Returns: a positive integer if the texture was found; otherwise 0.
 */
guint
gsk_next_driver_lookup_texture (GskNextDriver       *self,
                                const GskTextureKey *key)
{
  gpointer id;

  g_return_val_if_fail (GSK_IS_NEXT_DRIVER (self), FALSE);
  g_return_val_if_fail (key != NULL, FALSE);

  if (g_hash_table_lookup_extended (self->key_to_texture_id, key, NULL, &id))
    {
      GskGLTexture *texture = g_hash_table_lookup (self->textures, id);

      if (texture != NULL)
        texture->last_used_in_frame = self->current_frame_id;

      return GPOINTER_TO_UINT (id);
    }

  return 0;
}

/**
 * gsk_next_driver_cache_texture:
 * @self: a #GskNextDriver
 * @key: the key for the texture
 * @texture_id: the id of the texture to be cached
 *
 * Inserts @texture_id into the texture cache using @key.
 *
 * Textures can be looked up by @key after calling this function using
 * gsk_next_driver_lookup_texture().
 *
 * Textures that have not been used within a number of frames will be
 * purged from the texture cache automatically.
 */
void
gsk_next_driver_cache_texture (GskNextDriver       *self,
                               const GskTextureKey *key,
                               guint                texture_id)
{
  GskTextureKey *k;

  g_return_if_fail (GSK_IS_NEXT_DRIVER (self));
  g_return_if_fail (key != NULL);
  g_return_if_fail (texture_id > 0);

  k = g_memdup (key, sizeof *key);

  g_hash_table_insert (self->key_to_texture_id, k, GUINT_TO_POINTER (texture_id));
  g_hash_table_insert (self->texture_id_to_key, GUINT_TO_POINTER (texture_id), k);
}

/**
 * gsk_next_driver_load_texture:
 * @self: a #GdkTexture
 * @texture: a #GdkTexture
 * @min_filter: GL_NEAREST or GL_LINEAR
 * @mag_filter: GL_NEAREST or GL_LINEAR
 *
 * Loads a #GdkTexture by uploading the contents to the GPU when
 * necessary. If @texture is a #GdkGLTexture, it can be used without
 * uploading contents to the GPU.
 *
 * If the texture has already been uploaded and not yet released
 * from cache, this function returns that texture id without further
 * work.
 *
 * If the texture has not been used for a number of frames, it will
 * be removed from cache.
 *
 * There is no need to release the resulting texture identifier after
 * using it. It will be released automatically.
 *
 * Returns: a texture identifier
 */
guint
gsk_next_driver_load_texture (GskNextDriver *self,
                              GdkTexture    *texture,
                              int            min_filter,
                              int            mag_filter)
{
  GdkGLContext *context;
  GdkTexture *downloaded_texture = NULL;
  GdkTexture *source_texture;
  GskGLTexture *t;

  g_return_val_if_fail (GSK_IS_NEXT_DRIVER (self), 0);
  g_return_val_if_fail (GDK_IS_TEXTURE (texture), 0);
  g_return_val_if_fail (GSK_IS_GL_COMMAND_QUEUE (self->command_queue), 0);

  context = self->command_queue->context;

  if (GDK_IS_GL_TEXTURE (texture))
    {
      GdkGLContext *texture_context = gdk_gl_texture_get_context ((GdkGLTexture *)texture);
      GdkGLContext *shared_context = gdk_gl_context_get_shared_context (context);

      if (texture_context == context ||
          (shared_context != NULL &&
           shared_context == gdk_gl_context_get_shared_context (texture_context)))

        {
          /* A GL texture from the same GL context is a simple task... */
          return gdk_gl_texture_get_id ((GdkGLTexture *)texture);
        }
      else
        {
          cairo_surface_t *surface;

          /* In this case, we have to temporarily make the texture's
           * context the current one, download its data into our context
           * and then create a texture from it. */
          if (texture_context != NULL)
            gdk_gl_context_make_current (texture_context);

          surface = gdk_texture_download_surface (texture);
          downloaded_texture = gdk_texture_new_for_surface (surface);
          cairo_surface_destroy (surface);

          gdk_gl_context_make_current (context);

          source_texture = downloaded_texture;
        }
    }
  else
    {
      if ((t = gdk_texture_get_render_data (texture, self)))
        {
          if (t->min_filter == min_filter && t->mag_filter == mag_filter)
            return t->texture_id;
        }

      source_texture = texture;
    }

  t = g_slice_new0 (GskGLTexture);
  t->width = gdk_texture_get_width (texture);
  t->height = gdk_texture_get_height (texture);
  t->last_used_in_frame = self->current_frame_id;
  t->min_filter = min_filter;
  t->mag_filter = mag_filter;
  t->texture_id = gsk_gl_command_queue_upload_texture (self->command_queue,
                                                       source_texture,
                                                       0,
                                                       0,
                                                       t->width,
                                                       t->height,
                                                       t->min_filter,
                                                       t->mag_filter);

  if (gdk_texture_set_render_data (texture, self, t, gsk_gl_texture_destroyed))
    t->user = texture;

  gdk_gl_context_label_object_printf (context, GL_TEXTURE, t->texture_id,
                                      "GdkTexture<%p> %d", texture, t->texture_id);

  g_clear_object (&downloaded_texture);

  return t->texture_id;
}

/**
 * gsk_next_driver_create_texture:
 * @self: a #GskNextDriver
 * @width: the width of the texture
 * @height: the height of the texture
 * @min_filter: GL_NEAREST or GL_LINEAR
 * @mag_filter: GL_NEAREST or GL_FILTER
 *
 * Creates a new texture immediately that can be used by the caller
 * to upload data, map to a framebuffer, or other uses which may
 * modify the texture immediately.
 *
 * Use this instead of gsk_next_driver_acquire_texture() when you need
 * to be able to modify the texture immediately instead of just when the
 * pipeline is executing. Otherwise, gsk_next_driver_acquire_texture()
 * provides more chances for re-use of textures, reducing the VRAM overhead
 * on the GPU.
 *
 * Use gsk_next_driver_release_texture() to release this texture back into
 * the pool so it may be reused later in the pipeline.
 *
 * Returns: a #GskGLTexture which can be returned to the pool with
 *   gsk_next_driver_release_texture().
 */
GskGLTexture *
gsk_next_driver_create_texture (GskNextDriver *self,
                                float          width,
                                float          height,
                                int            min_filter,
                                int            mag_filter)
{
  g_return_val_if_fail (GSK_IS_NEXT_DRIVER (self), NULL);

  return gsk_gl_texture_pool_get (&self->texture_pool,
                                  width, height,
                                  min_filter, mag_filter,
                                  TRUE);
}

/**
 * gsk_next_driver_acquire_texture:
 * @self: a #GskNextDriver
 * @width: the min width of the texture necessary
 * @height: the min height of the texture necessary
 * @min_filter: GL_NEAREST or GL_LINEAR
 * @mag_filter: GL_NEAREST or GL_LINEAR
 *
 * This function acquires a #GskGLTexture from the texture pool. Doing
 * so increases the chances for reduced VRAM usage in the GPU by having
 * fewer textures in use at one time. Batches later in the stream can
 * use the same texture memory of a previous batch.
 *
 * Consumers of this function are not allowed to modify @texture
 * immediately, it must wait until batches are being processed as
 * the texture may contain contents used earlier in the pipeline.
 *
 * Returns: a #GskGLTexture that may be returned to the pool with
 *   gsk_next_driver_release_texture().
 */
GskGLTexture *
gsk_next_driver_acquire_texture (GskNextDriver *self,
                                 float          width,
                                 float          height,
                                 int            min_filter,
                                 int            mag_filter)
{
  g_return_val_if_fail (GSK_IS_NEXT_DRIVER (self), NULL);

  return gsk_gl_texture_pool_get (&self->texture_pool,
                                  width, height,
                                  min_filter, mag_filter,
                                  FALSE);
}

/**
 * gsk_gl_driver_release_texture:
 * @self: a #GskNextDriver
 * @texture: a #GskGLTexture
 *
 * Releases @texture back into the pool so that it can be used later
 * in the command stream by future batches. This helps reduce VRAM
 * usage on the GPU.
 *
 * When the frame has completed, pooled textures will be released
 * to free additional VRAM back to the system.
 */
void
gsk_next_driver_release_texture (GskNextDriver *self,
                                 GskGLTexture  *texture)
{
  g_return_if_fail (GSK_IS_NEXT_DRIVER (self));

  gsk_gl_texture_pool_put (&self->texture_pool, texture);
}
