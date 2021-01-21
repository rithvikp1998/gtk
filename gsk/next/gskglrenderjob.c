/* gskglrenderjob.c
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
#include <gdk/gdkrgbaprivate.h>
#include <gsk/gskrendernodeprivate.h>
#include <gsk/gskglshaderprivate.h>
#include <gdk/gdktextureprivate.h>
#include <gsk/gsktransformprivate.h>
#include <math.h>
#include <string.h>

#include "gskglcommandqueueprivate.h"
#include "gskgldriverprivate.h"
#include "gskglglyphlibraryprivate.h"
#include "gskgliconlibraryprivate.h"
#include "gskglprogramprivate.h"
#include "gskglrenderjobprivate.h"
#include "gskglshadowlibraryprivate.h"

#define ORTHO_NEAR_PLANE   -10000
#define ORTHO_FAR_PLANE     10000
#define MAX_GRADIENT_STOPS  6
#define SHADOW_EXTRA_SIZE   4
#define X1(r) ((r)->origin.x)
#define X2(r) ((r)->origin.x + (r)->size.width)
#define Y1(r) ((r)->origin.y)
#define Y2(r) ((r)->origin.y + (r)->size.height)

#define rounded_rect_top_left(r)                                                        \
  (GRAPHENE_RECT_INIT(r->bounds.origin.x,                                               \
                      r->bounds.origin.y,                                               \
                      r->corner[0].width, r->corner[0].height))
#define rounded_rect_top_right(r) \
  (GRAPHENE_RECT_INIT(r->bounds.origin.x + r->bounds.size.width - r->corner[1].width,   \
                      r->bounds.origin.y, \
                      r->corner[1].width, r->corner[1].height))
#define rounded_rect_bottom_right(r) \
  (GRAPHENE_RECT_INIT(r->bounds.origin.x + r->bounds.size.width - r->corner[2].width,   \
                      r->bounds.origin.y + r->bounds.size.height - r->corner[2].height, \
                      r->corner[2].width, r->corner[2].height))
#define rounded_rect_bottom_left(r)                                                     \
  (GRAPHENE_RECT_INIT(r->bounds.origin.x,                                               \
                      r->bounds.origin.y + r->bounds.size.height - r->corner[2].height, \
                      r->corner[3].width, r->corner[3].height))
#define rounded_rect_corner0(r)   rounded_rect_top_left(r)
#define rounded_rect_corner1(r)   rounded_rect_top_right(r)
#define rounded_rect_corner2(r)   rounded_rect_bottom_right(r)
#define rounded_rect_corner3(r)   rounded_rect_bottom_left(r)
#define rounded_rect_corner(r, i) (rounded_rect_corner##i(r))

struct _GskGLRenderJob
{
  GskNextDriver     *driver;
  GskGLCommandQueue *command_queue;
  cairo_region_t    *region;
  guint              framebuffer;
  graphene_rect_t    viewport;
  graphene_matrix_t  projection;
  GArray            *modelview;
  GArray            *clip;
  float              alpha;
  float              offset_x;
  float              offset_y;
  float              scale_x;
  float              scale_y;
  guint              debug_fallback : 1;
};

typedef struct GskGLRenderState
{
  graphene_rect_t   viewport;
  graphene_matrix_t projection;
  guint             framebuffer;
  float             alpha;
  float             offset_x;
  float             offset_y;
  float             scale_x;
  float             scale_y;
} GskGLRenderState;

typedef struct _GskGLRenderClip
{
  GskRoundedRect rect;
  bool           is_rectilinear;
} GskGLRenderClip;

typedef struct _GskGLRenderModelview
{
  GskTransform *transform;
  float scale_x;
  float scale_y;
  float offset_x_before;
  float offset_y_before;
  graphene_matrix_t matrix;
} GskGLRenderModelview;

typedef struct _GskGLRenderOffscreen
{
  const graphene_rect_t *bounds;
  graphene_rect_t area;
  guint texture_id;
  guint force_offscreen : 1;
  guint reset_clip : 1;
  guint do_not_cache : 1;
  guint linear_filter : 1;
  guint was_offscreen : 1;
} GskGLRenderOffscreen;

static void     gsk_gl_render_job_visit_node                (GskGLRenderJob       *job,
                                                             GskRenderNode        *node);
static gboolean gsk_gl_render_job_visit_node_with_offscreen (GskGLRenderJob       *job,
                                                             GskRenderNode        *node,
                                                             GskGLRenderOffscreen *offscreen);

static inline void
init_full_texture_region (graphene_rect_t *rect)
{
  rect->origin.x = 0;
  rect->origin.y = 0;
  rect->size.width = 1;
  rect->size.height = 1;
}

static inline gboolean G_GNUC_PURE
node_is_invisible (const GskRenderNode *node)
{
  return node->bounds.size.width == 0.0f ||
         node->bounds.size.height == 0.0f ||
         isnan (node->bounds.size.width) ||
         isnan (node->bounds.size.height);
}

static inline void
gsk_rounded_rect_shrink_to_minimum (GskRoundedRect *self)
{
  self->bounds.size.width  = MAX (self->corner[0].width + self->corner[1].width,
                                  self->corner[3].width + self->corner[2].width);
  self->bounds.size.height = MAX (self->corner[0].height + self->corner[3].height,
                                  self->corner[1].height + self->corner[2].height);
}

static inline gboolean G_GNUC_PURE
node_supports_transform (GskRenderNode *node)
{
  /* Some nodes can't handle non-trivial transforms without being
   * rendered to a texture (e.g. rotated clips, etc.). Some however work
   * just fine, mostly because they already draw their child to a
   * texture and just render the texture manipulated in some way, think
   * opacity or color matrix.
   */

  switch ((int)gsk_render_node_get_node_type (node))
    {
      case GSK_COLOR_NODE:
      case GSK_OPACITY_NODE:
      case GSK_COLOR_MATRIX_NODE:
      case GSK_TEXTURE_NODE:
      case GSK_CROSS_FADE_NODE:
      case GSK_LINEAR_GRADIENT_NODE:
      case GSK_DEBUG_NODE:
      case GSK_TEXT_NODE:
        return TRUE;

      case GSK_TRANSFORM_NODE:
        return node_supports_transform (gsk_transform_node_get_child (node));

      default:
        return FALSE;
    }
}

static inline gboolean G_GNUC_PURE
color_matrix_modifies_alpha (GskRenderNode *node)
{
  const graphene_matrix_t *matrix = gsk_color_matrix_node_get_color_matrix (node);
  const graphene_vec4_t *offset = gsk_color_matrix_node_get_color_offset (node);
  graphene_vec4_t row3;

  if (graphene_vec4_get_w (offset) != 0.0f)
    return TRUE;

  graphene_matrix_get_row (matrix, 3, &row3);

  return !graphene_vec4_equal (graphene_vec4_w_axis (), &row3);
}

static inline gboolean
rounded_inner_rect_contains_rect (const GskRoundedRect  *rounded,
                                  const graphene_rect_t *rect)
{
  const graphene_rect_t *rounded_bounds = &rounded->bounds;
  graphene_rect_t inner;
  float offset_x;
  float offset_y;

  /* TODO: This is pretty conservative and we could go further,
   *       more fine-grained checks to avoid offscreen drawing.
   */

  offset_x = MAX (rounded->corner[GSK_CORNER_TOP_LEFT].width,
                  rounded->corner[GSK_CORNER_BOTTOM_LEFT].width);
  offset_y = MAX (rounded->corner[GSK_CORNER_TOP_LEFT].height,
                  rounded->corner[GSK_CORNER_TOP_RIGHT].height);

  inner.origin.x = rounded_bounds->origin.x + offset_x;
  inner.origin.y = rounded_bounds->origin.y + offset_y;
  inner.size.width = rounded_bounds->size.width - offset_x -
                     MAX (rounded->corner[GSK_CORNER_TOP_RIGHT].width,
                          rounded->corner[GSK_CORNER_BOTTOM_RIGHT].width);
  inner.size.height = rounded_bounds->size.height - offset_y -
                      MAX (rounded->corner[GSK_CORNER_BOTTOM_LEFT].height,
                           rounded->corner[GSK_CORNER_BOTTOM_RIGHT].height);

  return graphene_rect_contains_rect (&inner, rect);
}

static inline gboolean G_GNUC_PURE
rect_intersects (const graphene_rect_t *r1,
                 const graphene_rect_t *r2)
{
  /* Assume both rects are already normalized, as they usually are */
  if (r1->origin.x > (r2->origin.x + r2->size.width) ||
      (r1->origin.x + r1->size.width) < r2->origin.x)
    return FALSE;
  else if (r1->origin.y > (r2->origin.y + r2->size.height) ||
      (r1->origin.y + r1->size.height) < r2->origin.y)
    return FALSE;
  else
    return TRUE;
}

static inline gboolean G_GNUC_PURE
rect_contains_rect (const graphene_rect_t *r1,
                    const graphene_rect_t *r2)
{
  if (r2->origin.x >= r1->origin.x &&
      (r2->origin.x + r2->size.width) <= (r1->origin.x + r1->size.width) &&
      r2->origin.y >= r1->origin.y &&
      (r2->origin.y + r2->size.height) <= (r1->origin.y + r1->size.height))
    return TRUE;
  else
    return FALSE;
}

static inline gboolean
rounded_rect_has_corner (const GskRoundedRect *r,
                         guint                 i)
{
  return r->corner[i].width > 0 && r->corner[i].height > 0;
}

/* Current clip is NOT rounded but new one is definitely! */
static inline gboolean
intersect_rounded_rectilinear (const graphene_rect_t *non_rounded,
                               const GskRoundedRect  *rounded,
                               GskRoundedRect        *result)
{
  bool corners[4];

  /* Intersects with top left corner? */
  corners[0] = rounded_rect_has_corner (rounded, 0) &&
               rect_intersects (non_rounded,
                                &rounded_rect_corner (rounded, 0));
  /* top right? */
  corners[1] = rounded_rect_has_corner (rounded, 1) &&
               rect_intersects (non_rounded,
                                &rounded_rect_corner (rounded, 1));
  /* bottom right? */
  corners[2] = rounded_rect_has_corner (rounded, 2) &&
               rect_intersects (non_rounded,
                                &rounded_rect_corner (rounded, 2));
  /* bottom left */
  corners[3] = rounded_rect_has_corner (rounded, 3) &&
               rect_intersects (non_rounded,
                                &rounded_rect_corner (rounded, 3));

  if (corners[0] && !rect_contains_rect (non_rounded, &rounded_rect_corner (rounded, 0)))
    return FALSE;
  if (corners[1] && !rect_contains_rect (non_rounded, &rounded_rect_corner (rounded, 1)))
    return FALSE;
  if (corners[2] && !rect_contains_rect (non_rounded, &rounded_rect_corner (rounded, 2)))
    return FALSE;
  if (corners[3] && !rect_contains_rect (non_rounded, &rounded_rect_corner (rounded, 3)))
    return FALSE;

  /* We do intersect with at least one of the corners, but in such a way that the
   * intersection between the two clips can still be represented by a single rounded
   * rect in a trivial way. do that. */
  graphene_rect_intersection (non_rounded, &rounded->bounds, &result->bounds);

  for (int i = 0; i < 4; i++)
    {
      if (corners[i])
        result->corner[i] = rounded->corner[i];
      else
        result->corner[i].width = result->corner[i].height = 0;
    }

  return TRUE;
}

static void
init_projection_matrix (graphene_matrix_t     *projection,
                        const graphene_rect_t *viewport)
{
  graphene_matrix_init_ortho (projection,
                              viewport->origin.x,
                              viewport->origin.x + viewport->size.width,
                              viewport->origin.y,
                              viewport->origin.y + viewport->size.height,
                              ORTHO_NEAR_PLANE,
                              ORTHO_FAR_PLANE);
  graphene_matrix_scale (projection, 1, -1, 1);
}

static inline graphene_matrix_t *
gsk_gl_render_job_get_modelview_matrix (GskGLRenderJob *job)
{
  g_assert (job->modelview->len > 0);

  return &g_array_index (job->modelview,
                         GskGLRenderModelview,
                         job->modelview->len - 1).matrix;
}

static inline GskGLRenderModelview *
gsk_gl_render_job_get_modelview (GskGLRenderJob *job)
{
  g_assert (job->modelview->len > 0);

  return &g_array_index (job->modelview,
                         GskGLRenderModelview,
                         job->modelview->len - 1);
}

static void
extract_matrix_metadata (GskGLRenderModelview *modelview)
{
  float dummy;
  graphene_matrix_t m;

  gsk_transform_to_matrix (modelview->transform, &modelview->matrix);

  switch (gsk_transform_get_category (modelview->transform))
    {
    case GSK_TRANSFORM_CATEGORY_IDENTITY:
    case GSK_TRANSFORM_CATEGORY_2D_TRANSLATE:
      modelview->scale_x = 1;
      modelview->scale_y = 1;
      break;

    case GSK_TRANSFORM_CATEGORY_2D_AFFINE:
      gsk_transform_to_affine (modelview->transform,
                               &modelview->scale_x, &modelview->scale_y,
                               &dummy, &dummy);
      break;

    case GSK_TRANSFORM_CATEGORY_UNKNOWN:
    case GSK_TRANSFORM_CATEGORY_ANY:
    case GSK_TRANSFORM_CATEGORY_3D:
    case GSK_TRANSFORM_CATEGORY_2D:
      {
        graphene_vec3_t col1;
        graphene_vec3_t col2;

        /* TODO: 90% sure this is incorrect. But we should never hit this code
         * path anyway. */
        graphene_vec3_init (&col1,
                            graphene_matrix_get_value (&m, 0, 0),
                            graphene_matrix_get_value (&m, 1, 0),
                            graphene_matrix_get_value (&m, 2, 0));

        graphene_vec3_init (&col2,
                            graphene_matrix_get_value (&m, 0, 1),
                            graphene_matrix_get_value (&m, 1, 1),
                            graphene_matrix_get_value (&m, 2, 1));

        modelview->scale_x = graphene_vec3_length (&col1);
        modelview->scale_y = graphene_vec3_length (&col2);
      }
      break;

    default:
      break;
    }
}

static void
gsk_gl_render_job_set_modelview (GskGLRenderJob *job,
                                 GskTransform   *transform)
{
  GskGLRenderModelview *modelview;

  g_assert (job != NULL);
  g_assert (job->modelview != NULL);

  g_array_set_size (job->modelview, job->modelview->len + 1);

  modelview = &g_array_index (job->modelview,
                              GskGLRenderModelview,
                              job->modelview->len - 1);

  modelview->transform = transform;

  modelview->offset_x_before = job->offset_x;
  modelview->offset_y_before = job->offset_y;

  extract_matrix_metadata (modelview);

  job->offset_x = 0;
  job->offset_y = 0;
  job->scale_x = modelview->scale_x;
  job->scale_y = modelview->scale_y;
}

static void
gsk_gl_render_job_push_modelview (GskGLRenderJob *job,
                                  GskTransform   *transform)
{
  GskGLRenderModelview *modelview;

  g_assert (job != NULL);
  g_assert (job->modelview != NULL);
  g_assert (transform != NULL);

  g_array_set_size (job->modelview, job->modelview->len + 1);

  modelview = &g_array_index (job->modelview,
                              GskGLRenderModelview,
                              job->modelview->len - 1);

  if G_LIKELY (job->modelview->len > 1)
    {
      GskGLRenderModelview *last;
      GskTransform *t = NULL;

      last = &g_array_index (job->modelview,
                             GskGLRenderModelview,
                             job->modelview->len - 2);

      /* Multiply given matrix with our previews modelview */
      t = gsk_transform_translate (gsk_transform_ref (last->transform),
                                   &(graphene_point_t) {
                                     job->offset_x,
                                     job->offset_y
                                   });
      t = gsk_transform_transform (t, transform);
      modelview->transform = t;
    }
  else
    {
      modelview->transform = gsk_transform_ref (transform);
    }

  modelview->offset_x_before = job->offset_x;
  modelview->offset_y_before = job->offset_y;

  extract_matrix_metadata (modelview);

  job->offset_x = 0;
  job->offset_y = 0;
  job->scale_x = modelview->scale_x;
  job->scale_y = modelview->scale_y;
}

static void
gsk_gl_render_job_pop_modelview (GskGLRenderJob *job)
{
  const GskGLRenderModelview *head;

  g_assert (job != NULL);
  g_assert (job->modelview);
  g_assert (job->modelview->len > 0);

  head = gsk_gl_render_job_get_modelview (job);

  job->offset_x = head->offset_x_before;
  job->offset_y = head->offset_y_before;

  gsk_transform_unref (head->transform);

  job->modelview->len--;

  if (job->modelview->len >= 1)
    {
      head = &g_array_index (job->modelview, GskGLRenderModelview, job->modelview->len - 1);

      job->scale_x = head->scale_x;
      job->scale_y = head->scale_y;
    }
}

static inline gboolean
gsk_gl_render_job_clip_is_rectilinear (GskGLRenderJob *job)
{
  if (job->clip->len == 0)
    return TRUE;
  else
    return g_array_index (job->clip, GskGLRenderClip, job->clip->len - 1).is_rectilinear;
}

