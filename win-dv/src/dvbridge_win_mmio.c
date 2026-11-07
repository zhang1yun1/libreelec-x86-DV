// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Windows Native HDMI Dolby Vision Hardware Bridge (Intel Gen9-14 & AMD DCN 1.0-4.01)
 * Implements exact MMIO register programming ported from:
 *   - drivers/gpu/drm/i915/display/intel_hdmi.c & intel_display.c
 *   - drivers/gpu/drm/amd/display/amdgpu_dm/amdgpu_dm_dv.c, dcn30_vpg.c,
 *     dcn30_dio_stream_encoder.c, dcn10_opp.c
 */

#include "../include/dvbridge_win_mmio.h"

/*
 * Intel Gen9–Gen14 Display Engine (Skylake, Kaby Lake, Coffee Lake, Ice Lake,
 * Tiger Lake, Alder Lake, Raptor Lake, Meteor Lake, Arrow Lake):
 *
 * When TV-Led Dolby Vision is enabled on pipe/transcoder `pipe_idx`:
 * 1. PIPE_MISC(pipe_idx):
 *    - Clear PIPE_MISC_COLORSPACE_YUV (bit 11 = 0 -> RGB output)
 *    - Set PIPE_MISC_BPC_8 (bits 7:5 = 0 -> 8 bpc)
 *    - Clear PIPE_MISC_DITHER_ENABLE (bit 4 = 0 -> bit-exact RGB8 tunnel, no dither!)
 * 2. HSW_TVIDEO_DIP_CTL(pipe_idx):
 *    - Disable VIDEO_DIP_ENABLE_VS_HSW (bit 8) and VIDEO_DIP_ENABLE_DRM_GLK (bit 28)
 *      before writing the DIP buffer
 * 3. HSW_TVIDEO_DIP_VS_DATA(pipe_idx, 0..8):
 *    - Write the 9 dwords (36 bytes) of the Dolby Vision VSIF (OUI 0x00D046 or 0x000C03)
 * 4. HSW_TVIDEO_DIP_AVI_DATA(pipe_idx, 0..8):
 *    - Write the 9 dwords of the RGB Full-Range AVI InfoFrame
 * 5. HSW_TVIDEO_DIP_CTL(pipe_idx):
 *    - Re-enable VIDEO_DIP_ENABLE_VS_HSW (bit 8) | VIDEO_DIP_ENABLE_AVI_HSW (bit 12),
 *      leaving VIDEO_DIP_ENABLE_DRM_GLK (bit 28) cleared so HDR10 Static Metadata
 *      does not conflict with TV-Led Dolby Vision.
 */
void dvbridge_win_intel_apply_dv(
    const struct dvbridge_mmio_ops *ops,
    unsigned pipe_idx,
    bool enable_dv,
    bool legacy_vsif,
    uint8_t cta_vic)
{
    if (!ops || !ops->read32 || !ops->write32 || pipe_idx >= INTEL_NUM_PIPES)
        return;

    struct dvbridge_hw_infoframe vsif;
    struct dvbridge_hw_infoframe avi;

    dvbridge_win_pack_dolby_vsif(&vsif, enable_dv, false, legacy_vsif, cta_vic);
    dvbridge_win_pack_avi_infoframe(&avi, cta_vic, legacy_vsif);

    /* 1. Configure PIPE_MISC for bit-exact 8-bpc RGB without dithering */
    uint32_t pipe_misc_reg = INTEL_REG_PIPE_MISC(pipe_idx);
    uint32_t pipe_misc = ops->read32(ops->user_ctx, pipe_misc_reg);
    if (enable_dv) {
        pipe_misc &= ~(INTEL_PIPE_MISC_COLORSPACE_YUV |
                       INTEL_PIPE_MISC_YUV420_ENABLE |
                       INTEL_PIPE_MISC_BPC_MASK |
                       INTEL_PIPE_MISC_DITHER_ENABLE |
                       INTEL_PIPE_MISC_DITHER_TYPE_MASK);
        pipe_misc |= INTEL_PIPE_MISC_BPC_8;
        ops->write32(ops->user_ctx, pipe_misc_reg, pipe_misc);
    }

    /* 2. Read HSW_TVIDEO_DIP_CTL and gate VS + DRM DIPs while programming */
    uint32_t dip_ctl_reg = INTEL_REG_VIDEO_DIP_CTL(pipe_idx);
    uint32_t dip_ctl = ops->read32(ops->user_ctx, dip_ctl_reg);

    if (enable_dv) {
        /* Gate VS and DRM DIPs before writing data registers */
        ops->write32(ops->user_ctx, dip_ctl_reg,
                     dip_ctl & ~(INTEL_VIDEO_DIP_ENABLE_VS_HSW | INTEL_VIDEO_DIP_ENABLE_DRM_GLK));

        /* Write 9 dwords of Dolby Vision VSIF to HSW_TVIDEO_DIP_VS_DATA(pipe, 0..8) */
        for (int i = 0; i < 9; ++i) {
            ops->write32(ops->user_ctx, INTEL_REG_VIDEO_DIP_VS_DATA(pipe_idx, i), vsif.dw[i]);
        }

        /* Write 9 dwords of RGB Full-Range AVI InfoFrame to HSW_TVIDEO_DIP_AVI_DATA(pipe, 0..8) */
        ops->write32(ops->user_ctx, dip_ctl_reg,
                     dip_ctl & ~(INTEL_VIDEO_DIP_ENABLE_VS_HSW |
                                 INTEL_VIDEO_DIP_ENABLE_DRM_GLK |
                                 INTEL_VIDEO_DIP_ENABLE_AVI_HSW));
        for (int i = 0; i < 9; ++i) {
            ops->write32(ops->user_ctx, INTEL_REG_VIDEO_DIP_AVI_DATA(pipe_idx, i), avi.dw[i]);
        }

        /* Clear DRM DIP data registers so stale HDR10 metadata cannot leak */
        for (int i = 0; i < 9; ++i) {
            ops->write32(ops->user_ctx, INTEL_REG_VIDEO_DIP_DRM_DATA(pipe_idx, i), 0U);
        }

        /* Enable VS + AVI InfoFrame transmission on every VSync, keep DRM DIP off */
        dip_ctl &= ~INTEL_VIDEO_DIP_ENABLE_DRM_GLK;
        dip_ctl |= (INTEL_VIDEO_DIP_ENABLE_VS_HSW | INTEL_VIDEO_DIP_ENABLE_AVI_HSW);
        ops->write32(ops->user_ctx, dip_ctl_reg, dip_ctl);
    } else {
        /* Disable Dolby Vision VSIF DIP and zero VS data buffer */
        dip_ctl &= ~INTEL_VIDEO_DIP_ENABLE_VS_HSW;
        ops->write32(ops->user_ctx, dip_ctl_reg, dip_ctl);
        for (int i = 0; i < 9; ++i) {
            ops->write32(ops->user_ctx, INTEL_REG_VIDEO_DIP_VS_DATA(pipe_idx, i), 0U);
        }
    }
}

