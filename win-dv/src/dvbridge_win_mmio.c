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
 *    - Clear slot 4 (`AMD_DCN3_PKT_SLOT_HDRSMD`, offset `4 * 9 = 36`) and slot 5
 *      (`AMD_DCN3_PKT_SLOT_HFVSIF`, offset `5 * 9 = 45`) so HDR10 Static Metadata
 *      and HF-VSIF are not transmitted simultaneously.
 * 3. HDMI_GENERIC_PACKET_CONTROL0 / HDMI_TB_ENC_GENERIC_PACKET_CONTROL0 + line regs:
 *    - Enable continuous transmission of Generic0 (AVI: bits 1:0 = 0x3) and
 *      Generic1 (Vendor/Dolby VSIF: bits 5:4 = 0x30) on Line 2 (bits 31:16 = 2).
 *    - Disable Generic4 (HDRSMD: bits 17:16 = 0) and Generic5 (HF-VSIF: bits 21:20 = 0)
 *      and clear their line numbers in pkt45_line_reg.
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
    bool is_dio = (params->gen_ctrl0_reg < AMD_DCN_SEG3_BYTE_OFFSET(0U));

    if (enable_dv) {
        if (is_dio) {
            /* Ensure DIO HDMI double-buffering does not stall InfoFrame updates across VUPDATE */
            uint32_t db_reg = params->gen_ctrl0_reg + (uint32_t)AMD_DCN3_DIG_HDMI_DB_CONTROL_DELTA * 4U;
            uint32_t db_val = ops->read32(ops->user_ctx, db_reg);
            if (db_val != 0xFFFFFFFFU && (db_val & AMD_HDMI_DB_DISABLE_MASK) == 0U)
                ops->write32(ops->user_ctx, db_reg, db_val | AMD_HDMI_DB_DISABLE_MASK);
        } else {
            uint32_t db_val = ops->read32(ops->user_ctx, AMD_REG_DCN_HPO_TB_DB_CTRL);
            if (db_val != 0xFFFFFFFFU && (db_val & AMD_HDMI_DB_DISABLE_MASK) == 0U)
                ops->write32(ops->user_ctx, AMD_REG_DCN_HPO_TB_DB_CTRL, db_val | AMD_HDMI_DB_DISABLE_MASK);
        }

        amd_dcn_vpg_write_packet(ops, params->vpg_access_reg, AMD_DCN3_PKT_SLOT_AVI, &avi, true);
        amd_dcn_vpg_write_packet(ops, params->vpg_access_reg, AMD_DCN3_PKT_SLOT_VENDOR, &vsif, true);
        amd_dcn_vpg_write_packet(ops, params->vpg_access_reg, AMD_DCN3_PKT_SLOT_HDRSMD, NULL, true);
        if (is_dio) {
            amd_dcn_vpg_write_packet(ops, params->vpg_access_reg, AMD_DCN3_PKT_SLOT_HFVSIF, NULL, true);
        } else {
            /* HPO FRL uses slots 5..10 for DSC PPS EMP and slot 11 for HF-VSIF */
            amd_dcn_vpg_write_packet(ops, params->vpg_access_reg, AMD_DCN_HPO_PKT_SLOT_HFVSIF, NULL, true);
        }

        /* Set HDMI_GENERIC0_LINE = 2 (bits 14:0/15:0) and HDMI_GENERIC1_LINE = 2 (bits 30:16/31:16) */
        ops->write32(ops->user_ctx, params->pkt01_line_reg, (2U << 0) | (2U << 16));

        /* Clear HDMI_GENERIC4_LINE (and GENERIC5_LINE on DIO; preserve slot 5 on HPO in case DSC uses it) */
        line45 &= is_dio ? 0U : ~0x00007FFFU;
        ops->write32(ops->user_ctx, params->pkt45_line_reg, line45);

        /* Enable GENERIC0_SEND|CONT (bits 1:0 = 0x3) + GENERIC1_SEND|CONT (bits 5:4 = 0x30),
         * and disable GENERIC4_SEND|CONT (bits 17:16 = 0) + GENERIC5_SEND|CONT on DIO / GENERIC11 on HPO */
        ctrl0 &= is_dio ? ~((0x3U << 16) | (0x3U << 20)) : ~(0x3U << 16);
        ctrl0 |= (0x3U << 0) | (0x3U << 4);
        ops->write32(ops->user_ctx, params->gen_ctrl0_reg, ctrl0);

        if (!is_dio) {
            uint32_t ctrl1 = ops->read32(ops->user_ctx, AMD_REG_DCN_HPO_TB_GEN_CTRL1);
            if (ctrl1 != 0xFFFFFFFFU && (ctrl1 & AMD_HPO_GEN_CTRL1_GENERIC11_MASK) != 0U) {
                ops->write32(ops->user_ctx, AMD_REG_DCN_HPO_TB_GEN_CTRL1,
                             ctrl1 & ~AMD_HPO_GEN_CTRL1_GENERIC11_MASK);
            }
            uint32_t line10_11 = ops->read32(ops->user_ctx, AMD_REG_DCN_HPO_TB_PKT10_11_LINE);
            if (line10_11 != 0xFFFFFFFFU && (line10_11 & 0x7FFF0000U) != 0U) {
                ops->write32(ops->user_ctx, AMD_REG_DCN_HPO_TB_PKT10_11_LINE,
                             line10_11 & ~0x7FFF0000U);
            }
        }
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
    params.pkt45_line_reg    = AMD_DCN3_DIG_REG_OFFSET_S(dig_idx, gen_ctrl0_dword + 6U, stride);

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

/* ============================================================================
 * Cross-Generation AMD DCN 3.x / 4.01 Pixel Pipeline Snapshot & Bypass Helpers
 * Verified directly against Linux 7.2.6 dcn_3_0..dcn_4_1_0 ASIC headers.
 * ============================================================================ */

#define AMD_MPC_OUT_CSC_DWORD_STRIDE 0x0DU
#define AMD_MPC_RMU_MAX_COUNT        3U

struct amd_dcn_tier_layout {
    uint32_t dscl_scl_mode_dw;
    uint32_t dscl_easf_h_dw;
    uint32_t dscl_easf_v_dw;
    uint32_t dscl_isharp_dw;
    uint32_t cm_control_dw;
    uint32_t cm_post_csc_dw;
    uint32_t cm_gamut_remap_dw;
    uint32_t cm_gamcor_dw;
    uint32_t cm_hdr_mult_dw;
    uint32_t mpcc_stride;
    uint32_t mpcc_ogam_base;
    uint32_t mpcc_ogam_stride;
    uint32_t mpcc_gamut_remap_delta;
    uint32_t mpcc_mcm_shaper_base;
    uint32_t mpcc_mcm_3dlut_delta;
    uint32_t mpcc_mcm_1dlut_delta;
    uint32_t mpcc_mcm_first_gamut_delta;
    uint32_t mpcc_mcm_second_gamut_delta;
    uint32_t mpcc_mcm_stride;
    uint32_t mpc_rmu_ctrl_dw;
    uint32_t mpc_rmu_shaper_base;
    uint32_t mpc_rmu_3dlut_delta;
    uint32_t mpc_rmu_stride;
    uint32_t mpc_rmu_count;
    uint32_t mpc_out_mux_base;
    uint32_t mpc_out_denorm_delta;
    uint32_t mpc_out_csc_delta;
};

static const struct amd_dcn_tier_layout g_dcn_tier_layouts[7] = {
    /* 0: DVBRIDGE_AMD_DCN_TIER_30_31: DCN 3.1 / 3.1.2 / 3.1.4 / 3.1.5 / 3.1.6 */
    {
        0x0CFBU, 0U, 0U, 0U,
        0x0D20U, 0x0D21U, 0x0D2EU, 0x0D3DU, 0x0DD1U,
        0x0020U, 0x0100U, 0x0080U, 0x004BU,
        0U, 0U, 0U, 0U, 0U, 0U,
        0x0680U, 0x0682U, 0x0037U, 0x0040U, 2U,
        0x0580U, 0x0001U, 0x0021U
    },
    /* 1: DVBRIDGE_AMD_DCN_TIER_32_35: DCN 3.2 / 3.2.1 / 3.5 / 3.5.1 / 3.6 */
    {
        0x0CFBU, 0U, 0U, 0U,
        0x0D20U, 0x0D21U, 0x0D2EU, 0x0D3DU, 0x0D87U,
        0x0015U, 0x00A8U, 0x005EU, 0x004BU,
        0x0453U, 0x0037U, 0x0040U, 0U, 0U, 0x0090U,
        0U, 0U, 0U, 0U, 0U,
        0x03D8U, 0x0001U, 0x0019U
    },
    /* 2: DVBRIDGE_AMD_DCN_TIER_401: DCN 4.0.1 (dcn_4_1_0, RDNA4 RX 9000) */
    {
        0x0D08U, 0x0D28U, 0x0D29U, 0x0D55U,
        0x0D67U, 0x0D68U, 0U,      0x0D77U, 0x0DC1U,
        0x0015U, 0x007EU, 0x005EU, 0x004BU,
        0x0453U, 0x0037U, 0x0040U, 0x008BU, 0x0099U, 0x00B0U,
        0U, 0U, 0U, 0U, 0U,
        0x02F2U, 0x0001U, 0x0019U
    },
    /* 3: DVBRIDGE_AMD_DCN_TIER_300: DCN 3.0.0 (Sienna Cichlid / Navy Flounder) */
    {
        0x0CFBU, 0U, 0U, 0U,
        0x0D20U, 0x0D21U, 0x0D2EU, 0x0D3DU, 0x0DD1U,
        0x0020U, 0x0100U, 0x0080U, 0x004BU,
        0U, 0U, 0U, 0U, 0U, 0U,
        0x0680U, 0x0682U, 0x0037U, 0x0040U, 3U,
        0x0580U, 0x0001U, 0x0019U
    },
    /* 4: DVBRIDGE_AMD_DCN_TIER_301: DCN 3.0.1 (Van Gogh) */
    {
        0x0CFBU, 0U, 0U, 0U,
        0x0D20U, 0x0D21U, 0x0D2EU, 0x0D3DU, 0x0DD1U,
        0x0020U, 0x0100U, 0x0080U, 0x004BU,
        0U, 0U, 0U, 0U, 0U, 0U,
        0x0680U, 0x0682U, 0x0037U, 0x0040U, 2U,
        0x0580U, 0x0001U, 0x0011U
    },
    /* 5: DVBRIDGE_AMD_DCN_TIER_302: DCN 3.0.2 (Dimgrey Cavefish) */
    {
        0x0CFBU, 0U, 0U, 0U,
        0x0D20U, 0x0D21U, 0x0D2EU, 0x0D3DU, 0x0DD1U,
        0x0020U, 0x0100U, 0x0080U, 0x004BU,
        0U, 0U, 0U, 0U, 0U, 0U,
        0x0680U, 0x0682U, 0x0037U, 0x0040U, 2U,
        0x0580U, 0x0001U, 0x0015U
    },
    /* 6: DVBRIDGE_AMD_DCN_TIER_303: DCN 3.0.3 (Beige Goby) */
    {
        0x0CFBU, 0U, 0U, 0U,
        0x0D20U, 0x0D21U, 0x0D2EU, 0x0D3DU, 0x0DD1U,
        0x0020U, 0x0100U, 0x0080U, 0x004BU,
        0U, 0U, 0U, 0U, 0U, 0U,
        0x0680U, 0x0682U, 0x0037U, 0x0040U, 1U,
        0x0580U, 0x0001U, 0x0019U
    }
};

static const struct amd_dcn_tier_layout *amd_dcn_get_tier_layout(enum dvbridge_amd_dcn_tier tier)
{
    unsigned idx = (unsigned)tier;
    if (idx >= (unsigned)(sizeof(g_dcn_tier_layouts) / sizeof(g_dcn_tier_layouts[0])))
        idx = 0U;
    return &g_dcn_tier_layouts[idx];
}

static uint32_t amd_dcn_mpc_out_csc_reg(const struct amd_dcn_tier_layout *L, uint32_t fmt_idx)
{
    return AMD_DCN_SEG3_BYTE_OFFSET(L->mpc_out_mux_base + L->mpc_out_csc_delta +
                                    fmt_idx * AMD_MPC_OUT_CSC_DWORD_STRIDE);
}

static uint32_t amd_dcn_read_opt(const struct dvbridge_mmio_ops *ops, uint32_t reg_offset)
{
    if (reg_offset == 0U)
        return 0U;
    return ops->read32(ops->user_ctx, reg_offset);
}

static bool amd_dcn_update_bits(
    const struct dvbridge_mmio_ops *ops,
    uint32_t reg_offset,
    uint32_t clear_mask,
    uint32_t set_mask,
    struct dvbridge_amd_dcn_reg_save *save)
{
    if (reg_offset == 0U)
        return false;
    uint32_t cur = ops->read32(ops->user_ctx, reg_offset);
    if (cur == 0xFFFFFFFFU)
        return false;
    uint32_t next = (cur & ~clear_mask) | set_mask;
    if (next == cur)
        return false;
    if (save)
        dvbridge_win_amd_dcn_record_reg(save, reg_offset, cur);
    ops->write32(ops->user_ctx, reg_offset, next);
    return true;
}

static uint32_t amd_dcn_dio_rel_reg(
    const struct dvbridge_amd_dcn_params *params,
    bool is_hpo_frl,
    int32_t dword_delta)
{
    if (is_hpo_frl || !params || params->gen_ctrl0_reg < AMD_DCN_REG_BYTE_OFFSET(0x2000U) ||
        params->gen_ctrl0_reg >= AMD_DCN_SEG3_BYTE_OFFSET(0U))
        return 0U;
    int32_t byte_off = (int32_t)params->gen_ctrl0_reg + dword_delta * 4;
    return (byte_off > 0) ? (uint32_t)byte_off : 0U;
}

enum dvbridge_amd_dcn_tier dvbridge_amd_dcn_resolve_tier(
    uint32_t device_id,
    uint32_t gen_ctrl0_dword,
    bool is_hpo_frl)
{
    /* DCN 4.0.1 dGPUs (RDNA4 RX 9000) */
    if (device_id >= 0x7550U && device_id <= 0x759FU)
        return DVBRIDGE_AMD_DCN_TIER_401;

    /* DCN 3.2/3.5/3.6: Navi3x dGPUs (Layout B) and Strix/Krackan APUs (Layout C) */
    if ((device_id >= 0x7440U && device_id <= 0x749FU) ||
        device_id == 0x150EU || device_id == 0x1586U || device_id == 0x1114U)
        return DVBRIDGE_AMD_DCN_TIER_32_35;

    /* DCN 3.1.x APUs: Phoenix/Hawk Point (Layout B DIG, DCN 3.1 MPC/DPP) & Rembrandt */
    if (device_id == 0x15BFU || device_id == 0x15C8U || device_id == 0x1900U ||
        device_id == 0x164DU || device_id == 0x1681U)
        return DVBRIDGE_AMD_DCN_TIER_30_31;

    /* DCN 3.0.x ASICs */
    if (device_id == 0x163FU || device_id == 0x1435U)
        return DVBRIDGE_AMD_DCN_TIER_301;
    if (device_id >= 0x73A0U && device_id <= 0x73DFU)
        return DVBRIDGE_AMD_DCN_TIER_300;
    if (device_id >= 0x73E0U && device_id <= 0x73FFU)
        return DVBRIDGE_AMD_DCN_TIER_302;
    if (device_id >= 0x7420U && device_id <= 0x743FU)
        return DVBRIDGE_AMD_DCN_TIER_303;

    /* Fallback for unlisted devices: Layout C (0x20A5) is always DCN 3.5+;
     * Layout B (0x209B) is DCN 3.1.4 on APUs (< 0x7000) and DCN 3.2 on dGPUs (>= 0x7000). */
    if (!is_hpo_frl && gen_ctrl0_dword == AMD_DCN3_C_DIG0_HDMI_GEN_CTRL0)
        return DVBRIDGE_AMD_DCN_TIER_32_35;
    if (!is_hpo_frl && gen_ctrl0_dword == AMD_DCN3_B_DIG0_HDMI_GEN_CTRL0 && device_id >= 0x7000U)
        return DVBRIDGE_AMD_DCN_TIER_32_35;
    return DVBRIDGE_AMD_DCN_TIER_30_31;
}

struct amd_dcn_rmu_pair {
    uint32_t shaper_reg;
    uint32_t lut3d_reg;
};

/* Collect SEG3 byte offsets of shared MPC_RMU blocks currently bound to mpcc_idx. */
static uint32_t amd_dcn_get_bound_rmus(
    const struct dvbridge_mmio_ops *ops,
    const struct amd_dcn_tier_layout *L,
    uint32_t mpcc_idx,
    struct amd_dcn_rmu_pair out_rmus[AMD_MPC_RMU_MAX_COUNT])
{
    if (L->mpc_rmu_ctrl_dw == 0U || L->mpc_rmu_shaper_base == 0U || L->mpc_rmu_count == 0U)
        return 0U;

    uint32_t rmu_ctrl = amd_dcn_read_opt(ops, AMD_DCN_SEG3_BYTE_OFFSET(L->mpc_rmu_ctrl_dw));
    if (rmu_ctrl == 0xFFFFFFFFU)
        return 0U;

    uint32_t count = 0U;
    uint32_t limit = (L->mpc_rmu_count < AMD_MPC_RMU_MAX_COUNT) ? L->mpc_rmu_count : AMD_MPC_RMU_MAX_COUNT;
    for (uint32_t r = 0U; r < limit; ++r) {
        uint32_t mux    = (rmu_ctrl >> (r * 8U)) & 0xFU;
        uint32_t status = (rmu_ctrl >> (r * 8U + 4U)) & 0xFU;
        if (mux == mpcc_idx || status == mpcc_idx) {
            uint32_t rmu_dw = L->mpc_rmu_shaper_base + r * L->mpc_rmu_stride;
            out_rmus[count].shaper_reg = AMD_DCN_SEG3_BYTE_OFFSET(rmu_dw);
            out_rmus[count].lut3d_reg  = AMD_DCN_SEG3_BYTE_OFFSET(rmu_dw + L->mpc_rmu_3dlut_delta);
            count++;
        }
    }
    return count;
}

void dvbridge_win_amd_dcn_record_reg(
    struct dvbridge_amd_dcn_reg_save *save,
    uint32_t reg_offset,
    uint32_t orig_value)
{
    if (!save || reg_offset == 0U || orig_value == 0xFFFFFFFFU)
        return;
    for (uint32_t i = 0U; i < save->count; ++i) {
        if (save->entries[i].reg_offset == reg_offset)
            return;
    }
    if (save->count < DVBRIDGE_AMD_MAX_SAVED_REGS) {
        save->entries[save->count].reg_offset = reg_offset;
        save->entries[save->count].orig_value = orig_value;
        save->count++;
    }
}

struct amd_dcn_active_mpcc {
    uint32_t mpcc_idx;
    uint32_t dpp_idx;
    uint32_t top_sel;
    uint32_t opp_id;
};

static uint32_t amd_dcn_find_active_mpccs(
    const struct dvbridge_mmio_ops *ops,
    const struct amd_dcn_tier_layout *L,
    uint32_t fmt_idx,
    struct amd_dcn_active_mpcc out_list[AMD_DCN_MAX_PIPES])
{
    uint32_t count = 0U;
    for (uint32_t m = 0U; m < AMD_DCN_MAX_PIPES; ++m) {
        uint32_t mpcc_base = m * L->mpcc_stride;
        uint32_t top_sel = amd_dcn_read_opt(ops, AMD_DCN_SEG3_BYTE_OFFSET(mpcc_base + 0U));
        uint32_t opp_id  = amd_dcn_read_opt(ops, AMD_DCN_SEG3_BYTE_OFFSET(mpcc_base + 2U));
        if (top_sel == 0xFFFFFFFFU || opp_id == 0xFFFFFFFFU)
            continue;
        uint32_t dpp_idx = top_sel & AMD_MPCC_TOP_SEL_MASK;
        if (dpp_idx == AMD_MPCC_TOP_SEL_NONE || dpp_idx >= AMD_DCN_MAX_PIPES ||
            (opp_id & AMD_MPCC_OPP_ID_MASK) != fmt_idx)
            continue;
        out_list[count].mpcc_idx = m;
        out_list[count].dpp_idx = dpp_idx;
        out_list[count].top_sel = top_sel;
        out_list[count].opp_id = opp_id;
        count++;
    }
    return count;
}

void dvbridge_win_amd_dcn_capture_snapshot(
    const struct dvbridge_mmio_ops *ops,
    enum dvbridge_amd_dcn_tier tier,
    const struct dvbridge_amd_dcn_params *params,
    bool is_hpo_frl,
    uint32_t fmt_idx,
    struct dvbridge_amd_dcn_snapshot *out_snap)
{
    if (!ops || !ops->read32 || !params || !out_snap || fmt_idx >= AMD_DCN_MAX_PIPES)
        return;

    uint8_t *raw = (uint8_t *)out_snap;
    for (uint32_t i = 0U; i < (uint32_t)sizeof(*out_snap); ++i)
        raw[i] = 0U;
    out_snap->tier = tier;
    out_snap->is_hpo_frl = is_hpo_frl;
    out_snap->fmt_idx = fmt_idx;

    const struct amd_dcn_tier_layout *L = amd_dcn_get_tier_layout(tier);

    if (is_hpo_frl) {
        out_snap->hdmi_control    = amd_dcn_read_opt(ops, AMD_REG_DCN_HPO_TB_PIXEL_FORMAT);
        out_snap->hdmi_infoframe0 = amd_dcn_read_opt(ops, AMD_REG_DCN_HPO_TB_GEN_CTRL1);
        out_snap->gen_ctrl0       = amd_dcn_read_opt(ops, params->gen_ctrl0_reg);
        out_snap->pkt01_line      = amd_dcn_read_opt(ops, params->pkt01_line_reg);
        out_snap->pkt23_line      = amd_dcn_read_opt(ops, AMD_REG_DCN_HPO_TB_PKT2_3_LINE);
        out_snap->pkt45_line      = amd_dcn_read_opt(ops, params->pkt45_line_reg);
        out_snap->hdmi_gc         = amd_dcn_read_opt(ops, AMD_REG_DCN_HPO_TB_GC_CTRL);
        out_snap->hdmi_db_control = amd_dcn_read_opt(ops, AMD_REG_DCN_HPO_TB_DB_CTRL);
    } else {
        out_snap->hdmi_control    = amd_dcn_read_opt(ops, amd_dcn_dio_rel_reg(params, false, AMD_DCN3_DIG_HDMI_CONTROL_DELTA));
        out_snap->hdmi_infoframe0 = amd_dcn_read_opt(ops, amd_dcn_dio_rel_reg(params, false, AMD_DCN3_DIG_HDMI_INFOFRAME0_DELTA));
        out_snap->gen_ctrl0       = amd_dcn_read_opt(ops, params->gen_ctrl0_reg);
        out_snap->pkt01_line      = amd_dcn_read_opt(ops, params->pkt01_line_reg);
        out_snap->pkt23_line      = amd_dcn_read_opt(ops, amd_dcn_dio_rel_reg(params, false, 5));
        out_snap->pkt45_line      = amd_dcn_read_opt(ops, params->pkt45_line_reg);
        out_snap->hdmi_gc         = amd_dcn_read_opt(ops, amd_dcn_dio_rel_reg(params, false, AMD_DCN3_DIG_HDMI_GC_DELTA));
        out_snap->hdmi_db_control = amd_dcn_read_opt(ops, amd_dcn_dio_rel_reg(params, false, AMD_DCN3_DIG_HDMI_DB_CONTROL_DELTA));
    }

    out_snap->fmt_dynamic_exp_cntl  = amd_dcn_read_opt(ops, AMD_REG_FMT_DYNAMIC_EXP_CNTL(fmt_idx));
    out_snap->fmt_control           = amd_dcn_read_opt(ops, AMD_REG_FMT_CONTROL(fmt_idx));
    out_snap->fmt_bit_depth_control = amd_dcn_read_opt(ops, params->fmt_bit_depth_reg);
    out_snap->fmt_clamp_cntl        = amd_dcn_read_opt(ops, AMD_REG_FMT_CLAMP_CNTL(fmt_idx));
    out_snap->opp_pipe_control      = amd_dcn_read_opt(ops, AMD_REG_OPP_PIPE_CONTROL(fmt_idx));

    uint32_t mpc_out_dw = L->mpc_out_mux_base + fmt_idx * 4U;
    out_snap->mpc_out_mux            = amd_dcn_read_opt(ops, AMD_DCN_SEG3_BYTE_OFFSET(mpc_out_dw));
    out_snap->mpc_out_denorm_control = amd_dcn_read_opt(ops, AMD_DCN_SEG3_BYTE_OFFSET(mpc_out_dw + L->mpc_out_denorm_delta));
    out_snap->mpc_out_csc_mode       = amd_dcn_read_opt(ops, amd_dcn_mpc_out_csc_reg(L, fmt_idx));

    struct amd_dcn_active_mpcc active[AMD_DCN_MAX_PIPES];
    uint32_t active_cnt = amd_dcn_find_active_mpccs(ops, L, fmt_idx, active);
    for (uint32_t i = 0U; i < active_cnt; ++i) {
        uint32_t m = active[i].mpcc_idx;
        uint32_t dpp_idx = active[i].dpp_idx;
        uint32_t mpcc_base = m * L->mpcc_stride;

        struct dvbridge_amd_dcn_mpcc_snap *entry = &out_snap->mpcc[out_snap->active_mpcc_count++];
        entry->active = true;
        entry->mpcc_idx = m;
        entry->dpp_idx = dpp_idx;
        entry->mpcc_top_sel = active[i].top_sel;
        entry->mpcc_opp_id = active[i].opp_id;
        entry->mpcc_control = amd_dcn_read_opt(ops, AMD_DCN_SEG3_BYTE_OFFSET(mpcc_base + 3U));
        entry->mpcc_update_lock_sel = amd_dcn_read_opt(ops, AMD_DCN_SEG3_BYTE_OFFSET(mpcc_base + 5U));

        uint32_t ogam_dw = L->mpcc_ogam_base + m * L->mpcc_ogam_stride;
        entry->mpcc_ogam_control     = amd_dcn_read_opt(ops, AMD_DCN_SEG3_BYTE_OFFSET(ogam_dw));
        entry->mpcc_gamut_remap_mode = amd_dcn_read_opt(ops, AMD_DCN_SEG3_BYTE_OFFSET(ogam_dw + L->mpcc_gamut_remap_delta));

        if (L->mpcc_mcm_shaper_base != 0U) {
            uint32_t mcm_dw = L->mpcc_mcm_shaper_base + m * L->mpcc_mcm_stride;
            entry->mpcc_mcm_shaper_control = amd_dcn_read_opt(ops, AMD_DCN_SEG3_BYTE_OFFSET(mcm_dw));
            entry->mpcc_mcm_3dlut_mode     = amd_dcn_read_opt(ops, AMD_DCN_SEG3_BYTE_OFFSET(mcm_dw + L->mpcc_mcm_3dlut_delta));
            entry->mpcc_mcm_1dlut_control  = amd_dcn_read_opt(ops, AMD_DCN_SEG3_BYTE_OFFSET(mcm_dw + L->mpcc_mcm_1dlut_delta));
            if (L->mpcc_mcm_first_gamut_delta != 0U)
                entry->mpcc_mcm_first_gamut_remap = amd_dcn_read_opt(ops, AMD_DCN_SEG3_BYTE_OFFSET(mcm_dw + L->mpcc_mcm_first_gamut_delta));
            if (L->mpcc_mcm_second_gamut_delta != 0U)
                entry->mpcc_mcm_second_gamut_remap = amd_dcn_read_opt(ops, AMD_DCN_SEG3_BYTE_OFFSET(mcm_dw + L->mpcc_mcm_second_gamut_delta));
        } else {
            struct amd_dcn_rmu_pair rmus[AMD_MPC_RMU_MAX_COUNT];
            if (amd_dcn_get_bound_rmus(ops, L, m, rmus) > 0U) {
                entry->mpcc_mcm_shaper_control = amd_dcn_read_opt(ops, rmus[0].shaper_reg);
                entry->mpcc_mcm_3dlut_mode     = amd_dcn_read_opt(ops, rmus[0].lut3d_reg);
            }
        }

        uint32_t dpp_off = dpp_idx * AMD_DCN_DPP_DWORD_STRIDE;
        entry->dpp_cnvc_surface_fmt = amd_dcn_read_opt(ops, AMD_REG_DPP_CNVC_SURFACE_FMT(dpp_idx));
        entry->dpp_cnvc_fmt_ctrl    = amd_dcn_read_opt(ops, AMD_REG_DPP_CNVC_FMT_CTRL(dpp_idx));
        entry->dpp_cnvc_pre_degam   = amd_dcn_read_opt(ops, AMD_REG_DPP_CNVC_PRE_DEGAM(dpp_idx));
        entry->dpp_dscl_scl_mode    = amd_dcn_read_opt(ops, AMD_DCN_REG_BYTE_OFFSET(L->dscl_scl_mode_dw + dpp_off));
        if (L->dscl_easf_h_dw != 0U)
            entry->dpp_dscl_easf_h_mode = amd_dcn_read_opt(ops, AMD_DCN_REG_BYTE_OFFSET(L->dscl_easf_h_dw + dpp_off));
        if (L->dscl_easf_v_dw != 0U)
            entry->dpp_dscl_easf_v_mode = amd_dcn_read_opt(ops, AMD_DCN_REG_BYTE_OFFSET(L->dscl_easf_v_dw + dpp_off));
        if (L->dscl_isharp_dw != 0U)
            entry->dpp_dscl_isharp_mode = amd_dcn_read_opt(ops, AMD_DCN_REG_BYTE_OFFSET(L->dscl_isharp_dw + dpp_off));
        entry->dpp_cm_control       = amd_dcn_read_opt(ops, AMD_DCN_REG_BYTE_OFFSET(L->cm_control_dw + dpp_off));
        entry->dpp_cm_post_csc_ctrl = amd_dcn_read_opt(ops, AMD_DCN_REG_BYTE_OFFSET(L->cm_post_csc_dw + dpp_off));
        if (L->cm_gamut_remap_dw != 0U)
            entry->dpp_cm_gamut_remap_ctrl = amd_dcn_read_opt(ops, AMD_DCN_REG_BYTE_OFFSET(L->cm_gamut_remap_dw + dpp_off));
        entry->dpp_cm_gamcor_ctrl   = amd_dcn_read_opt(ops, AMD_DCN_REG_BYTE_OFFSET(L->cm_gamcor_dw + dpp_off));
        entry->dpp_cm_hdr_mult_coef = amd_dcn_read_opt(ops, AMD_DCN_REG_BYTE_OFFSET(L->cm_hdr_mult_dw + dpp_off));
    }
}

bool dvbridge_win_amd_dcn_bypass_pipeline(
    const struct dvbridge_mmio_ops *ops,
    enum dvbridge_amd_dcn_tier tier,
    const struct dvbridge_amd_dcn_params *params,
    bool is_hpo_frl,
    uint32_t fmt_idx,
    struct dvbridge_amd_dcn_reg_save *save)
{
    if (!ops || !ops->read32 || !ops->write32 || !params || fmt_idx >= AMD_DCN_MAX_PIPES)
        return false;

    const struct amd_dcn_tier_layout *L = amd_dcn_get_tier_layout(tier);
    bool changed = false;

    /* Record core packet/FMT registers before apply_params mutates them */
    if (save) {
        dvbridge_win_amd_dcn_record_reg(save, params->pkt01_line_reg, amd_dcn_read_opt(ops, params->pkt01_line_reg));
        dvbridge_win_amd_dcn_record_reg(save, params->pkt45_line_reg, amd_dcn_read_opt(ops, params->pkt45_line_reg));
        dvbridge_win_amd_dcn_record_reg(save, params->gen_ctrl0_reg, amd_dcn_read_opt(ops, params->gen_ctrl0_reg));
        dvbridge_win_amd_dcn_record_reg(save, params->fmt_bit_depth_reg, amd_dcn_read_opt(ops, params->fmt_bit_depth_reg));
        if (is_hpo_frl) {
            dvbridge_win_amd_dcn_record_reg(save, AMD_REG_DCN_HPO_TB_GEN_CTRL1,
                                            amd_dcn_read_opt(ops, AMD_REG_DCN_HPO_TB_GEN_CTRL1));
            dvbridge_win_amd_dcn_record_reg(save, AMD_REG_DCN_HPO_TB_PKT10_11_LINE,
                                            amd_dcn_read_opt(ops, AMD_REG_DCN_HPO_TB_PKT10_11_LINE));
        }
    }

    /* 1. DIG/HPO HDMI controls: disable double buffering, AMD HW DV, deep color, YCbCr enc, AVMUTE */
    if (!is_hpo_frl) {
        uint32_t db_reg = amd_dcn_dio_rel_reg(params, false, AMD_DCN3_DIG_HDMI_DB_CONTROL_DELTA);
        uint32_t hdmi_ctl_reg = amd_dcn_dio_rel_reg(params, false, AMD_DCN3_DIG_HDMI_CONTROL_DELTA);
        if (amd_dcn_update_bits(ops, db_reg, 0U, AMD_HDMI_DB_DISABLE_MASK, save))
            changed = true;
        if (amd_dcn_update_bits(ops, hdmi_ctl_reg, AMD_HDMI_CTL_DV_TUNNEL_CLEAR_MASK, 0U, save))
            changed = true;
    } else {
        if (amd_dcn_update_bits(ops, AMD_REG_DCN_HPO_TB_DB_CTRL, 0U, AMD_HDMI_DB_DISABLE_MASK, save))
            changed = true;
        if (amd_dcn_update_bits(ops, AMD_REG_DCN_HPO_TB_PIXEL_FORMAT, AMD_HPO_PF_DV_TUNNEL_CLEAR_MASK, 0U, save))
            changed = true;
        if (amd_dcn_update_bits(ops, AMD_REG_DCN_HPO_TB_GC_CTRL, AMD_HPO_GC_AVMUTE_MASK, 0U, save))
            changed = true;
    }

    /* 2. OPP/FMT: bypass dynamic expansion, enforce RGB 4:4:4, bypass pixel clamping */
    if (amd_dcn_update_bits(ops, AMD_REG_FMT_DYNAMIC_EXP_CNTL(fmt_idx), AMD_FMT_DYNAMIC_EXP_ALL_MASK, 0U, save))
        changed = true;
    if (amd_dcn_update_bits(ops, AMD_REG_FMT_CONTROL(fmt_idx), AMD_FMT_COLOR_MASK, 0U, save))
        changed = true;
    if (amd_dcn_update_bits(ops, AMD_REG_FMT_CLAMP_CNTL(fmt_idx), AMD_FMT_CLAMP_ALL_MASK, 0U, save))
        changed = true;

    /* 3. MPC_OUT: bypass output CSC (preserve MPC_OUT_DENORM_CONTROL U1.13->8bpc mode!) */
    uint32_t mpc_out_csc_reg = amd_dcn_mpc_out_csc_reg(L, fmt_idx);
    if (amd_dcn_update_bits(ops, mpc_out_csc_reg, AMD_MODE_2BIT_MASK, 0U, save))
        changed = true;

    /* 4. Active MPCC + paired DPP pipes feeding fmt_idx */
    struct amd_dcn_active_mpcc active[AMD_DCN_MAX_PIPES];
    uint32_t active_cnt = amd_dcn_find_active_mpccs(ops, L, fmt_idx, active);
    for (uint32_t i = 0U; i < active_cnt; ++i) {
        uint32_t m = active[i].mpcc_idx;
        uint32_t dpp_idx = active[i].dpp_idx;

        uint32_t ogam_dw = L->mpcc_ogam_base + m * L->mpcc_ogam_stride;
        if (amd_dcn_update_bits(ops, AMD_DCN_SEG3_BYTE_OFFSET(ogam_dw), AMD_MODE_2BIT_MASK, 0U, save))
            changed = true;
        if (amd_dcn_update_bits(ops, AMD_DCN_SEG3_BYTE_OFFSET(ogam_dw + L->mpcc_gamut_remap_delta), AMD_MODE_2BIT_MASK, 0U, save))
            changed = true;

        if (L->mpcc_mcm_shaper_base != 0U) {
            uint32_t mcm_dw = L->mpcc_mcm_shaper_base + m * L->mpcc_mcm_stride;
            if (amd_dcn_update_bits(ops, AMD_DCN_SEG3_BYTE_OFFSET(mcm_dw), AMD_MODE_2BIT_MASK, 0U, save))
                changed = true;
            if (amd_dcn_update_bits(ops, AMD_DCN_SEG3_BYTE_OFFSET(mcm_dw + L->mpcc_mcm_3dlut_delta), AMD_MODE_2BIT_MASK, 0U, save))
                changed = true;
            if (amd_dcn_update_bits(ops, AMD_DCN_SEG3_BYTE_OFFSET(mcm_dw + L->mpcc_mcm_1dlut_delta), AMD_MODE_2BIT_MASK, 0U, save))
                changed = true;
            if (L->mpcc_mcm_first_gamut_delta != 0U &&
                amd_dcn_update_bits(ops, AMD_DCN_SEG3_BYTE_OFFSET(mcm_dw + L->mpcc_mcm_first_gamut_delta), AMD_MODE_2BIT_MASK, 0U, save))
                changed = true;
            if (L->mpcc_mcm_second_gamut_delta != 0U &&
                amd_dcn_update_bits(ops, AMD_DCN_SEG3_BYTE_OFFSET(mcm_dw + L->mpcc_mcm_second_gamut_delta), AMD_MODE_2BIT_MASK, 0U, save))
                changed = true;
        } else {
            struct amd_dcn_rmu_pair rmus[AMD_MPC_RMU_MAX_COUNT];
            uint32_t rmu_cnt = amd_dcn_get_bound_rmus(ops, L, m, rmus);
            for (uint32_t r = 0U; r < rmu_cnt; ++r) {
                if (amd_dcn_update_bits(ops, rmus[r].shaper_reg, AMD_MODE_2BIT_MASK, 0U, save))
                    changed = true;
                if (amd_dcn_update_bits(ops, rmus[r].lut3d_reg, AMD_MODE_2BIT_MASK, 0U, save))
                    changed = true;
            }
        }

        uint32_t dpp_off = dpp_idx * AMD_DCN_DPP_DWORD_STRIDE;
        if (amd_dcn_update_bits(ops, AMD_REG_DPP_CNVC_PRE_DEGAM(dpp_idx), AMD_CNVC_PRE_DEGAM_MODE_MASK, 0U, save))
            changed = true;
        if (L->dscl_easf_h_dw != 0U &&
            amd_dcn_update_bits(ops, AMD_DCN_REG_BYTE_OFFSET(L->dscl_easf_h_dw + dpp_off), AMD_DSCL_EASF_EN_MASK, 0U, save))
            changed = true;
        if (L->dscl_easf_v_dw != 0U &&
            amd_dcn_update_bits(ops, AMD_DCN_REG_BYTE_OFFSET(L->dscl_easf_v_dw + dpp_off), AMD_DSCL_EASF_EN_MASK, 0U, save))
            changed = true;
        if (L->dscl_isharp_dw != 0U &&
            amd_dcn_update_bits(ops, AMD_DCN_REG_BYTE_OFFSET(L->dscl_isharp_dw + dpp_off), AMD_DSCL_ISHARP_EN_MASK, 0U, save))
            changed = true;
        if (amd_dcn_update_bits(ops, AMD_DCN_REG_BYTE_OFFSET(L->cm_control_dw + dpp_off), 0U, AMD_CM_BYPASS_MASK, save))
            changed = true;
        if (amd_dcn_update_bits(ops, AMD_DCN_REG_BYTE_OFFSET(L->cm_post_csc_dw + dpp_off), AMD_MODE_2BIT_MASK, 0U, save))
            changed = true;
        if (L->cm_gamut_remap_dw != 0U &&
            amd_dcn_update_bits(ops, AMD_DCN_REG_BYTE_OFFSET(L->cm_gamut_remap_dw + dpp_off), AMD_MODE_2BIT_MASK, 0U, save))
            changed = true;
        if (amd_dcn_update_bits(ops, AMD_DCN_REG_BYTE_OFFSET(L->cm_gamcor_dw + dpp_off), AMD_MODE_2BIT_MASK, 0U, save))
            changed = true;
        if (amd_dcn_update_bits(ops, AMD_DCN_REG_BYTE_OFFSET(L->cm_hdr_mult_dw + dpp_off), AMD_CM_HDR_MULT_COEF_MASK, AMD_CM_HDR_MULT_UNITY, save))
            changed = true;
    }

    return changed;
}

static bool amd_dcn_reg_differs(
    const struct dvbridge_mmio_ops *ops,
    uint32_t reg_offset,
    uint32_t mask,
    uint32_t expected)
{
    if (reg_offset == 0U)
        return false;
    uint32_t v = ops->read32(ops->user_ctx, reg_offset);
    if (v == 0xFFFFFFFFU)
        return false;
    return (v & mask) != expected;
}

bool dvbridge_win_amd_dcn_needs_reapply(
    const struct dvbridge_mmio_ops *ops,
    enum dvbridge_amd_dcn_tier tier,
    const struct dvbridge_amd_dcn_params *params,
    bool is_hpo_frl,
    uint32_t fmt_idx)
{
    if (!ops || !ops->read32 || !params || fmt_idx >= AMD_DCN_MAX_PIPES)
        return false;

    const struct amd_dcn_tier_layout *L = amd_dcn_get_tier_layout(tier);

    if (amd_dcn_reg_differs(ops, params->fmt_bit_depth_reg, AMD_FMT_ALL_DITHER_TRUNC_MASK,
                            AMD_FMT_TRUNCATE_EN_MASK | AMD_FMT_TRUNCATE_DEPTH_8BPC) ||
        amd_dcn_reg_differs(ops, params->gen_ctrl0_reg,
                            is_hpo_frl ? 0x00030033U : 0x00330033U, 0x00000033U) ||
        amd_dcn_reg_differs(ops, AMD_REG_FMT_DYNAMIC_EXP_CNTL(fmt_idx), AMD_FMT_DYNAMIC_EXP_ALL_MASK, 0U) ||
        amd_dcn_reg_differs(ops, AMD_REG_FMT_CONTROL(fmt_idx), AMD_FMT_COLOR_MASK, 0U) ||
        amd_dcn_reg_differs(ops, AMD_REG_FMT_CLAMP_CNTL(fmt_idx), AMD_FMT_CLAMP_ALL_MASK, 0U))
        return true;

    if (!is_hpo_frl &&
        amd_dcn_reg_differs(ops, amd_dcn_dio_rel_reg(params, false, AMD_DCN3_DIG_HDMI_CONTROL_DELTA),
                            AMD_HDMI_CTL_DV_TUNNEL_CLEAR_MASK, 0U))
        return true;

    if (is_hpo_frl &&
        (amd_dcn_reg_differs(ops, AMD_REG_DCN_HPO_TB_PIXEL_FORMAT, AMD_HPO_PF_DV_TUNNEL_CLEAR_MASK, 0U) ||
         amd_dcn_reg_differs(ops, AMD_REG_DCN_HPO_TB_GEN_CTRL1, AMD_HPO_GEN_CTRL1_GENERIC11_MASK, 0U)))
        return true;

    uint32_t mpc_out_csc_reg = amd_dcn_mpc_out_csc_reg(L, fmt_idx);
    if (amd_dcn_reg_differs(ops, mpc_out_csc_reg, AMD_MODE_2BIT_MASK, 0U))
        return true;

    struct amd_dcn_active_mpcc active[AMD_DCN_MAX_PIPES];
    uint32_t active_cnt = amd_dcn_find_active_mpccs(ops, L, fmt_idx, active);
    for (uint32_t i = 0U; i < active_cnt; ++i) {
        uint32_t m = active[i].mpcc_idx;
        uint32_t dpp_idx = active[i].dpp_idx;

        uint32_t ogam_dw = L->mpcc_ogam_base + m * L->mpcc_ogam_stride;
        if (amd_dcn_reg_differs(ops, AMD_DCN_SEG3_BYTE_OFFSET(ogam_dw), AMD_MODE_2BIT_MASK, 0U) ||
            amd_dcn_reg_differs(ops, AMD_DCN_SEG3_BYTE_OFFSET(ogam_dw + L->mpcc_gamut_remap_delta), AMD_MODE_2BIT_MASK, 0U))
            return true;

        if (L->mpcc_mcm_shaper_base != 0U) {
            uint32_t mcm_dw = L->mpcc_mcm_shaper_base + m * L->mpcc_mcm_stride;
            if (amd_dcn_reg_differs(ops, AMD_DCN_SEG3_BYTE_OFFSET(mcm_dw), AMD_MODE_2BIT_MASK, 0U) ||
                amd_dcn_reg_differs(ops, AMD_DCN_SEG3_BYTE_OFFSET(mcm_dw + L->mpcc_mcm_3dlut_delta), AMD_MODE_2BIT_MASK, 0U) ||
                amd_dcn_reg_differs(ops, AMD_DCN_SEG3_BYTE_OFFSET(mcm_dw + L->mpcc_mcm_1dlut_delta), AMD_MODE_2BIT_MASK, 0U))
                return true;
            if (L->mpcc_mcm_first_gamut_delta != 0U &&
                amd_dcn_reg_differs(ops, AMD_DCN_SEG3_BYTE_OFFSET(mcm_dw + L->mpcc_mcm_first_gamut_delta), AMD_MODE_2BIT_MASK, 0U))
                return true;
            if (L->mpcc_mcm_second_gamut_delta != 0U &&
                amd_dcn_reg_differs(ops, AMD_DCN_SEG3_BYTE_OFFSET(mcm_dw + L->mpcc_mcm_second_gamut_delta), AMD_MODE_2BIT_MASK, 0U))
                return true;
        } else {
            struct amd_dcn_rmu_pair rmus[AMD_MPC_RMU_MAX_COUNT];
            uint32_t rmu_cnt = amd_dcn_get_bound_rmus(ops, L, m, rmus);
            for (uint32_t r = 0U; r < rmu_cnt; ++r) {
                if (amd_dcn_reg_differs(ops, rmus[r].shaper_reg, AMD_MODE_2BIT_MASK, 0U) ||
                    amd_dcn_reg_differs(ops, rmus[r].lut3d_reg, AMD_MODE_2BIT_MASK, 0U))
                    return true;
            }
        }

        uint32_t dpp_off = dpp_idx * AMD_DCN_DPP_DWORD_STRIDE;
        if (amd_dcn_reg_differs(ops, AMD_REG_DPP_CNVC_PRE_DEGAM(dpp_idx), AMD_CNVC_PRE_DEGAM_MODE_MASK, 0U) ||
            amd_dcn_reg_differs(ops, AMD_DCN_REG_BYTE_OFFSET(L->cm_control_dw + dpp_off), AMD_CM_BYPASS_MASK, AMD_CM_BYPASS_MASK) ||
            amd_dcn_reg_differs(ops, AMD_DCN_REG_BYTE_OFFSET(L->cm_post_csc_dw + dpp_off), AMD_MODE_2BIT_MASK, 0U) ||
            amd_dcn_reg_differs(ops, AMD_DCN_REG_BYTE_OFFSET(L->cm_gamcor_dw + dpp_off), AMD_MODE_2BIT_MASK, 0U) ||
            amd_dcn_reg_differs(ops, AMD_DCN_REG_BYTE_OFFSET(L->cm_hdr_mult_dw + dpp_off), AMD_CM_HDR_MULT_COEF_MASK, AMD_CM_HDR_MULT_UNITY))
            return true;
        if (L->cm_gamut_remap_dw != 0U &&
            amd_dcn_reg_differs(ops, AMD_DCN_REG_BYTE_OFFSET(L->cm_gamut_remap_dw + dpp_off), AMD_MODE_2BIT_MASK, 0U))
            return true;
        if ((L->dscl_easf_h_dw != 0U && amd_dcn_reg_differs(ops, AMD_DCN_REG_BYTE_OFFSET(L->dscl_easf_h_dw + dpp_off), AMD_DSCL_EASF_EN_MASK, 0U)) ||
            (L->dscl_easf_v_dw != 0U && amd_dcn_reg_differs(ops, AMD_DCN_REG_BYTE_OFFSET(L->dscl_easf_v_dw + dpp_off), AMD_DSCL_EASF_EN_MASK, 0U)) ||
            (L->dscl_isharp_dw != 0U && amd_dcn_reg_differs(ops, AMD_DCN_REG_BYTE_OFFSET(L->dscl_isharp_dw + dpp_off), AMD_DSCL_ISHARP_EN_MASK, 0U)))
            return true;
    }

    return false;
}

void dvbridge_win_amd_dcn_restore_saved(
    const struct dvbridge_mmio_ops *ops,
    struct dvbridge_amd_dcn_reg_save *save)
{
    if (!ops || !ops->write32 || !save)
        return;
    for (uint32_t i = save->count; i > 0U; --i) {
        const struct dvbridge_amd_dcn_saved_reg *e = &save->entries[i - 1U];
        ops->write32(ops->user_ctx, e->reg_offset, e->orig_value);
    }
    save->count = 0U;
}