static inline const GskRoundedRect *
gsk_gl_render_job_get_clip (GskGLRenderJob *job)
{
  g_assert (job->clip->len > 0);

  return &g_array_index (job->clip, GskGLRenderClip, job->clip->len - 1).rect;
}

static void
gsk_gl_render_job_push_clip (GskGLRenderJob       *job,
                             const GskRoundedRect *rect)
{
  GskGLRenderClip clip;

  g_assert (job != NULL);
  g_assert (job->clip != NULL);
  g_assert (rect != NULL);

  clip.rect = *rect;
  clip.is_rectilinear = gsk_rounded_rect_is_rectilinear (rect);

  g_array_append_val (job->clip, clip);
}

static void
gsk_gl_render_job_pop_clip (GskGLRenderJob *job)
{
  g_assert (job != NULL);
  g_assert (job->clip != NULL);
  g_assert (job->clip->len > 0);

  job->clip->len--;
}

static inline void
gsk_gl_render_job_offset (GskGLRenderJob *job,
                          float           offset_x,
                          float           offset_y)
{
  job->offset_x += offset_x;
  job->offset_y += offset_y;
}

static void
gsk_gl_render_state_save (GskGLRenderState *state,
                          GskGLRenderJob   *job)
{
  g_assert (state != NULL);
  g_assert (job != NULL);

  memcpy (&state->viewport, &job->viewport, sizeof state->viewport);
  memcpy (&state->projection, &job->projection, sizeof state->projection);

  state->framebuffer = job->command_queue->attachments->fbo.id;
  state->alpha = job->alpha;
  state->offset_x = job->offset_x;
  state->offset_y = job->offset_y;
  state->scale_x = job->scale_x;
  state->scale_y = job->scale_y;
}

static void
gsk_gl_render_state_restore (GskGLRenderState *state,
                             GskGLRenderJob   *job)
{
  g_assert (state != NULL);
  g_assert (job != NULL);

  memcpy (&job->viewport, &state->viewport, sizeof state->viewport);
  memcpy (&job->projection, &state->projection, sizeof state->projection);

  gsk_gl_command_queue_bind_framebuffer (job->command_queue, state->framebuffer);

  job->alpha = state->alpha;
  job->offset_x = state->offset_x;
  job->offset_y = state->offset_y;
  job->scale_x = state->scale_x;
  job->scale_y = state->scale_y;
}

static void
gsk_gl_render_job_transform_bounds (GskGLRenderJob        *job,
                                    const graphene_rect_t *rect,
                                    graphene_rect_t       *out_rect)
{
  GskGLRenderModelview *modelview;
  graphene_rect_t r;

  g_assert (job != NULL);
  g_assert (job->modelview->len > 0);
  g_assert (rect != NULL);
  g_assert (out_rect != NULL);

  r.origin.x = rect->origin.x + job->offset_x;
  r.origin.y = rect->origin.y + job->offset_y;
  r.size.width = rect->size.width;
  r.size.height = rect->size.height;

  modelview = gsk_gl_render_job_get_modelview (job);

  gsk_transform_transform_bounds (modelview->transform, &r, out_rect);
}

static inline void
gsk_gl_render_job_transform_rounded_rect (GskGLRenderJob       *job,
                                          const GskRoundedRect *rect,
                                          GskRoundedRect       *out_rect)
{
  out_rect->bounds.origin.x = job->offset_x + rect->bounds.origin.x;
  out_rect->bounds.origin.y = job->offset_y + rect->bounds.origin.y;
  out_rect->bounds.size.width = rect->bounds.size.width;
  out_rect->bounds.size.height = rect->bounds.size.height;
  out_rect->corner[0] = rect->corner[0];
  out_rect->corner[1] = rect->corner[1];
  out_rect->corner[2] = rect->corner[2];
  out_rect->corner[3] = rect->corner[3];
}

static inline gboolean
gsk_gl_render_job_node_overlaps_clip (GskGLRenderJob *job,
                                      GskRenderNode  *node)
{
  const GskRoundedRect *clip = gsk_gl_render_job_get_clip (job);

  if (clip != NULL)
    {
      graphene_rect_t transformed_bounds;
      gsk_gl_render_job_transform_bounds (job, &node->bounds, &transformed_bounds);
      return rect_intersects (&clip->bounds, &transformed_bounds);
    }

  return TRUE;
}

/* load_vertex_data_with_region */
static inline void
gsk_gl_render_job_load_vertices_from_offscreen (GskGLRenderJob             *job,
                                                const graphene_rect_t      *bounds,
                                                const GskGLRenderOffscreen *offscreen)
{
  GskGLDrawVertex *vertices = gsk_gl_command_queue_add_vertices (job->command_queue, NULL);
  float min_x = job->offset_x + bounds->origin.x;
  float min_y = job->offset_y + bounds->origin.y;
  float max_x = min_x + bounds->size.width;
  float max_y = min_y + bounds->size.height;
  float y1 = offscreen->was_offscreen ? Y2 (&offscreen->area) : Y1 (&offscreen->area);
  float y2 = offscreen->was_offscreen ? Y1 (&offscreen->area) : Y2 (&offscreen->area);

  vertices[0].position[0] = min_x;
  vertices[0].position[1] = min_y;
  vertices[0].uv[0] = X1 (&offscreen->area);
  vertices[0].uv[1] = y1;

  vertices[1].position[0] = min_x;
  vertices[1].position[1] = max_y;
  vertices[1].uv[0] = X1 (&offscreen->area);
  vertices[1].uv[1] = y2;

  vertices[2].position[0] = max_x;
  vertices[2].position[1] = min_y;
  vertices[2].uv[0] = X2 (&offscreen->area);
  vertices[2].uv[1] = y1;

  vertices[3].position[0] = max_x;
  vertices[3].position[1] = max_y;
  vertices[3].uv[0] = X2 (&offscreen->area);
  vertices[3].uv[1] = y2;

  vertices[4].position[0] = min_x;
  vertices[4].position[1] = max_y;
  vertices[4].uv[0] = X1 (&offscreen->area);
  vertices[4].uv[1] = y2;

  vertices[5].position[0] = max_x;
  vertices[5].position[1] = min_y;
  vertices[5].uv[0] = X2 (&offscreen->area);
  vertices[5].uv[1] = y1;
}

/* load_float_vertex_data */
static inline void
gsk_gl_render_job_draw (GskGLRenderJob *job,
                        float           x,
                        float           y,
                        float           width,
                        float           height)
{
  GskGLDrawVertex *vertices = gsk_gl_command_queue_add_vertices (job->command_queue, NULL);
  float min_x = job->offset_x + x;
  float min_y = job->offset_y + y;
  float max_x = min_x + width;
  float max_y = min_y + height;

  vertices[0].position[0] = min_x;
  vertices[0].position[1] = min_y;
  vertices[0].uv[0] = 0;
  vertices[0].uv[1] = 0;

  vertices[1].position[0] = min_x;
  vertices[1].position[1] = max_y;
  vertices[1].uv[0] = 0;
  vertices[1].uv[1] = 1;

  vertices[2].position[0] = max_x;
  vertices[2].position[1] = min_y;
  vertices[2].uv[0] = 1;
  vertices[2].uv[1] = 0;

  vertices[3].position[0] = max_x;
  vertices[3].position[1] = max_y;
  vertices[3].uv[0] = 1;
  vertices[3].uv[1] = 1;

  vertices[4].position[0] = min_x;
  vertices[4].position[1] = max_y;
  vertices[4].uv[0] = 0;
  vertices[4].uv[1] = 1;

  vertices[5].position[0] = max_x;
  vertices[5].position[1] = min_y;
  vertices[5].uv[0] = 1;
  vertices[5].uv[1] = 0;
}

/* load_vertex_data */
static inline void
gsk_gl_render_job_draw_rect (GskGLRenderJob        *job,
                             const graphene_rect_t *bounds)
{
  gsk_gl_render_job_draw (job,
                          bounds->origin.x,
                          bounds->origin.y,
                          bounds->size.width,
                          bounds->size.height);
}

/* fill_vertex_data */
static void
gsk_gl_render_job_draw_coords (GskGLRenderJob *job,
                               float           min_x,
                               float           min_y,
                               float           max_x,
                               float           max_y)
{
  GskGLDrawVertex *vertices = gsk_gl_command_queue_add_vertices (job->command_queue, NULL);

  vertices[0].position[0] = min_x;
  vertices[0].position[1] = min_y;
  vertices[0].uv[0] = 0;
  vertices[0].uv[1] = 1;

  vertices[1].position[0] = min_x;
  vertices[1].position[1] = max_y;
  vertices[1].uv[0] = 0;
  vertices[1].uv[1] = 0;

  vertices[2].position[0] = max_x;
  vertices[2].position[1] = min_y;
  vertices[2].uv[0] = 1;
  vertices[2].uv[1] = 1;

  vertices[3].position[0] = max_x;
  vertices[3].position[1] = max_y;
  vertices[3].uv[0] = 1;
  vertices[3].uv[1] = 0;

  vertices[4].position[0] = min_x;
  vertices[4].position[1] = max_y;
  vertices[4].uv[0] = 0;
  vertices[4].uv[1] = 0;

  vertices[5].position[0] = max_x;
  vertices[5].position[1] = min_y;
  vertices[5].uv[0] = 1;
  vertices[5].uv[1] = 1;
}

/* load_offscreen_vertex_data */
static inline void
gsk_gl_render_job_draw_offscreen_rect (GskGLRenderJob        *job,
                                       const graphene_rect_t *bounds)
{
  float min_x = job->offset_x + bounds->origin.x;
  float min_y = job->offset_y + bounds->origin.y;
  float max_x = min_x + bounds->size.width;
  float max_y = min_y + bounds->size.height;

  gsk_gl_render_job_draw_coords (job, min_x, min_y, max_x, max_y);
}

static void
gsk_gl_render_job_visit_as_fallback (GskGLRenderJob *job,
                                     GskRenderNode  *node)
{
  float scale_x = job->scale_x;
  float scale_y = job->scale_y;
  int surface_width = ceilf (node->bounds.size.width * scale_x);
  int surface_height = ceilf (node->bounds.size.height * scale_y);
  GdkTexture *texture;
  cairo_surface_t *surface;
  cairo_surface_t *rendered_surface;
  cairo_t *cr;
  int cached_id;
  int texture_id;
  GskTextureKey key;

  if (surface_width <= 0 || surface_height <= 0)
    return;

  key.pointer = node;
  key.pointer_is_child = FALSE;
  key.scale_x = scale_x;
  key.scale_y = scale_y;
  key.filter = GL_NEAREST;

  cached_id = gsk_next_driver_lookup_texture (job->driver, &key);

  if (cached_id != 0)
    {
      gsk_gl_program_begin_draw (job->driver->blit,
                                 &job->viewport,
                                 &job->projection,
                                 gsk_gl_render_job_get_modelview_matrix (job),
                                 gsk_gl_render_job_get_clip (job),
                                 job->alpha);
      gsk_gl_program_set_uniform_texture (job->driver->blit,
                                          UNIFORM_SHARED_SOURCE,
                                          GL_TEXTURE_2D, GL_TEXTURE0, cached_id);
      gsk_gl_render_job_draw_offscreen_rect (job, &node->bounds);
      gsk_gl_program_end_draw (job->driver->blit);
      return;
    }

  /* We first draw the recording surface on an image surface,
   * just because the scaleY(-1) later otherwise screws up the
   * rendering... */
  {
    rendered_surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                                   surface_width,
                                                   surface_height);

    cairo_surface_set_device_scale (rendered_surface, scale_x, scale_y);
    cr = cairo_create (rendered_surface);

    cairo_save (cr);
    cairo_translate (cr, - floorf (node->bounds.origin.x), - floorf (node->bounds.origin.y));
    gsk_render_node_draw (node, cr);
    cairo_restore (cr);
    cairo_destroy (cr);
  }

  surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                        surface_width,
                                        surface_height);
  cairo_surface_set_device_scale (surface, scale_x, scale_y);
  cr = cairo_create (surface);

  /* We draw upside down here, so it matches what GL does. */
  cairo_save (cr);
  cairo_scale (cr, 1, -1);
  cairo_translate (cr, 0, - surface_height / scale_y);
  cairo_set_source_surface (cr, rendered_surface, 0, 0);
  cairo_rectangle (cr, 0, 0, surface_width / scale_x, surface_height / scale_y);
  cairo_fill (cr);
  cairo_restore (cr);

#ifdef G_ENABLE_DEBUG
  if (job->debug_fallback)
    {
      cairo_move_to (cr, 0, 0);
      cairo_rectangle (cr, 0, 0, node->bounds.size.width, node->bounds.size.height);
      if (gsk_render_node_get_node_type (node) == GSK_CAIRO_NODE)
        cairo_set_source_rgba (cr, 0.3, 0, 1, 0.25);
      else
        cairo_set_source_rgba (cr, 1, 0, 0, 0.25);
      cairo_fill_preserve (cr);
      if (gsk_render_node_get_node_type (node) == GSK_CAIRO_NODE)
        cairo_set_source_rgba (cr, 0.3, 0, 1, 1);
      else
        cairo_set_source_rgba (cr, 1, 0, 0, 1);
      cairo_stroke (cr);
    }
#endif
  cairo_destroy (cr);

  /* Create texture to upload */
  texture = gdk_texture_new_for_surface (surface);
  texture_id = gsk_next_driver_load_texture (job->driver, texture,
                                             GL_NEAREST, GL_NEAREST);

  if (gdk_gl_context_has_debug (job->command_queue->context))
    gdk_gl_context_label_object_printf (job->command_queue->context, GL_TEXTURE, texture_id,
                                        "Fallback %s %d",
                                        g_type_name_from_instance ((GTypeInstance *) node),
                                        texture_id);

  g_object_unref (texture);
  cairo_surface_destroy (surface);
  cairo_surface_destroy (rendered_surface);

  gsk_next_driver_cache_texture (job->driver, &key, texture_id);

  gsk_gl_program_begin_draw (job->driver->blit,
                             &job->viewport,
                             &job->projection,
                             gsk_gl_render_job_get_modelview_matrix (job),
                             gsk_gl_render_job_get_clip (job),
                             job->alpha);
  gsk_gl_render_job_draw_offscreen_rect (job, &node->bounds);
  gsk_gl_program_end_draw (job->driver->blit);
}

static guint
blur_offscreen (GskGLRenderJob       *job,
                GskGLRenderOffscreen *offscreen,
                int                   texture_to_blur_width,
                int                   texture_to_blur_height,
                float                 blur_radius_x,
                float                 blur_radius_y)
{
  const GskRoundedRect new_clip = GSK_ROUNDED_RECT_INIT (0, 0, texture_to_blur_width, texture_to_blur_height);
  GskGLRenderTarget *pass1;
  GskGLRenderTarget *pass2;
  GskGLRenderState state;

  g_assert (blur_radius_x > 0);
  g_assert (blur_radius_y > 0);
  g_assert (offscreen->texture_id > 0);
  g_assert (offscreen->area.size.width > 0);
  g_assert (offscreen->area.size.height > 0);

  if (!gsk_next_driver_create_render_target (job->driver,
                                             MAX (texture_to_blur_width, 1),
                                             MAX (texture_to_blur_height, 1),
                                             GL_NEAREST, GL_NEAREST,
                                             &pass1))
    return 0;

  if (texture_to_blur_width <= 0 || texture_to_blur_height <= 0)
    return gsk_next_driver_release_render_target (job->driver, pass1, FALSE);

  if (!gsk_next_driver_create_render_target (job->driver,
                                             texture_to_blur_width,
                                             texture_to_blur_height,
                                             GL_NEAREST, GL_NEAREST,
                                             &pass2))
    return gsk_next_driver_release_render_target (job->driver, pass1, FALSE);

  gsk_gl_render_state_save (&state, job);

  init_projection_matrix (&job->projection, &new_clip.bounds);
  gsk_gl_render_job_set_modelview (job, NULL);
  job->viewport = new_clip.bounds;
  gsk_gl_render_job_push_clip (job, &new_clip);

  /* Bind new framebuffer and clear it */
  gsk_gl_command_queue_bind_framebuffer (job->command_queue, pass1->framebuffer_id);
  gsk_gl_command_queue_clear (job->command_queue, 0, &job->viewport);

  /* Begin drawing the first horizontal pass, using offscreen as the
   * source texture for the program.
   */
  gsk_gl_program_begin_draw (job->driver->blur,
                             &job->viewport,
                             &job->projection,
                             gsk_gl_render_job_get_modelview_matrix (job),
                             gsk_gl_render_job_get_clip (job),
                             job->alpha);
  gsk_gl_program_set_uniform_texture (job->driver->blur,
                                      UNIFORM_SHARED_SOURCE,
                                      GL_TEXTURE_2D,
                                      GL_TEXTURE0,
                                      offscreen->texture_id);
  gsk_gl_program_set_uniform1f (job->driver->blur,
                                UNIFORM_BLUR_RADIUS,
                                blur_radius_x);
  gsk_gl_program_set_uniform2f (job->driver->blur,
                                UNIFORM_BLUR_SIZE,
                                texture_to_blur_width,
                                texture_to_blur_height);
  gsk_gl_program_set_uniform2f (job->driver->blur,
                                UNIFORM_BLUR_DIR,
                                1, 0);
  gsk_gl_render_job_draw_coords (job, 0, 0, texture_to_blur_width, texture_to_blur_height);
  gsk_gl_program_end_draw (job->driver->blur);

  /* Bind second pass framebuffer and clear it */
  gsk_gl_command_queue_bind_framebuffer (job->command_queue, pass2->framebuffer_id);
  gsk_gl_command_queue_clear (job->command_queue, 0, &job->viewport);

  /* Draw using blur program with first pass as source texture */
  gsk_gl_program_begin_draw (job->driver->blur,
                             &job->viewport,
                             &job->projection,
                             gsk_gl_render_job_get_modelview_matrix (job),
                             gsk_gl_render_job_get_clip (job),
                             job->alpha);
  gsk_gl_program_set_uniform_texture (job->driver->blur,
                                      UNIFORM_SHARED_SOURCE,
                                      GL_TEXTURE_2D,
                                      GL_TEXTURE0,
                                      pass1->texture_id);
  gsk_gl_program_set_uniform1f (job->driver->blur,
                                UNIFORM_BLUR_RADIUS,
                                blur_radius_y);
  gsk_gl_program_set_uniform2f (job->driver->blur,
                                UNIFORM_BLUR_SIZE,
                                texture_to_blur_width,
                                texture_to_blur_height);
  gsk_gl_program_set_uniform2f (job->driver->blur,
                                UNIFORM_BLUR_DIR,
                                0, 1);
  gsk_gl_render_job_draw_coords (job, 0, 0, texture_to_blur_width, texture_to_blur_height);
  gsk_gl_program_end_draw (job->driver->blur);

  gsk_gl_render_job_pop_modelview (job);
  gsk_gl_render_job_pop_clip (job);

  gsk_gl_render_state_restore (&state, job);

  gsk_next_driver_release_render_target (job->driver, pass1, TRUE);

  return gsk_next_driver_release_render_target (job->driver, pass2, FALSE);
}

