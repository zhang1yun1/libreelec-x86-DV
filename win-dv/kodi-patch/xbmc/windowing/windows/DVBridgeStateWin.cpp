/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Windows Display & Hardware Transport State for CB1 / DVBridge (DVBridgeStateWin.cpp)
 */

#include "DVBridgeStateWin.h"
#include "utils/log.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifndef CB1_NO_LIBPLACEBO
#define CB1_NO_LIBPLACEBO 1
#endif
#include <algorithm>
extern "C" {
#include <libavutil/frame.h>
#include "dvbridge_fel.h"
#include "dvbridge_metadata.h"
}

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#include <dxgi1_6.h>
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "advapi32.lib")

static const GUID GUID_DEVINTERFACE_MONITOR_WIN_DV = {
    0xe6f07b5f, 0xee97, 0x4a90, {0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7}};
static const GUID GUID_DEVCLASS_DISPLAY_WIN_DV = {
    0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
#endif

namespace KODI::WINDOWING::WINDOWS
{

namespace
{
void ParseSinkHdrCapabilities(CDVBridgeStateWin::DisplaySinkInfo& info)
{
  if (info.rawEdid.size() < 128)
    return;

  info.dvCaps = dvbridge_parse_edid(info.rawEdid.data(), info.rawEdid.size());
  info.supportsDolbyVision =
      (info.dvCaps.status == DV_EDID_STANDARD || info.dvCaps.status == DV_EDID_LLDV_ONLY);

  const uint8_t* edid = info.rawEdid.data();
  const size_t edidLen = info.rawEdid.size();
  size_t extensions = edid[126];
  const size_t maxExt = (edidLen / 128) - 1;
  if (extensions > maxExt)
    extensions = maxExt;

  for (size_t extIdx = 1; extIdx <= extensions; ++extIdx)
  {
    const uint8_t* ext = edid + extIdx * 128;
    if (ext[0] != 0x02 || ext[1] < 0x03)
      continue;

    uint8_t sum = 0;
    for (size_t i = 0; i < 128; ++i)
      sum += ext[i];
    if (sum != 0)
      continue;

    const uint8_t dtdStart = ext[2];
    if (dtdStart < 4 || dtdStart > 127)
      continue;

    size_t pos = 4;
    while (pos < dtdStart)
    {
      const uint8_t tagHeader = ext[pos];
      const uint8_t tag = (tagHeader >> 5) & 0x07;
      const uint8_t len = tagHeader & 0x1F;
      if (pos + 1 + len > dtdStart)
        break;

      if (tag == 7 && len >= 2)
      {
        const uint8_t extTag = ext[pos + 1];
        if (extTag == 0x06 && len >= 3)
        {
          info.hdrEotfMask = ext[pos + 2];
          if ((info.hdrEotfMask & 0x04) != 0)
            info.supportsHDR10 = true;
          if ((info.hdrEotfMask & 0x08) != 0)
            info.supportsHLG = true;
        }
        else if (extTag == 0x01 && len >= 5)
        {
          const uint32_t oui = static_cast<uint32_t>(ext[pos + 2]) |
                               (static_cast<uint32_t>(ext[pos + 3]) << 8) |
                               (static_cast<uint32_t>(ext[pos + 4]) << 16);
          if (oui == 0x90848Bu)
            info.supportsHDR10Plus = true;
          else if (oui == 0x00D046u)
            info.supportsDolbyVision = true;
        }
      }
      pos += 1 + len;
    }
  }
}
} // namespace

CDVBridgeStateWin& CDVBridgeStateWin::GetInstance()
{
  static CDVBridgeStateWin s_instance;
  return s_instance;
}

void CDVBridgeStateWin::EnsureHardwareProbed()
{
  if (m_probed)
    return;
  m_probed = true;
  QueryPrimaryGpu(m_gpu);
  auto sinks = EnumerateDisplays();
  if (!sinks.empty())
  {
    m_sink = sinks.front();
    int bestRank = 0;
    for (const auto& s : sinks)
    {
      int rank = 0;
      if (s.dvCaps.status == DV_EDID_STANDARD)
        rank = 4;
      else if (s.supportsDolbyVision)
        rank = 3;
      else if (s.supportsHDR10Plus || s.supportsHDR10 || s.supportsHLG)
        rank = 2;
      else
        rank = 1;

      if (rank > bestRank)
      {
        bestRank = rank;
        m_sink = s;
      }
    }
  }
}

bool CDVBridgeStateWin::HasHDRDisplay()
{
  EnsureHardwareProbed();
  return m_sink.supportsHDR10 || m_sink.supportsHLG || m_sink.supportsHDR10Plus ||
         m_sink.supportsDolbyVision;
}

bool CDVBridgeStateWin::SupportsHDR10()
{
  EnsureHardwareProbed();
  return m_sink.supportsHDR10 || m_sink.supportsDolbyVision;
}

bool CDVBridgeStateWin::SupportsHLG()
{
  EnsureHardwareProbed();
  return m_sink.supportsHLG;
}

bool CDVBridgeStateWin::SupportsHDR10Plus()
{
  EnsureHardwareProbed();
  return m_sink.supportsHDR10Plus;
}

bool CDVBridgeStateWin::SupportsDolbyVision()
{
  EnsureHardwareProbed();
  return m_sink.supportsDolbyVision;
}

bool CDVBridgeStateWin::SupportsStandardDolbyVision()
{
  EnsureHardwareProbed();
  return m_sink.dvCaps.status == DV_EDID_STANDARD;
}

std::optional<unsigned> CDVBridgeStateWin::GetDisplayPeakPq()
{
  EnsureHardwareProbed();
  if (m_sink.dvCaps.status != DV_EDID_STANDARD ||
      m_sink.dvCaps.offset + m_sink.dvCaps.length > m_sink.rawEdid.size() ||
      m_sink.dvCaps.length < 7)
    return {};

  const uint8_t* data = m_sink.rawEdid.data() + m_sink.dvCaps.offset;
  if (m_sink.dvCaps.version == 0 && m_sink.dvCaps.length >= 10)
  {
    const unsigned pq = (static_cast<unsigned>(data[9]) << 4) | (data[8] >> 4);
    if (pq <= 4095)
      return pq;
  }
  else if (m_sink.dvCaps.version == 1)
  {
    const unsigned code = (data[5] >> 1) & 0x7fu;
    const double nits = 100.0 + 50.0 * code;
    const double l = std::clamp(nits / 10000.0, 0.0, 1.0);
    const double y = std::pow(l, 0.1593017578125);
    const double n = std::pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
    return static_cast<unsigned>(std::clamp(std::lround(n * 4095.0), 0L, 4095L));
  }
  else if (m_sink.dvCaps.version == 2)
  {
    const unsigned code = (data[6] >> 3) & 0x1fu;
    return 2055u + 65u * code;
  }
  return {};
}

DVBRIDGE::OutputCapabilities CDVBridgeStateWin::GetDVOutputCapabilities(bool activeDvSource)
{
  EnsureHardwareProbed();
  DVBRIDGE::OutputCapabilities caps{};
  caps.nativeDV = (m_sink.dvCaps.status == DV_EDID_STANDARD);
  caps.hdr10 = m_sink.supportsHDR10 || m_sink.supportsDolbyVision;
  caps.hdr10plus = m_sink.supportsHDR10Plus;
  caps.processingKnown = true;
  for (size_t mode = 1; mode < caps.processing.size(); ++mode)
  {
    caps.processing[mode] = {true, DVBRIDGE::OutputReason::None};
    caps.hdr10Processing[mode] = {true, DVBRIDGE::OutputReason::None};
  }
  if (activeDvSource)
  {
    caps.sourceKnown = true;
    caps.isDV = true;
  }
  return caps;
}

namespace
{
struct dvbridge_dv g_dvMetaState = [] {
  struct dvbridge_dv s{};
  s.last_pts = -1.0;
  s.fel_verified = true;
  return s;
}();

uint32_t g_savedIntelPipeMisc[4]{};
uint32_t g_savedIntelDipCtl[4]{};
uint32_t g_savedIntelAviData[4][8]{};
uint32_t g_savedIntelVsData[4][8]{};
uint32_t g_savedIntelCscMode[4]{};
uint32_t g_savedIntelGammaMode[4]{};
uint32_t g_savedIntelOutputCsc[4][12]{};
bool g_hasSavedIntelPipeMisc[4]{};

struct SavedAmdState
{
  bool valid{false};
  bool isHpoFrl{false};
  uint32_t digBeIdx{0};
  uint32_t digFeIdx{0};
  uint32_t fmtIdx{0};
  dvbridge_amd_dcn_params params{};
  uint32_t origFmtBitDepthCtrl{0};
  uint32_t origGenCtrl0{0};
  uint32_t origPkt01Line{0};
  uint32_t origPkt45Line{0};
} g_savedAmdState;

uint32_t g_mmioMappedSize = 0x100000U;

uint32_t MmioRead32Mapped(void* userCtx, uint32_t regOffset)
{
  if (!userCtx || regOffset + 4U > g_mmioMappedSize)
    return 0U;
  auto* ptr = reinterpret_cast<volatile uint32_t*>(static_cast<uint8_t*>(userCtx) + regOffset);
  return *ptr;
}

void MmioWrite32Mapped(void* userCtx, uint32_t regOffset, uint32_t value)
{
  if (!userCtx || regOffset + 4U > g_mmioMappedSize)
    return;
  auto* ptr = reinterpret_cast<volatile uint32_t*>(static_cast<uint8_t*>(userCtx) + regOffset);
  *ptr = value;
}

struct AmdDcnLayout
{
  uint32_t stride;
  uint32_t feCntlDword;
  uint32_t feClkCntlDword;
  uint32_t feEnCntlDword;
  uint32_t genCtrl0Dword;
  uint32_t beClkCntlDword;
  uint32_t beCntlDword;
  uint32_t beEnCntlDword;
  bool splitClkRegs;
};

struct AmdDcnTarget
{
  dvbridge_amd_dcn_params params{};
  uint32_t digBeIdx{0};
  uint32_t digFeIdx{0};
  uint32_t otgIdx{0};
  uint32_t fmtIdx{0};
  uint32_t genCtrl0Dword{AMD_DCN3_A_DIG0_HDMI_GEN_CTRL0};
  uint32_t digMode{0xFFU};
  bool isHpoFrl{false};
  bool found{false};
};

std::array<AmdDcnLayout, 3> GetOrderedAmdDcnLayouts(uint32_t deviceId)
{
  constexpr AmdDcnLayout kLayoutA{AMD_DCN3_DIG_DWORD_STRIDE,
                                  AMD_DCN3_A_DIG0_FE_CNTL,
                                  0U,
                                  0U,
                                  AMD_DCN3_A_DIG0_HDMI_GEN_CTRL0,
                                  0U,
                                  AMD_DCN3_A_DIG0_BE_CNTL,
                                  AMD_DCN3_A_DIG0_BE_CNTL + 1U,
                                  false};
  constexpr AmdDcnLayout kLayoutB{AMD_DCN3_DIG_DWORD_STRIDE,
                                  AMD_DCN3_B_DIG0_FE_CNTL,
                                  0U,
                                  0U,
                                  AMD_DCN3_B_DIG0_HDMI_GEN_CTRL0,
                                  0U,
                                  AMD_DCN3_B_DIG0_BE_CNTL,
                                  AMD_DCN3_B_DIG0_BE_CNTL + 1U,
                                  false};
  // DCN 3.5 / 3.5.1 / 3.6 / 4.01 (Strix, RX 9000): 0x124 stride + split clock/enable regs
  constexpr AmdDcnLayout kLayoutC{AMD_DCN3_C_DIG_DWORD_STRIDE,
                                  AMD_DCN3_C_DIG0_FE_CNTL,
                                  AMD_DCN3_C_DIG0_FE_CLK_CNTL,
                                  AMD_DCN3_C_DIG0_FE_EN_CNTL,
                                  AMD_DCN3_C_DIG0_HDMI_GEN_CTRL0,
                                  AMD_DCN3_C_DIG0_BE_CLK_CNTL,
                                  AMD_DCN3_C_DIG0_BE_CNTL,
                                  AMD_DCN3_C_DIG0_BE_EN_CNTL,
                                  true};

  if (deviceId == 0x150EU || deviceId == 0x1586U || deviceId == 0x1114U ||
      (deviceId >= 0x7550U && deviceId <= 0x759FU))
  {
    return {kLayoutC, kLayoutB, kLayoutA};
  }
  if (deviceId == 0x15BFU || deviceId == 0x15C8U || deviceId == 0x1900U ||
      (deviceId >= 0x7440U && deviceId <= 0x749FU))
  {
    return {kLayoutB, kLayoutA, kLayoutC};
  }
  return {kLayoutA, kLayoutB, kLayoutC};
}

dvbridge_amd_dcn_params MakeDioDcnParams(uint32_t feIdx,
                                         uint32_t fmtIdx,
                                         uint32_t genCtrl0Dword,
                                         uint32_t stride)
{
  dvbridge_amd_dcn_params p{};
  p.fmt_bit_depth_reg = AMD_REG_FMT_BIT_DEPTH_CONTROL(fmtIdx);
  p.vpg_access_reg = AMD_REG_DCN3_VPG_ACCESS_CTRL_S(feIdx, stride);
  p.gen_ctrl0_reg = AMD_DCN3_DIG_REG_OFFSET_S(feIdx, genCtrl0Dword, stride);
  p.pkt01_line_reg = AMD_DCN3_DIG_REG_OFFSET_S(feIdx, genCtrl0Dword + 4U, stride);
  p.pkt45_line_reg = AMD_DCN3_DIG_REG_OFFSET_S(feIdx, genCtrl0Dword + 5U, stride);
  return p;
}

uint32_t ResolveAmdFmtFromOtg(const dvbridge_mmio_ops& ops, uint32_t otgIdx)
{
  if (otgIdx >= AMD_DCN_MAX_PIPES)
    return 0U;
  const uint32_t opSel = ops.read32(ops.user_ctx, AMD_REG_ODM_OPTC_DATA_SOURCE(otgIdx));
  if (opSel != 0xFFFFFFFFU)
  {
    const uint32_t seg0 = (opSel & AMD_OPTC_SEG0_SRC_SEL_MASK) >> AMD_OPTC_SEG0_SRC_SEL_SHIFT;
    if (seg0 < AMD_DCN_MAX_PIPES)
      return seg0;
  }
  return otgIdx;
}

bool IsAmdOtgEnabled(const dvbridge_mmio_ops& ops, uint32_t otgIdx, bool splitClkRegs)
{
  if (otgIdx >= AMD_DCN_MAX_PIPES)
    return false;
  const uint32_t otgReg =
      splitClkRegs ? AMD_REG_OTG_CONTROL_C(otgIdx) : AMD_REG_OTG_CONTROL_A(otgIdx);
  const uint32_t otgCtrl = ops.read32(ops.user_ctx, otgReg);
  return otgCtrl != 0xFFFFFFFFU && (otgCtrl & AMD_OTG_MASTER_EN_MASK) != 0U;
}

AmdDcnTarget DetectAmdDcnTarget(const dvbridge_mmio_ops& ops, uint32_t deviceId)
{
  const auto layouts = GetOrderedAmdDcnLayouts(deviceId);
  AmdDcnTarget fallbackExt;
  std::array<bool, AMD_DCN_MAX_PIPES> dioActiveOtg{};

  for (const auto& layout : layouts)
  {
    for (uint32_t be = 0; be < AMD_DCN_MAX_DIGS; ++be)
    {
      const uint32_t beCntl = ops.read32(
          ops.user_ctx, AMD_DCN3_DIG_REG_OFFSET_S(be, layout.beCntlDword, layout.stride));
      const uint32_t beEnCntl = ops.read32(
          ops.user_ctx, AMD_DCN3_DIG_REG_OFFSET_S(be, layout.beEnCntlDword, layout.stride));
      if (beCntl == 0xFFFFFFFFU || beEnCntl == 0xFFFFFFFFU)
        continue;
      if ((beEnCntl & 1U) == 0U)
        continue;

      uint32_t digMode = 0xFFU;
      if (layout.splitClkRegs)
      {
        const uint32_t beClkCntl = ops.read32(
            ops.user_ctx, AMD_DCN3_DIG_REG_OFFSET_S(be, layout.beClkCntlDword, layout.stride));
        if (beClkCntl == 0xFFFFFFFFU || (beClkCntl & 0x10U) == 0U)
          continue;
        digMode = beClkCntl & 0x7U;
      }
      else
      {
        digMode = (beCntl >> 16) & 0x7U;
      }

      // DIG_BE_CNTL.DIG_FE_SOURCE_SELECT (bits 14:8) is a one-hot mask of the bound DIG_FE/VPG.
      const uint32_t feMask = (beCntl >> 8) & 0x7FU;
      if (feMask == 0U)
        continue;

      uint32_t fe = 0U;
      while (fe < AMD_DCN_MAX_DIGS && (feMask & (1U << fe)) == 0U)
        ++fe;
      if (fe >= AMD_DCN_MAX_DIGS)
        continue;

      const uint32_t feCntl = ops.read32(
          ops.user_ctx, AMD_DCN3_DIG_REG_OFFSET_S(fe, layout.feCntlDword, layout.stride));
      if (feCntl == 0xFFFFFFFFU)
        continue;

      if (layout.splitClkRegs)
      {
        const uint32_t feClkCntl = ops.read32(
            ops.user_ctx, AMD_DCN3_DIG_REG_OFFSET_S(fe, layout.feClkCntlDword, layout.stride));
        const uint32_t feEnCntl = ops.read32(
            ops.user_ctx, AMD_DCN3_DIG_REG_OFFSET_S(fe, layout.feEnCntlDword, layout.stride));
        if (feClkCntl == 0xFFFFFFFFU || feEnCntl == 0xFFFFFFFFU ||
            (feEnCntl & 1U) == 0U || (feClkCntl & 0x10U) == 0U)
        {
          continue;
        }
      }
      else
      {
        const bool symclkOn = ((feCntl >> 24) & 1U) != 0U;
        if (!symclkOn)
          continue;
      }

      const uint32_t otgIdx = feCntl & 0x7U;
      if (otgIdx >= AMD_DCN_MAX_PIPES || !IsAmdOtgEnabled(ops, otgIdx, layout.splitClkRegs))
        continue;

      dioActiveOtg[otgIdx] = true;
      const uint32_t fmtIdx = ResolveAmdFmtFromOtg(ops, otgIdx);
      const dvbridge_amd_dcn_params params =
          MakeDioDcnParams(fe, fmtIdx, layout.genCtrl0Dword, layout.stride);

      if (digMode == 3U) // DIG_MODE = 3: TMDS_HDMI
      {
        return AmdDcnTarget{params, be, fe, otgIdx, fmtIdx, layout.genCtrl0Dword, digMode, false, true};
      }
      if (!fallbackExt.found && digMode != 1U) // Non-LVDS external output fallback (e.g. DP-to-HDMI)
      {
        fallbackExt =
            AmdDcnTarget{params, be, fe, otgIdx, fmtIdx, layout.genCtrl0Dword, digMode, false, true};
      }
    }
  }

  // Probe HDMI 2.1 FRL (HPO Stream Encoder + dedicated SEG3 VPG) before falling back to DP.
  const uint32_t hpoClk = ops.read32(ops.user_ctx, AMD_REG_DCN_HPO_STREAM_CLK_CTRL);
  const uint32_t hpoTbCtrl = ops.read32(ops.user_ctx, AMD_REG_DCN_HPO_TB_ENC_CTRL);
  const uint32_t hpoMux = ops.read32(ops.user_ctx, AMD_REG_DCN_HPO_STREAM_MUX_CTRL);
  const uint32_t hpoGen0 = ops.read32(ops.user_ctx, AMD_REG_DCN_HPO_TB_GEN_CTRL0);
  const uint32_t hpoVpg = ops.read32(ops.user_ctx, AMD_REG_DCN_HPO_VPG_ACCESS_CTRL);
  if (hpoClk != 0xFFFFFFFFU && hpoTbCtrl != 0xFFFFFFFFU && hpoMux != 0xFFFFFFFFU &&
      hpoGen0 != 0xFFFFFFFFU && hpoVpg != 0xFFFFFFFFU &&
      (hpoClk & 1U) != 0U && (hpoTbCtrl & 1U) != 0U)
  {
    const uint32_t otgIdx = hpoMux & 0x7U;
    if (otgIdx < AMD_DCN_MAX_PIPES && !dioActiveOtg[otgIdx] &&
        IsAmdOtgEnabled(ops, otgIdx, layouts[0].splitClkRegs))
    {
      const uint32_t fmtIdx = ResolveAmdFmtFromOtg(ops, otgIdx);
      dvbridge_amd_dcn_params hpoParams{};
      hpoParams.fmt_bit_depth_reg = AMD_REG_FMT_BIT_DEPTH_CONTROL(fmtIdx);
      hpoParams.vpg_access_reg = AMD_REG_DCN_HPO_VPG_ACCESS_CTRL;
      hpoParams.gen_ctrl0_reg = AMD_REG_DCN_HPO_TB_GEN_CTRL0;
      hpoParams.pkt01_line_reg = AMD_REG_DCN_HPO_TB_PKT0_1_LINE;
      hpoParams.pkt45_line_reg = AMD_REG_DCN_HPO_TB_PKT4_5_LINE;
      return AmdDcnTarget{hpoParams, 0U, 0U, otgIdx, fmtIdx, 0x08E6U, 6U, true, true};
    }
  }

  return fallbackExt;
}

#ifdef _WIN32
struct KmtOpenAdapterFromLuid
{
  LUID AdapterLuid;
  uint32_t hAdapter;
};

struct KmtCloseAdapter
{
  uint32_t hAdapter;
};

struct KmtEscape
{
  uint32_t hAdapter;
  uint32_t hDevice;
  uint32_t Type;
  uint32_t Flags;
  void* pPrivateDriverData;
  uint32_t PrivateDriverDataSize;
  uint32_t hContext;
};

using PFN_D3DKMTOpenAdapterFromLuid = LONG(__stdcall*)(KmtOpenAdapterFromLuid*);
using PFN_D3DKMTCloseAdapter = LONG(__stdcall*)(const KmtCloseAdapter*);
using PFN_D3DKMTEscape = LONG(__stdcall*)(const KmtEscape*);

LONG CallIntelDisplayEscape(PFN_D3DKMTEscape fnEscape,
                            uint32_t hAdapter,
                            uint32_t subCmd,
                            uint8_t* payload,
                            uint32_t payloadSize)
{
  std::vector<uint8_t> buf(16 + payloadSize, 0);
  std::memcpy(&buf[0], &payloadSize, 4);
  const uint16_t majorVer = 1;
  const uint16_t minorVer = 0;
  const uint32_t escapeCode = 1; // table[1] = 0x38F40 -> 0x1AC6E0
  std::memcpy(&buf[4], &majorVer, 2);
  std::memcpy(&buf[6], &minorVer, 2);
  std::memcpy(&buf[8], &escapeCode, 4);
  std::memcpy(&buf[12], &subCmd, 4);
  std::memcpy(&buf[16], payload, payloadSize);

  KmtEscape esc{};
  esc.hAdapter = hAdapter;
  esc.Type = 0; // D3DKMT_ESCAPE_DRIVERPRIVATE
  esc.pPrivateDriverData = buf.data();
  esc.PrivateDriverDataSize = static_cast<uint32_t>(buf.size());
  LONG st = fnEscape(&esc);
  std::memcpy(payload, &buf[16], payloadSize);
  return st;
}

bool SetIntelQuantizationRangeViaEscape(bool fullRange)
{
  HMODULE hGdi = GetModuleHandleA("gdi32.dll");
  if (!hGdi)
    return false;

  auto fnOpen = reinterpret_cast<PFN_D3DKMTOpenAdapterFromLuid>(
      GetProcAddress(hGdi, "D3DKMTOpenAdapterFromLuid"));
  auto fnClose =
      reinterpret_cast<PFN_D3DKMTCloseAdapter>(GetProcAddress(hGdi, "D3DKMTCloseAdapter"));
  auto fnEscape = reinterpret_cast<PFN_D3DKMTEscape>(GetProcAddress(hGdi, "D3DKMTEscape"));
  if (!fnOpen || !fnClose || !fnEscape)
    return false;

  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) ||
      !factory)
    return false;

  bool anySuccess = false;
  IDXGIAdapter1* adapter = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
  {
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
        desc.VendorId == 0x8086)
    {
      KmtOpenAdapterFromLuid open{};
      open.AdapterLuid = desc.AdapterLuid;
      if (fnOpen(&open) == 0 && open.hAdapter != 0)
      {
        uint8_t p1[64]{};
        if (CallIntelDisplayEscape(fnEscape, open.hAdapter, 0x01U, p1, sizeof(p1)) == 0)
        {
          uint32_t count = 0;
          std::memcpy(&count, &p1[4], 4);
          for (uint32_t d = 0; d < count && d < 8U; ++d)
          {
            uint32_t uid = 0;
            std::memcpy(&uid, &p1[8 + d * 4], 4);
            if (uid == 0)
              continue;

            uint8_t p11[80]{};
            uint32_t opGet = 0;
            std::memcpy(&p11[0], &uid, 4);
            std::memcpy(&p11[4], &opGet, 4);
            if (CallIntelDisplayEscape(fnEscape, open.hAdapter, 0x11U, p11, sizeof(p11)) == 0)
            {
              uint32_t opSet = 1;
              uint32_t qAlt = fullRange ? 1U : 3U;
              uint32_t qTarget = fullRange ? 3U : 1U;

              // Toggle qAlt -> qTarget so igdkmdn64.sys 0x1B0C26 sees a state transition
              // and invokes 0x1A96D0 -> 0xB8CC4 -> 0x31CF0 (DSB/Pipe DMC Full-Range commit).
              std::memcpy(&p11[0], &uid, 4);
              std::memcpy(&p11[4], &opSet, 4);
              std::memcpy(&p11[0x35], &qAlt, 4);
              CallIntelDisplayEscape(fnEscape, open.hAdapter, 0x11U, p11, sizeof(p11));

              std::memcpy(&p11[0], &uid, 4);
              std::memcpy(&p11[4], &opSet, 4);
              std::memcpy(&p11[0x35], &qTarget, 4);
              if (CallIntelDisplayEscape(fnEscape, open.hAdapter, 0x11U, p11, sizeof(p11)) == 0)
                anySuccess = true;
            }
          }
        }
        KmtCloseAdapter close{open.hAdapter};
        fnClose(&close);
      }
    }
    adapter->Release();
  }
  factory->Release();
  return anySuccess;
}
#endif
} // namespace

