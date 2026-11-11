/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Windows Display & Hardware Transport State for CB1 / DVBridge (DVBridgeStateWin.cpp)
 */

#include "DVBridgeStateWin.h"
#include <cstring>

#ifndef CB1_NO_LIBPLACEBO
#define CB1_NO_LIBPLACEBO 1
#endif
extern "C" {
#include "../include/dvbridge_metadata.h"
}

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <setupapi.h>
#include <dxgi1_6.h>
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "advapi32.lib")

static const GUID GUID_DEVINTERFACE_MONITOR_WIN_DV = {
    0xe6f07b5f, 0xee97, 0x4a90, {0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7}};
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

bool CDVBridgeStateWin::ProcessDoviMetadata(const void* doviMetaPtr, size_t doviMetaSize, double pts)
{
  EnsureHardwareProbed();
  if (!doviMetaPtr || !dvbridge_metadata_bounds(doviMetaPtr, doviMetaSize))
    return false;

  const auto* meta = static_cast<const AVDOVIMetadata*>(doviMetaPtr);
  struct dvbridge_dv state{};
  state.last_pts = -1.0;
  unsigned margins[4] = {0, 0, 0, 0};
  if (!dvbridge_dv_metadata_output(&state, meta, pts, margins, true))
    return false;

  ++m_packedFrames;
  return state.count > 0;
}

CDVBridgeStateWin::~CDVBridgeStateWin()
{
  if (m_captured)
  {
    Stage(true);
    Restored();
  }
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
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
  {
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc)))
    {
      if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
          (desc.VendorId == 0x1002 || desc.VendorId == 0x8086 || desc.VendorId == 0x10DE))
      {
        outGpu.vendorId = desc.VendorId;
        outGpu.deviceId = desc.DeviceId;
        outGpu.subSysId = desc.SubSysId;
        outGpu.revision = desc.Revision;
        char narrowDesc[256]{};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                            narrowDesc, sizeof(narrowDesc), nullptr, nullptr);
        outGpu.description = narrowDesc;
        found = true;
        adapter->Release();
        break;
      }
    }
    adapter->Release();
  }
  factory->Release();
  return found;
#else
  return false;
#endif
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
    dvbridge_win_intel_apply_dv(
        &m_mmioOps, m_pipeOrInst,
        !restore, m_legacyVsif, m_ctaVic);
    return true;
  }
  else if (m_backend == Backend::AMD_DCN_MMIO)
  {
    dvbridge_win_amd_dcn3_apply_dv(
        &m_mmioOps, m_pipeOrInst,
        !restore, m_legacyVsif, m_ctaVic);
    return true;
  }

  return Reject("invalid-backend");
}

} // namespace KODI::WINDOWING::WINDOWS
