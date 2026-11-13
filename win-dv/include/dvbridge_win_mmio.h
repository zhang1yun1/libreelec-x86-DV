/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Windows Native HDMI Dolby Vision Hardware Bridge (Intel Display Gen9-14 & AMD DCN 1.0-4.01)
 * Ported from CroqueMr/libreelec-x86-DV (intel_hdmi.c, amdgpu_dm_dv.c, drm_dvbridge.h, cb1).
 */
#ifndef DVBRIDGE_WIN_MMIO_H
#define DVBRIDGE_WIN_MMIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "drm_dvbridge.h"
#include "drm_dvbridge_edid.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 1. HDMI InfoFrame Constants & Hardware 9-Dword (36-Byte) Layout
 *
 * Both Intel HSW_TVIDEO_DIP_*_DATA and AMD DCN VPG_GENERIC_PACKET_DATA / AFMT_GENERIC
 * use the identical 9-dword (36-byte) register layout:
 *   DW0: [HB3/Reserved(0) : 24..31] | [HB2/Length : 16..23] | [HB1/Version : 8..15] | [HB0/Type : 0..7]
 *   DW1: [PB3 : 24..31]             | [PB2 : 16..23]        | [PB1 : 8..15]         | [PB0/Checksum : 0..7]
 *   DW2: [PB7 : 24..31]             | [PB6 : 16..23]        | [PB5 : 8..15]         | [PB4 : 0..7]
 *   ...
 *   DW8: [PB31 : 24..31]            | [PB30 : 16..23]       | [PB29 : 8..15]        | [PB28 : 0..7]
 * ============================================================================ */

#define HDMI_INFOFRAME_TYPE_VENDOR  0x81U
#define HDMI_INFOFRAME_TYPE_AVI     0x82U
#define HDMI_INFOFRAME_TYPE_SPD     0x83U
#define HDMI_INFOFRAME_TYPE_DRM     0x87U

#define HDMI_IEEE_OUI_DOLBY         0x00D046U
#define HDMI_IEEE_OUI_HDMI_FORUM    0xC45DD8U
#define HDMI_IEEE_OUI_HDMI14        0x000C03U

struct dvbridge_hw_infoframe {
    uint32_t dw[9]; /* 36 bytes: DW0 = Header (HB0..HB3), DW1..DW8 = Payload (PB0/Checksum..PB31) */
    uint8_t  hb0;
    uint8_t  hb1;
    uint8_t  hb2;
    uint8_t  checksum;
    uint8_t  sb[28]; /* PB0 (checksum) .. PB27 */
    bool     valid;
};

/*
 * Pack HDMI Dolby Vision Vendor-Specific InfoFrame (VSIF):
 * - When legacy_vsif == false (Modern Dolby Forum VSVDB OUI 0x00D046):
 *   HB0=0x81, HB1=0x01, HB2=0x1B (27 bytes), Checksum=0x4B (for TV-Led signal=1, LL=0),
 *   PB1=0x46, PB2=0xD0, PB3=0x00, PB4=0x02 (DV_Signal=1, Low_Latency=0), PB5..PB27=0x00.
 *   -> DW0 = 0x001B0181, DW1 = 0x00D0464B, DW2 = 0x00000002, DW3..DW8 = 0x00000000.
 * - When legacy_vsif == true (HDMI 1.4 OUI 0x000C03, 24 bytes):
 *   HB0=0x81, HB1=0x01, HB2=0x18 (24 bytes), PB1=0x03, PB2=0x0C, PB3=0x00,
 *   PB4=(hdmi_vic ? 0x20 : 0x00), PB5=hdmi_vic, PB6..PB24=0x00.
 */