void CDVBridgeStateWin::ResetMetadataState()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  g_dvMetaState = {};
  g_dvMetaState.last_pts = -1.0;
  g_dvMetaState.fel_verified = true;
  m_packedBytes.fill(0);
  m_packedPacketCount = 0;
  m_elQueue.clear();
  m_activeElFrame.reset();
  m_felActive = false;
  m_felLut.fill(0);
}

void CDVBridgeStateWin::StoreEnhancementFrame(double ptsUs, std::shared_ptr<AVFrame> elFrame)
{
  if (!elFrame)
    return;
  std::lock_guard<std::mutex> lock(m_mutex);
  while (m_elQueue.size() >= 24)
    m_elQueue.pop_front();
  m_elQueue.emplace_back(ptsUs / 1000000.0, std::move(elFrame));
}

bool CDVBridgeStateWin::ProcessDoviMetadata(const void* doviMetaPtr, size_t doviMetaSize, double pts)
{
  EnsureHardwareProbed();
  if (!doviMetaPtr || !dvbridge_metadata_bounds(doviMetaPtr, doviMetaSize))
    return false;

  const auto* meta = static_cast<const AVDOVIMetadata*>(doviMetaPtr);
  std::lock_guard<std::mutex> lock(m_mutex);
  g_dvMetaState.fel_verified = true;
  unsigned margins[4] = {0, 0, 0, 0};
  const bool forceRefresh = (g_dvMetaState.frames == 0);
  if (!dvbridge_dv_metadata_output(&g_dvMetaState, meta, pts, margins, forceRefresh))
    return false;

  if (g_dvMetaState.count <= 0)
    return false;

  for (size_t i = 0; i < m_packedBytes.size(); ++i)
    m_packedBytes[i] = static_cast<uint8_t>(g_dvMetaState.packets[i] & 0xFFU);
  m_packedPacketCount = static_cast<uint32_t>(g_dvMetaState.count);
  ++m_packedFrames;

  const auto* hdr = av_dovi_get_header(meta);
  const auto* map = av_dovi_get_mapping(meta);
  const auto* color = av_dovi_get_color(meta);

  // Match paired FEL Enhancement Layer frame by PTS (within 10ms)
  m_activeElFrame.reset();
  m_felActive = false;
  for (auto it = m_elQueue.begin(); it != m_elQueue.end();)
  {
    if (it->first < pts - 0.25)
    {
      it = m_elQueue.erase(it);
      continue;
    }
    if (std::fabs(it->first - pts) < 0.010)
    {
      m_activeElFrame = it->second;
      break;
    }
    ++it;
  }

  if (m_activeElFrame && hdr && map && color && !hdr->disable_residual_flag &&
      !dvbridge_dv_is_mel(meta) && hdr->bl_bit_depth == 10 &&
      (hdr->el_bit_depth == 8 || hdr->el_bit_depth == 10) && hdr->coef_log2_denom <= 23)
  {
    for (int c = 0; c < 3; ++c)
    {
      const AVDOVIReshapingCurve* curve = &map->curves[c];
      const uint32_t lowPivot = curve->num_pivots >= 2 ? curve->pivots[0] : 0U;
      const uint32_t highPivot =
          curve->num_pivots >= 2 ? curve->pivots[curve->num_pivots - 1] : 1023U;

      for (uint32_t sVal = 0; sVal < 1024U; ++sVal)
      {
        const uint32_t s = std::clamp(sVal, lowPivot, highPivot);
        int seg = 0;
        for (int p = 0; p + 1 < curve->num_pivots; ++p)
        {
          if (p + 2 == curve->num_pivots || s < curve->pivots[p + 1])
          {
            seg = p;
            break;
          }
        }
        uint32_t prediction = (s << 6U);
        if (curve->mapping_idc[seg] == AV_DOVI_MAPPING_POLYNOMIAL && curve->poly_order[seg] <= 2)
        {
          int64_t acc = 0;
          for (int i = 0; i <= curve->poly_order[seg]; ++i)
          {
            int64_t term = 0;
            if (i == 0)
              term = 1LL << 20;
            else if (i == 1)
              term = int64_t(s) << (20 - hdr->bl_bit_depth);
            else if (i == 2)
              term = int64_t(s * s) << (20 - 2 * hdr->bl_bit_depth);
            acc += curve->poly_coef[seg][i] * term;
          }
          if (acc < 0)
            acc = 0;
          uint64_t mapped = static_cast<uint64_t>(acc) >> (4 + hdr->coef_log2_denom);
          prediction = (mapped > 65535ULL) ? 65535U : static_cast<uint32_t>(mapped);
        }
        m_felLut[c * 1024 + sVal] = static_cast<int32_t>(prediction);
      }

      const AVDOVINLQParams* q = &map->nlq[c];
      for (uint32_t elVal = 0; elVal < 1024U; ++elVal)
      {
        int32_t residual = 0;
        if (map->nlq_method_idc == AV_DOVI_NLQ_LINEAR_DZ && q->vdr_in_max > 0)
        {
          const int64_t delta = int64_t(elVal) - int64_t(q->nlq_offset);
          if (delta != 0)
          {
            const int64_t signbit = (delta < 0) ? -1 : 1;
            const int64_t scaled = (2 * delta - signbit) * (1LL << (10 - hdr->el_bit_depth));
            const uint64_t absScaled = static_cast<uint64_t>(scaled < 0 ? -scaled : scaled);
            uint64_t dq = absScaled * uint64_t(q->linear_deadzone_slope) +
                          (uint64_t(q->linear_deadzone_threshold) << (11 - hdr->el_bit_depth));
            const uint64_t bound = uint64_t(q->vdr_in_max) << (11 - hdr->el_bit_depth);
            if (dq > bound)
              dq = bound;
            const int shift = int(hdr->coef_log2_denom) - 5 - int(hdr->el_bit_depth);
            const uint64_t mag = (shift >= 0) ? (dq >> shift) : (dq << (-shift));
            const int32_t clampedMag =
                static_cast<int32_t>(mag > 65535ULL ? 65535ULL : mag);
            residual = (signbit < 0) ? -clampedMag : clampedMag;
          }
        }
        m_felLut[(3 + c) * 1024 + elVal] = residual;
      }

      if (color->ycc_to_rgb_offset[c].den > 0)
      {
        m_felYccOffset[c] = static_cast<float>(color->ycc_to_rgb_offset[c].num) /
                            static_cast<float>(color->ycc_to_rgb_offset[c].den);
      }
    }

    for (int r = 0; r < 3; ++r)
    {
      float* dstRow = (r == 0) ? m_felYccRow0.data()
                               : ((r == 1) ? m_felYccRow1.data() : m_felYccRow2.data());
      for (int col = 0; col < 3; ++col)
      {
        const AVRational& m = color->ycc_to_rgb_matrix[r * 3 + col];
        if (m.den > 0)
          dstRow[col] = static_cast<float>(m.num) / static_cast<float>(m.den);
      }
      dstRow[3] = 0.0f;
    }
    m_felActive = true;
  }

  if (m_packedFrames == 1 || (m_packedFrames % 300) == 0)
  {
    CLog::Log(LOGINFO,
              "[DVBridge-Win] Packed Dolby Vision metadata frame #{}: count={} packet(s), "
              "id={}, bl_bit_depth={}, el_bit_depth={}, disable_residual={}, felActive={}, "
              "pts={:.3f}, hdr=[{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}]",
              m_packedFrames, m_packedPacketCount, g_dvMetaState.id,
              hdr ? int(hdr->bl_bit_depth) : -1, hdr ? int(hdr->el_bit_depth) : -1,
              hdr ? int(hdr->disable_residual_flag) : -1, m_felActive, pts,
              m_packedBytes[0], m_packedBytes[1], m_packedBytes[2], m_packedBytes[3],
              m_packedBytes[4], m_packedBytes[5], m_packedBytes[6], m_packedBytes[7]);
  }
  return true;
}