/*
 * Write a 9-dword (36-byte) HDMI Generic InfoFrame packet into an AMD DCN 3.x VPG
 * (Video Packet Generator) slot (`slot_idx`: 0=AVI, 1=Vendor/Dolby VSIF, 4=HDRSMD)
 * and trigger an immediate GSP packet update (`VPG_GSP_IMMEDIATE_UPDATE_CTRL`).
 */
static void amd_dcn3_vpg_write_packet(
    const struct dvbridge_mmio_ops *ops,
    unsigned vpg_inst,
    unsigned slot_idx,
    const struct dvbridge_hw_infoframe *pkt,
    bool immediate_update)
{
    uint32_t access_reg = AMD_REG_DCN3_VPG_ACCESS_CTRL(vpg_inst);
    uint32_t data_reg   = AMD_REG_DCN3_VPG_PACKET_DATA(vpg_inst);
    uint32_t immed_reg  = AMD_REG_DCN3_VPG_IMMED_UPDATE(vpg_inst);
    uint32_t frame_reg  = AMD_REG_DCN3_VPG_FRAME_UPDATE(vpg_inst);

    /* VPG_GENERIC_PACKET_ACCESS_CTRL: VPG_GENERIC_DATA_INDEX = slot_idx * 9 */
    ops->write32(ops->user_ctx, access_reg, slot_idx * 9U);

    /* Write DW0 (HB0..HB3) .. DW8 (PB28..PB31) sequentially to VPG_GENERIC_PACKET_DATA */
    for (int i = 0; i < 9; ++i) {
        ops->write32(ops->user_ctx, data_reg, pkt ? pkt->dw[i] : 0U);
    }

    /* Trigger update for slot_idx (bit = 1U << slot_idx) */
    uint32_t bit = (1U << slot_idx);
    if (immediate_update) {
        uint32_t cur = ops->read32(ops->user_ctx, immed_reg);
        ops->write32(ops->user_ctx, immed_reg, cur | bit);
    } else {
        uint32_t cur = ops->read32(ops->user_ctx, frame_reg);
        ops->write32(ops->user_ctx, frame_reg, cur | bit);
    }
}