static inline void dvbridge_win_pack_dolby_vsif(
    struct dvbridge_hw_infoframe *out,
    bool enable_dv,
    bool low_latency,
    bool legacy_vsif,
    uint8_t cta_vic)
{
    for (int i = 0; i < 9; ++i)
        out->dw[i] = 0;
    for (int i = 0; i < 28; ++i)
        out->sb[i] = 0;

    if (!enable_dv) {
        out->valid = false;
        return;
    }

    out->hb0 = HDMI_INFOFRAME_TYPE_VENDOR;
    out->hb1 = 0x01U;

    if (!legacy_vsif) {
        out->hb2 = 0x1BU; /* 27-byte Dolby VSIF */
        out->sb[1] = 0x46U; /* OUI 0x00D046 LSB */
        out->sb[2] = 0xD0U;
        out->sb[3] = 0x00U; /* OUI 0x00D046 MSB */
        out->sb[4] = (uint8_t)((enable_dv ? 0x02U : 0x00U) | (low_latency ? 0x01U : 0x00U));
    } else {
        uint8_t hdmi_vic = 0;
        if (cta_vic == 95) hdmi_vic = 1;      /* 3840x2160p30 */
        else if (cta_vic == 94) hdmi_vic = 2; /* 3840x2160p25 */
        else if (cta_vic == 93) hdmi_vic = 3; /* 3840x2160p24 */
        else if (cta_vic == 98) hdmi_vic = 4; /* 4096x2160p24 */

        out->hb2 = 0x18U; /* 24-byte Legacy Dolby HDMI 1.4 VSIF */
        out->sb[1] = 0x03U; /* OUI 0x000C03 LSB */
        out->sb[2] = 0x0CU;
        out->sb[3] = 0x00U; /* OUI 0x000C03 MSB */
        if (hdmi_vic > 0) {
            out->sb[4] = 0x20U; /* HDMI_Video_Format = 1 (Extended resolution format) */
            out->sb[5] = hdmi_vic;
        }
    }

    uint32_t sum = (uint32_t)out->hb0 + (uint32_t)out->hb1 + (uint32_t)out->hb2;
    for (uint8_t i = 1; i <= out->hb2; ++i)
        sum += out->sb[i];
    out->checksum = (uint8_t)((0x100U - (sum & 0xFFU)) & 0xFFU);
    out->sb[0] = out->checksum;

    out->dw[0] = (uint32_t)out->hb0 |
                 ((uint32_t)out->hb1 << 8) |
                 ((uint32_t)out->hb2 << 16);
    for (int w = 0; w < 7; ++w) {
        out->dw[w + 1] = (uint32_t)out->sb[w * 4 + 0] |
                         ((uint32_t)out->sb[w * 4 + 1] << 8) |
                         ((uint32_t)out->sb[w * 4 + 2] << 16) |
                         ((uint32_t)out->sb[w * 4 + 3] << 24);
    }
    out->valid = true;
}

/*
 * Pack CTA-861 AVI InfoFrame for TV-Led Dolby Vision RGB8 Tunneling:
 * - Colorspace = RGB (Y1:Y0 = 00)
 * - Active Format Aspect Ratio = Same as Picture (R3..R0 = 1000, A0 = 1 -> PB1 = 0x18)
 * - Colorimetry = None (C1:C0 = 00), Picture Aspect Ratio = 16:9 (M1:M0 = 10 -> PB2 = 0x20)
 * - RGB Quantization Range = Full (Q1:Q0 = 10 -> PB3 = 0x08)
 * - VIC = cta_vic (or 0 if legacy_vsif carries HDMI_VIC 1..4 for CTA VIC 95/94/93/98)
 */
