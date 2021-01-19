#ifndef __GSK_GL_TEXTURE_ATLAS_H__
#define __GSK_GL_TEXTURE_ATLAS_H__

#include "../gl/stb_rect_pack.h"
#include "gskgldriverprivate.h"

G_BEGIN_DECLS

typedef GPtrArray GskGLTextureAtlases;


GskGLTextureAtlases *gsk_gl_texture_atlases_new            (void);
void                 gsk_gl_texture_atlases_begin_frame    (GskGLTextureAtlases      *atlases,
                                                            GPtrArray                *removed);
gboolean             gsk_gl_texture_atlases_pack           (GskGLTextureAtlases      *atlases,
                                                            int                       width,
                                                            int                       height,
                                                            GskGLTextureAtlas       **atlas_out,
                                                            int                      *out_x,
                                                            int                      *out_y);
void                 gsk_gl_texture_atlas_init             (GskGLTextureAtlas        *self,
                                                            int                       width,
                                                            int                       height);
void                 gsk_gl_texture_atlas_free             (GskGLTextureAtlas        *self);
void                 gsk_gl_texture_atlas_realize          (GskGLTextureAtlas        *self);
gboolean             gsk_gl_texture_atlas_pack             (GskGLTextureAtlas        *self,
                                                            int                       width,
                                                            int                       height,
                                                            int                      *out_x,
                                                            int                      *out_y);
double               gsk_gl_texture_atlas_get_unused_ratio (const GskGLTextureAtlas  *self);


G_END_DECLS

#endif /* __GSK_GL_TEXTURE_ATLAS_PRIVATE_H__ */
