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

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cores/VideoPlayer/DVDCodecs/Video/DVPlaybackInfo.h"

struct AVFrame;

extern "C" {
#include "drm_dvbridge.h"
#include "drm_dvbridge_edid.h"
#include "dvbridge_win_mmio.h"
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
    uint8_t hdrEotfMask = 0;
    bool supportsHDR10 = false;
    bool supportsHLG = false;
    bool supportsHDR10Plus = false;
    bool supportsDolbyVision = false;
  };

  CDVBridgeStateWin() = default;
  ~CDVBridgeStateWin();

  static CDVBridgeStateWin& GetInstance();

  /* Lazily probes Windows DXGI GPU and SetupAPI Monitor EDID VSVDB once */
  void EnsureHardwareProbed();

  /* Queries display HDR & Dolby Vision capabilities parsed from EDID */
  bool HasHDRDisplay();
  bool SupportsHDR10();
  bool SupportsHLG();
  bool SupportsHDR10Plus();
  bool SupportsDolbyVision();
  bool SupportsStandardDolbyVision();
  std::optional<unsigned> GetDisplayPeakPq();
  DVBRIDGE::OutputCapabilities GetDVOutputCapabilities(bool activeDvSource = false);

  /* Validates and packs ETSI GS CCM 001 V1.1.1 Dolby Vision metadata on Windows */
  bool ProcessDoviMetadata(const void* doviMetaPtr, size_t doviMetaSize, double pts);
  void ResetMetadataState();
  bool GetPackedMetadata(uint8_t outPackets[640], uint32_t& outCount, uint64_t& outRevision) const;
  void StoreEnhancementFrame(double ptsUs, std::shared_ptr<AVFrame> elFrame);
  void SetDoviElConfig(const uint8_t* data, size_t size);
  std::vector<uint8_t> GetDoviElConfig() const;
  bool GetFelData(std::shared_ptr<AVFrame>& outElFrame,
                  int32_t outLut[6 * 1024],
                  float outYccOffset[4],
                  float outYccRow0[4],
                  float outYccRow1[4],
                  float outYccRow2[4],
                  uint64_t& outRevision) const;

  void SetWantsNativeDvTunnel(bool wants);
  bool WantsNativeDvTunnel() const;
  void SetDvTunnelActive(bool active, uint8_t ctaVic = 93);
  bool IsDvTunnelActive() const;
  void RefreshHwTransportIfNeeded();
  static uint8_t ComputeCtaVic(unsigned width, unsigned height, float fps);
  bool EnsureMmioInitialized();

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

  mutable std::mutex m_mutex;
  bool m_probed = false;
  bool m_captured = false;
  bool m_hasMmioOps = false;
  bool m_mmioProbed = false;
  bool m_wantsNativeDvTunnel = false;
  bool m_tunnelActive = false;
  uint64_t m_packedFrames = 0;
  uint32_t m_packedPacketCount = 0;
  std::array<uint8_t, 640> m_packedBytes{};
  std::deque<std::pair<double, std::shared_ptr<AVFrame>>> m_elQueue;
  std::shared_ptr<AVFrame> m_activeElFrame;
  std::vector<uint8_t> m_doviElConfig;
  bool m_felActive = false;
  std::array<int32_t, 6 * 1024> m_felLut{};
  std::array<float, 4> m_felYccOffset{1.0f / 16.0f, 0.5f, 0.5f, 0.0f};
  std::array<float, 4> m_felYccRow0{1.1687012f, 0.0f, 1.6848145f, 0.0f};
  std::array<float, 4> m_felYccRow1{1.1687012f, -0.1879883f, -0.6528320f, 0.0f};
  std::array<float, 4> m_felYccRow2{1.1687012f, 2.1496582f, 0.0f, 0.0f};
  uint64_t m_hwRefreshCounter = 0;
  void* m_mmioMappedBase = nullptr;
  void* m_mmioSectionHandle = nullptr;
  void* m_mmioDllModule = nullptr;
  void* m_mmioDeviceHandle = nullptr;
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
