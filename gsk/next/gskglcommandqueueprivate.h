/* gskglcommandqueueprivate.h
 *
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

#ifndef __GSK_GL_COMMAND_QUEUE_PRIVATE_H__
#define __GSK_GL_COMMAND_QUEUE_PRIVATE_H__

#include "gskgltypesprivate.h"

#include "gskglattachmentstateprivate.h"
#include "gskgluniformstateprivate.h"

G_BEGIN_DECLS

#define GSK_TYPE_GL_COMMAND_QUEUE (gsk_gl_command_queue_get_type())

G_DECLARE_FINAL_TYPE (GskGLCommandQueue, gsk_gl_command_queue, GSK, GL_COMMAND_QUEUE, GObject)

struct _GskGLCommandQueue
{
  GObject parent_instance;

  /* The GdkGLContext we make current before executing GL commands. */
  GdkGLContext *context;

  /* Array of GskGLCommandBatch which is a fixed size structure that will
   * point into offsets of other arrays so that all similar data is stored
   * together. The idea here is that we reduce the need for pointers so that
   * using g_realloc()'d arrays is fine.
   */
  GArray *batches;

  /* Contains array of vertices and some wrapper code to help upload them
   * to the GL driver. We can also tweak this to use double buffered arrays
   * if we find that to be faster on some hardware and/or drivers.
   */
  GskGLBuffer *vertices;

  /* The GskGLAttachmentState contains information about our FBO and texture
   * attachments as we process incoming operations. We snapshot them into
   * various batches so that we can compare differences between merge
   * candidates.
   */
  GskGLAttachmentState *attachments;

  /* The uniform state across all programs. We snapshot this into batches so
   * that we can compare uniform state between batches to give us more
   * chances at merging draw commands.
   */
  GskGLUniformState *uniforms;

  /* Array of GskGLCommandDraw which allows us to have a static size field
   * in GskGLCommandBatch to coalesce draws. Multiple GskGLCommandDraw may
   * be processed together (and out-of-order) to reduce the number of state
   * changes when submitting commands.
   */
  GArray *batch_draws;

  /* Array of GskGLCommandBind which denote what textures need to be attached
   * to which slot. GskGLCommandDraw.bind_offset and bind_count reference this
   * array to determine what to attach.
   */
  GArray *batch_binds;

  /* Array of GskGLCommandUniform denoting which uniforms must be updated
   * before the glDrawArrays() may be called. These are referenced from the
   * GskGLCommandDraw.uniform_offset and uniform_count fields.
   */
  GArray *batch_uniforms;

  /* Sometimes we want to save attachment state so that operations we do
   * cannot affect anything that is known to the command queue. We call
   * gsk_gl_command_queue_save()/restore() which stashes attachment state
   * into this pointer array.
   */
  GPtrArray *saved_state;

  /* String storage for debug groups */
  GStringChunk *debug_groups;

  /* Discovered max texture size when loading the command queue so that we
   * can either scale down or slice textures to fit within this size. Assumed
   * to be both height and width.
   */
  int max_texture_size;

  /* The index of the last batch in @batches, which may not be the element
   * at the end of the array, as batches can be reordered. This is used to
   * update the "next" index when adding a new batch.
   */
  int tail_batch_index;

  /* If we're inside of a begin_draw()/end_draw() pair. */
  guint in_draw : 1;
};

GskGLCommandQueue *gsk_gl_command_queue_new                  (GdkGLContext             *context,
                                                              GskGLUniformState        *uniforms);
GdkGLContext      *gsk_gl_command_queue_get_context          (GskGLCommandQueue        *self);
void               gsk_gl_command_queue_make_current         (GskGLCommandQueue        *self);
void               gsk_gl_command_queue_begin_frame          (GskGLCommandQueue        *self);
void               gsk_gl_command_queue_end_frame            (GskGLCommandQueue        *self);
void               gsk_gl_command_queue_execute              (GskGLCommandQueue        *self,
                                                              guint                     surface_height,
                                                              guint                     scale_factor,
                                                              const cairo_region_t     *scissor);
