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
    for (const auto& s : sinks)
    {
      if (s.dvCaps.status == DV_EDID_STANDARD || s.dvCaps.status == DV_EDID_LLDV_ONLY)
      {
        m_sink = s;
        break;
      }
    }
  }
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
        info.dvCaps = dvbridge_parse_edid(info.rawEdid.data(), info.rawEdid.size());
        sinks.push_back(std::move(info));
      }
      RegCloseKey(hDevReg);
    }
  }
  SetupDiDestroyDeviceInfoList(devInfo);
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