static void
blur_node (GskGLRenderJob       *job,
           GskGLRenderOffscreen *offscreen,
           GskRenderNode        *node,
           float                 blur_radius,
           float                *min_x,
           float                *max_x,
           float                *min_y,
           float                *max_y)
{
  const float blur_extra = blur_radius * 2.0; /* 2.0 = shader radius_multiplier */
  const float half_blur_extra = (blur_extra / 2.0);
  float scale_x = job->scale_x;
  float scale_y = job->scale_y;
  float texture_width;
  float texture_height;

  g_assert (blur_radius > 0);

  /* Increase texture size for the given blur radius and scale it */
  texture_width  = ceilf ((node->bounds.size.width  + blur_extra));
  texture_height = ceilf ((node->bounds.size.height + blur_extra));

  /* Only blur this if the out region has no texture id yet */
  if (offscreen->texture_id == 0)
    {
      const graphene_rect_t bounds = GRAPHENE_RECT_INIT (node->bounds.origin.x - half_blur_extra,
                                                         node->bounds.origin.y - half_blur_extra,
                                                         texture_width, texture_height);

      offscreen->bounds = &bounds;
      offscreen->reset_clip = TRUE;
      offscreen->force_offscreen = TRUE;

      if (!gsk_gl_render_job_visit_node_with_offscreen (job, node, offscreen))
        g_assert_not_reached ();

      /* Ensure that we actually got a real texture_id */
      g_assert (offscreen->texture_id != 0);

      /* If this wasn't FALSE, we'd have to release the texture */
      g_assert (offscreen->do_not_cache == FALSE);

      offscreen->texture_id = blur_offscreen (job,
                                              offscreen,
                                              texture_width * scale_x,
                                              texture_height * scale_y,
                                              blur_radius * scale_x,
                                              blur_radius * scale_y);
      init_full_texture_region (&offscreen->area);
    }

  *min_x = job->offset_x + node->bounds.origin.x - half_blur_extra;
  *max_x = job->offset_x + node->bounds.origin.x + node->bounds.size.width + half_blur_extra;
  *min_y = job->offset_y + node->bounds.origin.y - half_blur_extra;
  *max_y = job->offset_y + node->bounds.origin.y + node->bounds.size.height + half_blur_extra;
}

static void
gsk_gl_render_job_visit_color_node (GskGLRenderJob *job,
                                    GskRenderNode  *node)
{
  gsk_gl_program_begin_draw (job->driver->color,
                             &job->viewport,
                             &job->projection,
                             gsk_gl_render_job_get_modelview_matrix (job),
                             gsk_gl_render_job_get_clip (job),
                             job->alpha);
  gsk_gl_program_set_uniform_color (job->driver->color,
                                    UNIFORM_COLOR_COLOR,
                                    gsk_color_node_get_color (node));
  gsk_gl_render_job_draw_rect (job, &node->bounds);
  gsk_gl_program_end_draw (job->driver->color);
}

static void
gsk_gl_render_job_visit_linear_gradient_node (GskGLRenderJob *job,
                                              GskRenderNode  *node)
{
  const GskColorStop *stops = gsk_linear_gradient_node_get_color_stops (node, NULL);
  const graphene_point_t *start = gsk_linear_gradient_node_get_start (node);
  const graphene_point_t *end = gsk_linear_gradient_node_get_end (node);
  int n_color_stops = gsk_linear_gradient_node_get_n_color_stops (node);

  g_assert (n_color_stops < MAX_GRADIENT_STOPS);

  gsk_gl_program_begin_draw (job->driver->linear_gradient,
                             &job->viewport,
                             &job->projection,
                             gsk_gl_render_job_get_modelview_matrix (job),
                             gsk_gl_render_job_get_clip (job),
                             job->alpha);
  gsk_gl_program_set_uniform1i (job->driver->linear_gradient,
                                UNIFORM_LINEAR_GRADIENT_NUM_COLOR_STOPS,
                                n_color_stops);
  gsk_gl_program_set_uniform1fv (job->driver->linear_gradient,
                                 UNIFORM_LINEAR_GRADIENT_COLOR_STOPS,
                                 n_color_stops * 5,
                                 (const float *)stops);
  gsk_gl_program_set_uniform2f (job->driver->linear_gradient,
                                UNIFORM_LINEAR_GRADIENT_START_POINT,
                                job->offset_x + start->x,
                                job->offset_y + start->y);
  gsk_gl_program_set_uniform2f (job->driver->linear_gradient,
                                UNIFORM_LINEAR_GRADIENT_END_POINT,
                                job->offset_x + end->x,
                                job->offset_y + end->y);
  gsk_gl_render_job_draw_rect (job, &node->bounds);
  gsk_gl_program_end_draw (job->driver->linear_gradient);
}

static void
gsk_gl_render_job_visit_conic_gradient_node (GskGLRenderJob *job,
                                             GskRenderNode  *node)
{
  const GskColorStop *stops = gsk_conic_gradient_node_get_color_stops (node, NULL);
  const graphene_point_t *center = gsk_conic_gradient_node_get_center (node);
  float rotation = gsk_conic_gradient_node_get_rotation (node);
  int n_color_stops = gsk_conic_gradient_node_get_n_color_stops (node);

  g_assert (n_color_stops < MAX_GRADIENT_STOPS);

  gsk_gl_program_begin_draw (job->driver->conic_gradient,
                             &job->viewport,
                             &job->projection,
                             gsk_gl_render_job_get_modelview_matrix (job),
                             gsk_gl_render_job_get_clip (job),
                             job->alpha);
  gsk_gl_program_set_uniform1i (job->driver->conic_gradient,
                                UNIFORM_CONIC_GRADIENT_NUM_COLOR_STOPS,
                                n_color_stops);
  gsk_gl_program_set_uniform1fv (job->driver->conic_gradient,
                                 UNIFORM_CONIC_GRADIENT_COLOR_STOPS,
                                 n_color_stops * 5,
                                 (const float *)stops);
  gsk_gl_program_set_uniform2f (job->driver->conic_gradient,
                                UNIFORM_CONIC_GRADIENT_CENTER,
                                center->x, center->y);
  gsk_gl_program_set_uniform1f (job->driver->conic_gradient,
                                UNIFORM_CONIC_GRADIENT_ROTATION,
                                rotation);
  gsk_gl_render_job_draw_rect (job, &node->bounds);
  gsk_gl_program_end_draw (job->driver->conic_gradient);
}

static void
gsk_gl_render_job_visit_radial_gradient_node (GskGLRenderJob *job,
                                              GskRenderNode  *node)
{
  int n_color_stops = gsk_radial_gradient_node_get_n_color_stops (node);
  const GskColorStop *stops = gsk_radial_gradient_node_get_color_stops (node, NULL);
  const graphene_point_t *center = gsk_radial_gradient_node_get_center (node);
  float start = gsk_radial_gradient_node_get_start (node);
  float end = gsk_radial_gradient_node_get_end (node);
  float hradius = gsk_radial_gradient_node_get_hradius (node);
  float vradius = gsk_radial_gradient_node_get_vradius (node);

  g_assert (n_color_stops < MAX_GRADIENT_STOPS);

  gsk_gl_program_begin_draw (job->driver->radial_gradient,
                             &job->viewport,
                             &job->projection,
                             gsk_gl_render_job_get_modelview_matrix (job),
                             gsk_gl_render_job_get_clip (job),
                             job->alpha);
  gsk_gl_program_set_uniform1i (job->driver->radial_gradient,
                                UNIFORM_RADIAL_GRADIENT_NUM_COLOR_STOPS,
                                n_color_stops);
  gsk_gl_program_set_uniform1fv (job->driver->radial_gradient,
                                 UNIFORM_RADIAL_GRADIENT_COLOR_STOPS,
                                 n_color_stops * 5,
                                 (const float *)stops);
  gsk_gl_program_set_uniform1f (job->driver->radial_gradient,
                                UNIFORM_RADIAL_GRADIENT_START,
                                start);
  gsk_gl_program_set_uniform1f (job->driver->radial_gradient,
                                UNIFORM_RADIAL_GRADIENT_END,
                                end);
  gsk_gl_program_set_uniform2f (job->driver->radial_gradient,
                                UNIFORM_RADIAL_GRADIENT_RADIUS,
                                job->scale_x * hradius,
                                job->scale_y * vradius);
  gsk_gl_program_set_uniform2f (job->driver->radial_gradient,
                                UNIFORM_RADIAL_GRADIENT_CENTER,
                                job->offset_x + center->x,
                                job->offset_y + center->y);
  gsk_gl_render_job_draw_rect (job, &node->bounds);
  gsk_gl_program_end_draw (job->driver->radial_gradient);
}

static void
gsk_gl_render_job_visit_clipped_child (GskGLRenderJob        *job,
                                       GskRenderNode         *child,
                                       const graphene_rect_t *clip)
{
  graphene_rect_t transformed_clip;
  GskRoundedRect intersection;

  gsk_gl_render_job_transform_bounds (job, clip, &transformed_clip);

  if (gsk_gl_render_job_clip_is_rectilinear (job))
    {
      const GskRoundedRect *current_clip = gsk_gl_render_job_get_clip (job);

      memset (&intersection, 0, sizeof intersection);
      graphene_rect_intersection (&transformed_clip,
                                  &current_clip->bounds,
                                  &intersection.bounds);

      gsk_gl_render_job_push_clip (job, &intersection);
      gsk_gl_render_job_visit_node (job, child);
      gsk_gl_render_job_pop_clip (job);
    }
  else if (intersect_rounded_rectilinear (&transformed_clip,
                                          gsk_gl_render_job_get_clip (job),
                                          &intersection))
    {
      gsk_gl_render_job_push_clip (job, &intersection);
      gsk_gl_render_job_visit_node (job, child);
      gsk_gl_render_job_pop_clip (job);
    }
  else
    {
      GskRoundedRect scaled_clip;
      GskGLRenderOffscreen offscreen = {0};

      offscreen.bounds = &child->bounds;
      offscreen.force_offscreen = TRUE;

      scaled_clip = GSK_ROUNDED_RECT_INIT (clip->origin.x * job->scale_x,
                                           clip->origin.y * job->scale_y,
                                           clip->size.width * job->scale_x,
                                           clip->size.height * job->scale_y);

      gsk_gl_render_job_push_clip (job, &scaled_clip);
      gsk_gl_render_job_visit_node_with_offscreen (job, child, &offscreen);
      gsk_gl_render_job_pop_clip (job);

      g_assert (offscreen.texture_id);

      gsk_gl_program_begin_draw (job->driver->blit,
                                 &job->viewport,
                                 &job->projection,
                                 gsk_gl_render_job_get_modelview_matrix (job),
                                 gsk_gl_render_job_get_clip (job),
                                 job->alpha);
      gsk_gl_program_set_uniform_texture (job->driver->blit,
                                          UNIFORM_SHARED_SOURCE,
                                          GL_TEXTURE_2D,
                                          GL_TEXTURE0,
                                          offscreen.texture_id);
      gsk_gl_render_job_draw_offscreen_rect (job, &child->bounds);
      gsk_gl_program_end_draw (job->driver->blit);
    }
}

static void
gsk_gl_render_job_visit_clip_node (GskGLRenderJob *job,
                                   GskRenderNode  *node)
{
  const graphene_rect_t *clip = gsk_clip_node_get_clip (node);
  GskRenderNode *child = gsk_clip_node_get_child (node);

  gsk_gl_render_job_visit_clipped_child (job, child, clip);
}

static void
gsk_gl_render_job_visit_rounded_clip_node (GskGLRenderJob *job,
                                           GskRenderNode  *node)
{
  GskRenderNode *child = gsk_rounded_clip_node_get_child (node);
  const GskRoundedRect *clip = gsk_rounded_clip_node_get_clip (node);
  GskRoundedRect current_clip;
  GskRoundedRect transformed_clip;
  float scale_x = job->scale_x;
  float scale_y = job->scale_y;
  gboolean need_offscreen;

  if (node_is_invisible (child))
    return;

  current_clip = *gsk_gl_render_job_get_clip (job);

  gsk_gl_render_job_transform_bounds (job, &clip->bounds, &transformed_clip.bounds);

  for (guint i = 0; i < 4; i++)
    {
      transformed_clip.corner[i].width = clip->corner[i].width * scale_x;
      transformed_clip.corner[i].height = clip->corner[i].height * scale_y;
    }

  if (gsk_rounded_rect_is_rectilinear (clip))
    {
      GskRoundedRect intersected_clip;

      if (intersect_rounded_rectilinear (&current_clip.bounds,
                                         &transformed_clip,
                                         &intersected_clip))
        {
          gsk_gl_render_job_push_clip (job, &intersected_clip);
          gsk_gl_render_job_visit_node (job, child);
          gsk_gl_render_job_pop_clip (job);
          return;
        }
    }

  /* After this point we are really working with a new and a current clip
   * which both have rounded corners.
   */

  if (job->clip->len <= 1)
    need_offscreen = FALSE;
  else if (rounded_inner_rect_contains_rect (&current_clip, &transformed_clip.bounds))
    need_offscreen = FALSE;
  else
    need_offscreen = TRUE;

  if (!need_offscreen)
    {
      /* If the new clip entirely contains the current clip, the intersection is simply
       * the current clip, so we can ignore the new one.
       */
      if (rounded_inner_rect_contains_rect (&transformed_clip, &current_clip.bounds))
        {
          gsk_gl_render_job_visit_node (job, child);
        }
      else
        {
          /* TODO: Intersect current and new clip */
          gsk_gl_render_job_push_clip (job, &transformed_clip);
          gsk_gl_render_job_visit_node (job, child);
          gsk_gl_render_job_pop_clip (job);
        }
    }
  else
    {
      GskGLRenderOffscreen offscreen = {0};
      GskRoundedRect scaled_clip;

      offscreen.bounds = &node->bounds;

      /* NOTE: We are *not* transforming the clip by the current modelview here.
       *       We instead draw the untransformed clip to a texture and then transform
       *       that texture.
       *
       *       We do, however, apply the scale factor to the child clip of course.
       */
      scaled_clip.bounds.origin.x = clip->bounds.origin.x * scale_x;
      scaled_clip.bounds.origin.y = clip->bounds.origin.y * scale_y;
      scaled_clip.bounds.size.width = clip->bounds.size.width * scale_x;
      scaled_clip.bounds.size.height = clip->bounds.size.height * scale_y;

      /* Increase corner radius size by scale factor */
      for (guint i = 0; i < 4; i ++)
        {
          scaled_clip.corner[i].width = clip->corner[i].width * scale_x;
          scaled_clip.corner[i].height = clip->corner[i].height * scale_y;
        }

      gsk_gl_render_job_push_clip (job, &scaled_clip);
      if (!gsk_gl_render_job_visit_node_with_offscreen (job, child, &offscreen))
        g_assert_not_reached ();
      gsk_gl_render_job_pop_clip (job);

      g_assert (offscreen.texture_id);

      gsk_gl_program_begin_draw (job->driver->blit,
                                 &job->viewport,
                                 &job->projection,
                                 gsk_gl_render_job_get_modelview_matrix (job),
                                 gsk_gl_render_job_get_clip (job),
                                 job->alpha);
      gsk_gl_program_set_uniform_texture (job->driver->blit,
                                          UNIFORM_SHARED_SOURCE,
                                          GL_TEXTURE_2D,
                                          GL_TEXTURE0,
                                          offscreen.texture_id);
      gsk_gl_render_job_draw_offscreen_rect (job, &node->bounds);
      gsk_gl_program_end_draw (job->driver->blit);
    }
}