int                gsk_gl_command_queue_upload_texture       (GskGLCommandQueue        *self,
                                                              GdkTexture               *texture,
                                                              guint                     x_offset,
                                                              guint                     y_offset,
                                                              guint                     width,
                                                              guint                     height,
                                                              int                       min_filter,
                                                              int                       mag_filter);
int                gsk_gl_command_queue_create_texture       (GskGLCommandQueue        *self,
                                                              int                       width,
                                                              int                       height,
                                                              int                       min_filter,
                                                              int                       mag_filter);
guint              gsk_gl_command_queue_create_framebuffer   (GskGLCommandQueue        *self);
gboolean           gsk_gl_command_queue_create_render_target (GskGLCommandQueue        *self,
                                                              int                       width,
                                                              int                       height,
                                                              int                       min_filter,
                                                              int                       mag_filter,
                                                              guint                    *out_fbo_id,
                                                              guint                    *out_texture_id);
void               gsk_gl_command_queue_delete_program       (GskGLCommandQueue        *self,
                                                              guint                     program_id);
void               gsk_gl_command_queue_bind_framebuffer     (GskGLCommandQueue        *self,
                                                              guint                     framebuffer);
void               gsk_gl_command_queue_clear                (GskGLCommandQueue        *self,
                                                              guint                     clear_bits,
                                                              const graphene_rect_t    *viewport);
void               gsk_gl_command_queue_push_debug_group     (GskGLCommandQueue        *self,
                                                              const char               *message);
void               gsk_gl_command_queue_pop_debug_group      (GskGLCommandQueue        *self);
void               gsk_gl_command_queue_begin_draw           (GskGLCommandQueue        *self,
                                                              guint                     program,
                                                              const graphene_rect_t    *viewport);
void               gsk_gl_command_queue_end_draw             (GskGLCommandQueue        *self);
GskGLDrawVertex   *gsk_gl_command_queue_add_vertices         (GskGLCommandQueue        *self,
                                                              const GskGLDrawVertex     vertices[GSK_GL_N_VERTICES]);

static inline void
gsk_gl_command_queue_set_uniform1ui (GskGLCommandQueue *self,
                                     guint              program,
                                     guint              location,
                                     int                value0)
{
  gsk_gl_uniform_state_set1ui (self->uniforms, program, location, value0);
}

static inline void
gsk_gl_command_queue_set_uniform1i (GskGLCommandQueue *self,
                                    guint              program,
                                    guint              location,
                                    int                value0)
{
  gsk_gl_uniform_state_set1i (self->uniforms, program, location, value0);
}

static inline void
gsk_gl_command_queue_set_uniform2i (GskGLCommandQueue *self,
                                    guint              program,
                                    guint              location,
                                    int                value0,
                                    int                value1)
{
  gsk_gl_uniform_state_set2i (self->uniforms, program, location, value0, value1);
}

static inline void
gsk_gl_command_queue_set_uniform3i (GskGLCommandQueue *self,
                                    guint              program,
                                    guint              location,
                                    int                value0,
                                    int                value1,
                                    int                value2)
{
  gsk_gl_uniform_state_set3i (self->uniforms, program, location, value0, value1, value2);
}

static inline void
gsk_gl_command_queue_set_uniform4i (GskGLCommandQueue *self,
                                    guint              program,
                                    guint              location,
                                    int                value0,
                                    int                value1,
                                    int                value2,
                                    int                value3)
{
  gsk_gl_uniform_state_set4i (self->uniforms, program, location, value0, value1, value2, value3);
}

static inline void
gsk_gl_command_queue_set_uniform1f (GskGLCommandQueue *self,
                                    guint              program,
                                    guint              location,
                                    float              value0)
{
  gsk_gl_uniform_state_set1f (self->uniforms, program, location, value0);
}

static inline void
gsk_gl_command_queue_set_uniform2f (GskGLCommandQueue *self,
                                    guint              program,
                                    guint              location,
                                    float              value0,
                                    float              value1)
{
  gsk_gl_uniform_state_set2f (self->uniforms, program, location, value0, value1);
}

static inline void
gsk_gl_command_queue_set_uniform3f (GskGLCommandQueue *self,
                                    guint              program,
                                    guint              location,
                                    float              value0,
                                    float              value1,
                                    float              value2)
{
  gsk_gl_uniform_state_set3f (self->uniforms, program, location, value0, value1, value2);
}

