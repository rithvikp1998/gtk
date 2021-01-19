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

#define GSK_TYPE_GL_GLYPH_LIBRARY (gsk_gl_glyph_library_get_type())

typedef struct _GskGLGlyphKey
{
  PangoFont *font;
  PangoGlyph glyph;
  guint xshift : 3;
  guint yshift : 3;
  guint scale  : 26; /* times 1024 */
} GskGLGlyphKey;

typedef struct _GskGLGlyphValue
{
  GskGLTextureAtlasEntry entry;
  PangoRectangle ink_rect;
} GskGLGlyphValue;

#if GLIB_SIZEOF_VOID_P == 8
G_STATIC_ASSERT (sizeof (GskGLGlyphKey) == 16);
#elif GLIB_SIZEOF_VOID_P == 4
G_STATIC_ASSERT (sizeof (GskGLGlyphKey) == 12);
#endif

G_DECLARE_FINAL_TYPE (GskGLGlyphLibrary, gsk_gl_glyph_library, GSK, GL_GLYPH_LIBRARY, GskGLTextureLibrary)

struct _GskGLGlyphLibrary
{
  GskGLTextureLibrary  parent_instance;
  GHashTable          *hash_table;
  guint8              *surface_data;
  gsize                surface_data_len;
};

GskGLGlyphLibrary *gsk_gl_glyph_library_new (GskNextDriver          *driver);
gboolean           gsk_gl_glyph_library_add (GskGLGlyphLibrary      *self,
                                             GskGLGlyphKey          *key,
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
  GskGLTextureAtlasEntry *entry;
  GskGLGlyphKey *k;

  if G_LIKELY (gsk_gl_texture_library_lookup ((GskGLTextureLibrary *)self, key, &entry))
    {
      *out_value = (GskGLGlyphValue *)entry;
      return TRUE;
    }

  k = g_slice_copy (sizeof *key, key);
  g_object_ref (k->font);

  return gsk_gl_glyph_library_add (self, k, out_value);
}

G_END_DECLS

#endif /* __GSK_GL_GLYPH_LIBRARY_PRIVATE_H__ */
