/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Windows Profile 7 FEL Video Post-Processing (VPP) Shim for CB1
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#ifndef CB1_NO_LIBPLACEBO
#define CB1_NO_LIBPLACEBO 1
#endif
#include "../include/dvbridge_gl_frame.h"

struct AVBufferRef;
struct AVFrame;

struct dvbridge_fel_vpp {
    int width, height;
};

struct dvbridge_fel_vpp_image {
    struct dvbridge_gl_plane plane;
    int width, height;
};

enum dvbridge_fel_vpp_result {
    DVBRIDGE_VPP_OK,
    DVBRIDGE_VPP_BUSY,
    DVBRIDGE_VPP_FAILED,
    DVBRIDGE_VPP_PENDING
};

struct dvbridge_fel_vpp *dvbridge_fel_vpp_create(struct AVBufferRef *device, int width, int height)
{
    (void)device;
    if (width <= 0 || height <= 0)
        return NULL;
    struct dvbridge_fel_vpp *vpp = calloc(1, sizeof(*vpp));
    if (vpp) {
        vpp->width  = width;
        vpp->height = height;
    }
    return vpp;
}

enum dvbridge_fel_vpp_result dvbridge_fel_vpp_acquire(struct dvbridge_fel_vpp *vpp,
    const struct AVFrame *source, const struct dvbridge_fel_vpp_image **image)
{
    (void)vpp;
    (void)source;
    if (image)
        *image = NULL;
    return DVBRIDGE_VPP_FAILED;
}

enum dvbridge_fel_vpp_result dvbridge_fel_vpp_acquire_chroma(struct dvbridge_fel_vpp *vpp,
    const struct AVFrame *source, int chroma,
    const struct dvbridge_fel_vpp_image **image)
{
    (void)vpp;
    (void)source;
    (void)chroma;
    if (image)
        *image = NULL;
    return DVBRIDGE_VPP_FAILED;
}

enum dvbridge_fel_vpp_result dvbridge_fel_vpp_prefetch_chroma(struct dvbridge_fel_vpp *vpp,
    const struct AVFrame *source, int chroma,
    const struct dvbridge_fel_vpp_image **image)
{
    (void)vpp;
    (void)source;
    (void)chroma;
    if (image)
        *image = NULL;
    return DVBRIDGE_VPP_FAILED;
}

enum dvbridge_fel_vpp_result dvbridge_fel_vpp_finish(struct dvbridge_fel_vpp *vpp,
    const struct dvbridge_fel_vpp_image *image)
{
    (void)vpp;
    (void)image;
    return DVBRIDGE_VPP_FAILED;
}

void dvbridge_fel_vpp_release(struct dvbridge_fel_vpp *vpp, const struct dvbridge_fel_vpp_image *image)
{
    (void)vpp;
    (void)image;
}

bool dvbridge_fel_vpp_destroy(struct dvbridge_fel_vpp **vpp)
{
    if (!vpp || !*vpp)
        return true;
    free(*vpp);
    *vpp = NULL;
    return true;
}