static inline void
gsk_gl_command_queue_set_uniform4f (GskGLCommandQueue *self,
                                    guint              program,
                                    guint              location,
                                    float              value0,
                                    float              value1,
                                    float              value2,
                                    float              value3)
{
  gsk_gl_uniform_state_set4f (self->uniforms, program, location, value0, value1, value2, value3);
}

static inline void
gsk_gl_command_queue_set_uniform1fv (GskGLCommandQueue *self,
                                     guint              program,
                                     guint              location,
                                     gsize              count,
                                     const float       *value)
{
  gsk_gl_uniform_state_set1fv (self->uniforms, program, location, count, value);
}

static inline void
gsk_gl_command_queue_set_uniform2fv (GskGLCommandQueue *self,
                                     guint              program,
                                     guint              location,
                                     gsize              count,
                                     const float       *value)
{
  gsk_gl_uniform_state_set2fv (self->uniforms, program, location, count, value);
}

static inline void
gsk_gl_command_queue_set_uniform3fv (GskGLCommandQueue *self,
                                     guint              program,
                                     guint              location,
                                     gsize              count,
                                     const float       *value)
{
  gsk_gl_uniform_state_set3fv (self->uniforms, program, location, count, value);
}

static inline void
gsk_gl_command_queue_set_uniform4fv (GskGLCommandQueue *self,
                                     guint              program,
                                     guint              location,
                                     gsize              count,
                                     const float       *value)
{
  gsk_gl_uniform_state_set4fv (self->uniforms, program, location, count, value);
}

static inline void
gsk_gl_command_queue_set_uniform_matrix (GskGLCommandQueue       *self,
                                         guint                    program,
                                         guint                    location,
                                         const graphene_matrix_t *matrix)
{
  gsk_gl_uniform_state_set_matrix (self->uniforms, program, location, matrix);
}

static inline void
gsk_gl_command_queue_set_uniform_color (GskGLCommandQueue *self,
                                        guint              program,
                                        guint              location,
                                        const GdkRGBA     *color)
{
  gsk_gl_uniform_state_set_color (self->uniforms, program, location, color);
}

/**
 * gsk_gl_command_queue_set_uniform_texture:
 * @self: A #GskGLCommandQueue
 * @program: the program id
 * @location: the location of the uniform
 * @texture_target: a texture target such as %GL_TEXTURE_2D
 * @texture_slot: the texture slot such as %GL_TEXTURE0 or %GL_TEXTURE1
 * @texture_id: the id of the texture from glGenTextures()
 *
 * This sets the value of a uniform to map to @texture_slot (after subtracting
 * GL_TEXTURE0 from the value) and ensures that @texture_id is available in the
 * same texturing slot, ensuring @texture_target.
 */
static inline void
gsk_gl_command_queue_set_uniform_texture (GskGLCommandQueue *self,
                                          guint              program,
                                          guint              location,
                                          GLenum             texture_target,
                                          GLenum             texture_slot,
                                          guint              texture_id)
{
  gsk_gl_attachment_state_bind_texture (self->attachments, texture_target, texture_slot, texture_id);
  gsk_gl_uniform_state_set_texture (self->uniforms, program, location, texture_slot);
}

/**
 * gsk_gl_command_queue_set_uniform_rounded_rect:
 * @self: a #GskGLCommandQueue
 * @program: the program to execute
 * @location: the location of the uniform
 * @rounded_rect: the rounded rect to apply
 *
 * Sets a uniform that is expecting a rounded rect. This is stored as a
 * 4fv using glUniform4fv() when uniforms are applied to the progrma.
 */
static inline void
gsk_gl_command_queue_set_uniform_rounded_rect (GskGLCommandQueue    *self,
                                               guint                 program,
                                               guint                 location,
                                               const GskRoundedRect *rounded_rect)
{
  gsk_gl_uniform_state_set_rounded_rect (self->uniforms, program, location, rounded_rect);
}

G_END_DECLS

#endif /* __GSK_GL_COMMAND_QUEUE_PRIVATE_H__ */