static inline void dvbridge_win_pack_avi_infoframe(
    struct dvbridge_hw_infoframe *out,
    uint8_t cta_vic,
    bool legacy_vsif)
{
    for (int i = 0; i < 9; ++i)
        out->dw[i] = 0;
    for (int i = 0; i < 28; ++i)
        out->sb[i] = 0;

    uint8_t effective_vic = cta_vic;
    if (legacy_vsif && (cta_vic == 93 || cta_vic == 94 || cta_vic == 95 || cta_vic == 98))
        effective_vic = 0; /* Carried in HDMI 1.4 VSIF PB5 */

    out->hb0 = HDMI_INFOFRAME_TYPE_AVI; /* 0x82 */
    out->hb1 = 0x02U;                   /* Version 2 */
    out->hb2 = 0x0DU;                   /* Length 13 bytes */

    out->sb[1] = 0x10U | 0x02U; /* PB1: RGB (0<<5), Active Info Present (1<<4), Scan=Underscan (2) */
    out->sb[2] = 0x28U;         /* PB2: Colorimetry=None (0<<6), Aspect=16:9 (2<<4), ActiveAspect=Same (8) */
    out->sb[3] = 0x08U;         /* PB3: Quantization=Full Range (2<<2 = 0x08) */
    out->sb[4] = effective_vic; /* PB4: CTA-861 VIC */
    out->sb[5] = 0x00U;         /* PB5: YCC Quantization=Limited, Pixel Repetition=0 */

    uint32_t sum = (uint32_t)out->hb0 + (uint32_t)out->hb1 + (uint32_t)out->hb2;
    for (uint8_t i = 1; i <= out->hb2; ++i)
        sum += out->sb[i];
    out->checksum = (uint8_t)((0x100U - (sum & 0xFFU)) & 0xFFU);
    out->sb[0] = out->checksum;

    out->dw[0] = (uint32_t)out->hb0 |
                 ((uint32_t)out->hb1 << 8) |
                 ((uint32_t)out->hb2 << 16);
    for (int w = 0; w < 7; ++w) {
        out->dw[w + 1] = (uint32_t)out->sb[w * 4 + 0] |
                         ((uint32_t)out->sb[w * 4 + 1] << 8) |
                         ((uint32_t)out->sb[w * 4 + 2] << 16) |
                         ((uint32_t)out->sb[w * 4 + 3] << 24);
    }
    out->valid = true;
}

/* ============================================================================
 * 2. Intel Display (Gen9 Skylake .. Gen14 Meteor/Arrow Lake) MMIO Register Map
 *    BAR0 MMIO Base + Offset
 * ============================================================================ */

#define INTEL_NUM_PIPES                     4
#define INTEL_PIPE_STRIDE                   0x1000U

/* Per-Transcoder / Pipe Registers (Transcoder A = 0, B = 1, C = 2, D = 3) */
#define INTEL_REG_TRANS_CONF(t)             (0x70008U + (uint32_t)(t) * INTEL_PIPE_STRIDE)
#define   INTEL_TRANS_CONF_ENABLE           (1U << 31)

#define INTEL_REG_PIPE_MISC(p)              (0x70030U + (uint32_t)(p) * INTEL_PIPE_STRIDE)
#define   INTEL_PIPE_MISC_YUV420_ENABLE     (1U << 27)
#define   INTEL_PIPE_MISC_HDR_PRECISION     (1U << 23)
#define   INTEL_PIPE_MISC_COLORSPACE_YUV    (1U << 11)
#define   INTEL_PIPE_MISC_BPC_MASK          (7U << 5)
#define   INTEL_PIPE_MISC_BPC_8             (0U << 5)
#define   INTEL_PIPE_MISC_BPC_10            (1U << 5)
#define   INTEL_PIPE_MISC_BPC_6             (2U << 5)
#define   INTEL_PIPE_MISC_BPC_12_ADLP       (4U << 5)
#define   INTEL_PIPE_MISC_DITHER_ENABLE     (1U << 4)
#define   INTEL_PIPE_MISC_DITHER_TYPE_MASK  (3U << 2)

#define INTEL_REG_VIDEO_DIP_CTL(t)          (0x60200U + (uint32_t)(t) * INTEL_PIPE_STRIDE)
#define   INTEL_VIDEO_DIP_ENABLE_DRM_GLK    (1U << 28)
#define   INTEL_VIDEO_DIP_ENABLE_AS_ADL     (1U << 23)
#define   INTEL_VIDEO_DIP_ENABLE_VSC_HSW    (1U << 20)
#define   INTEL_VIDEO_DIP_ENABLE_GCP_HSW    (1U << 16)
#define   INTEL_VIDEO_DIP_ENABLE_AVI_HSW    (1U << 12)
#define   INTEL_VIDEO_DIP_ENABLE_VS_HSW     (1U << 8)
#define   INTEL_VIDEO_DIP_ENABLE_GMP_HSW    (1U << 4)
#define   INTEL_VIDEO_DIP_ENABLE_SPD_HSW    (1U << 0)