bool CDVBridgeStateWin::GetPackedMetadata(uint8_t outPackets[640],
                                          uint32_t& outCount,
                                          uint64_t& outRevision) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_packedPacketCount == 0)
  {
    outCount = 0;
    outRevision = 0;
    return false;
  }
  std::memcpy(outPackets, m_packedBytes.data(), m_packedBytes.size());
  outCount = m_packedPacketCount;
  outRevision = m_packedFrames;
  return true;
}

void CDVBridgeStateWin::SetDoviElConfig(const uint8_t* data, size_t size)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (data && size > 0)
    m_doviElConfig.assign(data, data + size);
  else
    m_doviElConfig.clear();
}

std::vector<uint8_t> CDVBridgeStateWin::GetDoviElConfig() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_doviElConfig;
}

bool CDVBridgeStateWin::GetFelData(std::shared_ptr<AVFrame>& outElFrame,
                                   int32_t outLut[6 * 1024],
                                   float outYccOffset[4],
                                   float outYccRow0[4],
                                   float outYccRow1[4],
                                   float outYccRow2[4],
                                   uint64_t& outRevision) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_felActive || !m_activeElFrame)
  {
    outElFrame.reset();
    outRevision = 0;
    return false;
  }
  outElFrame = m_activeElFrame;
  std::memcpy(outLut, m_felLut.data(), m_felLut.size() * sizeof(int32_t));
  std::memcpy(outYccOffset, m_felYccOffset.data(), 4 * sizeof(float));
  std::memcpy(outYccRow0, m_felYccRow0.data(), 4 * sizeof(float));
  std::memcpy(outYccRow1, m_felYccRow1.data(), 4 * sizeof(float));
  std::memcpy(outYccRow2, m_felYccRow2.data(), 4 * sizeof(float));
  outRevision = m_packedFrames;
  return true;
}

