// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Windows Native HDMI Dolby Vision Hardware Bridge (Intel Gen9-14 & AMD DCN 1.0-4.01)
 * Implements exact MMIO register programming ported from:
 *   - drivers/gpu/drm/i915/display/intel_hdmi.c & intel_display.c
 *   - drivers/gpu/drm/amd/display/amdgpu_dm/amdgpu_dm_dv.c, dcn30_vpg.c,
 *     dcn30_dio_stream_encoder.c, dcn10_opp.c
 */

#include "dvbridge_win_mmio.h"

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

    /* Preserve the active HDMI mode's CTA-861 VIC from the existing AVI InfoFrame
     * (PB4 = byte 4 of AVI payload = bits 7:0 of DW2) if already valid, so we never
     * mismatch the physical HDMI link clock (e.g. 3840x2160@60Hz VIC=97 vs 24fps VIC=93). */
    uint32_t orig_avi_dw2 = ops->read32(ops->user_ctx, INTEL_REG_VIDEO_DIP_AVI_DATA(pipe_idx, 2));
    uint8_t active_vic = (uint8_t)(orig_avi_dw2 & 0xFFU);
    if (active_vic != 0U)
        cta_vic = active_vic;

    dvbridge_win_pack_dolby_vsif(&vsif, enable_dv, false, legacy_vsif, cta_vic);
    dvbridge_win_pack_avi_infoframe(&avi, cta_vic, legacy_vsif);

    /* 1. Configure PIPE_MISC, GAMMA_MODE, and PIPE_CSC_MODE for 100% bit-exact 8-bit RGB
     * passthrough (verified via Intel hardware PIPE_CRC_CTL 0x60050 and Mi TV #1 DV lock):
     *   - PIPE_MISC (0x70030): Disable dithering (bits 4:2 = 0) AND enable
     *     INTEL_PIPE_MISC_PIXEL_ROUNDING_TRUNC (bit 8 = 1) so 12->8-bit pipe conversion
     *     truncates ((x<<4)|(x>>4))>>4 = x instead of rounding values >= 128 up by +1.
     *   - GAMMA_MODE (0x4A480): Clear CC1 (bits 31:30) and Display 13+ CC2 De-Gamma/Re-Gamma
     *     (bits 28:27) so 0x18000012 -> 0x00000012.
     *   - PIPE_CSC_MODE (0x49028): Clear CC1/Output/CC2 CSCs (0x00000000). */
    uint32_t pipe_misc_reg = INTEL_REG_PIPE_MISC(pipe_idx);
    uint32_t pipe_misc = ops->read32(ops->user_ctx, pipe_misc_reg);
    if (enable_dv) {
        uint32_t new_misc = (pipe_misc & ~(INTEL_PIPE_MISC_DITHER_ENABLE |
                                           INTEL_PIPE_MISC_DITHER_TYPE_MASK)) |
                            INTEL_PIPE_MISC_PIXEL_ROUNDING_TRUNC;
        if (new_misc != pipe_misc)
            ops->write32(ops->user_ctx, pipe_misc_reg, new_misc);

        uint32_t gamma_mode = ops->read32(ops->user_ctx, INTEL_REG_GAMMA_MODE(pipe_idx));
        uint32_t new_gamma = gamma_mode & ~(INTEL_GAMMA_ALL_ENABLE_MASK | (1U << 29));
        if (new_gamma != gamma_mode)
            ops->write32(ops->user_ctx, INTEL_REG_GAMMA_MODE(pipe_idx), new_gamma);

        uint32_t csc_mode = ops->read32(ops->user_ctx, INTEL_REG_PIPE_CSC_MODE(pipe_idx));
        if (csc_mode != 0U)
            ops->write32(ops->user_ctx, INTEL_REG_PIPE_CSC_MODE(pipe_idx), 0U);
    }

    /* 2. Program HSW_TVIDEO_DIP_VS_DATA and HSW_TVIDEO_DIP_AVI_DATA one InfoFrame at a time,
     * matching Linux i915 hsw_write_infoframe() (drivers/gpu/drm/i915/display/intel_hdmi.c:540):
     *   - Gate the specific InfoFrame bit in HSW_TVIDEO_DIP_CTL
     *   - Write all 8 dwords (32 bytes, i = 0..7) to force hardware ECC recalculation
     *     (Never write i = 8, which is the hardware VIDEO_DIP_ECC register!)
     *   - Re-enable the specific InfoFrame bit in HSW_TVIDEO_DIP_CTL and do a posting read. */
    uint32_t dip_ctl_reg = INTEL_REG_VIDEO_DIP_CTL(pipe_idx);
    uint32_t dip_ctl = ops->read32(ops->user_ctx, dip_ctl_reg);

    if (enable_dv) {
        /* If DRM InfoFrame (HDR10 static metadata) is enabled, disable and clear it first */
        if ((dip_ctl & INTEL_VIDEO_DIP_ENABLE_DRM_GLK) != 0U) {
            dip_ctl &= ~INTEL_VIDEO_DIP_ENABLE_DRM_GLK;
            ops->write32(ops->user_ctx, dip_ctl_reg, dip_ctl);
            (void)ops->read32(ops->user_ctx, dip_ctl_reg);
            for (int i = 0; i < 8; ++i) {
                ops->write32(ops->user_ctx, INTEL_REG_VIDEO_DIP_DRM_DATA(pipe_idx, i), 0U);
            }
        }

        /* Write 8 dwords (32 bytes) of Dolby Vision VSIF to HSW_TVIDEO_DIP_VS_DATA(pipe, 0..7) */
        dip_ctl &= ~INTEL_VIDEO_DIP_ENABLE_VS_HSW;
        ops->write32(ops->user_ctx, dip_ctl_reg, dip_ctl);
        (void)ops->read32(ops->user_ctx, dip_ctl_reg);
        for (int i = 0; i < 8; ++i) {
            ops->write32(ops->user_ctx, INTEL_REG_VIDEO_DIP_VS_DATA(pipe_idx, i), vsif.dw[i]);
        }
        dip_ctl |= INTEL_VIDEO_DIP_ENABLE_VS_HSW;
        ops->write32(ops->user_ctx, dip_ctl_reg, dip_ctl);
        (void)ops->read32(ops->user_ctx, dip_ctl_reg);

        /* Write 8 dwords (32 bytes) of RGB Full-Range AVI InfoFrame to HSW_TVIDEO_DIP_AVI_DATA(pipe, 0..7) */
        dip_ctl &= ~INTEL_VIDEO_DIP_ENABLE_AVI_HSW;
        ops->write32(ops->user_ctx, dip_ctl_reg, dip_ctl);
        (void)ops->read32(ops->user_ctx, dip_ctl_reg);
        for (int i = 0; i < 8; ++i) {
            ops->write32(ops->user_ctx, INTEL_REG_VIDEO_DIP_AVI_DATA(pipe_idx, i), avi.dw[i]);
        }
        dip_ctl |= INTEL_VIDEO_DIP_ENABLE_AVI_HSW;
        ops->write32(ops->user_ctx, dip_ctl_reg, dip_ctl);
        (void)ops->read32(ops->user_ctx, dip_ctl_reg);
    } else {
        /* Disable Dolby Vision VSIF DIP and zero 8 dwords of VS data buffer */
        dip_ctl &= ~INTEL_VIDEO_DIP_ENABLE_VS_HSW;
        ops->write32(ops->user_ctx, dip_ctl_reg, dip_ctl);
        (void)ops->read32(ops->user_ctx, dip_ctl_reg);
        for (int i = 0; i < 8; ++i) {
            ops->write32(ops->user_ctx, INTEL_REG_VIDEO_DIP_VS_DATA(pipe_idx, i), 0U);
        }
    }
}