#define INTEL_REG_VIDEO_DIP_AVI_DATA(t, i)  (0x60220U + (uint32_t)(t) * INTEL_PIPE_STRIDE + (uint32_t)(i) * 4U)
#define INTEL_REG_VIDEO_DIP_VS_DATA(t, i)   (0x60260U + (uint32_t)(t) * INTEL_PIPE_STRIDE + (uint32_t)(i) * 4U)
#define INTEL_REG_VIDEO_DIP_SPD_DATA(t, i)  (0x602A0U + (uint32_t)(t) * INTEL_PIPE_STRIDE + (uint32_t)(i) * 4U)
#define INTEL_REG_VIDEO_DIP_GMP_DATA(t, i)  (0x602E0U + (uint32_t)(t) * INTEL_PIPE_STRIDE + (uint32_t)(i) * 4U)
#define INTEL_REG_VIDEO_DIP_DRM_DATA(t, i)  (0x60440U + (uint32_t)(t) * INTEL_PIPE_STRIDE + (uint32_t)(i) * 4U)

/* ============================================================================
 * 3. AMD DCN (DCN 1.0 .. DCN 4.01, incl. Rembrandt 0x1681 DCN 3.1.2) MMIO Map
 *    All DCN generations share DCN_BASE__INST0_SEG2 = 0x000034C0 (dword offset)
 *    = 0x0000D300 (byte offset in PCI BAR5/BAR2 MMIO aperture).
 * ============================================================================ */

#define AMD_DCN_SEG2_DWORD_BASE             0x000034C0U
#define AMD_DCN_REG_BYTE_OFFSET(dword_reg)  ((AMD_DCN_SEG2_DWORD_BASE + (uint32_t)(dword_reg)) * 4U)

/* Output Pixel Processor (OPP) / Formatter (FMT0..FMT3): Disable Truncation & Dithering */
#define AMD_DCN_FMT_DWORD_STRIDE            0x005AU /* 0x189B - 0x1841 = 0x5A dwords */
#define AMD_REG_FMT_BIT_DEPTH_CONTROL(inst) AMD_DCN_REG_BYTE_OFFSET(0x1841U + (uint32_t)(inst) * AMD_DCN_FMT_DWORD_STRIDE)
#define   AMD_FMT_TRUNCATE_EN_MASK          0x00000001U
#define   AMD_FMT_TRUNCATE_DEPTH_MASK       0x00000030U
#define   AMD_FMT_SPATIAL_DITHER_EN_MASK    0x00000100U
#define   AMD_FMT_SPATIAL_DITHER_MODE_MASK  0x00000600U
#define   AMD_FMT_SPATIAL_DITHER_DEPTH_MASK 0x00001800U
#define   AMD_FMT_FRAME_RANDOM_ENABLE_MASK  0x00002000U
#define   AMD_FMT_RGB_RANDOM_ENABLE_MASK    0x00004000U
#define   AMD_FMT_HIGHPASS_RANDOM_EN_MASK   0x00008000U
#define   AMD_FMT_TEMPORAL_DITHER_EN_MASK   0x00010000U
#define   AMD_FMT_ALL_DITHER_TRUNC_MASK     0x0001FF33U

/* Video Packet Generator (VPG0..VPG3) in DCN 3.0 / 3.1 / 3.1.2 / 3.1.4 / 3.2 / 3.5 / 4.01 */
#define AMD_DCN3_VPG_DWORD_STRIDE           0x0100U
#define AMD_REG_DCN3_VPG_ACCESS_CTRL(inst)  AMD_DCN_REG_BYTE_OFFSET(0x2068U + (uint32_t)(inst) * AMD_DCN3_VPG_DWORD_STRIDE)
#define AMD_REG_DCN3_VPG_PACKET_DATA(inst)  AMD_DCN_REG_BYTE_OFFSET(0x2069U + (uint32_t)(inst) * AMD_DCN3_VPG_DWORD_STRIDE)
#define AMD_REG_DCN3_VPG_FRAME_UPDATE(inst) AMD_DCN_REG_BYTE_OFFSET(0x206AU + (uint32_t)(inst) * AMD_DCN3_VPG_DWORD_STRIDE)
#define AMD_REG_DCN3_VPG_IMMED_UPDATE(inst) AMD_DCN_REG_BYTE_OFFSET(0x206BU + (uint32_t)(inst) * AMD_DCN3_VPG_DWORD_STRIDE)
#define AMD_REG_DCN3_VPG_STATUS(inst)       AMD_DCN_REG_BYTE_OFFSET(0x206CU + (uint32_t)(inst) * AMD_DCN3_VPG_DWORD_STRIDE)