void CDVBridgeStateWin::SetWantsNativeDvTunnel(bool wants)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_wantsNativeDvTunnel = wants;
}

bool CDVBridgeStateWin::WantsNativeDvTunnel() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_wantsNativeDvTunnel;
}

uint8_t CDVBridgeStateWin::ComputeCtaVic(unsigned width, unsigned height, float fps)
{
  if (width >= 3840 || height >= 1600)
  {
    if (fps > 0.0f && fps <= 24.5f)
      return 93; // 3840x2160p24 / 23.976
    if (fps > 24.5f && fps <= 25.5f)
      return 94; // 3840x2160p25
    if (fps > 25.5f && fps <= 30.5f)
      return 95; // 3840x2160p30 / 29.97
    if (fps > 49.0f && fps <= 51.0f)
      return 96; // 3840x2160p50
    return 97;   // 3840x2160p60 / 59.94
  }
  if (fps > 0.0f && fps <= 24.5f)
    return 32; // 1920x1080p24
  if (fps > 49.0f && fps <= 51.0f)
    return 31; // 1920x1080p50
  return 16;   // 1920x1080p60
}

void CDVBridgeStateWin::SetDvTunnelActive(bool active, uint8_t ctaVic)
{
  EnsureHardwareProbed();
  if (active)
  {
    if (!m_tunnelActive)
      ResetMetadataState();
    m_tunnelActive = true;
    m_wantsNativeDvTunnel = true;
    m_ctaVic = ctaVic;
    m_legacyVsif = m_sink.dvCaps.legacy_vsif;
    if (EnsureMmioInitialized())
    {
      if (!m_captured)
        Capture(ctaVic, m_legacyVsif);
      else
        Stage(false);
    }
  }
  else
  {
    const bool wasActive = m_tunnelActive;
    m_tunnelActive = false;
    ResetMetadataState();
    if (m_captured)
    {
      Stage(true);
      Restored();
    }
    if (wasActive)
      CLog::Log(LOGINFO, "[DVBridge-Win] Native Dolby Vision RGB8 Tunnel deactivated.");
  }
}

