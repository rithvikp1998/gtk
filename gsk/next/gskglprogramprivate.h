/* gskglprogramprivate.h
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

#ifndef __GSK_GL_PROGRAM_PRIVATE_H__
#define __GSK_GL_PROGRAM_PRIVATE_H__

#include "gskgltypes.h"

#include "gskglcommandqueueprivate.h"

G_BEGIN_DECLS

#define GSK_TYPE_GL_PROGRAM (gsk_gl_program_get_type())

G_DECLARE_FINAL_TYPE (GskGLProgram, gsk_gl_program, GSK, GL_PROGRAM, GObject)

struct _GskGLProgram
{
  GObject            parent_instance;
  int                id;
  char              *name;
  GArray            *uniform_locations;
  GskGLCommandQueue *command_queue;
  int                projection_location;
  int                modelview_location;
  int                viewport_location;
  int                clip_rect_location;
};

GskGLProgram *gsk_gl_program_new         (GskGLCommandQueue       *command_queue,
                                          const char              *name,
                                          int                      program_id);
gboolean      gsk_gl_program_add_uniform (GskGLProgram            *self,
                                          const char              *name,
                                          guint                    key);
void          gsk_gl_program_delete      (GskGLProgram            *self);
void          gsk_gl_program_begin_draw  (GskGLProgram            *self,
                                          const graphene_rect_t   *viewport,
                                          const graphene_matrix_t *projection,
                                          const graphene_matrix_t *modelview,
                                          const GskRoundedRect    *clip);
void          gsk_gl_program_end_draw    (GskGLProgram            *self);

static inline int
gsk_gl_program_get_uniform_location (GskGLProgram *self,
                                     guint         key)
{
  if G_LIKELY (key < self->uniform_locations->len)
    return g_array_index (self->uniform_locations, GLint, key);
  else
    return -1;
}

static inline void
gsk_gl_program_set_uniform1fv (GskGLProgram *self,
                               guint         key,
                               guint         count,
                               const float  *values)
{
  gsk_gl_command_queue_set_uniform1fv (self->command_queue, self->id,
                                       gsk_gl_program_get_uniform_location (self, key),
                                       count, values);
}

static inline void
gsk_gl_program_set_uniform_rounded_rect (GskGLProgram         *self,
                                         guint                 key,
                                         const GskRoundedRect *rounded_rect)
{
  gsk_gl_command_queue_set_uniform_rounded_rect (self->command_queue, self->id,
                                                 gsk_gl_program_get_uniform_location (self, key),
                                                 rounded_rect);
}

static inline void
gsk_gl_program_set_uniform1i (GskGLProgram *self,
                              guint         key,
                              int           value0)
{
  gsk_gl_command_queue_set_uniform1i (self->command_queue,
                                      self->id,
                                      gsk_gl_program_get_uniform_location (self, key),
                                      value0);
}

static inline void
gsk_gl_program_set_uniform2i (GskGLProgram *self,
                              guint         key,
                              int           value0,
                              int           value1)
{
  gsk_gl_command_queue_set_uniform2i (self->command_queue,
                                      self->id,
                                      gsk_gl_program_get_uniform_location (self, key),
                                      value0,
                                      value1);
}

static inline void
gsk_gl_program_set_uniform3i (GskGLProgram *self,
                              guint         key,
                              int           value0,
                              int           value1,
                              int           value2)
{
  gsk_gl_command_queue_set_uniform3i (self->command_queue,
                                      self->id,
                                      gsk_gl_program_get_uniform_location (self, key),
                                      value0,
                                      value1,
                                      value2);
}

static inline void
gsk_gl_program_set_uniform4i (GskGLProgram *self,
                              guint         key,
                              int           value0,
                              int           value1,
                              int           value2,
                              int           value3)
{
  gsk_gl_command_queue_set_uniform4i (self->command_queue,
                                      self->id,
                                      gsk_gl_program_get_uniform_location (self, key),
                                      value0,
                                      value1,
                                      value2,
                                      value3);
}

static inline void
gsk_gl_program_set_uniform1f (GskGLProgram *self,
                              guint         key,
                              float         value0)
{
  gsk_gl_command_queue_set_uniform1f (self->command_queue,
                                      self->id,
                                      gsk_gl_program_get_uniform_location (self, key),
                                      value0);
}

static inline void
gsk_gl_program_set_uniform2f (GskGLProgram *self,
                              guint         key,
                              float         value0,
                              float         value1)
{
  gsk_gl_command_queue_set_uniform2f (self->command_queue,
                                      self->id,
                                      gsk_gl_program_get_uniform_location (self, key),
                                      value0,
                                      value1);
}

static inline void
gsk_gl_program_set_uniform3f (GskGLProgram *self,
                              guint         key,
                              float         value0,
                              float         value1,
                              float         value2)
{
  gsk_gl_command_queue_set_uniform3f (self->command_queue,
                                      self->id,
                                      gsk_gl_program_get_uniform_location (self, key),
                                      value0,
                                      value1,
                                      value2);
}

static inline void
gsk_gl_program_set_uniform4f (GskGLProgram *self,
                              guint         key,
                              float         value0,
                              float         value1,
                              float         value2,
                              float         value3)
{
  gsk_gl_command_queue_set_uniform4f (self->command_queue,
                                      self->id,
                                      gsk_gl_program_get_uniform_location (self, key),
                                      value0,
                                      value1,
                                      value2,
                                      value3);
}

static inline void
gsk_gl_program_set_uniform_color (GskGLProgram  *self,
                                  guint          key,
                                  const GdkRGBA *color)
{
  gsk_gl_command_queue_set_uniform_color (self->command_queue,
                                          self->id,
                                          gsk_gl_program_get_uniform_location (self, key),
                                          color);
}

static inline void
gsk_gl_program_set_uniform_texture (GskGLProgram *self,
                                    guint         key,
                                    GLenum        texture_target,
                                    GLenum        texture_slot,
                                    guint         texture_id)
{
  gsk_gl_command_queue_set_uniform_texture (self->command_queue,
                                            self->id,
                                            gsk_gl_program_get_uniform_location (self, key),
                                            texture_target,
                                            texture_slot,
                                            texture_id);
}

static inline void
gsk_gl_program_set_uniform_matrix (GskGLProgram            *self,
                                   guint                    key,
                                   const graphene_matrix_t *matrix)
{
  gsk_gl_command_queue_set_uniform_matrix (self->command_queue,
                                           self->id,
                                           gsk_gl_program_get_uniform_location (self, key),
                                           matrix);
}

G_END_DECLS

#endif /* __GSK_GL_PROGRAM_PRIVATE_H__ */
