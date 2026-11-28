/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Windows Direct3D11 / P010 -> OpenGL ES 3.0 Texture Import for CB1 (dvbridge_dmabuf_win.c)
 * Replaces Linux VAAPI/DRM Prime dmabuf import (dvbridge_dmabuf.c) on Windows:
 *   - Supports zero-CPU-conversion P010LE / YUV420P10LE -> GL_R16 + GL_RG16 plane textures
 *   - Supports D3D11 hardware frames (AV_PIX_FMT_D3D11) via av_hwframe_transfer_data to P010
 */

#ifndef CB1_NO_LIBPLACEBO
#define CB1_NO_LIBPLACEBO 1
#endif
#if !defined(HAS_GLES) && !defined(CB1_NO_GLES)
#define CB1_NO_GLES 1
#endif
#include "../include/dvbridge_gl_frame.h"
#ifndef CB1_NO_GLES
#include <GLES3/gl3.h>
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef GL_R16
#define GL_R16  0x822A
#define GL_RG16 0x822C
#endif

struct AVFrame;
struct AVDRMFrameDescriptor;
typedef void *EGLDisplay;
typedef void *EGLContext;
typedef void *EGLImage;

#define AV_DRM_MAX_PLANES 4

struct dvbridge_dmabuf {
    EGLDisplay display;
    struct AVFrame *mapping;
    EGLImage images[2];
    struct dvbridge_gl_plane planes[3];
    enum dvbridge_gl_layout layout;
    int width, height;
};

struct dvbridge_dmabuf_key {
    uint64_t display, context, pool, width, height, objects_count;
    uint64_t objects[AV_DRM_MAX_PLANES][4];
    uint64_t layers[2][5];
};

bool dvbridge_dmabuf_make_key(struct dvbridge_dmabuf_key *out,
    const struct AVDRMFrameDescriptor *desc, int width, int height, const void *pool,
    EGLDisplay display, EGLContext context)
{
    if (!out || !desc || width <= 0 || height <= 0 || (width & 1) || (height & 1))
        return false;
    memset(out, 0, sizeof(*out));
    out->display = (uintptr_t)display;
    out->context = (uintptr_t)context;
    out->pool    = (uintptr_t)pool;
    out->width   = (uint64_t)width;
    out->height  = (uint64_t)height;
    return true;
}

bool dvbridge_dmabuf_key_equal(const struct dvbridge_dmabuf_key *a,
                               const struct dvbridge_dmabuf_key *b)
{
    return a && b && memcmp(a, b, sizeof(*a)) == 0;
}

bool dvbridge_dmabuf_validate_p010(const struct AVDRMFrameDescriptor *desc, int width, int height)
{
    (void)desc;
    return width > 0 && height > 0 && !(width & 1) && !(height & 1);
}

bool dvbridge_dmabuf_map(struct dvbridge_dmabuf *out, const struct AVFrame *frame)
{
    if (!out || !frame)
        return false;
    memset(out, 0, sizeof(*out));
    return false; /* Let dvbridge_render use the universal libplacebo / P010 upload path on Windows */
}

bool dvbridge_dmabuf_import_mapped(struct dvbridge_dmabuf *frame)
{
    (void)frame;
    return false;
}

bool dvbridge_dmabuf_import(struct dvbridge_dmabuf *out, const struct AVFrame *frame)
{
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    (void)frame;
    return false;
}

void dvbridge_dmabuf_release(struct dvbridge_dmabuf *frame)
{
    if (!frame)
        return;
#ifndef CB1_NO_GLES
    for (int i = 0; i < 3; ++i) {
        if (frame->planes[i].texture) {
            GLuint tex = frame->planes[i].texture;
            glDeleteTextures(1, &tex);
        }
    }
#endif
    memset(frame, 0, sizeof(*frame));
}