static inline void
sort_border_sides (const GdkRGBA *colors,
                   int           *indices)
{
  gboolean done[4] = {0, 0, 0, 0};
  guint cur = 0;

  for (guint i = 0; i < 3; i++)
    {
      if (done[i])
        continue;

      indices[cur] = i;
      done[i] = TRUE;
      cur++;

      for (guint k = i + 1; k < 4; k ++)
        {
          if (gdk_rgba_equal (&colors[k], &colors[i]))
            {
              indices[cur] = k;
              done[k] = TRUE;
              cur++;
            }
        }

      if (cur >= 4)
        break;
    }
}

static void
gsk_gl_render_job_visit_uniform_border_node (GskGLRenderJob *job,
                                             GskRenderNode  *node)
{
  const GskRoundedRect *rounded_outline = gsk_border_node_get_outline (node);
  const GdkRGBA *colors = gsk_border_node_get_colors (node);
  const float *widths = gsk_border_node_get_widths (node);
  GskRoundedRect outline;

  gsk_gl_render_job_transform_rounded_rect (job, rounded_outline, &outline);

  gsk_gl_program_begin_draw (job->driver->inset_shadow,
                             &job->viewport,
                             &job->projection,
                             gsk_gl_render_job_get_modelview_matrix (job),
                             gsk_gl_render_job_get_clip (job),
                             job->alpha);
  gsk_gl_program_set_uniform_rounded_rect (job->driver->inset_shadow,
                                           UNIFORM_INSET_SHADOW_OUTLINE_RECT,
                                           &outline);
  gsk_gl_program_set_uniform_color (job->driver->inset_shadow,
                                    UNIFORM_INSET_SHADOW_COLOR,
                                    &colors[0]);
  gsk_gl_program_set_uniform1f (job->driver->inset_shadow,
                                UNIFORM_INSET_SHADOW_SPREAD,
                                widths[0]);
  gsk_gl_program_set_uniform2f (job->driver->inset_shadow,
                                UNIFORM_INSET_SHADOW_OFFSET,
                                0, 0);
  gsk_gl_render_job_draw_rect (job, &node->bounds);
  gsk_gl_program_end_draw (job->driver->inset_shadow);
}

static void
gsk_gl_render_job_visit_border_node (GskGLRenderJob *job,
                                     GskRenderNode  *node)
{
  const GskRoundedRect *rounded_outline = gsk_border_node_get_outline (node);
  const GdkRGBA *colors = gsk_border_node_get_colors (node);
  const float *widths = gsk_border_node_get_widths (node);
  struct {
    float w;
    float h;
  } sizes[4];

  /* Top left */
  if (widths[3] > 0)
    sizes[0].w = MAX (widths[3], rounded_outline->corner[0].width);
  else
    sizes[0].w = 0;

  if (widths[0] > 0)
    sizes[0].h = MAX (widths[0], rounded_outline->corner[0].height);
  else
    sizes[0].h = 0;

  /* Top right */
  if (widths[1] > 0)
    sizes[1].w = MAX (widths[1], rounded_outline->corner[1].width);
  else
    sizes[1].w = 0;

  if (widths[0] > 0)
    sizes[1].h = MAX (widths[0], rounded_outline->corner[1].height);
  else
    sizes[1].h = 0;

  /* Bottom right */
  if (widths[1] > 0)
    sizes[2].w = MAX (widths[1], rounded_outline->corner[2].width);
  else
    sizes[2].w = 0;

  if (widths[2] > 0)
    sizes[2].h = MAX (widths[2], rounded_outline->corner[2].height);
  else
    sizes[2].h = 0;


  /* Bottom left */
  if (widths[3] > 0)
    sizes[3].w = MAX (widths[3], rounded_outline->corner[3].width);
  else
    sizes[3].w = 0;

  if (widths[2] > 0)
    sizes[3].h = MAX (widths[2], rounded_outline->corner[3].height);
  else
    sizes[3].h = 0;

  {
    float min_x = job->offset_x + node->bounds.origin.x;
    float min_y = job->offset_y + node->bounds.origin.y;
    float max_x = min_x + node->bounds.size.width;
    float max_y = min_y + node->bounds.size.height;
    const GskGLDrawVertex side_data[4][6] = {
      /* Top */
      {
        { { min_x,              min_y              }, { 0, 1 }, }, /* Upper left */
        { { min_x + sizes[0].w, min_y + sizes[0].h }, { 0, 0 }, }, /* Lower left */
        { { max_x,              min_y              }, { 1, 1 }, }, /* Upper right */

        { { max_x - sizes[1].w, min_y + sizes[1].h }, { 1, 0 }, }, /* Lower right */
        { { min_x + sizes[0].w, min_y + sizes[0].h }, { 0, 0 }, }, /* Lower left */
        { { max_x,              min_y              }, { 1, 1 }, }, /* Upper right */
      },
      /* Right */
      {
        { { max_x - sizes[1].w, min_y + sizes[1].h }, { 0, 1 }, }, /* Upper left */
        { { max_x - sizes[2].w, max_y - sizes[2].h }, { 0, 0 }, }, /* Lower left */
        { { max_x,              min_y              }, { 1, 1 }, }, /* Upper right */

        { { max_x,              max_y              }, { 1, 0 }, }, /* Lower right */
        { { max_x - sizes[2].w, max_y - sizes[2].h }, { 0, 0 }, }, /* Lower left */
        { { max_x,              min_y              }, { 1, 1 }, }, /* Upper right */
      },
      /* Bottom */
      {
        { { min_x + sizes[3].w, max_y - sizes[3].h }, { 0, 1 }, }, /* Upper left */
        { { min_x,              max_y              }, { 0, 0 }, }, /* Lower left */
        { { max_x - sizes[2].w, max_y - sizes[2].h }, { 1, 1 }, }, /* Upper right */

        { { max_x,              max_y              }, { 1, 0 }, }, /* Lower right */
        { { min_x            ,  max_y              }, { 0, 0 }, }, /* Lower left */
        { { max_x - sizes[2].w, max_y - sizes[2].h }, { 1, 1 }, }, /* Upper right */
      },
      /* Left */
      {
        { { min_x,              min_y              }, { 0, 1 }, }, /* Upper left */
        { { min_x,              max_y              }, { 0, 0 }, }, /* Lower left */
        { { min_x + sizes[0].w, min_y + sizes[0].h }, { 1, 1 }, }, /* Upper right */

        { { min_x + sizes[3].w, max_y - sizes[3].h }, { 1, 0 }, }, /* Lower right */
        { { min_x,              max_y              }, { 0, 0 }, }, /* Lower left */
        { { min_x + sizes[0].w, min_y + sizes[0].h }, { 1, 1 }, }, /* Upper right */
      }
    };
    int indices[4] = { 0, 1, 2, 3 };
    GskRoundedRect outline;

    /* We sort them by color */
    sort_border_sides (colors, indices);

    /* Prepare outline */
    gsk_gl_render_job_transform_rounded_rect (job, rounded_outline, &outline);

    gsk_gl_program_set_uniform4fv (job->driver->border,
                                   UNIFORM_BORDER_WIDTHS,
                                   1,
                                   widths);
    gsk_gl_program_set_uniform_rounded_rect (job->driver->border,
                                             UNIFORM_BORDER_OUTLINE_RECT,
                                             &outline);

    for (guint i = 0; i < 4; i++)
      {
        if (widths[indices[i]] <= 0)
          continue;

        gsk_gl_program_begin_draw (job->driver->border,
                                   &job->viewport,
                                   &job->projection,
                                   gsk_gl_render_job_get_modelview_matrix (job),
                                   gsk_gl_render_job_get_clip (job),
                                   job->alpha);
        gsk_gl_program_set_uniform4fv (job->driver->border,
                                       UNIFORM_BORDER_COLOR,
                                       1,
                                       (const float *)&colors[indices[i]]);
        gsk_gl_command_queue_add_vertices (job->command_queue, side_data[indices[i]]);
        gsk_gl_program_end_draw (job->driver->border);
      }
  }
}

/* Returns TRUE if applying @transform to @bounds
 * yields an axis-aligned rectangle
 */
static gboolean
result_is_axis_aligned (GskTransform          *transform,
                        const graphene_rect_t *bounds)
{
  graphene_matrix_t m;
  graphene_quad_t q;
  graphene_rect_t b;
  graphene_point_t b1, b2;
  const graphene_point_t *p;
  int i;

  gsk_transform_to_matrix (transform, &m);
  gsk_matrix_transform_rect (&m, bounds, &q);
  graphene_quad_bounds (&q, &b);
  graphene_rect_get_top_left (&b, &b1);
  graphene_rect_get_bottom_right (&b, &b2);

  for (i = 0; i < 4; i++)
    {
      p = graphene_quad_get_point (&q, i);
      if (fabs (p->x - b1.x) > FLT_EPSILON && fabs (p->x - b2.x) > FLT_EPSILON)
        return FALSE;
      if (fabs (p->y - b1.y) > FLT_EPSILON && fabs (p->y - b2.y) > FLT_EPSILON)
        return FALSE;
    }

  return TRUE;
}

static void
gsk_gl_render_job_visit_transform_node (GskGLRenderJob *job,
                                        GskRenderNode  *node)
{
  GskTransform *transform = gsk_transform_node_get_transform (node);
  const GskTransformCategory category = gsk_transform_get_category (transform);
  GskRenderNode *child = gsk_transform_node_get_child (node);

  switch (category)
    {
    case GSK_TRANSFORM_CATEGORY_IDENTITY:
      gsk_gl_render_job_visit_node (job, child);
    break;

    case GSK_TRANSFORM_CATEGORY_2D_TRANSLATE:
      {
        float dx, dy;

        gsk_transform_to_translate (transform, &dx, &dy);
        gsk_gl_render_job_offset (job, dx, dy);
        gsk_gl_render_job_visit_node (job, child);
        gsk_gl_render_job_offset (job, -dx, -dy);
      }
    break;

    case GSK_TRANSFORM_CATEGORY_2D_AFFINE:
      {
        gsk_gl_render_job_push_modelview (job, transform);
        gsk_gl_render_job_visit_node (job, child);
        gsk_gl_render_job_pop_modelview (job);
      }
    break;

    case GSK_TRANSFORM_CATEGORY_2D:
    case GSK_TRANSFORM_CATEGORY_3D:
    case GSK_TRANSFORM_CATEGORY_ANY:
    case GSK_TRANSFORM_CATEGORY_UNKNOWN:
      if (node_supports_transform (child))
        {
          gsk_gl_render_job_push_modelview (job, transform);
          gsk_gl_render_job_visit_node (job, child);
          gsk_gl_render_job_pop_modelview (job);
        }
      else
        {
          GskGLRenderOffscreen offscreen = {0};

          offscreen.bounds = &child->bounds;
          offscreen.reset_clip = TRUE;

          if (!result_is_axis_aligned (transform, &child->bounds))
            offscreen.linear_filter = TRUE;

          if (gsk_gl_render_job_visit_node_with_offscreen (job, child, &offscreen))
            {
              /* For non-trivial transforms, we draw everything on a texture and then
               * draw the texture transformed. */
              /* TODO: We should compute a modelview containing only the "non-trivial"
               *       part (e.g. the rotation) and use that. We want to keep the scale
               *       for the texture.
               */
              gsk_gl_render_job_push_modelview (job, transform);

              gsk_gl_program_begin_draw (job->driver->blit,
                                         &job->viewport,
                                         &job->projection,
                                         gsk_gl_render_job_get_modelview_matrix (job),
                                         gsk_gl_render_job_get_clip (job),
                                         job->alpha);
              gsk_gl_program_set_uniform_texture (job->driver->blit,
                                                  UNIFORM_SHARED_SOURCE,
                                                  GL_TEXTURE_2D,
                                                  GL_TEXTURE0,
                                                  offscreen.texture_id);
              gsk_gl_render_job_load_vertices_from_offscreen (job, &child->bounds, &offscreen);
              gsk_gl_program_end_draw (job->driver->blit);

              gsk_gl_render_job_pop_modelview (job);
            }
        }
    break;

    default:
      g_assert_not_reached ();
    }
}

static void
gsk_gl_render_job_visit_unblurred_inset_shadow_node (GskGLRenderJob *job,
                                                     GskRenderNode  *node)
{
  gsk_gl_program_begin_draw (job->driver->inset_shadow,
                             &job->viewport,
                             &job->projection,
                             gsk_gl_render_job_get_modelview_matrix (job),
                             gsk_gl_render_job_get_clip (job),
                             job->alpha);
  gsk_gl_program_set_uniform_rounded_rect (job->driver->inset_shadow,
                                           UNIFORM_INSET_SHADOW_OUTLINE_RECT,
                                           gsk_inset_shadow_node_get_outline (node));
  gsk_gl_program_set_uniform_color (job->driver->inset_shadow,
                                    UNIFORM_INSET_SHADOW_COLOR,
                                    gsk_inset_shadow_node_get_color (node));
  gsk_gl_program_set_uniform1f (job->driver->inset_shadow,
                                UNIFORM_INSET_SHADOW_SPREAD,
                                gsk_inset_shadow_node_get_spread (node));
  gsk_gl_program_set_uniform2f (job->driver->inset_shadow,
                                UNIFORM_INSET_SHADOW_OFFSET,
                                gsk_inset_shadow_node_get_dx (node),
                                gsk_inset_shadow_node_get_dy (node));
  gsk_gl_render_job_draw_rect (job, &node->bounds);
  gsk_gl_program_end_draw (job->driver->inset_shadow);
}