/*
 * AMD DCN 3.0 / 3.1 / 3.1.2 (Rembrandt) / 3.1.4 / 3.2 / 3.5 / 4.01:
 *
 * When TV-Led Dolby Vision is enabled on pipe/stream encoder `pipe_idx`:
 * 1. FMT_BIT_DEPTH_CONTROL(pipe_idx):
 *    - Clear FMT_TRUNCATE_EN, FMT_SPATIAL_DITHER_EN, FMT_TEMPORAL_DITHER_EN
 *      so the 24-bit RGB8 Dolby Vision tunnel + embedded CRC-16 metadata arrives
 *      at the HDMI sink bit-perfectly.
 * 2. VPG(pipe_idx):
 *    - Program slot 1 (`AMD_DCN3_PKT_SLOT_VENDOR`, offset `1 * 9 = 9`) with the
 *      9-dword Dolby Vision VSIF (`DW0=0x001B0181, DW1=0x00D0464B, DW2=0x00000002`).
 *    - Program slot 0 (`AMD_DCN3_PKT_SLOT_AVI`, offset `0`) with the RGB Full-Range
 *      AVI InfoFrame.
 *    - Clear slot 4 (`AMD_DCN3_PKT_SLOT_HDRSMD`, offset `4 * 9 = 36`) so HDR10 Static
 *      Metadata is not transmitted simultaneously.
 * 3. DIG_HDMI_GENERIC_PACKET_CONTROL0 & CONTROL1(pipe_idx):
 *    - Enable continuous transmission of Generic0 (AVI: bits 1:0 = 0x3) and
 *      Generic1 (Vendor/Dolby VSIF: bits 5:4 = 0x30) on Line 2 (CONTROL1 bits 31:16 = 2).
 *    - Disable Generic4 (HDRSMD: bits 17:16 = 0).
 */
void dvbridge_win_amd_dcn3_apply_dv(
    const struct dvbridge_mmio_ops *ops,
    unsigned pipe_idx,
    bool enable_dv,
    bool legacy_vsif,
    uint8_t cta_vic)
{
    if (!ops || !ops->read32 || !ops->write32 || pipe_idx >= 4U)
        return;

    struct dvbridge_hw_infoframe vsif;
    struct dvbridge_hw_infoframe avi;

    dvbridge_win_pack_dolby_vsif(&vsif, enable_dv, false, legacy_vsif, cta_vic);
    dvbridge_win_pack_avi_infoframe(&avi, cta_vic, legacy_vsif);

    /* 1. Disable spatial/temporal dithering & truncation in OPP/FMT */
    uint32_t fmt_reg = AMD_REG_FMT_BIT_DEPTH_CONTROL(pipe_idx);
    uint32_t fmt_val = ops->read32(ops->user_ctx, fmt_reg);
    if (enable_dv) {
        fmt_val &= ~AMD_FMT_ALL_DITHER_TRUNC_MASK;
        ops->write32(ops->user_ctx, fmt_reg, fmt_val);
    }

    /* 2. Program VPG packet memory and DIG HDMI Generic Packet Control */
    uint32_t ctrl0_reg = AMD_REG_DCN3_HDMI_GEN_CTRL0(pipe_idx);
    uint32_t ctrl1_reg = AMD_REG_DCN3_HDMI_GEN_CTRL1(pipe_idx);
    uint32_t ctrl2_reg = AMD_REG_DCN3_HDMI_GEN_CTRL2(pipe_idx);

    uint32_t ctrl0 = ops->read32(ops->user_ctx, ctrl0_reg);
    uint32_t ctrl1 = ops->read32(ops->user_ctx, ctrl1_reg);
    uint32_t ctrl2 = ops->read32(ops->user_ctx, ctrl2_reg);

    if (enable_dv) {
        /* Write AVI InfoFrame to slot 0 and Dolby VSIF to slot 1 */
        amd_dcn3_vpg_write_packet(ops, pipe_idx, AMD_DCN3_PKT_SLOT_AVI, &avi, true);
        amd_dcn3_vpg_write_packet(ops, pipe_idx, AMD_DCN3_PKT_SLOT_VENDOR, &vsif, true);
        amd_dcn3_vpg_write_packet(ops, pipe_idx, AMD_DCN3_PKT_SLOT_HDRSMD, NULL, true);

        /* Set HDMI_GENERIC0_LINE = 2 (bits 15:0) and HDMI_GENERIC1_LINE = 2 (bits 31:16) */
        ctrl1 = (2U << 0) | (2U << 16);
        ops->write32(ops->user_ctx, ctrl1_reg, ctrl1);

        /* Clear HDMI_GENERIC4_LINE (bits 15:0 of CONTROL2) */
        ctrl2 &= ~0x0000FFFFU;
        ops->write32(ops->user_ctx, ctrl2_reg, ctrl2);

        /* Enable GENERIC0_SEND|CONT (bits 1:0 = 0x3) + GENERIC1_SEND|CONT (bits 5:4 = 0x30),
         * and disable GENERIC4_SEND|CONT (bits 17:16 = 0) */
        ctrl0 &= ~(0x3U << 16);
        ctrl0 |= (0x3U << 0) | (0x3U << 4);
        ops->write32(ops->user_ctx, ctrl0_reg, ctrl0);
    } else {
        /* Disable GENERIC1_SEND|CONT (bits 5:4) and clear VPG slot 1 */
        ctrl0 &= ~(0x3U << 4);
        ops->write32(ops->user_ctx, ctrl0_reg, ctrl0);
        amd_dcn3_vpg_write_packet(ops, pipe_idx, AMD_DCN3_PKT_SLOT_VENDOR, NULL, true);
    }
}