/* DIG Stream Encoder HDMI Generic Packet Control in DCN 3.0 / 3.1.2 (Rembrandt) */
#define AMD_DCN3_DIG_DWORD_STRIDE           0x0100U
#define AMD_REG_DCN3_HDMI_GEN_CTRL0(inst)   AMD_DCN_REG_BYTE_OFFSET(0x209AU + (uint32_t)(inst) * AMD_DCN3_DIG_DWORD_STRIDE)
#define AMD_REG_DCN3_HDMI_GEN_CTRL1(inst)   AMD_DCN_REG_BYTE_OFFSET(0x209EU + (uint32_t)(inst) * AMD_DCN3_DIG_DWORD_STRIDE)
#define AMD_REG_DCN3_HDMI_GEN_CTRL2(inst)   AMD_DCN_REG_BYTE_OFFSET(0x209FU + (uint32_t)(inst) * AMD_DCN3_DIG_DWORD_STRIDE)
#define AMD_REG_DCN3_HDMI_GEN_CTRL3(inst)   AMD_DCN_REG_BYTE_OFFSET(0x20A0U + (uint32_t)(inst) * AMD_DCN3_DIG_DWORD_STRIDE)
#define AMD_REG_DCN3_HDMI_GEN_CTRL4(inst)   AMD_DCN_REG_BYTE_OFFSET(0x20A1U + (uint32_t)(inst) * AMD_DCN3_DIG_DWORD_STRIDE)

/* Packet slot indices in DCN 3.0+ enc3_stream_encoder_update_hdmi_info_packets():
 *   slot 0 = AVI InfoFrame
 *   slot 1 = Vendor-Specific InfoFrame (Dolby Vision VSIF)
 *   slot 2 = Gamut Metadata
 *   slot 3 = SPD (FreeSync)
 *   slot 4 = HDR Static Metadata (DRM InfoFrame - disabled in TV-Led DV!)
 *   slot 5 = HF-VSIF
 */
#define AMD_DCN3_PKT_SLOT_AVI               0U
#define AMD_DCN3_PKT_SLOT_VENDOR            1U
#define AMD_DCN3_PKT_SLOT_GAMUT             2U
#define AMD_DCN3_PKT_SLOT_SPD               3U
#define AMD_DCN3_PKT_SLOT_HDRSMD            4U
#define AMD_DCN3_PKT_SLOT_HFVSIF            5U

/* Abstract MMIO Read/Write Callback Table (usable with kernel driver or mock/dump) */
struct dvbridge_mmio_ops {
    void     *user_ctx;
    uint32_t (*read32)(void *user_ctx, uint32_t reg_offset);
    void     (*write32)(void *user_ctx, uint32_t reg_offset, uint32_t value);
};

/* Apply or disable TV-Led Dolby Vision on an Intel Display pipe/transcoder (0..3) */
void dvbridge_win_intel_apply_dv(
    const struct dvbridge_mmio_ops *ops,
    unsigned pipe_idx,
    bool enable_dv,
    bool legacy_vsif,
    uint8_t cta_vic);

/* Apply or disable TV-Led Dolby Vision on an AMD DCN 3.x pipe/encoder (0..3) */
void dvbridge_win_amd_dcn3_apply_dv(
    const struct dvbridge_mmio_ops *ops,
    unsigned pipe_idx,
    bool enable_dv,
    bool legacy_vsif,
    uint8_t cta_vic);

#ifdef __cplusplus
}
#endif

#endif /* DVBRIDGE_WIN_MMIO_H */
