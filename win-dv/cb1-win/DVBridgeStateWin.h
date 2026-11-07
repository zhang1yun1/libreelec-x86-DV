/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Windows Display & Hardware Transport State for CB1 / DVBridge (DVBridgeStateWin.h)
 * Replaces Linux GBM/DRM Atomic CDVBridgeState (xbmc/windowing/gbm/drm/DVBridgeState.h) on Windows:
 *   1. Reads Monitor EDID from Windows SetupAPI / Display Device Registry and parses VSVDB via dvbridge_parse_edid()
 *   2. Detects GPU Vendor (AMD DCN / Intel Gen9-14 / NVIDIA) via DXGI & PnP Resource Map
 *   3. Captures & programs HDMI Dolby Vision VSIF + AVI InfoFrame + Dithering Disabler via MMIO
 *   4. Configures DXGI SwapChain4 for either:
 *      - Native TV-Led Dolby Vision RGB8 8-bit 4:4:4 Tunnel (DXGI_FORMAT_R8G8B8A8_UNORM, Full RGB, No Dither)
 *      - CB1 Master Reference / HDR10 Reconstruction (DXGI_FORMAT_R10G10B10A2_UNORM, G2084_P2020, DXGI_HDR_METADATA_HDR10)
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "../include/drm_dvbridge.h"
#include "../include/drm_dvbridge_edid.h"
#include "../include/dvbridge_win_mmio.h"
}

namespace KODI::WINDOWING::WINDOWS
{

class CDVBridgeStateWin
{
public:
  enum class Backend
  {
    NONE,
    INTEL_MMIO,
    AMD_DCN_MMIO,
    NVIDIA_NVAPI,
    DXGI_HDR10_ONLY
  };

  struct GpuAdapterInfo
  {
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint32_t subSysId = 0;
    uint32_t revision = 0;
    std::string description;
    uint64_t mmioPhysBase = 0;
    uint64_t mmioPhysLength = 0;
  };

  struct DisplaySinkInfo
  {
    std::string monitorInstanceId;
    std::string friendlyName;
    std::vector<uint8_t> rawEdid;
    dvbridge_sink_caps dvCaps{};
  };

  CDVBridgeStateWin() = default;
  ~CDVBridgeStateWin();

  static CDVBridgeStateWin& GetInstance();

  /* Lazily probes Windows DXGI GPU and SetupAPI Monitor EDID VSVDB once */
  void EnsureHardwareProbed();

  /* Validates and packs ETSI GS CCM 001 V1.1.1 Dolby Vision metadata on Windows */
  bool ProcessDoviMetadata(const void* doviMetaPtr, size_t doviMetaSize, double pts);

  /* Enumerates active Windows displays and parses Dolby Vision VSVDB from EDID */
  static std::vector<DisplaySinkInfo> EnumerateDisplays();

  /* Queries primary GPU Vendor/Device ID and PCI BAR MMIO physical address on Windows */
  static bool QueryPrimaryGpu(GpuAdapterInfo& outGpu);

  /* Attaches a user-mode/kernel-mode MMIO backend (or simulation backend) */
  void AttachMmioContext(const dvbridge_mmio_ops* ops, uint32_t pipeOrInst = 0);

  /* Captures current HDMI/Display state and activates Native Dolby Vision RGB8 Tunnel */
  bool Capture(uint8_t ctaVic = 93, bool legacyVsif = false);

  /* Stages or restores Native Dolby Vision HDMI VSIF + AVI InfoFrame + 8bpc No-Dither state */
  bool Stage(bool restore);

  void Restored()
  {
    m_captured = false;
    m_backend = Backend::NONE;
  }

  bool Captured() const { return m_captured; }
  Backend CapturedBackend() const { return m_backend; }
  const char* CaptureFailure() const { return m_failure; }
  const DisplaySinkInfo& ActiveSink() const { return m_sink; }
  const GpuAdapterInfo& ActiveGpu() const { return m_gpu; }

private:
  bool Reject(const char* reason)
  {
    m_failure = reason;
    return false;
  }

  bool m_probed = false;
  bool m_captured = false;
  bool m_hasMmioOps = false;
  uint64_t m_packedFrames = 0;
  dvbridge_mmio_ops m_mmioOps{};
  uint32_t m_pipeOrInst = 0;
  uint8_t m_ctaVic = 93;
  bool m_legacyVsif = false;
  Backend m_backend = Backend::NONE;
  const char* m_failure = "none";
  GpuAdapterInfo m_gpu{};
  DisplaySinkInfo m_sink{};
};

} // namespace KODI::WINDOWING::WINDOWS