static void
gsk_gl_render_job_visit_blurred_inset_shadow_node (GskGLRenderJob *job,
                                                   GskRenderNode  *node)
{
  const GskRoundedRect *node_outline = gsk_inset_shadow_node_get_outline (node);
  float blur_radius = gsk_inset_shadow_node_get_blur_radius (node);
  float offset_x = gsk_inset_shadow_node_get_dx (node);
  float offset_y = gsk_inset_shadow_node_get_dy (node);
  float scale_x = job->scale_x;
  float scale_y = job->scale_y;
  float blur_extra = blur_radius * 2.0; /* 2.0 = shader radius_multiplier */
  float half_blur_extra = blur_radius;
  float texture_width;
  float texture_height;
  int blurred_texture_id;
  GskTextureKey key;
  GskGLRenderOffscreen offscreen = {0};

  g_assert (blur_radius > 0);

  texture_width = ceilf ((node_outline->bounds.size.width + blur_extra) * scale_x);
  texture_height = ceilf ((node_outline->bounds.size.height + blur_extra) * scale_y);

  key.pointer = node;
  key.pointer_is_child = FALSE;
  key.scale_x = scale_x;
  key.scale_y = scale_y;
  key.filter = GL_NEAREST;

  blurred_texture_id = gsk_next_driver_lookup_texture (job->driver, &key);

  if (blurred_texture_id == 0)
    {
      float spread = gsk_inset_shadow_node_get_spread (node) + half_blur_extra;
      GskRoundedRect outline_to_blur;
      GskGLRenderTarget *render_target;
      GskGLRenderState state;

      /* TODO: In the following code, we have to be careful about where we apply the scale.
       * We're manually scaling stuff (e.g. the outline) so we can later use texture_width
       * and texture_height (which are already scaled) as the geometry and keep the modelview
       * at a scale of 1. That's kinda complicated though... */

      /* Outline of what we actually want to blur later.
       * Spread grows inside, so we don't need to account for that. But the blur will need
       * to read outside of the inset shadow, so we need to draw some color in there. */
      outline_to_blur = *node_outline;
      gsk_rounded_rect_shrink (&outline_to_blur,
                               -half_blur_extra,
                               -half_blur_extra,
                               -half_blur_extra,
                               -half_blur_extra);

      /* Fit to our texture */
      outline_to_blur.bounds.origin.x = 0;
      outline_to_blur.bounds.origin.y = 0;
      outline_to_blur.bounds.size.width *= scale_x;
      outline_to_blur.bounds.size.height *= scale_y;

      for (guint i = 0; i < 4; i ++)
        {
          outline_to_blur.corner[i].width *= scale_x;
          outline_to_blur.corner[i].height *= scale_y;
        }

      if (!gsk_next_driver_create_render_target (job->driver,
                                                 texture_width, texture_height,
                                                 GL_NEAREST, GL_NEAREST,
                                                 &render_target))
        g_assert_not_reached ();

      gsk_gl_render_state_save (&state, job);

      job->viewport.origin.x = 0;
      job->viewport.origin.y = 0;
      job->viewport.size.width = texture_width;
      job->viewport.size.height = texture_height;

      init_projection_matrix (&job->projection,
                              &GRAPHENE_RECT_INIT (0, 0, texture_width, texture_height));
      gsk_gl_render_job_set_modelview (job, NULL);
      gsk_gl_render_job_push_clip (job, &GSK_ROUNDED_RECT_INIT (0, 0, texture_width, texture_height));

      gsk_gl_command_queue_bind_framebuffer (job->command_queue, render_target->framebuffer_id);
      gsk_gl_command_queue_clear (job->command_queue, 0, &job->viewport);

      /* Actual inset shadow outline drawing */
      gsk_gl_program_begin_draw (job->driver->inset_shadow,
                                 &job->viewport,
                                 &job->projection,
                                 gsk_gl_render_job_get_modelview_matrix (job),
                                 gsk_gl_render_job_get_clip (job),
                                 job->alpha);
      gsk_gl_program_set_uniform_rounded_rect (job->driver->inset_shadow,
                                               UNIFORM_INSET_SHADOW_OUTLINE_RECT,
                                               &outline_to_blur);
      gsk_gl_program_set_uniform_color (job->driver->inset_shadow,
                                        UNIFORM_INSET_SHADOW_COLOR,
                                        gsk_inset_shadow_node_get_color (node));
      gsk_gl_program_set_uniform1f (job->driver->inset_shadow,
                                    UNIFORM_INSET_SHADOW_SPREAD,
                                    spread * MAX (scale_x, scale_y));
      gsk_gl_program_set_uniform2f (job->driver->inset_shadow,
                                    UNIFORM_INSET_SHADOW_OFFSET,
                                    offset_x * scale_x,
                                    offset_y * scale_y);
      gsk_gl_render_job_draw (job, 0, 0, texture_width, texture_height);
      gsk_gl_program_end_draw (job->driver->inset_shadow);

      gsk_gl_render_job_pop_modelview (job);
      gsk_gl_render_job_pop_clip (job);

      gsk_gl_render_state_restore (&state, job);

      offscreen.texture_id = render_target->texture_id;
      init_full_texture_region (&offscreen.area);

      blurred_texture_id = blur_offscreen (job,
                                           &offscreen,
                                           texture_width,
                                           texture_height,
                                           blur_radius * scale_x,
                                           blur_radius * scale_y);

      gsk_next_driver_release_render_target (job->driver, render_target, TRUE);
    }

  g_assert (blurred_texture_id != 0);

  /* Blur the rendered unblurred inset shadow */
  /* Use a clip to cut away the unwanted parts outside of the original outline */
  {
    const gboolean needs_clip = !gsk_rounded_rect_is_rectilinear (node_outline);
    const float tx1 = half_blur_extra * scale_x / texture_width;
    const float tx2 = 1.0 - tx1;
    const float ty1 = half_blur_extra * scale_y / texture_height;
    const float ty2 = 1.0 - ty1;

    gsk_next_driver_cache_texture (job->driver, &key, blurred_texture_id);

    if (needs_clip)
      {
        GskRoundedRect node_clip;

        gsk_gl_render_job_transform_bounds (job, &node_outline->bounds, &node_clip.bounds);

        for (guint i = 0; i < 4; i ++)
          {
            node_clip.corner[i].width = node_outline->corner[i].width * scale_x;
            node_clip.corner[i].height = node_outline->corner[i].height * scale_y;
          }

        gsk_gl_render_job_push_clip (job, &node_clip);
      }

    offscreen.was_offscreen = TRUE;
    offscreen.area.origin.x = tx1;
    offscreen.area.origin.y = ty1;
    offscreen.area.size.width = tx2 - tx1;
    offscreen.area.size.height = ty2 - ty1;

    gsk_gl_program_begin_draw (job->driver->blit,
                               &job->viewport,
                               &job->projection,
                               gsk_gl_render_job_get_modelview_matrix (job),
                               gsk_gl_render_job_get_clip (job),
                               job->alpha);
    gsk_gl_program_set_uniform_texture (job->driver->blit,
                                        UNIFORM_SHARED_SOURCE,
                                        GL_TEXTURE_2D,
                                        GL_TEXTURE0,
                                        blurred_texture_id);
    gsk_gl_render_job_load_vertices_from_offscreen (job, &node->bounds, &offscreen);
    gsk_gl_program_end_draw (job->driver->blit);

    if (needs_clip)
      gsk_gl_render_job_pop_clip (job);
  }
}

static void
gsk_gl_render_job_visit_unblurred_outset_shadow_node (GskGLRenderJob *job,
                                                      GskRenderNode  *node)
{
  const GskRoundedRect *outline = gsk_outset_shadow_node_get_outline (node);
  float x = node->bounds.origin.x;
  float y = node->bounds.origin.y;
  float w = node->bounds.size.width;
  float h = node->bounds.size.height;
  float spread = gsk_outset_shadow_node_get_spread (node);
  float dx = gsk_outset_shadow_node_get_dx (node);
  float dy = gsk_outset_shadow_node_get_dy (node);
  const float edge_sizes[] = { // Top, right, bottom, left
    spread - dy, spread + dx, spread + dy, spread - dx
  };
  const float corner_sizes[][2] = { // top left, top right, bottom right, bottom left
    { outline->corner[0].width + spread - dx, outline->corner[0].height + spread - dy },
    { outline->corner[1].width + spread + dx, outline->corner[1].height + spread - dy },
    { outline->corner[2].width + spread + dx, outline->corner[2].height + spread + dy },
    { outline->corner[3].width + spread - dx, outline->corner[3].height + spread + dy },
  };

  gsk_gl_program_begin_draw (job->driver->unblurred_outset_shadow,
                             &job->viewport,
                             &job->projection,
                             gsk_gl_render_job_get_modelview_matrix (job),
                             gsk_gl_render_job_get_clip (job),
                             job->alpha);
  gsk_gl_program_set_uniform_rounded_rect (job->driver->unblurred_outset_shadow,
                                           UNIFORM_UNBLURRED_OUTSET_SHADOW_OUTLINE_RECT,
                                           outline);
  gsk_gl_program_set_uniform_color (job->driver->unblurred_outset_shadow,
                                    UNIFORM_UNBLURRED_OUTSET_SHADOW_COLOR,
                                    gsk_outset_shadow_node_get_color (node));
  gsk_gl_program_set_uniform1f (job->driver->unblurred_outset_shadow,
                                UNIFORM_UNBLURRED_OUTSET_SHADOW_SPREAD,
                                spread);
  gsk_gl_program_set_uniform2f (job->driver->unblurred_outset_shadow,
                                UNIFORM_UNBLURRED_OUTSET_SHADOW_OFFSET,
                                dx, dy);

  /* Corners... */
  if (corner_sizes[0][0] > 0 && corner_sizes[0][1] > 0) /* Top left */
    gsk_gl_render_job_draw (job,
                            x, y,
                            corner_sizes[0][0], corner_sizes[0][1]);
  if (corner_sizes[1][0] > 0 && corner_sizes[1][1] > 0) /* Top right */
    gsk_gl_render_job_draw (job,
                            x + w - corner_sizes[1][0], y,
                            corner_sizes[1][0], corner_sizes[1][1]);
  if (corner_sizes[2][0] > 0 && corner_sizes[2][1] > 0) /* Bottom right */
    gsk_gl_render_job_draw (job,
                            x + w - corner_sizes[2][0], y + h - corner_sizes[2][1],
                            corner_sizes[2][0], corner_sizes[2][1]);
  if (corner_sizes[3][0] > 0 && corner_sizes[3][1] > 0) /* Bottom left */
    gsk_gl_render_job_draw (job,
                            x, y + h - corner_sizes[3][1],
                            corner_sizes[3][0], corner_sizes[3][1]);
  /* Edges... */;
  if (edge_sizes[0] > 0) /* Top */
    gsk_gl_render_job_draw (job,
                            x + corner_sizes[0][0], y,
                            w - corner_sizes[0][0] - corner_sizes[1][0], edge_sizes[0]);
  if (edge_sizes[1] > 0) /* Right */
    gsk_gl_render_job_draw (job,
                            x + w - edge_sizes[1], y + corner_sizes[1][1],
                            edge_sizes[1], h - corner_sizes[1][1] - corner_sizes[2][1]);
  if (edge_sizes[2] > 0) /* Bottom */
    gsk_gl_render_job_draw (job,
                            x + corner_sizes[3][0], y + h - edge_sizes[2],
                            w - corner_sizes[3][0] - corner_sizes[2][0], edge_sizes[2]);
  if (edge_sizes[3] > 0) /* Left */
    gsk_gl_render_job_draw (job,
                            x, y + corner_sizes[0][1],
                            edge_sizes[3], h - corner_sizes[0][1] - corner_sizes[3][1]);

  gsk_gl_program_end_draw (job->driver->unblurred_outset_shadow);
}

static void
gsk_gl_render_job_visit_blurred_outset_shadow_node (GskGLRenderJob *job,
                                                    GskRenderNode  *node)
{
  const GskRoundedRect *outline = gsk_outset_shadow_node_get_outline (node);
  const GdkRGBA *color = gsk_outset_shadow_node_get_color (node);
  const float scale_x = job->scale_x;
  const float scale_y = job->scale_y;
  const float blur_radius = gsk_outset_shadow_node_get_blur_radius (node);
  const float blur_extra = blur_radius * 2.0f; /* 2.0 = shader radius_multiplier */
  const int extra_blur_pixels = (int) ceilf(blur_extra / 2.0 * MAX (scale_x, scale_y)); /* TODO: No need to MAX() here actually */
  const float spread = gsk_outset_shadow_node_get_spread (node);
  const float dx = gsk_outset_shadow_node_get_dx (node);
  const float dy = gsk_outset_shadow_node_get_dy (node);
  GskRoundedRect scaled_outline;
  int texture_width, texture_height;
  int blurred_texture_id;
  int cached_tid;
  gboolean do_slicing;

  /* scaled_outline is the minimal outline we need to draw the given drop shadow,
   * enlarged by the spread and offset by the blur radius. */
  scaled_outline = *outline;

  if (outline->bounds.size.width < blur_extra ||
      outline->bounds.size.height < blur_extra)
    {
      do_slicing = false;
      gsk_rounded_rect_shrink (&scaled_outline, -spread, -spread, -spread, -spread);
    }
  else
    {
      /* Shrink our outline to the minimum size that can still hold all the border radii */
      gsk_rounded_rect_shrink_to_minimum (&scaled_outline);
      /* Increase by the spread */
      gsk_rounded_rect_shrink (&scaled_outline, -spread, -spread, -spread, -spread);
      /* Grow bounds but don't grow corners */
      graphene_rect_inset (&scaled_outline.bounds, - blur_extra / 2.0, - blur_extra / 2.0);
      /* For the center part, we add a few pixels */
      scaled_outline.bounds.size.width += SHADOW_EXTRA_SIZE;
      scaled_outline.bounds.size.height += SHADOW_EXTRA_SIZE;

      do_slicing = true;
    }

  texture_width  = (int)ceil ((scaled_outline.bounds.size.width  + blur_extra) * scale_x);
  texture_height = (int)ceil ((scaled_outline.bounds.size.height + blur_extra) * scale_y);

  scaled_outline.bounds.origin.x = extra_blur_pixels;
  scaled_outline.bounds.origin.y = extra_blur_pixels;
  scaled_outline.bounds.size.width = texture_width - (extra_blur_pixels * 2);
  scaled_outline.bounds.size.height = texture_height - (extra_blur_pixels * 2);

  for (guint i = 0; i < 4; i ++)
    {
      scaled_outline.corner[i].width *= scale_x;
      scaled_outline.corner[i].height *= scale_y;
    }

#if 0
  cached_tid = gsk_gl_shadow_library_get_texture_id (job->driver->shadows,
                                                     &scaled_outline,
                                                     blur_radius);


  if (cached_tid == 0)
    {
      int texture_id, render_target;
      int prev_render_target;
      graphene_matrix_t prev_projection;
      graphene_rect_t prev_viewport;
      graphene_matrix_t item_proj;

      gsk_gl_driver_create_render_target (self->gl_driver,
                                          texture_width, texture_height,
                                          GL_NEAREST, GL_NEAREST,
                                          &texture_id, &render_target);
      if (gdk_gl_context_has_debug (self->gl_context))
        {
          gdk_gl_context_label_object_printf (self->gl_context, GL_TEXTURE, texture_id,
                                              "Outset Shadow Temp %d", texture_id);
          gdk_gl_context_label_object_printf  (self->gl_context, GL_FRAMEBUFFER, render_target,
                                               "Outset Shadow FB Temp %d", render_target);
        }

      ops_set_program (builder, &self->programs->color_program);
      init_projection_matrix (&item_proj,
                              &GRAPHENE_RECT_INIT (0, 0, texture_width, texture_height));

      prev_render_target = ops_set_render_target (builder, render_target);
      ops_begin (builder, OP_CLEAR);
      prev_projection = ops_set_projection (builder, &item_proj);
      ops_set_modelview (builder, NULL);
      prev_viewport = ops_set_viewport (builder, &GRAPHENE_RECT_INIT (0, 0, texture_width, texture_height));

      /* Draw outline */
      ops_push_clip (builder, &scaled_outline);
      ops_set_color (builder, &COLOR_WHITE);
      load_float_vertex_data (ops_draw (builder, NULL), builder,
                              0, 0, texture_width, texture_height);

      ops_pop_clip (builder);
      ops_set_viewport (builder, &prev_viewport);
      ops_pop_modelview (builder);
      ops_set_projection (builder, &prev_projection);
      ops_set_render_target (builder, prev_render_target);

      /* Now blur the outline */
      blurred_texture_id = blur_texture (self, builder,
                                         &(TextureRegion) { texture_id, 0, 0, 1, 1 },
                                         texture_width,
                                         texture_height,
                                         blur_radius * scale_x,
                                         blur_radius * scale_y);

      gsk_gl_driver_mark_texture_permanent (self->gl_driver, blurred_texture_id);
      gsk_gl_shadow_cache_commit (&self->shadow_cache,
                                  &scaled_outline,
                                  blur_radius,
                                  blurred_texture_id);
    }
  else
    {
      blurred_texture_id = cached_tid;
    }


  if (!do_slicing)
    {
      const float min_x = floorf (outline->bounds.origin.x - spread - (blur_extra / 2.0) + dx);
      const float min_y = floorf (outline->bounds.origin.y - spread - (blur_extra / 2.0) + dy);

      ops_set_program (builder, &self->programs->outset_shadow_program);
      ops_set_color (builder, color);
      ops_set_texture (builder, blurred_texture_id);

      shadow = ops_begin (builder, OP_CHANGE_OUTSET_SHADOW);
      shadow->outline.value = transform_rect (self, builder, outline);
      shadow->outline.send = TRUE;

      load_vertex_data_with_region (ops_draw (builder, NULL),
                                    &GRAPHENE_RECT_INIT (
                                      min_x, min_y,
                                      texture_width / scale_x, texture_height / scale_y
                                    ), builder,
                                    &(TextureRegion) { 0, 0, 0, 1, 1 },
                                    FALSE);
      return;
    }


  ops_set_program (builder, &self->programs->outset_shadow_program);
  ops_set_color (builder, color);
  ops_set_texture (builder, blurred_texture_id);

  shadow = ops_begin (builder, OP_CHANGE_OUTSET_SHADOW);
  shadow->outline.value = transform_rect (self, builder, outline);
  shadow->outline.send = TRUE;

  {
    const float min_x = floorf (outline->bounds.origin.x - spread - (blur_extra / 2.0) + dx);
    const float min_y = floorf (outline->bounds.origin.y - spread - (blur_extra / 2.0) + dy);
    const float max_x = ceilf (outline->bounds.origin.x + outline->bounds.size.width +
                               (blur_extra / 2.0) + dx + spread);
    const float max_y = ceilf (outline->bounds.origin.y + outline->bounds.size.height +
                               (blur_extra / 2.0) + dy + spread);
    cairo_rectangle_int_t slices[9];
    TextureRegion tregs[9];

    /* TODO: The slicing never changes and could just go into the cache */
    nine_slice_rounded_rect (&scaled_outline, slices);
    nine_slice_grow (slices, extra_blur_pixels);
    nine_slice_to_texture_coords (slices, texture_width, texture_height, tregs);

    /* Our texture coordinates MUST be scaled, while the actual vertex coords
     * MUST NOT be scaled. */

    /* Top left */
    if (slice_is_visible (&slices[NINE_SLICE_TOP_LEFT]))
      {
        load_vertex_data_with_region (ops_draw (builder, NULL),
                                      &GRAPHENE_RECT_INIT (
                                        min_x, min_y,
                                        slices[NINE_SLICE_TOP_LEFT].width / scale_x,
                                        slices[NINE_SLICE_TOP_LEFT].height / scale_y
                                      ),
                                      builder,
                                      &tregs[NINE_SLICE_TOP_LEFT], TRUE);
      }

    /* Top center */
    if (slice_is_visible (&slices[NINE_SLICE_TOP_CENTER]))
      {
        const float width = (max_x - min_x) - (slices[NINE_SLICE_TOP_LEFT].width / scale_x +
                                               slices[NINE_SLICE_TOP_RIGHT].width / scale_x);
        load_vertex_data_with_region (ops_draw (builder, NULL),
                                      &GRAPHENE_RECT_INIT (
                                        min_x + (slices[NINE_SLICE_TOP_LEFT].width / scale_x),
                                        min_y,
                                        width,
                                        slices[NINE_SLICE_TOP_CENTER].height / scale_y
                                      ),
                                      builder,
                                      &tregs[NINE_SLICE_TOP_CENTER], TRUE);
      }
    /* Top right */
    if (slice_is_visible (&slices[NINE_SLICE_TOP_RIGHT]))
      {
        load_vertex_data_with_region (ops_draw (builder, NULL),
                                      &GRAPHENE_RECT_INIT (
                                        max_x - (slices[NINE_SLICE_TOP_RIGHT].width / scale_x),
                                        min_y,
                                        slices[NINE_SLICE_TOP_RIGHT].width / scale_x,
                                        slices[NINE_SLICE_TOP_RIGHT].height / scale_y
                                      ),
                                      builder,
                                      &tregs[NINE_SLICE_TOP_RIGHT], TRUE);
      }

    /* Bottom right */
    if (slice_is_visible (&slices[NINE_SLICE_BOTTOM_RIGHT]))
      {
        load_vertex_data_with_region (ops_draw (builder, NULL),
                                      &GRAPHENE_RECT_INIT (
                                        max_x - (slices[NINE_SLICE_BOTTOM_RIGHT].width / scale_x),
                                        max_y - (slices[NINE_SLICE_BOTTOM_RIGHT].height / scale_y),
                                        slices[NINE_SLICE_BOTTOM_RIGHT].width / scale_x,
                                        slices[NINE_SLICE_BOTTOM_RIGHT].height / scale_y
                                      ),
                                      builder,
                                      &tregs[NINE_SLICE_BOTTOM_RIGHT], TRUE);
      }

    /* Bottom left */
    if (slice_is_visible (&slices[NINE_SLICE_BOTTOM_LEFT]))
      {
        load_vertex_data_with_region (ops_draw (builder, NULL),
                                      &GRAPHENE_RECT_INIT (
                                        min_x,
                                        max_y - (slices[NINE_SLICE_BOTTOM_LEFT].height / scale_y),
                                        slices[NINE_SLICE_BOTTOM_LEFT].width / scale_x,
                                        slices[NINE_SLICE_BOTTOM_LEFT].height / scale_y
                                      ),
                                      builder,
                                      &tregs[NINE_SLICE_BOTTOM_LEFT], TRUE);
      }

    /* Left side */
    if (slice_is_visible (&slices[NINE_SLICE_LEFT_CENTER]))
      {
        const float height = (max_y - min_y) - (slices[NINE_SLICE_TOP_LEFT].height / scale_y +
                                                slices[NINE_SLICE_BOTTOM_LEFT].height / scale_y);
        load_vertex_data_with_region (ops_draw (builder, NULL),
                                      &GRAPHENE_RECT_INIT (
                                        min_x,
                                        min_y + (slices[NINE_SLICE_TOP_LEFT].height / scale_y),
                                        slices[NINE_SLICE_LEFT_CENTER].width / scale_x,
                                        height
                                      ),
                                      builder,
                                      &tregs[NINE_SLICE_LEFT_CENTER], TRUE);
      }

    /* Right side */
    if (slice_is_visible (&slices[NINE_SLICE_RIGHT_CENTER]))
      {
        const float height = (max_y - min_y) - (slices[NINE_SLICE_TOP_RIGHT].height / scale_y +
                                                slices[NINE_SLICE_BOTTOM_RIGHT].height / scale_y);
        load_vertex_data_with_region (ops_draw (builder, NULL),
                                      &GRAPHENE_RECT_INIT (
                                        max_x - (slices[NINE_SLICE_RIGHT_CENTER].width / scale_x),
                                        min_y + (slices[NINE_SLICE_TOP_LEFT].height / scale_y),
                                        slices[NINE_SLICE_RIGHT_CENTER].width / scale_x,
                                        height
                                      ),
                                      builder,
                                      &tregs[NINE_SLICE_RIGHT_CENTER], TRUE);
      }

    /* Bottom side */
    if (slice_is_visible (&slices[NINE_SLICE_BOTTOM_CENTER]))
      {
        const float width = (max_x - min_x) - (slices[NINE_SLICE_BOTTOM_LEFT].width / scale_x +
                                               slices[NINE_SLICE_BOTTOM_RIGHT].width / scale_x);
        load_vertex_data_with_region (ops_draw (builder, NULL),
                                      &GRAPHENE_RECT_INIT (
                                        min_x + (slices[NINE_SLICE_BOTTOM_LEFT].width / scale_x),
                                        max_y - (slices[NINE_SLICE_BOTTOM_CENTER].height / scale_y),
                                        width,
                                        slices[NINE_SLICE_BOTTOM_CENTER].height / scale_y
                                      ),
                                      builder,
                                      &tregs[NINE_SLICE_BOTTOM_CENTER], TRUE);
      }

    /* Middle */
    if (slice_is_visible (&slices[NINE_SLICE_CENTER]))
      {
        const float width = (max_x - min_x) - (slices[NINE_SLICE_LEFT_CENTER].width / scale_x +
                                               slices[NINE_SLICE_RIGHT_CENTER].width / scale_x);
        const float height = (max_y - min_y) - (slices[NINE_SLICE_TOP_CENTER].height / scale_y +
                                                slices[NINE_SLICE_BOTTOM_CENTER].height / scale_y);

        load_vertex_data_with_region (ops_draw (builder, NULL),
                                      &GRAPHENE_RECT_INIT (
                                        min_x + (slices[NINE_SLICE_LEFT_CENTER].width / scale_x),
                                        min_y + (slices[NINE_SLICE_TOP_CENTER].height / scale_y),
                                        width, height
                                      ),
                                      builder,
                                      &tregs[NINE_SLICE_CENTER], TRUE);
      }
  }
#endif
}