bool CDVBridgeStateWin::IsDvTunnelActive() const
{
  return m_tunnelActive;
}

void CDVBridgeStateWin::RefreshHwTransportIfNeeded()
{
  if (!m_tunnelActive)
    return;
  ++m_hwRefreshCounter;
  if (EnsureMmioInitialized())
  {
    if (!m_captured)
      Capture(m_ctaVic, m_legacyVsif);
    else
      Stage(false);
  }
}

CDVBridgeStateWin::~CDVBridgeStateWin()
{
  if (m_captured)
  {
    Stage(true);
    Restored();
  }
#ifdef _WIN32
  if (m_mmioDllModule && m_mmioMappedBase)
  {
    using PFN_UnmapPhysicalMemory = BOOL(__stdcall*)(HANDLE, PBYTE);
    auto fnUnmap = reinterpret_cast<PFN_UnmapPhysicalMemory>(
        GetProcAddress(static_cast<HMODULE>(m_mmioDllModule), "UnmapPhysicalMemory"));
    if (fnUnmap)
      fnUnmap(static_cast<HANDLE>(m_mmioSectionHandle), static_cast<PBYTE>(m_mmioMappedBase));
    m_mmioMappedBase = nullptr;
  }
  if (m_mmioDeviceHandle && m_mmioDeviceHandle != INVALID_HANDLE_VALUE)
  {
    CloseHandle(static_cast<HANDLE>(m_mmioDeviceHandle));
    m_mmioDeviceHandle = nullptr;
  }
#endif
}

std::vector<CDVBridgeStateWin::DisplaySinkInfo> CDVBridgeStateWin::EnumerateDisplays()
{
  std::vector<DisplaySinkInfo> sinks;

#ifdef _WIN32
  HDEVINFO devInfo = SetupDiGetClassDevsA(
      &GUID_DEVINTERFACE_MONITOR_WIN_DV, nullptr, nullptr,
      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (devInfo == INVALID_HANDLE_VALUE)
    return sinks;

  for (DWORD idx = 0;; ++idx)
  {
    SP_DEVINFO_DATA devData{};
    devData.cbSize = sizeof(devData);
    if (!SetupDiEnumDeviceInfo(devInfo, idx, &devData))
      break;

    char instanceId[256]{};
    SetupDiGetDeviceInstanceIdA(devInfo, &devData, instanceId, sizeof(instanceId), nullptr);

    char friendlyName[256]{};
    if (!SetupDiGetDeviceRegistryPropertyA(devInfo, &devData, SPDRP_FRIENDLYNAME, nullptr,
                                           reinterpret_cast<PBYTE>(friendlyName),
                                           sizeof(friendlyName), nullptr))
    {
      SetupDiGetDeviceRegistryPropertyA(devInfo, &devData, SPDRP_DEVICEDESC, nullptr,
                                        reinterpret_cast<PBYTE>(friendlyName),
                                        sizeof(friendlyName), nullptr);
    }

    HKEY hDevReg = SetupDiOpenDevRegKey(devInfo, &devData, DICS_FLAG_GLOBAL, 0,
                                        DIREG_DEV, KEY_READ);
    if (hDevReg != INVALID_HANDLE_VALUE)
    {
      uint8_t edidBuf[1024]{};
      DWORD edidLen = sizeof(edidBuf);
      DWORD regType = 0;
      if (RegQueryValueExA(hDevReg, "EDID", nullptr, &regType, edidBuf, &edidLen) == ERROR_SUCCESS &&
          edidLen >= 128)
      {
        DisplaySinkInfo info;
        info.monitorInstanceId = instanceId;
        info.friendlyName = friendlyName[0] ? friendlyName : instanceId;
        info.rawEdid.assign(edidBuf, edidBuf + edidLen);
        ParseSinkHdrCapabilities(info);
        sinks.push_back(std::move(info));
      }
      RegCloseKey(hDevReg);
    }
  }
  SetupDiDestroyDeviceInfoList(devInfo);

  HKEY hDisplayRoot = nullptr;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Enum\\DISPLAY", 0, KEY_READ,
                    &hDisplayRoot) == ERROR_SUCCESS)
  {
    char monHwId[256]{};
    for (DWORD i = 0; RegEnumKeyA(hDisplayRoot, i, monHwId, sizeof(monHwId)) == ERROR_SUCCESS; ++i)
    {
      HKEY hMonKey = nullptr;
      if (RegOpenKeyExA(hDisplayRoot, monHwId, 0, KEY_READ, &hMonKey) != ERROR_SUCCESS)
        continue;

      char instId[256]{};
      for (DWORD j = 0; RegEnumKeyA(hMonKey, j, instId, sizeof(instId)) == ERROR_SUCCESS; ++j)
      {
        std::string fullInst = std::string("DISPLAY\\") + monHwId + "\\" + instId;
        bool alreadyPresent = false;
        for (const auto& existing : sinks)
        {
          if (_stricmp(existing.monitorInstanceId.c_str(), fullInst.c_str()) == 0)
          {
            alreadyPresent = true;
            break;
          }
        }
        if (alreadyPresent)
          continue;

        HKEY hInstKey = nullptr;
        if (RegOpenKeyExA(hMonKey, instId, 0, KEY_READ, &hInstKey) != ERROR_SUCCESS)
          continue;

        char friendly[256]{};
        DWORD friendlyLen = sizeof(friendly);
        if (RegQueryValueExA(hInstKey, "FriendlyName", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(friendly), &friendlyLen) != ERROR_SUCCESS)
        {
          friendlyLen = sizeof(friendly);
          RegQueryValueExA(hInstKey, "DeviceDesc", nullptr, nullptr,
                           reinterpret_cast<LPBYTE>(friendly), &friendlyLen);
        }

        HKEY hDevParam = nullptr;
        if (RegOpenKeyExA(hInstKey, "Device Parameters", 0, KEY_READ, &hDevParam) == ERROR_SUCCESS)
        {
          uint8_t edidBuf[1024]{};
          DWORD edidLen = sizeof(edidBuf);
          DWORD regType = 0;
          if (RegQueryValueExA(hDevParam, "EDID", nullptr, &regType, edidBuf, &edidLen) ==
                  ERROR_SUCCESS &&
              edidLen >= 128)
          {
            DisplaySinkInfo info;
            info.monitorInstanceId = fullInst;
            info.friendlyName = friendly[0] ? friendly : monHwId;
            info.rawEdid.assign(edidBuf, edidBuf + edidLen);
            ParseSinkHdrCapabilities(info);
            sinks.push_back(std::move(info));
          }
          RegCloseKey(hDevParam);
        }
        RegCloseKey(hInstKey);
      }
      RegCloseKey(hMonKey);
    }
    RegCloseKey(hDisplayRoot);
  }
#endif

  return sinks;
}

