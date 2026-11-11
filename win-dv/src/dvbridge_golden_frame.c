// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Windows Native HDMI Dolby Vision RGB8 Tunneling Engine & Self-Test Suite
 * Implements:
 *   1. Exact ETSI GS CCM 001 V1.1.1 / CB1 RGB8 4:4:4 8-bit HDMI Tunneling Packer & Unpacker
 *   2. EDID CEA-861 Dolby Vision VSVDB (OUI 0x00D046 v0/v1/v2) Parser Verification
 *   3. Intel Gen9-14 & AMD DCN 3.1.2 (Rembrandt 0x1681) MMIO Register State Verification
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../include/dvbridge_core.h"
#include "../include/dvbridge_policy.h"
#include "../include/dvbridge_metadata.h"
#include "../include/dvbridge_win_mmio.h"

#define DV_TUNNEL_PACKET_SIZE 128

/*
 * Pack a 12-bit YCbCr 4:2:2 / 4:4:4 SMPTE ST.2084 (PQ) frame + 128-byte Dolby Vision
 * DM metadata packets into an 8-bit RGBA8/BGRA8 framebuffer using the exact
 * ETSI GS CCM 001 V1.1.1 / CB1 transport bit layout (matching pack_gles300.frag):
 *
 * For each horizontal pixel pair (x_even = 2*p, x_odd = 2*p + 1):
 *   Even pixel (2*p):
 *     R = Y0[11:4]
 *     G = Cb[11:4]
 *     B = (Y0[3:0] << 4) | (Cb[3:2] << 2) | (0 << 1) | m_bit
 *         where on row y == 0 and p < (num_packets * 512):
 *           byte_idx = p >> 3
 *           bit_idx  = 7 - (p & 7)
 *           m_bit    = (packets[byte_idx] >> bit_idx) & 1
 *         and on row y > 0 (or p >= num_packets * 512):
 *           m_bit    = Cr[0]
 *   Odd pixel (2*p + 1):
 *     R = Y1[11:4]
 *     G = Cr[11:4]
 *     B = (Y1[3:0] << 4) | (Cr[3:2] << 2) | (Cb[1] << 1) | Cr[1]
 */
/*
 * Compute 12-bit parity using the exact ETSI GS CCM 001 / cb1_transport_glsl.h
 * lookup polynomial: (0x6996u >> (p & 15u)) & 1u
 */
static inline uint32_t cb1_parity12(uint32_t v)
{
    v ^= v >> 8U;
    v ^= v >> 4U;
    return (0x6996U >> (v & 15U)) & 1U;
}

/*
 * Pack a 12-bit YCbCr 4:2:2 SMPTE ST.2084 (PQ) scanline + 128-byte Dolby Vision
 * DM metadata packets into an 8-bit RGBA8 framebuffer using the exact
 * ETSI GS CCM 001 V1.1.1 Clause 6.4 / cb1_transport_glsl.h layout:
 *
 *   1. Each pixel `raster = y_row * width + x` carries `code = (Y, C)`:
 *      - Even pixel (x = 2*p):     code.x = Y0 (12-bit), code.y = Cb (12-bit)
 *      - Odd pixel  (x = 2*p + 1): code.x = Y1 (12-bit), code.y = Cr (12-bit)
 *   2. Metadata scrambling (`cb1_packet`):
 *      - `packet = raster / 3072u` (each 1024-bit packet is repeated 3x across 3072 pixels)
 *      - If `packet < num_packets`:
 *        `bit = raster % 1024u`, `address = packet * 128u + bit / 8u`
 *        `data = (tunnel_packets[address] >> (7u - (bit % 8u))) & 1u`
 *        `parity = cb1_parity12((code.y >> 1u) ^ code.x)`
 *        `code.y = (code.y & 0xFFEu) | (data ^ parity)`
 *   3. Three-byte RGB8 carrier (`cb1_carrier`):
 *      - `R = code.y >> 4u`
 *      - `G = code.x >> 4u`
 *      - `B = ((code.y & 15u) << 4u) | (code.x & 15u)`
 *      - `A = 0xFF`
 */