/*
 * Write a 9-dword (36-byte) HDMI Generic InfoFrame packet into an AMD DCN 3.x/4.x VPG
 * (Video Packet Generator) slot (`slot_idx`: 0=AVI, 1=Vendor/Dolby VSIF, 4=HDRSMD)
 * and trigger an immediate GSP packet update (`VPG_GSP_IMMEDIATE_UPDATE_CTRL`).
 */
static void amd_dcn_vpg_write_packet(
    const struct dvbridge_mmio_ops *ops,
    uint32_t access_reg,
    unsigned slot_idx,
    const struct dvbridge_hw_infoframe *pkt,
    bool immediate_update)
{
    uint32_t data_reg   = access_reg + 4U;
    uint32_t frame_reg  = access_reg + 8U;
    uint32_t immed_reg  = access_reg + 12U;

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
 * AMD DCN 3.0 .. DCN 4.01 (both DIO TMDS via DIG/VPG and HDMI 2.1 FRL via HPO/VPG):
 *
 * When TV-Led Dolby Vision is enabled:
 * 1. FMT_BIT_DEPTH_CONTROL(fmt_idx):
 *    - Enable 8bpc truncation and clear spatial/temporal dithering so the 24-bit RGB8
 *      Dolby Vision tunnel + embedded CRC-16 arrives at the HDMI sink bit-perfectly.
 * 2. VPG(vpg_access_reg):
 *    - Program slot 1 (`AMD_DCN3_PKT_SLOT_VENDOR`, offset `1 * 9 = 9`) with the
 *      9-dword Dolby Vision VSIF (`DW0=0x001B0181, DW1=0x00D0464B, DW2=0x00000002`).
 *    - Program slot 0 (`AMD_DCN3_PKT_SLOT_AVI`, offset `0`) with the RGB Full-Range
 *      AVI InfoFrame.
 *    - Clear slot 4 (`AMD_DCN3_PKT_SLOT_HDRSMD`, offset `4 * 9 = 36`) so HDR10 Static
 *      Metadata is not transmitted simultaneously.
 * 3. HDMI_GENERIC_PACKET_CONTROL0 / HDMI_TB_ENC_GENERIC_PACKET_CONTROL0 + line regs:
 *    - Enable continuous transmission of Generic0 (AVI: bits 1:0 = 0x3) and
 *      Generic1 (Vendor/Dolby VSIF: bits 5:4 = 0x30) on Line 2 (bits 31:16 = 2).
 *    - Disable Generic4 (HDRSMD: bits 17:16 = 0).
 */
void dvbridge_win_amd_dcn_apply_params(
    const struct dvbridge_mmio_ops *ops,
    const struct dvbridge_amd_dcn_params *params,
    bool enable_dv,
    bool legacy_vsif,
    uint8_t cta_vic)
{
    if (!ops || !ops->read32 || !ops->write32 || !params ||
        params->fmt_bit_depth_reg == 0U || params->vpg_access_reg == 0U ||
        params->gen_ctrl0_reg == 0U || params->pkt01_line_reg == 0U ||
        params->pkt45_line_reg == 0U)
        return;

    struct dvbridge_hw_infoframe vsif;
    struct dvbridge_hw_infoframe avi;

    dvbridge_win_pack_dolby_vsif(&vsif, enable_dv, false, legacy_vsif, cta_vic);
    dvbridge_win_pack_avi_infoframe(&avi, cta_vic, legacy_vsif);

    /* 1. Configure OPP/FMT for 8bpc truncation (DITHER_OPTION_TRUN8) with dithering disabled */
    if (enable_dv) {
        uint32_t fmt_val = ops->read32(ops->user_ctx, params->fmt_bit_depth_reg);
        fmt_val &= ~AMD_FMT_ALL_DITHER_TRUNC_MASK;
        fmt_val |= (AMD_FMT_TRUNCATE_EN_MASK | AMD_FMT_TRUNCATE_DEPTH_8BPC);
        ops->write32(ops->user_ctx, params->fmt_bit_depth_reg, fmt_val);
    }

    /* 2. Program VPG packet memory and Generic Packet Control / Line registers */
    uint32_t ctrl0 = ops->read32(ops->user_ctx, params->gen_ctrl0_reg);
    uint32_t line45 = ops->read32(ops->user_ctx, params->pkt45_line_reg);

    if (enable_dv) {
        amd_dcn_vpg_write_packet(ops, params->vpg_access_reg, AMD_DCN3_PKT_SLOT_AVI, &avi, true);
        amd_dcn_vpg_write_packet(ops, params->vpg_access_reg, AMD_DCN3_PKT_SLOT_VENDOR, &vsif, true);
        amd_dcn_vpg_write_packet(ops, params->vpg_access_reg, AMD_DCN3_PKT_SLOT_HDRSMD, NULL, true);

        /* Set HDMI_GENERIC0_LINE = 2 (bits 14:0/15:0) and HDMI_GENERIC1_LINE = 2 (bits 30:16/31:16) */
        ops->write32(ops->user_ctx, params->pkt01_line_reg, (2U << 0) | (2U << 16));

        /* Clear HDMI_GENERIC4_LINE (lower 16 bits of CONTROL2 / PACKET4_5_LINE) */
        line45 &= ~0x0000FFFFU;
        ops->write32(ops->user_ctx, params->pkt45_line_reg, line45);

        /* Enable GENERIC0_SEND|CONT (bits 1:0 = 0x3) + GENERIC1_SEND|CONT (bits 5:4 = 0x30),
         * and disable GENERIC4_SEND|CONT (bits 17:16 = 0) */
        ctrl0 &= ~(0x3U << 16);
        ctrl0 |= (0x3U << 0) | (0x3U << 4);
        ops->write32(ops->user_ctx, params->gen_ctrl0_reg, ctrl0);
    } else {
        /* Disable GENERIC1_SEND|CONT (bits 5:4) and clear VPG slot 1 */
        ctrl0 &= ~(0x3U << 4);
        ops->write32(ops->user_ctx, params->gen_ctrl0_reg, ctrl0);
        amd_dcn_vpg_write_packet(ops, params->vpg_access_reg, AMD_DCN3_PKT_SLOT_VENDOR, NULL, true);
    }
}

void dvbridge_win_amd_dcn3_apply_dv_ex(
    const struct dvbridge_mmio_ops *ops,
    unsigned dig_idx,
    unsigned fmt_idx,
    uint32_t gen_ctrl0_dword,
    bool enable_dv,
    bool legacy_vsif,
    uint8_t cta_vic)
{
    if (dig_idx >= AMD_DCN_MAX_DIGS || fmt_idx >= AMD_DCN_MAX_PIPES)
        return;

    if (gen_ctrl0_dword == 0U)
        gen_ctrl0_dword = AMD_DCN3_A_DIG0_HDMI_GEN_CTRL0;

    const uint32_t stride = (gen_ctrl0_dword == AMD_DCN3_C_DIG0_HDMI_GEN_CTRL0)
                                ? AMD_DCN3_C_DIG_DWORD_STRIDE
                                : AMD_DCN3_DIG_DWORD_STRIDE;

    struct dvbridge_amd_dcn_params params;
    params.fmt_bit_depth_reg = AMD_REG_FMT_BIT_DEPTH_CONTROL(fmt_idx);
    params.vpg_access_reg    = AMD_REG_DCN3_VPG_ACCESS_CTRL_S(dig_idx, stride);
    params.gen_ctrl0_reg     = AMD_DCN3_DIG_REG_OFFSET_S(dig_idx, gen_ctrl0_dword, stride);
    params.pkt01_line_reg    = AMD_DCN3_DIG_REG_OFFSET_S(dig_idx, gen_ctrl0_dword + 4U, stride);
    params.pkt45_line_reg    = AMD_DCN3_DIG_REG_OFFSET_S(dig_idx, gen_ctrl0_dword + 5U, stride);

    dvbridge_win_amd_dcn_apply_params(ops, &params, enable_dv, legacy_vsif, cta_vic);
}

void dvbridge_win_amd_dcn3_apply_dv(
    const struct dvbridge_mmio_ops *ops,
    unsigned pipe_idx,
    bool enable_dv,
    bool legacy_vsif,
    uint8_t cta_vic)
{
    dvbridge_win_amd_dcn3_apply_dv_ex(ops, pipe_idx, pipe_idx, AMD_DCN3_A_DIG0_HDMI_GEN_CTRL0,
                                      enable_dv, legacy_vsif, cta_vic);
}
