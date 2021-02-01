/* gskglprogram.c
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

#include "config.h"

#include "gskglcommandqueueprivate.h"
#include "gskglprogramprivate.h"
#include "gskgluniformstateprivate.h"

G_DEFINE_TYPE (GskGLProgram, gsk_gl_program, G_TYPE_OBJECT)

GskGLProgram *
gsk_gl_program_new (GskNextDriver *driver,
                    const char    *name,
                    int            program_id)
{
  GskGLProgram *self;

  g_return_val_if_fail (GSK_IS_NEXT_DRIVER (driver), NULL);
  g_return_val_if_fail (program_id >= -1, NULL);

  self = g_object_new (GSK_TYPE_GL_PROGRAM, NULL);
  self->id = program_id;
  self->name = g_strdup (name);
  self->driver = g_object_ref (driver);

  return self;
}

static void
gsk_gl_program_finalize (GObject *object)
{
  GskGLProgram *self = (GskGLProgram *)object;

  if (self->id >= 0)
    g_warning ("Leaking GLSL program %d (%s)",
               self->id,
               self->name ? self->name : "");

  g_clear_pointer (&self->name, g_free);
  g_clear_pointer (&self->uniform_locations, g_array_unref);
  g_clear_object (&self->driver);

  G_OBJECT_CLASS (gsk_gl_program_parent_class)->finalize (object);
}

static void
gsk_gl_program_class_init (GskGLProgramClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gsk_gl_program_finalize;
}

static void
gsk_gl_program_init (GskGLProgram *self)
{
  self->id = -1;
  self->uniform_locations = g_array_new (FALSE, TRUE, sizeof (GLint));
  self->viewport_location = -1;
  self->projection_location = -1;
  self->modelview_location = -1;
  self->clip_rect_location = -1;
  self->alpha_location = -1;
}

/**
 * gsk_gl_program_add_uniform:
 * @self: a #GskGLProgram
 * @name: the name of the uniform such as "u_source"
 * @key: the identifier to use for the uniform
 *
 * This method will create a mapping between @key and the location
 * of the uniform on the GPU. This simplifies calling code to not
 * need to know where the uniform location is and only register it
 * when creating the program.
 *
 * You might use this with an enum of all your uniforms for the
 * program and then register each of them like:
 *
 * ```
 * gsk_gl_program_add_uniform (program, "u_source", UNIFORM_SOURCE);
 * ```
 *
 * That allows you to set values for the program with something
 * like the following:
 *
 * ```
 * gsk_gl_program_set_uniform1i (program, UNIFORM_SOURCE, 1);
 * ```
 *
 * Returns: %TRUE if the uniform was found; otherwise %FALSE
 */
gboolean
gsk_gl_program_add_uniform (GskGLProgram *self,
                            const char   *name,
                            guint         key)
{
  const GLint invalid = -1;
  GLint location;

  g_return_val_if_fail (GSK_IS_GL_PROGRAM (self), FALSE);
  g_return_val_if_fail (name != NULL, FALSE);
  g_return_val_if_fail (key < 1024, FALSE);

  if (-1 == (location = glGetUniformLocation (self->id, name)))
    return FALSE;

  while (key >= self->uniform_locations->len)
    g_array_append_val (self->uniform_locations, invalid);
  g_array_index (self->uniform_locations, GLint, key) = location;

  if (key == UNIFORM_SHARED_MODELVIEW)
    self->modelview_location = location;
  else if (key == UNIFORM_SHARED_PROJECTION)
    self->projection_location = location;
  else if (key == UNIFORM_SHARED_VIEWPORT)
    self->viewport_location = location;
  else if (key == UNIFORM_SHARED_CLIP_RECT)
    self->clip_rect_location = location;
  else if (key == UNIFORM_SHARED_ALPHA)
    self->alpha_location = location;

#if 0
  g_print ("program [%d] %s uniform %s at location %d.\n",
           self->id, self->name, name, location);
#endif

  return TRUE;
}

/**
 * gsk_gl_program_delete:
 * @self: a #GskGLProgram
 *
 * Deletes the GLSL program.
 *
 * You must call gsk_gl_program_use() before and
 * gsk_gl_program_unuse() after this function.
 */
void
gsk_gl_program_delete (GskGLProgram *self)
{
  g_return_if_fail (GSK_IS_GL_PROGRAM (self));
  g_return_if_fail (self->driver->command_queue != NULL);

  gsk_gl_command_queue_delete_program (self->driver->command_queue, self->id);
  self->id = -1;
}

void
gsk_gl_program_begin_draw (GskGLProgram            *self,
                           const graphene_rect_t   *viewport,
                           const graphene_matrix_t *projection,
                           const graphene_matrix_t *modelview,
                           const GskRoundedRect    *clip,
                           float                    alpha)
{
  g_assert (GSK_IS_GL_PROGRAM (self));
  g_assert (viewport != NULL);
  g_assert (projection != NULL);
  g_assert (modelview != NULL);
  g_assert (clip != NULL);

  gsk_gl_command_queue_begin_draw (self->driver->command_queue, self->id, viewport);

  if (self->viewport_location > -1)
    gsk_gl_command_queue_set_uniform4f (self->driver->command_queue,
                                        self->id,
                                        self->viewport_location,
                                        viewport->origin.x,
                                        viewport->origin.y,
                                        viewport->size.width,
                                        viewport->size.height);

  if (self->modelview_location > -1)
    gsk_gl_command_queue_set_uniform_matrix (self->driver->command_queue,
                                             self->id,
                                             self->modelview_location,
                                             modelview);

  if (self->projection_location > -1)
    gsk_gl_command_queue_set_uniform_matrix (self->driver->command_queue,
                                             self->id,
                                             self->projection_location,
                                             projection);

  if (self->clip_rect_location > -1)
    {
      if (clip != NULL)
        gsk_gl_command_queue_set_uniform_rounded_rect (self->driver->command_queue,
                                                       self->id,
                                                       self->clip_rect_location,
                                                       clip);
      else
        gsk_gl_command_queue_set_uniform_rounded_rect (self->driver->command_queue,
                                                       self->id,
                                                       self->clip_rect_location,
                                                       &GSK_ROUNDED_RECT_INIT (0,
                                                                               0,
                                                                               viewport->size.width,
                                                                               viewport->size.height));
    }

  if (self->alpha_location > -1)
    gsk_gl_command_queue_set_uniform1f (self->driver->command_queue,
                                        self->id,
                                        self->alpha_location,
                                        alpha);
}