void dvbridge_win_pack_rgb8_scanline(
    uint8_t *dst_rgba,
    int width,
    int y_row,
    const uint16_t *src_y,  /* width elements, 12-bit [0..4095] */
    const uint16_t *src_cb, /* width/2 elements, 12-bit [0..4095] */
    const uint16_t *src_cr, /* width/2 elements, 12-bit [0..4095] */
    const uint8_t *tunnel_packets,
    int num_packets)
{
    for (int x = 0; x < width; ++x) {
        uint32_t code_x = (uint32_t)(src_y[x] & 0xFFFU);
        uint32_t code_y = (uint32_t)(((x & 1) ? src_cr[x >> 1] : src_cb[x >> 1]) & 0xFFFU);

        uint32_t raster = (uint32_t)y_row * (uint32_t)width + (uint32_t)x;
        uint32_t packet = raster / 3072U;
        if (tunnel_packets && (int)packet < num_packets) {
            uint32_t bit     = raster % 1024U;
            uint32_t address = packet * 128U + (bit >> 3U);
            uint32_t data    = (tunnel_packets[address] >> (7U - (bit & 7U))) & 1U;
            uint32_t parity  = cb1_parity12((code_y >> 1U) ^ code_x);
            code_y = (code_y & 0xFFEU) | (data ^ parity);
        }

        dst_rgba[x * 4 + 0] = (uint8_t)((code_y >> 4U) & 0xFFU);
        dst_rgba[x * 4 + 1] = (uint8_t)((code_x >> 4U) & 0xFFU);
        dst_rgba[x * 4 + 2] = (uint8_t)(((code_y & 0x0FU) << 4U) | (code_x & 0x0FU));
        dst_rgba[x * 4 + 3] = 0xFFU;
    }
}

/*
 * Extract any of the 3 redundant copies (`copy_idx = 0, 1, 2`) of a 128-byte
 * tunnel packet from a packed RGBA8 framebuffer and verify its ETSI GS CCM 001 V1.1.1
 * Clause 6.3 IEEE CRC-32 checksum (dvbridge_dv_crc(pkt, 128) == 0).
 */
bool dvbridge_win_extract_and_verify_packet(
    const uint8_t *rgba_pixels,
    int total_pixels,
    int packet_idx,
    int copy_idx,
    uint8_t out_pkt[DV_TUNNEL_PACKET_SIZE])
{
    int start_raster = packet_idx * 3072 + copy_idx * 1024;
    if (start_raster + 1024 > total_pixels)
        return false;

    memset(out_pkt, 0, DV_TUNNEL_PACKET_SIZE);
    for (int bit = 0; bit < 1024; ++bit) {
        int raster = start_raster + bit;
        uint32_t r = rgba_pixels[raster * 4 + 0];
        uint32_t g = rgba_pixels[raster * 4 + 1];
        uint32_t b = rgba_pixels[raster * 4 + 2];

        uint32_t code_y = (r << 4U) | (b >> 4U);
        uint32_t code_x = (g << 4U) | (b & 0x0FU);
        uint32_t data   = cb1_parity12(code_y ^ code_x);

        out_pkt[bit >> 3] |= (uint8_t)(data << (7 - (bit & 7)));
    }

    uint32_t expected_crc = dvbridge_dv_crc(out_pkt, 124);
    uint32_t actual_crc   = AV_RB32(out_pkt + 124);
    return (expected_crc == actual_crc) && (dvbridge_dv_crc(out_pkt, 128) == 0U);
}

/* Mock MMIO register space for testing Intel & AMD hardware register programming */
struct mock_mmio_space {
    uint32_t regs[0x80000 / 4];
};

static uint32_t mock_mmio_read32(void *ctx, uint32_t reg_offset)
{
    struct mock_mmio_space *m = (struct mock_mmio_space *)ctx;
    assert(reg_offset < sizeof(m->regs));
    return m->regs[reg_offset >> 2];
}

static void mock_mmio_write32(void *ctx, uint32_t reg_offset, uint32_t value)
{
    struct mock_mmio_space *m = (struct mock_mmio_space *)ctx;
    assert(reg_offset < sizeof(m->regs));
    m->regs[reg_offset >> 2] = value;
}