static inline bool G_GNUC_PURE
equal_texture_nodes (GskRenderNode *node1,
                     GskRenderNode *node2)
{
  if (gsk_render_node_get_node_type (node1) != GSK_TEXTURE_NODE ||
      gsk_render_node_get_node_type (node2) != GSK_TEXTURE_NODE)
    return false;

  if (gsk_texture_node_get_texture (node1) !=
      gsk_texture_node_get_texture (node2))
    return false;

  return graphene_rect_equal (&node1->bounds, &node2->bounds);
}

static void
gsk_gl_render_job_visit_cross_fade_node (GskGLRenderJob *job,
                                         GskRenderNode  *node)
{
  GskRenderNode *start_node = gsk_cross_fade_node_get_start_child (node);
  GskRenderNode *end_node = gsk_cross_fade_node_get_end_child (node);
  float progress = gsk_cross_fade_node_get_progress (node);
  GskGLRenderOffscreen offscreen_start = {0};
  GskGLRenderOffscreen offscreen_end = {0};

  g_assert (progress > 0.0);
  g_assert (progress < 1.0);

  offscreen_start.force_offscreen = TRUE;
  offscreen_start.reset_clip = TRUE;
  offscreen_start.bounds = &node->bounds;

  offscreen_end.force_offscreen = TRUE;
  offscreen_end.reset_clip = TRUE;
  offscreen_end.bounds = &node->bounds;

  if (!gsk_gl_render_job_visit_node_with_offscreen (job, start_node, &offscreen_start))
    {
      gsk_gl_render_job_visit_node (job, end_node);
      return;
    }

  g_assert (offscreen_start.texture_id);

  if (!gsk_gl_render_job_visit_node_with_offscreen (job, end_node, &offscreen_end))
    {
      float prev_alpha = job->alpha;
      gsk_gl_render_job_visit_node (job, start_node);
      job->alpha = prev_alpha;
      return;
    }

  g_assert (offscreen_end.texture_id);

  gsk_gl_program_begin_draw (job->driver->cross_fade,
                             &job->viewport,
                             &job->projection,
                             gsk_gl_render_job_get_modelview_matrix (job),
                             gsk_gl_render_job_get_clip (job),
                             job->alpha);
  gsk_gl_program_set_uniform_texture (job->driver->cross_fade,
                                      UNIFORM_SHARED_SOURCE,
                                      GL_TEXTURE_2D,
                                      GL_TEXTURE0,
                                      offscreen_start.texture_id);
  gsk_gl_program_set_uniform_texture (job->driver->cross_fade,
                                      UNIFORM_CROSS_FADE_SOURCE2,
                                      GL_TEXTURE_2D,
                                      GL_TEXTURE1,
                                      offscreen_end.texture_id);
  gsk_gl_program_set_uniform1i (job->driver->cross_fade,
                                UNIFORM_CROSS_FADE_PROGRESS,
                                gsk_cross_fade_node_get_progress (node));
  gsk_gl_program_end_draw (job->driver->cross_fade);
}

static void
gsk_gl_render_job_visit_opacity_node (GskGLRenderJob *job,
                                      GskRenderNode  *node)
{
  GskRenderNode *child = gsk_opacity_node_get_child (node);
  float opacity = gsk_opacity_node_get_opacity (node);

  if (gsk_render_node_get_node_type (child) == GSK_CONTAINER_NODE)
    {
      GskGLRenderOffscreen offscreen = {0};

      /* The semantics of an opacity node mandate that when, e.g., two
       * color nodes overlap, there may not be any blending between them.
       */

      offscreen.bounds = &child->bounds;
      offscreen.force_offscreen = TRUE;
      offscreen.reset_clip = TRUE;

      gsk_gl_render_job_visit_node_with_offscreen (job, child, &offscreen);

      g_assert (offscreen.texture_id);

      gsk_gl_program_begin_draw (job->driver->blit,
                                 &job->viewport,
                                 &job->projection,
                                 gsk_gl_render_job_get_modelview_matrix (job),
                                 gsk_gl_render_job_get_clip (job),
                                 job->alpha * opacity);
      gsk_gl_program_set_uniform_texture (job->driver->blit,
                                          UNIFORM_SHARED_SOURCE,
                                          GL_TEXTURE_2D,
                                          GL_TEXTURE0,
                                          offscreen.texture_id);
      gsk_gl_render_job_load_vertices_from_offscreen (job, &node->bounds, &offscreen);
      gsk_gl_program_end_draw (job->driver->blit);
    }
  else
    {
      float prev_alpha = job->alpha;

      job->alpha = job->alpha * opacity;
      gsk_gl_render_job_visit_node (job, child);
      job->alpha = prev_alpha;
    }
}

static void
gsk_gl_render_job_visit_text_node (GskGLRenderJob *job,
                                   GskRenderNode  *node,
                                   const GdkRGBA  *color,
                                   gboolean        force_color)
{
  const PangoFont *font = gsk_text_node_get_font (node);
  const PangoGlyphInfo *glyphs = gsk_text_node_get_glyphs (node, NULL);
  const graphene_point_t *offset = gsk_text_node_get_offset (node);
  float text_scale = MAX (job->scale_x, job->scale_y); /* TODO: Fix for uneven scales? */
  guint num_glyphs = gsk_text_node_get_num_glyphs (node);
  float x = offset->x + job->offset_x;
  float y = offset->y + job->offset_y;
  GskGLGlyphLibrary *library = job->driver->glyphs;
  GskGLProgram *program;
  int x_position = 0;
  GskGLGlyphKey lookup;
  guint last_texture = 0;

  /* If the font has color glyphs, we don't need to recolor anything */
  if (!force_color && gsk_text_node_has_color_glyphs (node))
    {
      program = job->driver->blit;
    }
  else
    {
      program = job->driver->coloring;
      gsk_gl_program_set_uniform_color (program, UNIFORM_COLORING_COLOR, color);
    }

  lookup.font = (PangoFont *)font;
  lookup.scale = (guint) (text_scale * 1024);

  gsk_gl_program_begin_draw (program,
                             &job->viewport,
                             &job->projection,
                             gsk_gl_render_job_get_modelview_matrix (job),
                             gsk_gl_render_job_get_clip (job),
                             job->alpha);

  /* We use one quad per character */
  for (guint i = 0; i < num_glyphs; i++)
    {
      GskGLDrawVertex *vertices;
      const PangoGlyphInfo *gi = &glyphs[i];
      const GskGLGlyphValue *glyph;
      float glyph_x, glyph_y, glyph_x2, glyph_y2;
      float tx, ty, tx2, ty2;
      float cx;
      float cy;
      guint texture_id;

      if (gi->glyph == PANGO_GLYPH_EMPTY)
        continue;

      cx = (float)(x_position + gi->geometry.x_offset) / PANGO_SCALE;
      cy = (float)(gi->geometry.y_offset) / PANGO_SCALE;

      gsk_gl_glyph_key_set_glyph_and_shift (&lookup, gi->glyph, x + cx, y + cy);

      if (!gsk_gl_glyph_library_lookup_or_add (library, &lookup, &glyph))
        goto next;

      texture_id = GSK_GL_TEXTURE_ATLAS_ENTRY_TEXTURE (glyph);

      g_assert (texture_id > 0);

      if G_UNLIKELY (last_texture != texture_id)
        {
          gsk_gl_program_end_draw (program);
          gsk_gl_program_set_uniform_texture (program,
                                              UNIFORM_SHARED_SOURCE,
                                              GL_TEXTURE_2D,
                                              GL_TEXTURE0,
                                              texture_id);
          gsk_gl_program_begin_draw (program,
                                     &job->viewport,
                                     &job->projection,
                                     gsk_gl_render_job_get_modelview_matrix (job),
                                     gsk_gl_render_job_get_clip (job),
                                     job->alpha);
          last_texture = texture_id;
        }

      tx = glyph->entry.area.origin.x;
      ty = glyph->entry.area.origin.y;
      tx2 = tx + glyph->entry.area.size.width;
      ty2 = ty + glyph->entry.area.size.height;

      glyph_x = floor (x + cx + 0.125) + glyph->ink_rect.x;
      glyph_y = floor (y + cy + 0.125) + glyph->ink_rect.y;
      glyph_x2 = glyph_x + glyph->ink_rect.width;
      glyph_y2 = glyph_y + glyph->ink_rect.height;

      vertices = gsk_gl_command_queue_add_vertices (job->command_queue, NULL);

      vertices[0].position[0] = glyph_x;
      vertices[0].position[1] = glyph_y;
      vertices[0].uv[0] = tx;
      vertices[0].uv[1] = ty;

      vertices[1].position[0] = glyph_x;
      vertices[1].position[1] = glyph_y2;
      vertices[1].uv[0] = tx;
      vertices[1].uv[1] = ty2;

      vertices[2].position[0] = glyph_x2;
      vertices[2].position[1] = glyph_y;
      vertices[2].uv[0] = tx2;
      vertices[2].uv[1] = ty;

      vertices[3].position[0] = glyph_x2;
      vertices[3].position[1] = glyph_y2;
      vertices[3].uv[0] = tx2;
      vertices[3].uv[1] = ty2;

      vertices[4].position[0] = glyph_x;
      vertices[4].position[1] = glyph_y2;
      vertices[4].uv[0] = tx;
      vertices[4].uv[1] = ty2;

      vertices[5].position[0] = glyph_x2;
      vertices[5].position[1] = glyph_y;
      vertices[5].uv[0] = tx2;
      vertices[5].uv[1] = ty;

next:
      x_position += gi->geometry.width;
    }

  gsk_gl_program_end_draw (program);
}

static void
gsk_gl_render_job_visit_shadow_node (GskGLRenderJob *job,
                                     GskRenderNode  *node)
{
  const gsize n_shadows = gsk_shadow_node_get_n_shadows (node);
  GskRenderNode *original_child = gsk_shadow_node_get_child (node);
  GskRenderNode *shadow_child = original_child;

  /* Shadow nodes recolor every pixel of the source texture, but leave the alpha in tact.
   * If the child is a color matrix node that doesn't touch the alpha, we can throw that away. */
  if (gsk_render_node_get_node_type (shadow_child) == GSK_COLOR_MATRIX_NODE &&
      !color_matrix_modifies_alpha (shadow_child))
    shadow_child = gsk_color_matrix_node_get_child (shadow_child);

  for (guint i = 0; i < n_shadows; i ++)
    {
      const GskShadow *shadow = gsk_shadow_node_get_shadow (node, i);
      const float dx = shadow->dx;
      const float dy = shadow->dy;
      GskGLRenderOffscreen offscreen = {0};
      graphene_rect_t bounds;

      if (shadow->radius == 0 &&
          gsk_render_node_get_node_type (shadow_child) == GSK_TEXT_NODE)
        {
          gsk_gl_render_job_offset (job, dx, dy);
          gsk_gl_render_job_visit_text_node (job, shadow_child, &shadow->color, TRUE);
          gsk_gl_render_job_offset (job, -dx, -dy);
          continue;
        }

      if (gdk_rgba_is_clear (&shadow->color))
        continue;

      if (node_is_invisible (shadow_child))
        continue;

      if (shadow->radius > 0)
        {
          float min_x;
          float min_y;
          float max_x;
          float max_y;

          offscreen.do_not_cache = TRUE;

          blur_node (job,
                     &offscreen,
                     shadow_child,
                     shadow->radius,
                     &min_x, &max_x,
                     &min_y, &max_y);

          bounds.origin.x = min_x - job->offset_x;
          bounds.origin.y = min_y - job->offset_y;
          bounds.size.width = max_x - min_x;
          bounds.size.height = max_y - min_y;

          offscreen.was_offscreen = TRUE;
        }
      else if (dx == 0 && dy == 0)
        {
          continue; /* Invisible anyway */
        }
      else
        {
          offscreen.bounds = &shadow_child->bounds;
          offscreen.reset_clip = TRUE;
          offscreen.do_not_cache = TRUE;

          if (!gsk_gl_render_job_visit_node_with_offscreen (job, shadow_child, &offscreen))
            g_assert_not_reached ();

          bounds = shadow_child->bounds;
        }

      gsk_gl_render_job_offset (job, dx, dy);
      gsk_gl_program_begin_draw (job->driver->coloring,
                                 &job->viewport,
                                 &job->projection,
                                 gsk_gl_render_job_get_modelview_matrix (job),
                                 gsk_gl_render_job_get_clip (job),
                                 job->alpha);
      gsk_gl_program_set_uniform_texture (job->driver->coloring,
                                          UNIFORM_SHARED_SOURCE,
                                          GL_TEXTURE_2D,
                                          GL_TEXTURE0,
                                          offscreen.texture_id);
      gsk_gl_program_set_uniform_color (job->driver->coloring,
                                        UNIFORM_COLORING_COLOR,
                                        &shadow->color);
      gsk_gl_render_job_load_vertices_from_offscreen (job, &bounds, &offscreen);
      gsk_gl_program_end_draw (job->driver->coloring);
      gsk_gl_render_job_offset (job, -dx, -dy);
    }

  /* Now draw the child normally */
  gsk_gl_render_job_visit_node (job, original_child);
}

