/* gskglglyphlibraryprivate.h
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

#ifndef __GSK_GL_GLYPH_LIBRARY_PRIVATE_H__
#define __GSK_GL_GLYPH_LIBRARY_PRIVATE_H__

#include <pango/pango.h>

#include "gskgltexturelibraryprivate.h"

G_BEGIN_DECLS

typedef struct _GskGLGlyphKey
{
  PangoFont *font;
  PangoGlyph glyph;
  guint xshift : 3;
  guint yshift : 3;
  guint scale  : 26; /* times 1024 */
} GskGLGlyphKey;

#if GLIB_SIZEOF_VOID_P == 8
G_STATIC_ASSERT (sizeof (GskGLGlyphKey) == 16);
#elif GLIB_SIZEOF_VOID_P == 4
G_STATIC_ASSERT (sizeof (GskGLGlyphKey) == 12);
#endif

typedef struct _GskGLGlyphValue
{
  GskGLTextureAtlas *atlas;
  guint texture_id;

  float tx;
  float ty;
  float tw;
  float th;

  int draw_x;
  int draw_y;
  int draw_width;
  int draw_height;

  guint accessed : 1; /* accessed since last check */
  guint used     : 1; /* accounted as used in the atlas */
} GskGLGlyphValue;

#define GSK_TYPE_GL_GLYPH_LIBRARY (gsk_gl_glyph_library_get_type())

G_DECLARE_FINAL_TYPE (GskGLGlyphLibrary, gsk_gl_glyph_library, GSK, GL_GLYPH_LIBRARY, GskGLTextureLibrary)

struct _GskGLGlyphLibrary
{
  GskGLTextureLibrary parent_instance;
  GHashTable *hash_table;
};

GskGLGlyphLibrary *gsk_gl_glyph_library_new (GdkGLContext           *context);
gboolean           gsk_gl_glyph_library_add (GskGLGlyphLibrary      *self,
                                             const GskGLGlyphKey    *key,
                                             const GskGLGlyphValue **out_value);

static inline int
gsk_gl_glyph_key_phase (float value)
{
  return floor (4 * (value + 0.125)) - 4 * floor (value + 0.125);
}

static inline void
gsk_gl_glyph_key_set_glyph_and_shift (GskGLGlyphKey *key,
                                      PangoGlyph     glyph,
                                      float          x,
                                      float          y)
{
  key->glyph = glyph;
  key->xshift = gsk_gl_glyph_key_phase (x);
  key->yshift = gsk_gl_glyph_key_phase (y);
}

static inline gboolean
gsk_gl_glyph_library_lookup_or_add (GskGLGlyphLibrary      *self,
                                    const GskGLGlyphKey    *key,
                                    const GskGLGlyphValue **out_value)
{
  GskGLGlyphValue *value = g_hash_table_lookup (self->hash_table, key);

  /* Optimize for the fast path (repeated lookups of a character */
  if G_LIKELY (value && value->accessed && value->used)
    {
      *out_value = value;
      return value->texture_id > 0;
    }

  /* We found it, but haven't marked as used for this frame */
  if (value != NULL)
    {
      value->accessed = TRUE;

      if (!value->used)
        {
          gsk_gl_texture_library_mark_used (self,
                                            value->atlas,
                                            value->draw_width,
                                            value->draw_height);
          value->used = TRUE;
        }

      *out_value = value;

      return value->texture_id > 0;
    }

  return gsk_gl_glyph_library_add (self, key, out_value);
}

G_END_DECLS

#endif /* __GSK_GL_GLYPH_LIBRARY_PRIVATE_H__ */