int main(void)
{
    printf("=== Windows Native Dolby Vision Bridge (win-dv) Verification Suite ===\n\n");

    /* 1. Test Dolby Vision VSIF & AVI InfoFrame 9-dword Hardware Packing */
    struct dvbridge_hw_infoframe vsif_modern, vsif_legacy, avi;
    dvbridge_win_pack_dolby_vsif(&vsif_modern, true, false, false, 97); /* 4K60 CTA VIC 97 */
    printf("[1] Modern Dolby VSIF (OUI 0x00D046, TV-Led Standard):\n");
    printf("    DW0 (HB0..HB3) = 0x%08X (expected 0x001B0181)\n", vsif_modern.dw[0]);
    printf("    DW1 (PB0..PB3) = 0x%08X (expected 0x00D0464B)\n", vsif_modern.dw[1]);
    printf("    DW2 (PB4..PB7) = 0x%08X (expected 0x00000002)\n", vsif_modern.dw[2]);
    assert(vsif_modern.dw[0] == 0x001B0181U);
    assert(vsif_modern.dw[1] == 0x00D0464BU);
    assert(vsif_modern.dw[2] == 0x00000002U);

    dvbridge_win_pack_dolby_vsif(&vsif_legacy, true, false, true, 93); /* 4K24 CTA VIC 93 -> HDMI_VIC 3 */
    printf("    Legacy Dolby VSIF (OUI 0x000C03, 4K24 HDMI_VIC=3):\n");
    printf("    DW0 = 0x%08X, DW1 = 0x%08X, DW2 = 0x%08X\n",
           vsif_legacy.dw[0], vsif_legacy.dw[1], vsif_legacy.dw[2]);
    assert(vsif_legacy.dw[0] == 0x00180181U);
    assert((vsif_legacy.dw[1] & 0xFFFFFF00U) == 0x000C0300U);
    assert((vsif_legacy.dw[2] & 0x0000FFFFU) == 0x00000320U);

    dvbridge_win_pack_avi_infoframe(&avi, 97, false);
    printf("    AVI InfoFrame (RGB Full-Range, VIC=97): DW0=0x%08X DW1=0x%08X DW2=0x%08X\n",
           avi.dw[0], avi.dw[1], avi.dw[2]);
    assert(avi.dw[0] == 0x000D0282U);

    /* 2. Test Intel Gen9-14 MMIO Register Programming */
    struct mock_mmio_space *mmio = calloc(1, sizeof(*mmio));
    assert(mmio != NULL);
    struct dvbridge_mmio_ops ops = {
        .user_ctx = mmio,
        .read32   = mock_mmio_read32,
        .write32  = mock_mmio_write32,
    };

    /* Simulate Pipe A with 10-bpc YUV + Dither enabled + HDR10 DRM DIP enabled */
    mmio->regs[INTEL_REG_PIPE_MISC(0) >> 2] =
        INTEL_PIPE_MISC_COLORSPACE_YUV | INTEL_PIPE_MISC_BPC_10 | INTEL_PIPE_MISC_DITHER_ENABLE;
    mmio->regs[INTEL_REG_VIDEO_DIP_CTL(0) >> 2] =
        INTEL_VIDEO_DIP_ENABLE_DRM_GLK | INTEL_VIDEO_DIP_ENABLE_GCP_HSW;

    dvbridge_win_intel_apply_dv(&ops, 0, true, false, 97);

    uint32_t intel_pipe_misc = mmio->regs[INTEL_REG_PIPE_MISC(0) >> 2];
    uint32_t intel_dip_ctl   = mmio->regs[INTEL_REG_VIDEO_DIP_CTL(0) >> 2];
    uint32_t intel_vs_dw0    = mmio->regs[INTEL_REG_VIDEO_DIP_VS_DATA(0, 0) >> 2];
    uint32_t intel_vs_dw1    = mmio->regs[INTEL_REG_VIDEO_DIP_VS_DATA(0, 1) >> 2];
    uint32_t intel_vs_dw2    = mmio->regs[INTEL_REG_VIDEO_DIP_VS_DATA(0, 2) >> 2];
    printf("\n[2] Intel Gen9-14 Pipe A MMIO Programming Verified:\n");
    printf("    PIPE_MISC(A) [0x%05X] = 0x%08X (RGB=0, BPC=8, Dither=0)\n",
           INTEL_REG_PIPE_MISC(0), intel_pipe_misc);
    printf("    VIDEO_DIP_CTL(A) [0x%05X] = 0x%08X (VS_HSW=1, AVI_HSW=1, DRM_GLK=0)\n",
           INTEL_REG_VIDEO_DIP_CTL(0), intel_dip_ctl);
    printf("    VIDEO_DIP_VS_DATA(A,0..2) [0x%05X] = 0x%08X 0x%08X 0x%08X\n",
           INTEL_REG_VIDEO_DIP_VS_DATA(0, 0), intel_vs_dw0, intel_vs_dw1, intel_vs_dw2);
    assert((intel_pipe_misc & (INTEL_PIPE_MISC_COLORSPACE_YUV | INTEL_PIPE_MISC_BPC_MASK | INTEL_PIPE_MISC_DITHER_ENABLE)) == 0U);
    assert((intel_dip_ctl & INTEL_VIDEO_DIP_ENABLE_VS_HSW) != 0U);
    assert((intel_dip_ctl & INTEL_VIDEO_DIP_ENABLE_AVI_HSW) != 0U);
    assert((intel_dip_ctl & INTEL_VIDEO_DIP_ENABLE_DRM_GLK) == 0U);
    assert(intel_vs_dw0 == 0x001B0181U && intel_vs_dw1 == 0x00D0464BU && intel_vs_dw2 == 0x00000002U);

    /* 3. Test AMD DCN 3.1.2 (Rembrandt 0x1681) MMIO Register Programming */
    memset(mmio, 0, sizeof(*mmio));
    mmio->regs[AMD_REG_FMT_BIT_DEPTH_CONTROL(0) >> 2] =
        AMD_FMT_TRUNCATE_EN_MASK | AMD_FMT_SPATIAL_DITHER_EN_MASK | AMD_FMT_TEMPORAL_DITHER_EN_MASK;
    mmio->regs[AMD_REG_DCN3_HDMI_GEN_CTRL0(0) >> 2] = (0x3U << 16); /* HDRSMD enabled */

    dvbridge_win_amd_dcn3_apply_dv(&ops, 0, true, false, 97);

    uint32_t amd_fmt   = mmio->regs[AMD_REG_FMT_BIT_DEPTH_CONTROL(0) >> 2];
    uint32_t amd_ctrl0 = mmio->regs[AMD_REG_DCN3_HDMI_GEN_CTRL0(0) >> 2];
    uint32_t amd_ctrl1 = mmio->regs[AMD_REG_DCN3_HDMI_GEN_CTRL1(0) >> 2];
    uint32_t amd_immed = mmio->regs[AMD_REG_DCN3_VPG_IMMED_UPDATE(0) >> 2];
    printf("\n[3] AMD DCN 3.1.2 (Rembrandt 0x1681) Pipe 0 MMIO Programming Verified:\n");
    printf("    FMT0_BIT_DEPTH_CONTROL [0x%05X] = 0x%08X (Truncate=0, Spatial/Temporal Dither=0)\n",
           AMD_REG_FMT_BIT_DEPTH_CONTROL(0), amd_fmt);
    printf("    DIG0_HDMI_GENERIC_PACKET_CONTROL0 [0x%05X] = 0x%08X (GEN0=0x3, GEN1/VSIF=0x30, GEN4/HDR=0)\n",
           AMD_REG_DCN3_HDMI_GEN_CTRL0(0), amd_ctrl0);
    printf("    DIG0_HDMI_GENERIC_PACKET_CONTROL1 [0x%05X] = 0x%08X (GEN0_LINE=2, GEN1_LINE=2)\n",
           AMD_REG_DCN3_HDMI_GEN_CTRL1(0), amd_ctrl1);
    printf("    VPG0_GSP_IMMEDIATE_UPDATE_CTRL    [0x%05X] = 0x%08X\n",
           AMD_REG_DCN3_VPG_IMMED_UPDATE(0), amd_immed);
    assert(amd_fmt == 0U);
    assert(amd_ctrl0 == 0x00000033U);
    assert(amd_ctrl1 == 0x00020002U);

    /* 4. Test EDID Dolby Vision VSVDB Parser (Simulated LG OLED C2/C3/G4 VSVDB v1/v2) */
    uint8_t fake_edid[256];
    memset(fake_edid, 0, sizeof(fake_edid));
    /* Base EDID header + 1 extension block */
    const uint8_t edid_hdr[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    memcpy(fake_edid, edid_hdr, 8);
    fake_edid[126] = 1; /* 1 CEA-861 extension block */
    uint8_t sum0 = 0;
    for (int i = 0; i < 127; ++i) sum0 += fake_edid[i];
    fake_edid[127] = (uint8_t)((0x100U - sum0) & 0xFFU);

    /* CEA-861-G Extension Block at offset 128 */
    uint8_t *cea = &fake_edid[128];
    cea[0] = 0x02; /* CEA-861 tag */
    cea[1] = 0x03; /* Revision 3 */
    cea[2] = 4 + 12; /* DTD offset = 16 */
    /* Vendor-Specific Video Data Block (Tag=7, Length=11, ExtTag=0x01, OUI=0x00D046, VSVDB v1) */
    cea[4]  = (7U << 5) | 11U; /* Extended Tag (7), payload length 11 */
    cea[5]  = 0x01U;           /* Video Capability / VSVDB Extended Tag = 0x01 */
    cea[6]  = 0x46U;           /* Dolby OUI 0x00D046 LSB */
    cea[7]  = 0xD0U;
    cea[8]  = 0x00U;           /* Dolby OUI 0x00D046 MSB */
    cea[9]  = (1U << 5) | (1U << 1) | 1U; /* Version=1, DM v3, 2160p60=1, YUV422=1 */
    cea[10] = (65U << 1) | 0U; /* data[5]: Target Max PQ, Low-Latency=0 */
    cea[11] = (10U << 3) | 0U; /* data[6]: Target Min PQ */
    cea[12] = 0x01U;           /* data[7]: bit 0 = 1 -> supports Dolby VSIF (OUI 0x00D046) */
    uint8_t sum1 = 0;
    for (int i = 0; i < 127; ++i) sum1 += cea[i];
    cea[127] = (uint8_t)((0x100U - sum1) & 0xFFU);

    struct dvbridge_sink_caps caps = dvbridge_parse_edid(fake_edid, sizeof(fake_edid));
    printf("\n[4] EDID Dolby Vision VSVDB Parser (drm_dvbridge_edid.h) Verified:\n");
    printf("    status=%d (%s), version=%u, supports_2160p60=%d, legacy_vsif=%d\n",
           caps.status, dvbridge_edid_reason(caps.status),
           caps.version, caps.supports_2160p60, caps.legacy_vsif);
    assert(caps.status == DV_EDID_STANDARD);
    assert(caps.version == 1);
    assert(caps.supports_2160p60 == true);
    assert(caps.legacy_vsif == false);

    /* 5. Test ETSI GS CCM 001 V1.1.1 Tunnel Packet Builder (dvbridge_core.c) +
     *    Clause 6.4 RGB8 Carrier + 3x Parity Scrambling + IEEE CRC-32 Round-Trip */
    struct dvbridge_context *dv_ctx = dvbridge_create();
    assert(dv_ctx != NULL);

    size_t dovi_bytes = 0;
    AVDOVIMetadata *dovi = av_dovi_metadata_alloc(&dovi_bytes);
    assert(dovi != NULL);
    dovi->num_ext_blocks = 2;
    AVDOVIRpuDataHeader *hdr = av_dovi_get_header(dovi);
    hdr->disable_residual_flag = 1;
    hdr->vdr_bit_depth = 12;
    hdr->bl_bit_depth = 10;

    AVDOVIColorMetadata *col = av_dovi_get_color(dovi);
    col->scene_refresh_flag = 1;
    col->signal_eotf = 65535;
    col->signal_bit_depth = 12;
    col->signal_color_space = 0;
    col->signal_full_range_flag = 1;
    col->source_min_pq = 7;
    col->source_max_pq = 3079; /* 1000 nits */
    col->source_diagonal = 42;

    AVDOVIDmData *l1 = av_dovi_get_ext(dovi, 0);
    l1->level = 1;
    l1->l1.min_pq = 12;
    l1->l1.max_pq = 2850;
    l1->l1.avg_pq = 1100;

    AVDOVIDmData *l2 = av_dovi_get_ext(dovi, 1);
    l2->level = 2;
    l2->l2.target_max_pq = 2851; /* 600 nits target */
    l2->l2.trim_slope = 2048;
    l2->l2.trim_offset = 2048;
    l2->l2.trim_power = 2048;
    l2->l2.trim_chroma_weight = 2048;
    l2->l2.trim_saturation_gain = 2048;
    l2->l2.ms_weight = 2048;

    struct dvbridge_geometry geom = {
        .source_width = 3840, .source_height = 2160,
        .x = 0, .y = 0, .width = 3840, .height = 2160,
        .target_width = 3840, .target_height = 2160,
    };
    struct dvbridge_candidate *cand = dvbridge_prepare(dv_ctx, dovi, dovi_bytes, 0.0, geom, false);
    assert(cand != NULL);

    unsigned num_packets = 0;
    const uint32_t *pkt_words = dvbridge_packets(cand, &num_packets);
    assert(pkt_words != NULL && num_packets >= 1);

    uint8_t tunnel_packets[5 * DV_TUNNEL_PACKET_SIZE];
    for (unsigned i = 0; i < num_packets * DV_TUNNEL_PACKET_SIZE; ++i)
        tunnel_packets[i] = (uint8_t)(pkt_words[i] & 0xFFU);

    int width = 3840;
    uint16_t *src_y  = calloc(width, sizeof(uint16_t));
    uint16_t *src_cb = calloc(width / 2, sizeof(uint16_t));
    uint16_t *src_cr = calloc(width / 2, sizeof(uint16_t));
    uint8_t  *row0   = calloc(width * 4, sizeof(uint8_t));
    for (int x = 0; x < width; ++x)
        src_y[x] = (uint16_t)((x * 4095) / (width - 1));
    for (int p = 0; p < width / 2; ++p) {
        src_cb[p] = 2048U;
        src_cr[p] = 2048U;
    }

    dvbridge_win_pack_rgb8_scanline(row0, width, 0, src_y, src_cb, src_cr, tunnel_packets, (int)num_packets);

    printf("\n[5] ETSI GS CCM 001 V1.1.1 Clause 6.4 RGB8 Carrier + 3x Parity Scrambling Verified:\n");
    printf("    Prepared %u tunnel packet(s) (%u bytes) via dvbridge_core.c\n",
           num_packets, num_packets * DV_TUNNEL_PACKET_SIZE);
    for (int copy = 0; copy < 3; ++copy) {
        uint8_t extracted_pkt[DV_TUNNEL_PACKET_SIZE];
        bool crc_ok = dvbridge_win_extract_and_verify_packet(row0, width, 0, copy, extracted_pkt);
        printf("    Copy[%d] (pixels %4d..%4d) Header=[0x%02X 0x%02X 0x%02X 0x%02X 0x%02X] CRC32=0x%08X -> CRC Valid: %s\n",
               copy, copy * 1024, (copy + 1) * 1024 - 1,
               extracted_pkt[0], extracted_pkt[1], extracted_pkt[2], extracted_pkt[3], extracted_pkt[4],
               AV_RB32(extracted_pkt + 124),
               crc_ok ? "YES" : "NO");
        assert(crc_ok == true);
        assert(memcmp(extracted_pkt, tunnel_packets, DV_TUNNEL_PACKET_SIZE) == 0);
    }

    bool committed = dvbridge_commit(dv_ctx, cand);
    assert(committed == true);
    dvbridge_candidate_destroy(cand);
    dvbridge_destroy(dv_ctx);
    av_free(dovi);

    free(src_y);
    free(src_cb);
    free(src_cr);
    free(row0);
    free(mmio);

    printf("\nALL 5 HARDWARE & PROTOCOL VERIFICATION TESTS PASSED!\n");
    return 0;
}
