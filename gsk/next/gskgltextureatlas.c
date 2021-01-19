#include "config.h"

#include <epoxy/gl.h>

#include "gskgltextureatlasprivate.h"
#include "gskdebugprivate.h"
#include "gdkglcontextprivate.h"

#define ATLAS_SIZE    512
#define MAX_OLD_RATIO 0.5

void
gsk_gl_texture_atlas_free (GskGLTextureAtlas *atlas)
{
  if (atlas->texture_id != 0)
    {
      glDeleteTextures (1, &atlas->texture_id);
      atlas->texture_id = 0;
    }

  g_clear_pointer (&atlas->nodes, g_free);
  g_slice_free (GskGLTextureAtlas, atlas);
}

GskGLTextureAtlases *
gsk_gl_texture_atlases_new (void)
{
  return g_ptr_array_new_with_free_func ((GDestroyNotify)gsk_gl_texture_atlas_free);
}

#if 0
static void
write_atlas_to_png (GskGLTextureAtlas *atlas,
                    const char        *filename)
{
  int stride = cairo_format_stride_for_width (CAIRO_FORMAT_ARGB32, atlas->width);
  guchar *data = g_malloc (atlas->height * stride);
  cairo_surface_t *s;

  glBindTexture (GL_TEXTURE_2D, atlas->texture_id);
  glGetTexImage (GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, data);
  s = cairo_image_surface_create_for_data (data, CAIRO_FORMAT_ARGB32, atlas->width, atlas->height, stride);
  cairo_surface_write_to_png (s, filename);

  cairo_surface_destroy (s);
  g_free (data);
}
#endif

void
gsk_gl_texture_atlases_begin_frame (GskGLTextureAtlases *self,
                                    GPtrArray           *removed)
{
  g_return_if_fail (self != NULL);

  for (int i = self->len - 1; i >= 0; i--)
    {
      GskGLTextureAtlas *atlas = g_ptr_array_index (self, i);

      if (gsk_gl_texture_atlas_get_unused_ratio (atlas) > MAX_OLD_RATIO)
        {
          if (atlas->texture_id != 0)
            {
              glDeleteTextures (1, &atlas->texture_id);
              atlas->texture_id = 0;
            }

          g_ptr_array_add (removed, atlas);
          g_ptr_array_remove_index (self, i);
       }
    }

#if 0
  {
    static guint timestamp;

    timestamp++;
    if (timestamp % 10 == 0)
      for (i = 0; i < self->len; i++)
        {
          GskGLTextureAtlas *atlas = g_ptr_array_index (self, i);

          if (atlas->texture_id)
            {
              char *filename;

              filename = g_strdup_printf ("textureatlas%d-%u.png", i, timestamp);
              write_atlas_to_png (atlas, filename);
              g_free (filename);
            }
         }
   }
#endif
}

gboolean
gsk_gl_texture_atlases_pack (GskGLTextureAtlases *self,
                             int                  width,
                             int                  height,
                             GskGLTextureAtlas  **atlas_out,
                             int                 *out_x,
                             int                 *out_y)
{
  GskGLTextureAtlas *atlas;
  int x, y;

  g_assert (width  < ATLAS_SIZE);
  g_assert (height < ATLAS_SIZE);

  atlas = NULL;

  for (guint i = 0; i < self->len; i++)
    {
      atlas = g_ptr_array_index (self, i);

      if (gsk_gl_texture_atlas_pack (atlas, width, height, &x, &y))
        break;

      atlas = NULL;
    }

  if (atlas == NULL)
    {
      /* No atlas has enough space, so create a new one... */
      atlas = g_malloc (sizeof (GskGLTextureAtlas));
      gsk_gl_texture_atlas_init (atlas, ATLAS_SIZE, ATLAS_SIZE);
      gsk_gl_texture_atlas_realize (atlas);
      g_ptr_array_add (self, atlas);

      /* Pack it onto that one, which surely has enough space... */
      if (!gsk_gl_texture_atlas_pack (atlas, width, height, &x, &y))
        g_assert_not_reached ();
    }

  *atlas_out = atlas;
  *out_x = x;
  *out_y = y;

  return TRUE;
}

void
gsk_gl_texture_atlas_init (GskGLTextureAtlas *self,
                           int                width,
                           int                height)
{
  memset (self, 0, sizeof (*self));

  self->texture_id = 0;
  self->width = width;
  self->height = height;

  /* TODO: We might want to change the strategy about the amount of
   *       nodes here? stb_rect_pack.h says with is optimal. */
  self->nodes = g_malloc0 (sizeof (struct stbrp_node) * width);
  stbrp_init_target (&self->context,
                     width, height,
                     self->nodes,
                     width);

  gsk_gl_texture_atlas_realize (self);
}

gboolean
gsk_gl_texture_atlas_pack (GskGLTextureAtlas *self,
                           int                width,
                           int                height,
                           int               *out_x,
                           int               *out_y)
{
  stbrp_rect rect;

  g_assert (out_x);
  g_assert (out_y);

  rect.w = width;
  rect.h = height;

  stbrp_pack_rects (&self->context, &rect, 1);

  if (rect.was_packed)
    {
      *out_x = rect.x;
      *out_y = rect.y;
    }

  return rect.was_packed;
}

double
gsk_gl_texture_atlas_get_unused_ratio (const GskGLTextureAtlas *self)
{
  if (self->unused_pixels > 0)
    return (double)(self->unused_pixels) / (double)(self->width * self->height);

  return 0.0;
}

/* Not using gdk_gl_driver_create_texture here, since we want
 * this texture to survive the driver and stay around until
 * the display gets closed.
 */
static guint
create_shared_texture (int width,
                       int height)
{
  guint texture_id;

  glGenTextures (1, &texture_id);
  glBindTexture (GL_TEXTURE_2D, texture_id);

  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  if (gdk_gl_context_get_use_es (gdk_gl_context_get_current ()))
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  else
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);

  glBindTexture (GL_TEXTURE_2D, 0);

  return texture_id;
}

void
gsk_gl_texture_atlas_realize (GskGLTextureAtlas *atlas)
{
  if (atlas->texture_id)
    return;

  atlas->texture_id = create_shared_texture (atlas->width, atlas->height);
  gdk_gl_context_label_object_printf (gdk_gl_context_get_current (),
                                      GL_TEXTURE, atlas->texture_id,
                                      "Texture atlas %d", atlas->texture_id);
}
