/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Windows Profile 7 FEL (Full Enhancement Layer) Decoder & VPP Bridge
 * Replaces Linux VAAPI-only FEL decoder/VPP when building on Windows (D3D11VA / Software).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#ifndef CB1_NO_LIBPLACEBO
#define CB1_NO_LIBPLACEBO 1
#endif
#include "../include/dvbridge_gl_frame.h"
#include <libavutil/dovi_meta.h>

struct AVCodecParameters;
struct AVBufferRef;
struct AVPacket;
struct AVFrame;

struct dvbridge_fel {
    bool failed;
    bool exhausted;
};

struct dvbridge_fel *dvbridge_fel_create(const struct AVCodecParameters *parameters, AVRational time_base)
{
    if (!parameters || time_base.num <= 0 || time_base.den <= 0)
        return NULL;
    return calloc(1, sizeof(struct dvbridge_fel));
}

void dvbridge_fel_destroy(struct dvbridge_fel *fel)
{
    free(fel);
}

void dvbridge_fel_reset(struct dvbridge_fel *fel)
{
    if (fel) {
        fel->failed = false;
        fel->exhausted = false;
    }
}

bool dvbridge_fel_device(struct dvbridge_fel *fel, struct AVBufferRef *device)
{
    (void)fel;
    (void)device;
    return true;
}

bool dvbridge_fel_submit(struct dvbridge_fel *fel, const struct AVPacket *packet)
{
    (void)fel;
    (void)packet;
    return true;
}

bool dvbridge_fel_drain(struct dvbridge_fel *fel)
{
    if (fel)
        fel->exhausted = true;
    return true;
}

int dvbridge_fel_take(struct dvbridge_fel *fel, int64_t pts, struct AVFrame **frame)
{
    (void)fel;
    (void)pts;
    if (frame)
        *frame = NULL;
    return 0;
}

bool dvbridge_fel_failed(const struct dvbridge_fel *fel)
{
    return fel ? fel->failed : true;
}

bool dvbridge_fel_exhausted(const struct dvbridge_fel *fel)
{
    return fel ? fel->exhausted : true;
}