bool CDVBridgeStateWin::QueryPrimaryGpu(GpuAdapterInfo& outGpu)
{
  outGpu = {};

#ifdef _WIN32
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory)
    return false;

  IDXGIAdapter1* adapter = nullptr;
  bool found = false;
  GpuAdapterInfo fallbackGpu{};
  bool hasFallback = false;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
  {
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc)))
    {
      if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
          (desc.VendorId == 0x1002 || desc.VendorId == 0x8086 || desc.VendorId == 0x10DE))
      {
        GpuAdapterInfo candidate{};
        candidate.vendorId = desc.VendorId;
        candidate.deviceId = desc.DeviceId;
        candidate.subSysId = desc.SubSysId;
        candidate.revision = desc.Revision;
        char narrowDesc[256]{};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                            narrowDesc, sizeof(narrowDesc), nullptr, nullptr);
        candidate.description = narrowDesc;

        IDXGIOutput* dxgiOut = nullptr;
        const bool hasOutput = SUCCEEDED(adapter->EnumOutputs(0, &dxgiOut)) && dxgiOut != nullptr;
        if (dxgiOut)
          dxgiOut->Release();

        if (!hasFallback)
        {
          fallbackGpu = candidate;
          hasFallback = true;
        }
        // Prefer Intel/AMD adapter with an attached display output for MMIO backend
        if ((candidate.vendorId == 0x8086 || candidate.vendorId == 0x1002) && hasOutput)
        {
          outGpu = std::move(candidate);
          found = true;
          adapter->Release();
          break;
        }
      }
    }
    adapter->Release();
  }
  factory->Release();
  if (!found && hasFallback)
  {
    outGpu = std::move(fallbackGpu);
    found = true;
  }

  if (found)
  {
    HDEVINFO devInfo = SetupDiGetClassDevsA(&GUID_DEVCLASS_DISPLAY_WIN_DV, nullptr, nullptr,
                                            DIGCF_PRESENT);
    if (devInfo != INVALID_HANDLE_VALUE)
    {
      char targetVenDev[32]{};
      snprintf(targetVenDev, sizeof(targetVenDev), "VEN_%04X&DEV_%04X",
               outGpu.vendorId, outGpu.deviceId);
      for (DWORD idx = 0;; ++idx)
      {
        SP_DEVINFO_DATA devData{};
        devData.cbSize = sizeof(devData);
        if (!SetupDiEnumDeviceInfo(devInfo, idx, &devData))
          break;

        char hwIds[1024]{};
        if (SetupDiGetDeviceRegistryPropertyA(devInfo, &devData, SPDRP_HARDWAREID, nullptr,
                                              reinterpret_cast<PBYTE>(hwIds), sizeof(hwIds) - 1,
                                              nullptr))
        {
          bool matchedId = false;
          for (const char* p = hwIds; *p; p += std::strlen(p) + 1)
          {
            if (_strnicmp(p, "PCI\\", 4) == 0 && strstr(p, targetVenDev) != nullptr)
            {
              matchedId = true;
              break;
            }
            // Also try case-insensitive search
            std::string upper(p);
            for (char& ch : upper)
              ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
            if (upper.find(targetVenDev) != std::string::npos)
            {
              matchedId = true;
              break;
            }
          }
          if (!matchedId)
            continue;
        }

        LOG_CONF logConf = 0;
        if (CM_Get_First_Log_Conf(&logConf, devData.DevInst, ALLOC_LOG_CONF) != CR_SUCCESS)
          continue;

        RES_DES prevResDes = static_cast<RES_DES>(logConf);
        for (RESOURCEID resType : {ResType_Mem, ResType_MemLarge})
        {
          RES_DES curResDes = static_cast<RES_DES>(logConf);
          bool first = true;
          while (CM_Get_Next_Res_Des(&curResDes, first ? static_cast<RES_DES>(logConf) : prevResDes,
                                     resType, nullptr, 0) == CR_SUCCESS)
          {
            if (!first)
              CM_Free_Res_Des_Handle(prevResDes);
            first = false;
            prevResDes = curResDes;

            ULONG dataSize = 0;
            if (CM_Get_Res_Des_Data_Size(&dataSize, curResDes, 0) == CR_SUCCESS && dataSize > 0)
            {
              std::vector<uint8_t> buf(dataSize);
              if (CM_Get_Res_Des_Data(curResDes, buf.data(), dataSize, 0) == CR_SUCCESS)
              {
                uint64_t base = 0;
                uint64_t end = 0;
                if (resType == ResType_Mem && dataSize >= sizeof(MEM_RESOURCE))
                {
                  const auto* mr = reinterpret_cast<const MEM_RESOURCE*>(buf.data());
                  base = mr->MEM_Header.MD_Alloc_Base;
                  end = mr->MEM_Header.MD_Alloc_End;
                }
                else if (resType == ResType_MemLarge && dataSize >= sizeof(MEM_LARGE_RESOURCE))
                {
                  const auto* mlr = reinterpret_cast<const MEM_LARGE_RESOURCE*>(buf.data());
                  base = mlr->MEM_LARGE_Header.MLD_Alloc_Base;
                  end = mlr->MEM_LARGE_Header.MLD_Alloc_End;
                }

                if (end > base)
                {
                  const uint64_t length = (end - base) + 1ULL;
                  if (outGpu.vendorId == 0x1002)
                  {
                    // AMD DCN MMIO register aperture is BAR5 (512KB = 0x80000, < 4GB);
                    // reject 2MB Doorbell BAR2 (0x200000 at > 4GB).
                    if (length >= 0x40000ULL && length <= 0x100000ULL)
                    {
                      const bool is32BitBar = (base < 0x100000000ULL);
                      const bool curIs32Bit = (outGpu.mmioPhysBase != 0 && outGpu.mmioPhysBase < 0x100000000ULL);
                      if (outGpu.mmioPhysBase == 0 || (is32BitBar && !curIs32Bit) ||
                          (is32BitBar && length == 0x80000ULL))
                      {
                        outGpu.mmioPhysBase = base;
                        outGpu.mmioPhysLength = length;
                      }
                    }
                  }
                  else
                  {
                    // Intel Gen9-14 BAR0 MMIO is 16MB (0x1000000)
                    if ((length >= 0x100000ULL && length <= 0x10000000ULL &&
                         (outGpu.mmioPhysBase == 0 || length >= 0x400000ULL)) ||
                        outGpu.mmioPhysBase == 0)
                    {
                      outGpu.mmioPhysBase = base;
                      outGpu.mmioPhysLength = length;
                    }
                  }
                }
              }
            }
          }
          if (!first)
            CM_Free_Res_Des_Handle(prevResDes);
        }
        CM_Free_Log_Conf_Handle(logConf);
        if ((outGpu.vendorId == 0x1002 && outGpu.mmioPhysLength == 0x80000ULL &&
             outGpu.mmioPhysBase != 0 && outGpu.mmioPhysBase < 0x100000000ULL) ||
            (outGpu.vendorId != 0x1002 && outGpu.mmioPhysBase != 0))
          break;
      }
      SetupDiDestroyDeviceInfoList(devInfo);
    }
  }
  return found;
#else
  return false;
#endif
}