static void
gsk_gl_render_job_visit_blur_node (GskGLRenderJob *job,
                                   GskRenderNode  *node)
{
  GskRenderNode *child = gsk_blur_node_get_child (node);
  float blur_radius = gsk_blur_node_get_radius (node);
  GskGLRenderOffscreen offscreen = {0};
  GskTextureKey key;
  gboolean cache_texture;
  float min_x;
  float max_x;
  float min_y;
  float max_y;

  g_assert (blur_radius > 0);

  if (node_is_invisible (child))
    return;

  key.pointer = node;
  key.pointer_is_child = FALSE;
  key.scale_x = job->scale_x;
  key.scale_y = job->scale_y;
  key.filter = GL_NEAREST;

  offscreen.texture_id = gsk_next_driver_lookup_texture (job->driver, &key);
  cache_texture = offscreen.texture_id == 0;

  blur_node (job,
             &offscreen,
             child,
             blur_radius,
             &min_x, &max_x, &min_y, &max_y);

  g_assert (offscreen.texture_id != 0);

  if (cache_texture)
    gsk_next_driver_cache_texture (job->driver, &key, offscreen.texture_id);

  gsk_gl_program_begin_draw (job->driver->blit,
                             &job->viewport,
                             &job->projection,
                             gsk_gl_render_job_get_modelview_matrix (job),
                             gsk_gl_render_job_get_clip (job),
                             job->alpha);
  gsk_gl_program_set_uniform_texture (job->driver->blit,
                                      UNIFORM_SHARED_SOURCE,
                                      GL_TEXTURE_2D,
                                      GL_TEXTURE0,
                                      offscreen.texture_id);
  gsk_gl_render_job_draw_coords (job, min_x, min_y, max_x, max_y);
  gsk_gl_program_end_draw (job->driver->blit);
}

static void
gsk_gl_render_job_visit_blend_node (GskGLRenderJob *job,
                                    GskRenderNode  *node)
{
  GskRenderNode *top_child = gsk_blend_node_get_top_child (node);
  GskRenderNode *bottom_child = gsk_blend_node_get_bottom_child (node);
  GskGLRenderOffscreen top_offscreen = {0};
  GskGLRenderOffscreen bottom_offscreen = {0};

  top_offscreen.bounds = &node->bounds;
  top_offscreen.force_offscreen = TRUE;
  top_offscreen.reset_clip = TRUE;

  bottom_offscreen.bounds = &node->bounds;
  bottom_offscreen.force_offscreen = TRUE;
  bottom_offscreen.reset_clip = TRUE;

  /* TODO: We create 2 textures here as big as the blend node, but both the
   * start and the end node might be a lot smaller than that. */
  if (!gsk_gl_render_job_visit_node_with_offscreen (job, bottom_child, &bottom_offscreen))
    {
      gsk_gl_render_job_visit_node (job, top_child);
      return;
    }

  if (!gsk_gl_render_job_visit_node_with_offscreen (job, top_child, &top_offscreen))
    {
      gsk_gl_render_job_load_vertices_from_offscreen (job, &node->bounds, &bottom_offscreen);
      return;
    }

  gsk_gl_program_begin_draw (job->driver->blend,
                             &job->viewport,
                             &job->projection,
                             gsk_gl_render_job_get_modelview_matrix (job),
                             gsk_gl_render_job_get_clip (job),
                             job->alpha);
  gsk_gl_program_set_uniform_texture (job->driver->blend,
                                      UNIFORM_SHARED_SOURCE,
                                      GL_TEXTURE_2D,
                                      GL_TEXTURE0,
                                      bottom_offscreen.texture_id);
  gsk_gl_program_set_uniform_texture (job->driver->blend,
                                      UNIFORM_BLEND_SOURCE2,
                                      GL_TEXTURE_2D,
                                      GL_TEXTURE0,
                                      top_offscreen.texture_id);
  gsk_gl_program_set_uniform1i (job->driver->blend,
                                UNIFORM_BLEND_MODE,
                                gsk_blend_node_get_blend_mode (node));
  gsk_gl_render_job_draw_offscreen_rect (job, &node->bounds);
  gsk_gl_program_end_draw (job->driver->blend);
}

static void
gsk_gl_render_job_visit_color_matrix_node (GskGLRenderJob *job,
                                           GskRenderNode  *node)
{
  GskRenderNode *child = gsk_color_matrix_node_get_child (node);
  GskGLRenderOffscreen offscreen = {0};
  float offset[4];

  if (node_is_invisible (child))
    return;

  offscreen.bounds = &node->bounds;
  offscreen.reset_clip = TRUE;

  if (!gsk_gl_render_job_visit_node_with_offscreen (job, child, &offscreen))
    g_assert_not_reached ();

  g_assert (offscreen.texture_id > 0);

  graphene_vec4_to_float (gsk_color_matrix_node_get_color_offset (node), offset);

  gsk_gl_program_begin_draw (job->driver->color_matrix,
                             &job->viewport,
                             &job->projection,
                             gsk_gl_render_job_get_modelview_matrix (job),
                             gsk_gl_render_job_get_clip (job),
                             job->alpha);
  gsk_gl_program_set_uniform_texture (job->driver->color_matrix,
                                      UNIFORM_SHARED_SOURCE,
                                      GL_TEXTURE_2D,
                                      GL_TEXTURE0,
                                      offscreen.texture_id);
  gsk_gl_program_set_uniform_matrix (job->driver->color_matrix,
                                     UNIFORM_COLOR_MATRIX_COLOR_MATRIX,
                                     gsk_color_matrix_node_get_color_matrix (node));
  gsk_gl_program_set_uniform4fv (job->driver->color_matrix,
                                 UNIFORM_COLOR_MATRIX_COLOR_OFFSET,
                                 1,
                                 offset);
  gsk_gl_render_job_load_vertices_from_offscreen (job, &node->bounds, &offscreen);
  gsk_gl_program_end_draw (job->driver->color_matrix);
}

static void
gsk_gl_render_job_visit_gl_shader_node_fallback (GskGLRenderJob *job,
                                                 GskRenderNode  *node)
{
  static const GdkRGBA pink = { 255 / 255., 105 / 255., 180 / 255., 1.0 };

  gsk_gl_program_begin_draw (job->driver->color,
                             &job->viewport,
                             &job->projection,
                             gsk_gl_render_job_get_modelview_matrix (job),
                             gsk_gl_render_job_get_clip (job),
                             job->alpha);
  gsk_gl_program_set_uniform_color (job->driver->color,
                                    UNIFORM_COLOR_COLOR,
                                    &pink);
  gsk_gl_render_job_draw_rect (job, &node->bounds);
  gsk_gl_program_end_draw (job->driver->color);
}

static void
gsk_gl_render_job_visit_gl_shader_node (GskGLRenderJob *job,
                                        GskRenderNode  *node)
{
  GError *error = NULL;
  GskGLShader *shader;
  GskGLProgram *program;
  int n_children;

  shader = gsk_gl_shader_node_get_shader (node);
  program = gsk_next_driver_lookup_shader (job->driver, shader, &error);
  n_children = gsk_gl_shader_node_get_n_children (node);

  if G_UNLIKELY (program == NULL)
    {
      if (g_object_get_data (G_OBJECT (shader), "gsk-did-warn") == NULL)
        {
          g_object_set_data (G_OBJECT (shader), "gsk-did-warn", GUINT_TO_POINTER (1));
          g_warning ("Failed to compile gl shader: %s", error->message);
        }
      gsk_gl_render_job_visit_gl_shader_node_fallback (job, node);
      g_clear_error (&error);
    }
  else
    {
      GskGLRenderOffscreen offscreens[4] = {0};
      const GskGLUniform *uniforms;
      const guint8 *base;
      GBytes *args;
      int n_uniforms;

      g_assert (n_children < G_N_ELEMENTS (offscreens));

      for (guint i = 0; i < n_children; i++)
        {
          GskRenderNode *child = gsk_gl_shader_node_get_child (node, i);

          offscreens[i].bounds = &node->bounds;
          offscreens[i].force_offscreen = TRUE;
          offscreens[i].reset_clip = TRUE;

          if (!gsk_gl_render_job_visit_node_with_offscreen (job, child, &offscreens[i]))
            return;
        }

      args = gsk_gl_shader_node_get_args (node);
      base = g_bytes_get_data (args, NULL);
      uniforms = gsk_gl_shader_get_uniforms (shader, &n_uniforms);

      gsk_gl_program_begin_draw (program,
                                 &job->viewport,
                                 &job->projection,
                                 gsk_gl_render_job_get_modelview_matrix (job),
                                 gsk_gl_render_job_get_clip (job),
                                 job->alpha);
      for (guint i = 0; i < n_children; i++)
        gsk_gl_program_set_uniform_texture (program,
                                            UNIFORM_CUSTOM_TEXTURE1 + i,
                                            GL_TEXTURE_2D,
                                            GL_TEXTURE0 + i,
                                            offscreens[i].texture_id);
      gsk_gl_program_set_uniform2f (program,
                                    UNIFORM_CUSTOM_SIZE,
                                    node->bounds.size.width,
                                    node->bounds.size.height);
      for (guint i = 0; i < n_uniforms; i++)
        {
          const GskGLUniform *u = &uniforms[i];
          const guint8 *data = base + u->offset;

          switch (u->type)
            {
            default:
            case GSK_GL_UNIFORM_TYPE_NONE:
              break;
            case GSK_GL_UNIFORM_TYPE_FLOAT:
              gsk_gl_command_queue_set_uniform1fv (job->command_queue,
                                                   program->id,
                                                   program->args_locations[i],
                                                   1,
                                                   (const float *)data);
              break;
            case GSK_GL_UNIFORM_TYPE_INT:
              gsk_gl_command_queue_set_uniform1i (job->command_queue,
                                                  program->id,
                                                  program->args_locations[i],
                                                  *(const gint32 *)data);
              break;
            case GSK_GL_UNIFORM_TYPE_UINT:
            case GSK_GL_UNIFORM_TYPE_BOOL:
              gsk_gl_command_queue_set_uniform1ui (job->command_queue,
                                                   program->id,
                                                   program->args_locations[i],
                                                   *(const guint32 *)data);
              break;
            case GSK_GL_UNIFORM_TYPE_VEC2:
              gsk_gl_command_queue_set_uniform2fv (job->command_queue,
                                                   program->id,
                                                   program->args_locations[i],
                                                   1,
                                                   (const float *)data);
              break;
            case GSK_GL_UNIFORM_TYPE_VEC3:
              gsk_gl_command_queue_set_uniform3fv (job->command_queue,
                                                   program->id,
                                                   program->args_locations[i],
                                                   1,
                                                   (const float *)data);
              break;
            case GSK_GL_UNIFORM_TYPE_VEC4:
              gsk_gl_command_queue_set_uniform4fv (job->command_queue,
                                                   program->id,
                                                   program->args_locations[i],
                                                   1,
                                                   (const float *)data);
              break;
            }
        }
      gsk_gl_render_job_draw_offscreen_rect (job, &node->bounds);
      gsk_gl_program_end_draw (program);
    }
}

static void
gsk_gl_render_job_upload_texture (GskGLRenderJob       *job,
                                  GdkTexture           *texture,
                                  GskGLRenderOffscreen *offscreen)
{
  if (gsk_gl_texture_library_can_cache (GSK_GL_TEXTURE_LIBRARY (job->driver->icons),
                                        texture->width,
                                        texture->height) &&
      !GDK_IS_GL_TEXTURE (texture))
    {
      const GskGLIconData *icon_data;

      gsk_gl_icon_library_lookup_or_add (job->driver->icons, texture, &icon_data);
      offscreen->texture_id = GSK_GL_TEXTURE_ATLAS_ENTRY_TEXTURE (icon_data);
      offscreen->area = icon_data->entry.area;
    }
  else
    {
      offscreen->texture_id = gsk_next_driver_load_texture (job->driver, texture, GL_LINEAR, GL_LINEAR);
      init_full_texture_region (&offscreen->area);
    }
}

static void
gsk_gl_render_job_visit_texture_node (GskGLRenderJob *job,
                                      GskRenderNode  *node)
{
  GdkTexture *texture = gsk_texture_node_get_texture (node);
  int max_texture_size = job->command_queue->max_texture_size;

  if G_LIKELY (texture->width <= max_texture_size &&
               texture->height <= max_texture_size)
    {
      GskGLRenderOffscreen offscreen = {0};

      gsk_gl_render_job_upload_texture (job, texture, &offscreen);

      gsk_gl_program_begin_draw (job->driver->blit,
                                 &job->viewport,
                                 &job->projection,
                                 gsk_gl_render_job_get_modelview_matrix (job),
                                 gsk_gl_render_job_get_clip (job),
                                 job->alpha);
      gsk_gl_program_set_uniform_texture (job->driver->blit,
                                          UNIFORM_SHARED_SOURCE,
                                          GL_TEXTURE_2D,
                                          GL_TEXTURE0,
                                          offscreen.texture_id);
      gsk_gl_render_job_load_vertices_from_offscreen (job, &node->bounds, &offscreen);
      gsk_gl_program_end_draw (job->driver->blit);
    }
  else
    {
#if 0
      g_warning ("TODO: Slice too large texture\n");
#endif
    }
}

static void
gsk_gl_render_job_visit_repeat_node (GskGLRenderJob *job,
                                     GskRenderNode  *node)
{
  GskRenderNode *child = gsk_repeat_node_get_child (node);
  const graphene_rect_t *child_bounds = gsk_repeat_node_get_child_bounds (node);
  GskGLRenderOffscreen offscreen = {0};

  if (node_is_invisible (child))
    return;

  if (!graphene_rect_equal (child_bounds, &child->bounds))
    {
      /* TODO: implement these repeat nodes. */
      gsk_gl_render_job_visit_as_fallback (job, node);
      return;
    }

  /* If the size of the repeat node is smaller than the size of the
   * child node, we don't repeat at all and can just draw that part
   * of the child texture... */
  if (graphene_rect_contains_rect (child_bounds, &node->bounds))
    {
      gsk_gl_render_job_visit_clipped_child (job, child, child_bounds);
      return;
    }

  offscreen.bounds = &child->bounds;
  offscreen.reset_clip = TRUE;

  if (!gsk_gl_render_job_visit_node_with_offscreen (job, child, &offscreen))
    g_assert_not_reached ();

  gsk_gl_program_begin_draw (job->driver->repeat,
                             &job->viewport,
                             &job->projection,
                             gsk_gl_render_job_get_modelview_matrix (job),
                             gsk_gl_render_job_get_clip (job),
                             job->alpha);
  gsk_gl_program_set_uniform_texture (job->driver->repeat,
                                      UNIFORM_SHARED_SOURCE,
                                      GL_TEXTURE_2D,
                                      GL_TEXTURE0,
                                      offscreen.texture_id);
  gsk_gl_program_set_uniform4fv (job->driver->repeat,
                                 UNIFORM_REPEAT_CHILD_BOUNDS,
                                 1,
                                 (const float []) {
                                   node->bounds.origin.x - child_bounds->origin.x,
                                   node->bounds.origin.y - child_bounds->origin.y,
                                   node->bounds.size.width / child_bounds->size.width,
                                   node->bounds.size.height / child_bounds->size.height,
                                 });
  gsk_gl_program_set_uniform4fv (job->driver->repeat,
                                 UNIFORM_REPEAT_TEXTURE_RECT,
                                 1,
                                 (const float []) {
                                   X1 (&offscreen.area),
                                   offscreen.was_offscreen ? Y2 (&offscreen.area) : Y1 (&offscreen.area),
                                   X2 (&offscreen.area),
                                   offscreen.was_offscreen ? Y1 (&offscreen.area) : Y2 (&offscreen.area),
                                 });
  gsk_gl_render_job_load_vertices_from_offscreen (job, &node->bounds, &offscreen);
  gsk_gl_program_end_draw (job->driver->repeat);
}

