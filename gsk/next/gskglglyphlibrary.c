/* gskglglyphlibrary.c
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

#include "gskglglyphlibraryprivate.h"

G_DEFINE_TYPE (GskGLGlyphLibrary, gsk_gl_glyph_library, GSK_TYPE_GL_TEXTURE_LIBRARY)

GskGLGlyphLibrary *
gsk_gl_glyph_library_new (GdkGLContext *context)
{
  g_return_val_if_fail (GDK_IS_GL_CONTEXT (context), NULL);

  return g_object_new (GSK_TYPE_GL_GLYPH_LIBRARY,
                       "context", context,
                       NULL);
}

static guint
gsk_gl_glyph_key_hash (gconstpointer data)
{
  const GskGLGlyphKey *key = data;

  /* We do not store the hash within the key because GHashTable will already
   * store the hash value for us and so this is called only a single time per
   * cached item. This saves an extra 4 bytes per GskGLGlyphKey which means on
   * 64-bit, we fit nicely within 2 pointers (the smallest allocation size
   * for GSlice).
   */

  return GPOINTER_TO_UINT (key->font) ^
         key->glyph ^
         (key->xshift << 24) ^
         (key->yshift << 26) ^
         key->scale;
}

static gboolean
gsk_gl_glyph_key_equal (gconstpointer v1,
                        gconstpointer v2)
{
  return memcmp (v1, v2, sizeof (GskGLGlyphKey)) == 0;
}

static void
gsk_gl_glyph_key_free (gpointer data)
{
  g_slice_free (GskGLGlyphKey, data);
}

static void
gsk_gl_glyph_value_free (gpointer data)
{
  g_slice_free (GskGLGlyphValue, data);
}

static void
gsk_gl_glyph_library_finalize (GObject *object)
{
  GskGLGlyphLibrary *self = (GskGLGlyphLibrary *)object;

  g_clear_pointer (&self->hash_table, g_hash_table_unref);

  G_OBJECT_CLASS (gsk_gl_glyph_library_parent_class)->finalize (object);
}

static void
gsk_gl_glyph_library_class_init (GskGLGlyphLibraryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gsk_gl_glyph_library_finalize;
}

static void
gsk_gl_glyph_library_init (GskGLGlyphLibrary *self)
{
  self->hash_table = g_hash_table_new_full (gsk_gl_glyph_key_hash,
                                            gsk_gl_glyph_key_equal,
                                            gsk_gl_glyph_key_free,
                                            gsk_gl_glyph_value_free);
}

gboolean
gsk_gl_glyph_library_add (GskGLGlyphLibrary      *self,
                          const GskGLGlyphKey    *key,
                          const GskGLGlyphValue **out_value)
{
  g_assert (GSK_IS_GL_GLYPH_LIBRARY (self));
  g_assert (key != NULL);
  g_assert (out_value != NULL);

  return FALSE;
}