bool CDVBridgeStateWin::EnsureMmioInitialized()
{
  if (m_hasMmioOps && m_mmioOps.read32 && m_mmioOps.write32)
    return true;
  if (m_mmioProbed)
    return m_hasMmioOps;
  m_mmioProbed = true;

#ifdef _WIN32
  if (m_gpu.vendorId == 0)
    QueryPrimaryGpu(m_gpu);

  if (m_gpu.mmioPhysBase == 0)
  {
    CLog::Log(LOGWARNING, "[DVBridge-Win] Could not determine GPU MMIO BAR physical address.");
    return false;
  }

  const uint32_t mapSize =
      (m_gpu.mmioPhysLength >= 0x40000ULL && m_gpu.mmioPhysLength <= 0x100000ULL)
          ? static_cast<uint32_t>(m_gpu.mmioPhysLength)
          : (m_gpu.vendorId == 0x1002 ? 0x80000U : 0x100000U);

  // 1. Try loading inpoutx64.dll (provides signed kernel physical memory mapper)
  HMODULE hDll = LoadLibraryA("inpoutx64.dll");
  if (!hDll)
  {
    char exePath[MAX_PATH]{};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0)
    {
      char* lastSlash = strrchr(exePath, '\\');
      if (lastSlash)
      {
        strcpy_s(lastSlash + 1, MAX_PATH - (lastSlash + 1 - exePath), "inpoutx64.dll");
        hDll = LoadLibraryA(exePath);
      }
    }
  }

  if (hDll)
  {
    using PFN_MapPhysToLin = PBYTE(__stdcall*)(PBYTE pbPhysAddr, DWORD dwPhysSize, HANDLE* pPhysicalMemoryHandle);
    auto fnMap = reinterpret_cast<PFN_MapPhysToLin>(GetProcAddress(hDll, "MapPhysToLin"));
    if (fnMap)
    {
      HANDLE hSection = nullptr;
      PBYTE mapped = fnMap(reinterpret_cast<PBYTE>(static_cast<uintptr_t>(m_gpu.mmioPhysBase)),
                           mapSize, &hSection);
      if (mapped)
      {
        g_mmioMappedSize = mapSize;
        m_mmioDllModule = hDll;
        m_mmioSectionHandle = hSection;
        m_mmioMappedBase = mapped;
        m_mmioOps.user_ctx = mapped;
        m_mmioOps.read32 = &MmioRead32Mapped;
        m_mmioOps.write32 = &MmioWrite32Mapped;
        m_hasMmioOps = true;
        CLog::Log(LOGINFO,
                  "[DVBridge-Win] Mapped GPU ({}) MMIO Phys=0x{:X} (len=0x{:X}, map=0x{:X}) -> Virt=0x{:X} via inpoutx64.dll",
                  m_gpu.description, m_gpu.mmioPhysBase, m_gpu.mmioPhysLength, mapSize,
                  reinterpret_cast<uintptr_t>(mapped));
        return true;
      }
    }
  }

  // 2. Fallback: Try direct \\.\inpoutx64 DeviceIoControl (IOCTL_WINIO_MAPPHYSTOLIN = 0x9C402008)
  HANDLE hDev = CreateFileA("\\\\.\\inpoutx64", GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hDev != INVALID_HANDLE_VALUE)
  {
    struct InpOutPhysStruct
    {
      uint64_t dwPhysMemSizeInBytes;
      uint64_t pvPhysAddress;
      uint64_t PhysicalMemoryHandle;
      uint64_t pvPhysMemLin;
    } phys{};
    phys.dwPhysMemSizeInBytes = mapSize;
    phys.pvPhysAddress = m_gpu.mmioPhysBase;
    DWORD bytesRet = 0;
    if (DeviceIoControl(hDev, 0x9C402008U, &phys, sizeof(phys), &phys, sizeof(phys), &bytesRet,
                        nullptr) &&
        phys.pvPhysMemLin != 0)
    {
      g_mmioMappedSize = mapSize;
      m_mmioDeviceHandle = hDev;
      m_mmioSectionHandle = reinterpret_cast<void*>(static_cast<uintptr_t>(phys.PhysicalMemoryHandle));
      m_mmioMappedBase = reinterpret_cast<void*>(static_cast<uintptr_t>(phys.pvPhysMemLin));
      m_mmioOps.user_ctx = m_mmioMappedBase;
      m_mmioOps.read32 = &MmioRead32Mapped;
      m_mmioOps.write32 = &MmioWrite32Mapped;
      m_hasMmioOps = true;
      CLog::Log(LOGINFO,
                "[DVBridge-Win] Mapped GPU ({}) BAR0 MMIO Phys=0x{:X} -> Virt=0x{:X} via \\\\.\\inpoutx64",
                m_gpu.description, m_gpu.mmioPhysBase, phys.pvPhysMemLin);
      return true;
    }
    CloseHandle(hDev);
  }

  CLog::Log(LOGWARNING,
            "[DVBridge-Win] GPU ({}) BAR0 Phys=0x{:X} Len=0x{:X}: MMIO helper (inpoutx64.dll) not active; "
            "HDMI Dolby Vision VSIF register programming unavailable until inpoutx64.dll is placed in Kodi folder.",
            m_gpu.description, m_gpu.mmioPhysBase, m_gpu.mmioPhysLength);
#endif
  return false;
}

void CDVBridgeStateWin::AttachMmioContext(const dvbridge_mmio_ops* ops,
                                          uint32_t pipeOrInst)
{
  if (ops)
  {
    m_mmioOps = *ops;
    m_hasMmioOps = true;
  }
  else
  {
    m_mmioOps = {};
    m_hasMmioOps = false;
  }
  m_pipeOrInst = pipeOrInst;
}

bool CDVBridgeStateWin::Capture(uint8_t ctaVic, bool legacyVsif)
{
  m_failure = "none";
  if (m_captured)
    return Reject("already-captured");
  if (!m_hasMmioOps || !m_mmioOps.read32 || !m_mmioOps.write32)
    return Reject("mmio-context-unavailable");

  m_ctaVic = ctaVic;
  m_legacyVsif = legacyVsif;

  if (m_gpu.vendorId == 0)
    QueryPrimaryGpu(m_gpu);

  if (m_gpu.vendorId == 0x8086)
    m_backend = Backend::INTEL_MMIO;
  else if (m_gpu.vendorId == 0x1002 || m_gpu.vendorId == 0)
    m_backend = Backend::AMD_DCN_MMIO;
  else
    return Reject("unsupported-gpu-vendor");

  if (!Stage(false))
    return false;

  m_captured = true;
  return true;
}