static void
gsk_gl_render_job_visit_node (GskGLRenderJob *job,
                              GskRenderNode  *node)
{
  g_assert (job != NULL);
  g_assert (node != NULL);
  g_assert (GSK_IS_NEXT_DRIVER (job->driver));
  g_assert (GSK_IS_GL_COMMAND_QUEUE (job->command_queue));

  if (node_is_invisible (node) ||
      !gsk_gl_render_job_node_overlaps_clip (job, node))
    return;

  switch (gsk_render_node_get_node_type (node))
    {
    case GSK_BLEND_NODE:
      gsk_gl_render_job_visit_blend_node (job, node);
    break;

    case GSK_BLUR_NODE:
      if (gsk_blur_node_get_radius (node) > 0)
        gsk_gl_render_job_visit_blur_node (job, node);
      else
        gsk_gl_render_job_visit_node (job, gsk_blur_node_get_child (node));
    break;

    case GSK_BORDER_NODE:
      if (gsk_border_node_get_uniform (node))
        gsk_gl_render_job_visit_uniform_border_node (job, node);
      else
        gsk_gl_render_job_visit_border_node (job, node);
    break;

    case GSK_CLIP_NODE:
      gsk_gl_render_job_visit_clip_node (job, node);
    break;

    case GSK_COLOR_NODE:
      gsk_gl_render_job_visit_color_node (job, node);
    break;

    case GSK_COLOR_MATRIX_NODE:
      gsk_gl_render_job_visit_color_matrix_node (job, node);
    break;

    case GSK_CONIC_GRADIENT_NODE:
      if (gsk_conic_gradient_node_get_n_color_stops (node) < MAX_GRADIENT_STOPS)
        gsk_gl_render_job_visit_conic_gradient_node (job, node);
      else
        gsk_gl_render_job_visit_as_fallback (job, node);
    break;

    case GSK_CONTAINER_NODE:
      {
        guint n_children = gsk_container_node_get_n_children (node);

        for (guint i = 0; i < n_children; i++)
          {
            GskRenderNode *child = gsk_container_node_get_child (node, i);
            gsk_gl_render_job_visit_node (job, child);
          }
      }
    break;

    case GSK_CROSS_FADE_NODE:
      {
        GskRenderNode *start_node = gsk_cross_fade_node_get_start_child (node);
        GskRenderNode *end_node = gsk_cross_fade_node_get_end_child (node);
        float progress = gsk_cross_fade_node_get_progress (node);

        if (progress <= 0)
          gsk_gl_render_job_visit_node (job, gsk_cross_fade_node_get_start_child (node));
        else if (progress >= 1 || equal_texture_nodes (start_node, end_node))
          gsk_gl_render_job_visit_node (job, gsk_cross_fade_node_get_end_child (node));
        else
          gsk_gl_render_job_visit_cross_fade_node (job, node);
      }
    break;

    case GSK_DEBUG_NODE:
      {
        const char *message = gsk_debug_node_get_message (node);

        if (message != NULL)
          gsk_gl_command_queue_push_debug_group (job->command_queue, message);
        gsk_gl_render_job_visit_node (job, gsk_debug_node_get_child (node));
        if (message != NULL)
          gsk_gl_command_queue_pop_debug_group (job->command_queue);
      }
    break;

    case GSK_GL_SHADER_NODE:
      gsk_gl_render_job_visit_gl_shader_node (job, node);
    break;

    case GSK_INSET_SHADOW_NODE:
      if (gsk_inset_shadow_node_get_blur_radius (node) > 0)
        gsk_gl_render_job_visit_blurred_inset_shadow_node (job, node);
      else
        gsk_gl_render_job_visit_unblurred_inset_shadow_node (job, node);
    break;

    case GSK_LINEAR_GRADIENT_NODE:
      if (gsk_linear_gradient_node_get_n_color_stops (node) < MAX_GRADIENT_STOPS)
        gsk_gl_render_job_visit_linear_gradient_node (job, node);
      else
        gsk_gl_render_job_visit_as_fallback (job, node);
    break;

    case GSK_OPACITY_NODE:
      gsk_gl_render_job_visit_opacity_node (job, node);
    break;

    case GSK_OUTSET_SHADOW_NODE:
      if (gsk_outset_shadow_node_get_blur_radius (node) > 0)
        gsk_gl_render_job_visit_blurred_outset_shadow_node (job, node);
      else
        gsk_gl_render_job_visit_unblurred_outset_shadow_node (job, node);
    break;

    case GSK_RADIAL_GRADIENT_NODE:
      gsk_gl_render_job_visit_radial_gradient_node (job, node);
    break;

    case GSK_REPEAT_NODE:
      gsk_gl_render_job_visit_repeat_node (job, node);
    break;

    case GSK_ROUNDED_CLIP_NODE:
      gsk_gl_render_job_visit_rounded_clip_node (job, node);
    break;

    case GSK_SHADOW_NODE:
      gsk_gl_render_job_visit_shadow_node (job, node);
    break;

    case GSK_TEXT_NODE:
      gsk_gl_render_job_visit_text_node (job,
                                         node,
                                         gsk_text_node_get_color (node),
                                         FALSE);
    break;

    case GSK_TEXTURE_NODE:
      gsk_gl_render_job_visit_texture_node (job, node);
    break;

    case GSK_TRANSFORM_NODE:
      gsk_gl_render_job_visit_transform_node (job, node);
    break;

    case GSK_REPEATING_LINEAR_GRADIENT_NODE:
    case GSK_REPEATING_RADIAL_GRADIENT_NODE:
    case GSK_CAIRO_NODE:
      gsk_gl_render_job_visit_as_fallback (job, node);
    break;

    case GSK_NOT_A_RENDER_NODE:
    default:
      g_assert_not_reached ();
    break;
    }
}

static gboolean
gsk_gl_render_job_visit_node_with_offscreen (GskGLRenderJob       *job,
                                             GskRenderNode        *node,
                                             GskGLRenderOffscreen *offscreen)
{
  GskTextureKey key;
  guint cached_id;
  int filter;

  g_assert (job != NULL);
  g_assert (node != NULL);
  g_assert (offscreen != NULL);
  g_assert (offscreen->texture_id == 0);
  g_assert (offscreen->bounds != NULL);

  if (node_is_invisible (node))
    {
      /* Just to be safe. */
      offscreen->texture_id = 0;
      init_full_texture_region (&offscreen->area);
      offscreen->was_offscreen = FALSE;
      return FALSE;
    }

  if (gsk_render_node_get_node_type (node) == GSK_TEXTURE_NODE &&
      offscreen->force_offscreen == FALSE)
    {
      GdkTexture *texture = gsk_texture_node_get_texture (node);
      gsk_gl_render_job_upload_texture (job, texture, offscreen);
      g_assert (offscreen->was_offscreen == FALSE);
      return TRUE;
    }

  filter = offscreen->linear_filter ? GL_LINEAR : GL_NEAREST;

  /* Check if we've already cached the drawn texture. */
  key.pointer = node;
  key.pointer_is_child = TRUE; /* Don't conflict with the child using the cache too */
  key.parent_rect = *offscreen->bounds;
  key.scale_x = job->scale_x;
  key.scale_y = job->scale_y;
  key.filter = filter;

  cached_id = gsk_next_driver_lookup_texture (job->driver, &key);

  if (cached_id != 0)
    {
      offscreen->texture_id = cached_id;
      init_full_texture_region (&offscreen->area);
      /* We didn't render it offscreen, but hand out an offscreen texture id */
      offscreen->was_offscreen = TRUE;
      return TRUE;
    }

  float width = offscreen->bounds->size.width;
  float height = offscreen->bounds->size.height;
  float scale_x = job->scale_x;
  float scale_y = job->scale_y;

  g_assert (job->command_queue->max_texture_size > 0);

  /* Tweak the scale factor so that the required texture doesn't
   * exceed the max texture limit. This will render with a lower
   * resolution, but this is better than clipping.
   */
  {
    int max_texture_size = job->command_queue->max_texture_size;

    width = ceilf (width * scale_x);
    if (width > max_texture_size)
      {
        scale_x *= (float)max_texture_size / width;
        width = max_texture_size;
      }

    height = ceilf (height * scale_y);
    if (height > max_texture_size)
      {
        scale_y *= (float)max_texture_size / height;
        height = max_texture_size;
      }
  }

  GskGLRenderTarget *render_target;
  GskGLRenderState state;

  gsk_gl_render_state_save (&state, job);

  if (!gsk_next_driver_create_render_target (job->driver,
                                             width, height,
                                             filter, filter,
                                             &render_target))
    g_assert_not_reached ();

  if (gdk_gl_context_has_debug (job->command_queue->context))
    {
      gdk_gl_context_label_object_printf (job->command_queue->context,
                                          GL_TEXTURE,
                                          render_target->texture_id,
                                          "Offscreen<%s> %d",
                                          g_type_name_from_instance ((GTypeInstance *) node),
                                          render_target->texture_id);
      gdk_gl_context_label_object_printf (job->command_queue->context,
                                          GL_FRAMEBUFFER,
                                          render_target->framebuffer_id,
                                          "Offscreen<%s> FB %d",
                                          g_type_name_from_instance ((GTypeInstance *) node),
                                          render_target->framebuffer_id);
    }

  job->viewport.origin.x = offscreen->bounds->origin.x * scale_x;
  job->viewport.origin.y = offscreen->bounds->origin.y * scale_y;
  job->viewport.size.width = width;
  job->viewport.size.height = height;

  gsk_gl_command_queue_bind_framebuffer (job->command_queue, render_target->framebuffer_id);
  gsk_gl_command_queue_clear (job->command_queue, 0, &job->viewport);

  init_projection_matrix (&job->projection, &job->viewport);
  gsk_gl_render_job_set_modelview (job, gsk_transform_scale (NULL, scale_x, scale_y));

  if (offscreen->reset_clip)
    gsk_gl_render_job_push_clip (job, &(GskRoundedRect) { .bounds = job->viewport });

  job->offset_x = 0;
  job->offset_y = 0;

  gsk_gl_render_job_visit_node (job, node);

  if (offscreen->reset_clip)
    gsk_gl_render_job_pop_clip (job);

  gsk_gl_render_job_pop_modelview (job);

  gsk_gl_render_state_restore (&state, job);

  offscreen->was_offscreen = TRUE;
  offscreen->texture_id = gsk_next_driver_release_render_target (job->driver,
                                                                 render_target,
                                                                 FALSE);

  init_full_texture_region (&offscreen->area);

  if (!offscreen->do_not_cache)
    gsk_next_driver_cache_texture (job->driver, &key, offscreen->texture_id);

  return TRUE;
}

void
gsk_gl_render_job_render_flipped (GskGLRenderJob *job,
                                  GskRenderNode  *root)
{
  graphene_matrix_t proj;
  GdkGLContext *context;
  guint framebuffer_id;
  guint texture_id;

  g_return_if_fail (job != NULL);
  g_return_if_fail (root != NULL);
  g_return_if_fail (GSK_IS_NEXT_DRIVER (job->driver));

  context = gsk_next_driver_get_context (job->driver);

  gdk_gl_context_make_current (context);

  graphene_matrix_init_ortho (&proj,
                              job->viewport.origin.x,
                              job->viewport.origin.x + job->viewport.size.width,
                              job->viewport.origin.y,
                              job->viewport.origin.y + job->viewport.size.height,
                              ORTHO_NEAR_PLANE,
                              ORTHO_FAR_PLANE);
  graphene_matrix_scale (&proj, 1, -1, 1);

  if (!gsk_gl_command_queue_create_render_target (job->command_queue,
                                                  job->viewport.size.width,
                                                  job->viewport.size.height,
                                                  GL_NEAREST, GL_NEAREST,
                                                  &framebuffer_id, &texture_id))
    return;

  gsk_next_driver_begin_frame (job->driver);

  /* Setup drawing to our offscreen texture/framebuffer which is flipped */
  gsk_gl_command_queue_bind_framebuffer (job->command_queue, framebuffer_id);
  gsk_gl_command_queue_clear (job->command_queue, 0, &job->viewport);

  /* Visit all nodes creating batches */
  gdk_gl_context_push_debug_group (context, "Building command queue");
  gsk_gl_render_job_visit_node (job, root);
  gdk_gl_context_pop_debug_group (context);

  /* Now draw to our real destination, but flipped */
  gsk_gl_command_queue_bind_framebuffer (job->command_queue, job->framebuffer);
  gsk_gl_command_queue_clear (job->command_queue, 0, &job->viewport);
  gsk_gl_program_begin_draw (job->driver->blit,
                             &job->viewport,
                             &proj,
                             gsk_gl_render_job_get_modelview_matrix (job),
                             gsk_gl_render_job_get_clip (job),
                             1.0);
  gsk_gl_program_set_uniform_texture (job->driver->blit,
                                      UNIFORM_SHARED_SOURCE,
                                      GL_TEXTURE_2D,
                                      GL_TEXTURE0,
                                      texture_id);
  gsk_gl_render_job_draw_rect (job, &job->viewport);
  gsk_gl_program_end_draw (job->driver->blit);

  gdk_gl_context_push_debug_group (context, "Executing command queue");
  gsk_gl_command_queue_execute (job->command_queue);
  gdk_gl_context_pop_debug_group (context);

  gsk_next_driver_end_frame (job->driver);

  gdk_gl_context_make_current (context);

  glDeleteFramebuffers (1, &framebuffer_id);
  glDeleteTextures (1, &texture_id);
}

void
gsk_gl_render_job_render (GskGLRenderJob *job,
                          GskRenderNode  *root)
{
  GdkGLContext *context;

  g_return_if_fail (job != NULL);
  g_return_if_fail (root != NULL);
  g_return_if_fail (GSK_IS_NEXT_DRIVER (job->driver));

  context = gsk_next_driver_get_context (job->driver);

  gsk_next_driver_begin_frame (job->driver);

  if (job->framebuffer != 0)
    gsk_gl_command_queue_bind_framebuffer (job->command_queue, job->framebuffer);

  gsk_gl_command_queue_clear (job->command_queue, 0, &job->viewport);

  gdk_gl_context_push_debug_group (context, "Building command queue");
  gsk_gl_render_job_visit_node (job, root);
  gdk_gl_context_pop_debug_group (context);

#if 0
  /* At this point the atlases have uploaded content while we processed
   * nodes but have not necessarily been used by the commands in the queue.
   */
  gsk_next_driver_save_atlases_to_png (job->driver, NULL);
#endif

  gdk_gl_context_push_debug_group (context, "Executing command queue");
  gsk_gl_command_queue_execute (job->command_queue);
  gdk_gl_context_pop_debug_group (context);

  gsk_next_driver_end_frame (job->driver);
}

void
gsk_gl_render_job_set_debug_fallback (GskGLRenderJob *job,
                                      gboolean        debug_fallback)
{
  g_return_if_fail (job != NULL);

  job->debug_fallback = !!debug_fallback;
}

GskGLRenderJob *
gsk_gl_render_job_new (GskNextDriver         *driver,
                       const graphene_rect_t *viewport,
                       float                  scale_factor,
                       const cairo_region_t  *region,
                       guint                  framebuffer)
{
  const graphene_rect_t *clip_rect = viewport;
  graphene_rect_t transformed_extents;
  GskGLRenderJob *job;

  g_return_val_if_fail (GSK_IS_NEXT_DRIVER (driver), NULL);
  g_return_val_if_fail (viewport != NULL, NULL);
  g_return_val_if_fail (scale_factor > 0, NULL);

  job = g_slice_new0 (GskGLRenderJob);
  job->driver = g_object_ref (driver);
  job->command_queue = driver->command_queue;
  job->clip = g_array_new (FALSE, FALSE, sizeof (GskGLRenderClip));
  job->modelview = g_array_new (FALSE, FALSE, sizeof (GskGLRenderModelview));
  job->framebuffer = framebuffer;
  job->offset_x = 0;
  job->offset_y = 0;
  job->scale_x = scale_factor;
  job->scale_y = scale_factor;
  job->viewport = *viewport;
  job->region = region ? cairo_region_copy (region) : NULL;
  job->alpha = 1.0;

  init_projection_matrix (&job->projection, viewport);
  gsk_gl_render_job_set_modelview (job, gsk_transform_scale (NULL, scale_factor, scale_factor));

  /* Setup our initial clip. If region is NULL then we are drawing the
   * whole viewport. Otherwise, we need to convert the region to a
   * bounding box and clip based on that.
   */

  if (region != NULL)
    {
      cairo_rectangle_int_t extents;

      cairo_region_get_extents (region, &extents);
      gsk_gl_render_job_transform_bounds (job,
                                          &GRAPHENE_RECT_INIT (extents.x,
                                                               extents.y,
                                                               extents.width,
                                                               extents.height),
                                          &transformed_extents);
      clip_rect = &transformed_extents;
    }

  gsk_gl_render_job_push_clip (job,
                               &GSK_ROUNDED_RECT_INIT (clip_rect->origin.x,
                                                       clip_rect->origin.y,
                                                       clip_rect->size.width,
                                                       clip_rect->size.height));

  return job;
}

void
gsk_gl_render_job_free (GskGLRenderJob *job)
{
  while (job->modelview->len > 0)
    {
      GskGLRenderModelview *modelview = gsk_gl_render_job_get_modelview (job);
      g_clear_pointer (&modelview->transform, gsk_transform_unref);
      job->modelview->len--;
    }

  g_clear_object (&job->driver);
  g_clear_pointer (&job->region, cairo_region_destroy);
  g_clear_pointer (&job->modelview, g_array_unref);
  g_clear_pointer (&job->clip, g_array_unref);
  g_slice_free (GskGLRenderJob, job);
}