bool CDVBridgeStateWin::Stage(bool restore)
{
  if (!m_hasMmioOps || !m_mmioOps.read32 || !m_mmioOps.write32)
    return Reject("mmio-context-unavailable");

  if (m_backend == Backend::INTEL_MMIO)
  {
    bool anyPipeMatched = false;
    for (unsigned p = 0; p < INTEL_NUM_PIPES; ++p)
    {
      const uint32_t transConf = m_mmioOps.read32(m_mmioOps.user_ctx, INTEL_REG_TRANS_CONF(p));
      const uint32_t dipCtl = m_mmioOps.read32(m_mmioOps.user_ctx, INTEL_REG_VIDEO_DIP_CTL(p));
      if (transConf != 0xFFFFFFFFU && (transConf & INTEL_TRANS_CONF_ENABLE) != 0 &&
          (dipCtl & (INTEL_VIDEO_DIP_ENABLE_AVI_HSW | INTEL_VIDEO_DIP_ENABLE_VS_HSW)) != 0)
      {
        if (!restore && !g_hasSavedIntelPipeMisc[p])
        {
          g_savedIntelPipeMisc[p] = m_mmioOps.read32(m_mmioOps.user_ctx, INTEL_REG_PIPE_MISC(p));
          g_savedIntelDipCtl[p] = dipCtl;
          g_savedIntelCscMode[p] = m_mmioOps.read32(m_mmioOps.user_ctx, INTEL_REG_PIPE_CSC_MODE(p));
          g_savedIntelGammaMode[p] = m_mmioOps.read32(m_mmioOps.user_ctx, INTEL_REG_GAMMA_MODE(p));
          for (int i = 0; i < 12; ++i)
          {
            g_savedIntelOutputCsc[p][i] =
                m_mmioOps.read32(m_mmioOps.user_ctx, INTEL_REG_PIPE_OUTPUT_CSC(p, i));
          }
          for (int i = 0; i < 8; ++i)
          {
            g_savedIntelAviData[p][i] =
                m_mmioOps.read32(m_mmioOps.user_ctx, INTEL_REG_VIDEO_DIP_AVI_DATA(p, i));
            g_savedIntelVsData[p][i] =
                m_mmioOps.read32(m_mmioOps.user_ctx, INTEL_REG_VIDEO_DIP_VS_DATA(p, i));
          }
          g_hasSavedIntelPipeMisc[p] = true;
          const uint8_t hwVic = static_cast<uint8_t>(g_savedIntelAviData[p][2] & 0xFFU);
          CLog::Log(LOGINFO,
                    "[DVBridge-Win] Intel Pipe {} active HDMI detected (TRANS_CONF=0x{:08X}, "
                    "DIP_CTL=0x{:08X}, PIPE_MISC=0x{:08X}, CSC_MODE=0x{:08X}, OUT_CSC0=0x{:08X}, "
                    "AVI_DW0..2=[0x{:08X} 0x{:08X} 0x{:08X}], "
                    "hwVIC={} -> reqVIC={}) -> enabling TV-Led Dolby Vision VSIF",
                    char('A' + p), transConf, dipCtl, g_savedIntelPipeMisc[p],
                    g_savedIntelCscMode[p], g_savedIntelOutputCsc[p][0],
                    g_savedIntelAviData[p][0], g_savedIntelAviData[p][1], g_savedIntelAviData[p][2],
                    int(hwVic), int(m_ctaVic));
        }

        if (!restore)
        {
          const uint32_t curCscMode =
              m_mmioOps.read32(m_mmioOps.user_ctx, INTEL_REG_PIPE_CSC_MODE(p));
          const uint32_t curGammaMode =
              m_mmioOps.read32(m_mmioOps.user_ctx, INTEL_REG_GAMMA_MODE(p));
          const uint32_t curPipeMisc =
              m_mmioOps.read32(m_mmioOps.user_ctx, INTEL_REG_PIPE_MISC(p));
          const uint32_t curDipCtl =
              m_mmioOps.read32(m_mmioOps.user_ctx, INTEL_REG_VIDEO_DIP_CTL(p));
          const uint32_t curVsDw1 =
              m_mmioOps.read32(m_mmioOps.user_ctx, INTEL_REG_VIDEO_DIP_VS_DATA(p, 1));
          const uint32_t expectedOuiDw1 = m_legacyVsif ? 0x01000C03U : 0x00D0464BU;
          if (curCscMode != 0U || (curGammaMode & INTEL_GAMMA_ALL_ENABLE_MASK) != 0U ||
              (curPipeMisc & INTEL_PIPE_MISC_PIXEL_ROUNDING_TRUNC) == 0U ||
              (curDipCtl & INTEL_VIDEO_DIP_ENABLE_VS_HSW) == 0U || curVsDw1 != expectedOuiDw1)
          {
            dvbridge_win_intel_apply_dv(&m_mmioOps, p, true, m_legacyVsif, m_ctaVic);
          }
        }
        else
        {
          if (g_hasSavedIntelPipeMisc[p])
          {
            m_mmioOps.write32(m_mmioOps.user_ctx, INTEL_REG_VIDEO_DIP_CTL(p),
                              dipCtl & ~(INTEL_VIDEO_DIP_ENABLE_VS_HSW |
                                         INTEL_VIDEO_DIP_ENABLE_AVI_HSW));
            (void)m_mmioOps.read32(m_mmioOps.user_ctx, INTEL_REG_VIDEO_DIP_CTL(p));
            for (int i = 0; i < 8; ++i)
            {
              m_mmioOps.write32(m_mmioOps.user_ctx, INTEL_REG_VIDEO_DIP_AVI_DATA(p, i),
                                g_savedIntelAviData[p][i]);
              m_mmioOps.write32(m_mmioOps.user_ctx, INTEL_REG_VIDEO_DIP_VS_DATA(p, i),
                                g_savedIntelVsData[p][i]);
            }
            m_mmioOps.write32(m_mmioOps.user_ctx, INTEL_REG_VIDEO_DIP_CTL(p),
                              g_savedIntelDipCtl[p]);
            (void)m_mmioOps.read32(m_mmioOps.user_ctx, INTEL_REG_VIDEO_DIP_CTL(p));
            m_mmioOps.write32(m_mmioOps.user_ctx, INTEL_REG_GAMMA_MODE(p),
                              g_savedIntelGammaMode[p] & ~(1U << 29));
            m_mmioOps.write32(m_mmioOps.user_ctx, INTEL_REG_PIPE_CSC_MODE(p),
                              g_savedIntelCscMode[p] & ~(1U << 29));
            m_mmioOps.write32(m_mmioOps.user_ctx, INTEL_REG_PIPE_MISC(p), g_savedIntelPipeMisc[p]);
            g_hasSavedIntelPipeMisc[p] = false;
            CLog::Log(LOGINFO,
                      "[DVBridge-Win] Intel Pipe {} restored original AVI/VS DIP, CSC/GAMMA & PIPE_MISC",
                      char('A' + p));
          }
          else
          {
            dvbridge_win_intel_apply_dv(&m_mmioOps, p, false, m_legacyVsif, m_ctaVic);
          }
        }
        anyPipeMatched = true;
      }
    }
    if (!anyPipeMatched)
    {
      dvbridge_win_intel_apply_dv(&m_mmioOps, m_pipeOrInst, !restore, m_legacyVsif, m_ctaVic);
    }
    return true;
  }
  else if (m_backend == Backend::AMD_DCN_MMIO)
  {
    if (!restore)
    {
      dvbridge_amd_dcn_params params{};
      if (g_savedAmdState.valid)
      {
        params = g_savedAmdState.params;
      }
      else
      {
        const AmdDcnTarget target = DetectAmdDcnTarget(m_mmioOps, m_gpu.deviceId);
        uint32_t digBeIdx = 0U;
        uint32_t digFeIdx = 0U;
        uint32_t fmtIdx = 0U;
        uint32_t otgIdx = 0U;
        uint32_t genCtrl0Dword = 0U;
        bool isHpoFrl = false;

        if (target.found)
        {
          params = target.params;
          digBeIdx = target.digBeIdx;
          digFeIdx = target.digFeIdx;
          fmtIdx = target.fmtIdx;
          otgIdx = target.otgIdx;
          genCtrl0Dword = target.genCtrl0Dword;
          isHpoFrl = target.isHpoFrl;
        }
        else
        {
          const auto layouts = GetOrderedAmdDcnLayouts(m_gpu.deviceId);
          digBeIdx = (m_pipeOrInst < AMD_DCN_MAX_DIGS) ? m_pipeOrInst : 0U;
          digFeIdx = digBeIdx;
          fmtIdx = (m_pipeOrInst < AMD_DCN_MAX_PIPES) ? m_pipeOrInst : 0U;
          genCtrl0Dword = layouts[0].genCtrl0Dword;
          params = MakeDioDcnParams(digFeIdx, fmtIdx, genCtrl0Dword, layouts[0].stride);
          CLog::Log(LOGWARNING,
                    "[DVBridge-Win] AMD DCN active DIG/HPO not matched (DEV_0x{:04X}); falling back to DIG{}/FMT{} (GEN_CTRL0_DW=0x{:04X}).",
                    m_gpu.deviceId, digFeIdx, fmtIdx, genCtrl0Dword);
        }

        g_savedAmdState.isHpoFrl = isHpoFrl;
        g_savedAmdState.digBeIdx = digBeIdx;
        g_savedAmdState.digFeIdx = digFeIdx;
        g_savedAmdState.fmtIdx = fmtIdx;
        g_savedAmdState.params = params;
        g_savedAmdState.origFmtBitDepthCtrl =
            m_mmioOps.read32(m_mmioOps.user_ctx, params.fmt_bit_depth_reg);
        g_savedAmdState.origGenCtrl0 = m_mmioOps.read32(m_mmioOps.user_ctx, params.gen_ctrl0_reg);
        g_savedAmdState.origPkt01Line =
            m_mmioOps.read32(m_mmioOps.user_ctx, params.pkt01_line_reg);
        g_savedAmdState.origPkt45Line =
            m_mmioOps.read32(m_mmioOps.user_ctx, params.pkt45_line_reg);
        g_savedAmdState.valid = true;

        if (isHpoFrl)
        {
          CLog::Log(LOGINFO,
                    "[DVBridge-Win] AMD DCN active output: HPO_FRL (OTG{}, SEG3 VPG/TB_ENC) <- FMT{}, "
                    "FMT_BIT_DEPTH=0x{:08X}, GEN_CTRL0=0x{:08X}, reqVIC={} -> enabling TV-Led Dolby Vision VSIF",
                    otgIdx, fmtIdx, g_savedAmdState.origFmtBitDepthCtrl, g_savedAmdState.origGenCtrl0,
                    int(m_ctaVic));
        }
        else
        {
          CLog::Log(LOGINFO,
                    "[DVBridge-Win] AMD DCN active output: DIG_BE{}/FE{} (MODE={}, OTG{}) <- FMT{}, "
                    "GEN_CTRL0_DW=0x{:04X}, FMT_BIT_DEPTH=0x{:08X}, GEN_CTRL0=0x{:08X}, reqVIC={} -> enabling TV-Led Dolby Vision VSIF",
                    digBeIdx, digFeIdx, target.found ? target.digMode : 0xFFU, otgIdx, fmtIdx,
                    genCtrl0Dword, g_savedAmdState.origFmtBitDepthCtrl,
                    g_savedAmdState.origGenCtrl0, int(m_ctaVic));
        }
      }

      const uint32_t curFmt = m_mmioOps.read32(m_mmioOps.user_ctx, params.fmt_bit_depth_reg);
      const uint32_t curGen0 = m_mmioOps.read32(m_mmioOps.user_ctx, params.gen_ctrl0_reg);
      if ((curFmt & AMD_FMT_ALL_DITHER_TRUNC_MASK) !=
              (AMD_FMT_TRUNCATE_EN_MASK | AMD_FMT_TRUNCATE_DEPTH_8BPC) ||
          (curGen0 & 0x00030033U) != 0x00000033U)
      {
        dvbridge_win_amd_dcn_apply_params(&m_mmioOps, &params, true, m_legacyVsif, m_ctaVic);
      }
    }
    else
    {
      if (g_savedAmdState.valid)
      {
        const dvbridge_amd_dcn_params& params = g_savedAmdState.params;

        dvbridge_win_amd_dcn_apply_params(&m_mmioOps, &params, false, m_legacyVsif, m_ctaVic);

        m_mmioOps.write32(m_mmioOps.user_ctx, params.pkt01_line_reg, g_savedAmdState.origPkt01Line);
        m_mmioOps.write32(m_mmioOps.user_ctx, params.pkt45_line_reg, g_savedAmdState.origPkt45Line);
        m_mmioOps.write32(m_mmioOps.user_ctx, params.gen_ctrl0_reg, g_savedAmdState.origGenCtrl0);
        m_mmioOps.write32(m_mmioOps.user_ctx, params.fmt_bit_depth_reg,
                          g_savedAmdState.origFmtBitDepthCtrl);
        g_savedAmdState.valid = false;

        CLog::Log(LOGINFO,
                  "[DVBridge-Win] AMD DCN {}/FMT{} restored original GEN_CTRL0 & FMT_BIT_DEPTH_CONTROL",
                  g_savedAmdState.isHpoFrl ? std::string("HPO_FRL")
                                           : ("DIG_FE" + std::to_string(g_savedAmdState.digFeIdx)),
                  g_savedAmdState.fmtIdx);
      }
      else
      {
        const AmdDcnTarget target = DetectAmdDcnTarget(m_mmioOps, m_gpu.deviceId);
        if (target.found)
        {
          dvbridge_win_amd_dcn_apply_params(&m_mmioOps, &target.params, false, m_legacyVsif,
                                            m_ctaVic);
        }
        else
        {
          const auto layouts = GetOrderedAmdDcnLayouts(m_gpu.deviceId);
          dvbridge_win_amd_dcn3_apply_dv_ex(&m_mmioOps, m_pipeOrInst, m_pipeOrInst,
                                            layouts[0].genCtrl0Dword, false, m_legacyVsif,
                                            m_ctaVic);
        }
      }
    }
    return true;
  }

  return Reject("invalid-backend");
}

} // namespace KODI::WINDOWING::WINDOWS

extern "C" {
#include "dvbridge_win_mmio.c"
#include "dvbridge_fel.c"
}
