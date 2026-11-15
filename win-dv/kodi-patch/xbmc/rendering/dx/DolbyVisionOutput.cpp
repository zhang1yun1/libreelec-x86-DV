/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DolbyVisionOutput.h"

#include "DeviceResources.h"
#include "RenderContext.h"
#include "ServiceBroker.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "cores/VideoPlayer/VideoRenderers/windows/dolby/DvRpuTransport.h"
#include "filesystem/File.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "messaging/ApplicationMessenger.h"
#include "settings/DisplaySettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/StringUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/Resolution.h"
#include "windowing/WinSystem.h"

#include "platform/win32/CharsetConverter.h"
#include "platform/win32/WIN32Util.h"
#include "windowing/windows/DVBridgeStateWin.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>
#include <shellapi.h>
#include <wrl/client.h>

namespace
{
using NvU32 = uint32_t;
using NvU16 = uint16_t;
using NvU8 = uint8_t;
using NvAPI_Status = int32_t;

constexpr NvAPI_Status NVAPI_OK = 0;
constexpr NvAPI_Status NVAPI_ERROR = -1;

constexpr NvU32 NV_HDR_CMD_GET = 0;
constexpr NvU32 NV_HDR_CMD_SET = 1;

enum NV_HDR_MODE : int32_t
{
  NV_HDR_MODE_OFF = 0,
  NV_HDR_MODE_SDR = 4,
  NV_HDR_MODE_DOLBY_VISION = 7,
};

constexpr NvU8 NV_COLOR_CMD_GET = 1;
constexpr NvU8 NV_COLOR_CMD_SET = 2;
constexpr NvU8 NV_COLOR_FORMAT_RGB = 0;
constexpr NvU8 NV_DYNAMIC_RANGE_VESA = 0;
constexpr NvU32 NV_BPC_8 = 2;
constexpr NvU32 NV_COLOR_SELECTION_POLICY_USER = 0;

constexpr NvU32 NV_DISPLAY_OUTPUT_MODE_SDR = 0;
using NV_DISPLAY_OUTPUT_MODE = NvU32;

enum NV_TIMING_OVERRIDE : int32_t
{
  NV_TIMING_OVERRIDE_CURRENT = 0,
  NV_TIMING_OVERRIDE_EDID = 2,
};

constexpr NvU32 NV_FORMAT_A8R8G8B8 = 21;

struct NV_HDR_CAPABILITIES
{
  NvU32 version;
  NvU32 reservedFlags0 : 5;
  NvU32 isDolbyVisionSupported : 1;
  NvU32 reservedFlags1 : 26;
  NvU32 static_metadata_descriptor_id;
  NvU8 mastering_display_data[24];
  struct
  {
    NvU32 VSVDB_version : 3;
    NvU32 dm_bits : 16;
    NvU32 interface_supported_by_sink : 2;
    NvU32 reserved_bits : 11;
    NvU32 raw_words[6];
  } dv_static_metadata;
};
constexpr NvU32 NV_HDR_CAPABILITIES_VER = 0x30040;
static_assert(sizeof(NV_HDR_CAPABILITIES) == 0x40, "NV_HDR_CAPABILITIES size mismatch");

struct NV_HDR_COLOR_DATA
{
  NvU32 version;
  NvU32 cmd;
  NV_HDR_MODE hdrMode;
  NvU32 static_metadata_descriptor_id;
  NvU8 mastering_display_data[24];
  NvU32 hdrColorFormat;
  NvU32 hdrDynamicRange;
  NvU32 hdrBpc;
};
constexpr NvU32 NV_HDR_COLOR_DATA_VER = 0x20034;
static_assert(sizeof(NV_HDR_COLOR_DATA) == 0x34, "NV_HDR_COLOR_DATA size mismatch");
static_assert(offsetof(NV_HDR_COLOR_DATA, hdrMode) == 0x08, "hdrMode offset mismatch");
static_assert(offsetof(NV_HDR_COLOR_DATA, hdrColorFormat) == 0x28, "hdrColorFormat offset mismatch");
static_assert(offsetof(NV_HDR_COLOR_DATA, hdrDynamicRange) == 0x2C, "hdrDynamicRange offset mismatch");
static_assert(offsetof(NV_HDR_COLOR_DATA, hdrBpc) == 0x30, "hdrBpc offset mismatch");

struct NV_COLOR_DATA
{
  NvU32 version;
  NvU16 size;
  NvU8 cmd;
  NvU8 pad0;
  struct
  {
    NvU8 colorFormat;
    NvU8 colorimetry;
    NvU8 dynamicRange;
    NvU8 pad1;
    NvU32 bpc;
    NvU32 colorSelectionPolicy;
    NvU32 depth;
  } data;
};
constexpr NvU32 NV_COLOR_DATA_VER = 0x50018;
static_assert(sizeof(NV_COLOR_DATA) == 0x18, "NV_COLOR_DATA size mismatch");
static_assert(offsetof(NV_COLOR_DATA, size) == 0x04, "size offset mismatch");
static_assert(offsetof(NV_COLOR_DATA, cmd) == 0x06, "cmd offset mismatch");
static_assert(offsetof(NV_COLOR_DATA, data) == 0x08, "data offset mismatch");

struct NV_TIMING_INPUT
{
  NvU32 version;
  NvU32 width;
  NvU32 height;
  float rr;
  NvU32 flag[3];
  NV_TIMING_OVERRIDE type;
};
constexpr NvU32 NV_TIMING_INPUT_VER = 0x10020;
static_assert(sizeof(NV_TIMING_INPUT) == 0x20, "NV_TIMING_INPUT size mismatch");
static_assert(offsetof(NV_TIMING_INPUT, type) == 0x1C, "type offset mismatch");

struct NV_TIMING
{
  NvU16 HVisible;
  NvU16 HBorder;
  NvU16 HFrontPorch;
  NvU16 HSyncWidth;
  NvU16 HTotal;
  NvU8 HSyncPol;
  NvU8 VSyncPol;
  NvU16 VVisible;
  NvU16 VBorder;
  NvU16 VFrontPorch;
  NvU16 VSyncWidth;
  NvU16 VTotal;
  NvU16 pad0;
  NvU16 interlaced;
  NvU16 pad1;
  NvU32 pclk;
  struct
  {
    NvU32 flag;
    NvU16 rr;
    NvU16 pad0;
    NvU32 rrx1k;
    NvU32 aspect;
    NvU16 rep;
    NvU16 pad1;
    NvU32 status;
    NvU8 reserved[40];
  } etc;
};
static_assert(sizeof(NV_TIMING) == 0x60, "NV_TIMING size mismatch");
static_assert(offsetof(NV_TIMING, pclk) == 0x1C, "pclk offset mismatch");
static_assert(offsetof(NV_TIMING, etc) + 0x08 == 0x28, "rrx1k offset mismatch");
static_assert(offsetof(NV_TIMING, etc) + 0x14 == 0x34, "status offset mismatch");

struct NV_CUSTOM_DISPLAY
{
  NvU32 version;
  NvU32 width;
  NvU32 height;
  NvU32 depth;
  NvU32 colorFormat;
  struct
  {
    float x;
    float y;
    float w;
    float h;
  } srcPartition;
  float xRatio;
  float yRatio;
  NV_TIMING timing;
  NvU32 hwModeSetOnly;
};
constexpr NvU32 NV_CUSTOM_DISPLAY_VER = 0x10090;
static_assert(sizeof(NV_CUSTOM_DISPLAY) == 0x90, "NV_CUSTOM_DISPLAY size mismatch");
static_assert(offsetof(NV_CUSTOM_DISPLAY, timing) == 0x2C, "timing offset mismatch");

struct NvApiFunctions
{
  bool loaded{false};
  NvAPI_Status (*Initialize)(){};
  NvAPI_Status (*Unload)(){};
  NvAPI_Status (*GetDisplayIdByDisplayName)(const char*, NvU32*){};
  NvAPI_Status (*GetHdrCapabilities)(NvU32, NV_HDR_CAPABILITIES*){};
  NvAPI_Status (*HdrColorControl)(NvU32, NV_HDR_COLOR_DATA*){};
  NvAPI_Status (*ColorControl)(NvU32, NV_COLOR_DATA*){};
  NvAPI_Status (*GetOutputMode)(NvU32, NV_DISPLAY_OUTPUT_MODE*){};
  NvAPI_Status (*GetTiming)(NvU32, const NV_TIMING_INPUT*, NV_TIMING*){};
  NvAPI_Status (*TryCustomDisplay)(NvU32*, NvU32, NV_CUSTOM_DISPLAY*){};
  NvAPI_Status (*RevertCustomDisplayTrial)(NvU32*, NvU32){};
};

const NvApiFunctions& GetNvApi()
{
  static NvApiFunctions api;
  static std::once_flag once;
  std::call_once(once,
                 []()
                 {
                   HMODULE mod = LoadLibraryW(L"nvapi64.dll");
                   if (!mod)
                     return;
                   using QueryInterfaceFn = void* (*)(uint32_t);
                   auto query =
                       reinterpret_cast<QueryInterfaceFn>(GetProcAddress(mod, "nvapi_QueryInterface"));
                   if (!query)
                     return;
                   api.Initialize = reinterpret_cast<decltype(api.Initialize)>(query(0x0150e828u));
                   api.Unload = reinterpret_cast<decltype(api.Unload)>(query(0xd22bdd7eu));
                   api.GetDisplayIdByDisplayName =
                       reinterpret_cast<decltype(api.GetDisplayIdByDisplayName)>(query(0xae457190u));
                   api.GetHdrCapabilities =
                       reinterpret_cast<decltype(api.GetHdrCapabilities)>(query(0x84f2a8dfu));
                   api.HdrColorControl =
                       reinterpret_cast<decltype(api.HdrColorControl)>(query(0x351da224u));
                   api.ColorControl =
                       reinterpret_cast<decltype(api.ColorControl)>(query(0x92f9d80du));
                   api.GetOutputMode =
                       reinterpret_cast<decltype(api.GetOutputMode)>(query(0x81fed88du));
                   api.GetTiming =
                       reinterpret_cast<decltype(api.GetTiming)>(query(0x175167e9u));
                   api.TryCustomDisplay =
                       reinterpret_cast<decltype(api.TryCustomDisplay)>(query(0x1f7db630u));
                   api.RevertCustomDisplayTrial =
                       reinterpret_cast<decltype(api.RevertCustomDisplayTrial)>(query(0xcbbd40f0u));
                   api.loaded = api.Initialize && api.Unload && api.GetDisplayIdByDisplayName &&
                                api.GetHdrCapabilities && api.HdrColorControl && api.ColorControl &&
                                api.GetOutputMode && api.GetTiming && api.TryCustomDisplay &&
                                api.RevertCustomDisplayTrial;
                 });
  return api;
}

NvAPI_Status NvAPI_Initialize()
{
  const auto& api = GetNvApi();
  return api.loaded ? api.Initialize() : NVAPI_ERROR;
}

NvAPI_Status NvAPI_Unload()
{
  const auto& api = GetNvApi();
  return api.loaded ? api.Unload() : NVAPI_ERROR;
}

NvAPI_Status NvAPI_DISP_GetDisplayIdByDisplayName(const char* name, NvU32* displayId)
{
  const auto& api = GetNvApi();
  return api.loaded ? api.GetDisplayIdByDisplayName(name, displayId) : NVAPI_ERROR;
}

NvAPI_Status NvAPI_Disp_GetHdrCapabilities(NvU32 displayId, NV_HDR_CAPABILITIES* caps)
{
  const auto& api = GetNvApi();
  return api.loaded ? api.GetHdrCapabilities(displayId, caps) : NVAPI_ERROR;
}

NvAPI_Status NvAPI_Disp_HdrColorControl(NvU32 displayId, NV_HDR_COLOR_DATA* data)
{
  const auto& api = GetNvApi();
  return api.loaded ? api.HdrColorControl(displayId, data) : NVAPI_ERROR;
}

NvAPI_Status NvAPI_Disp_ColorControl(NvU32 displayId, NV_COLOR_DATA* data)
{
  const auto& api = GetNvApi();
  return api.loaded ? api.ColorControl(displayId, data) : NVAPI_ERROR;
}

NvAPI_Status NvAPI_Disp_GetOutputMode(NvU32 displayId, NV_DISPLAY_OUTPUT_MODE* mode)
{
  const auto& api = GetNvApi();
  return api.loaded ? api.GetOutputMode(displayId, mode) : NVAPI_ERROR;
}

NvAPI_Status NvAPI_DISP_GetTiming(NvU32 displayId,
                                  const NV_TIMING_INPUT* input,
                                  NV_TIMING* timing)
{
  const auto& api = GetNvApi();
  return api.loaded ? api.GetTiming(displayId, input, timing) : NVAPI_ERROR;
}

NvAPI_Status NvAPI_DISP_TryCustomDisplay(NvU32* dispIds, NvU32 count, NV_CUSTOM_DISPLAY* custom)
{
  const auto& api = GetNvApi();
  return api.loaded ? api.TryCustomDisplay(dispIds, count, custom) : NVAPI_ERROR;
}

NvAPI_Status NvAPI_DISP_RevertCustomDisplayTrial(NvU32* dispIds, NvU32 count)
{
  const auto& api = GetNvApi();
  return api.loaded ? api.RevertCustomDisplayTrial(dispIds, count) : NVAPI_ERROR;
}

bool IsNvidiaDisplay(const std::wstring& gdiName)
{
  if (NvAPI_Initialize() != NVAPI_OK)
    return false;
  NvU32 displayId = 0;
  std::string name;
  for (wchar_t c : gdiName)
    name += static_cast<char>(c);
  const bool ok = NvAPI_DISP_GetDisplayIdByDisplayName(name.c_str(), &displayId) == NVAPI_OK;
  NvAPI_Unload();
  return ok;
}

// The driver's timings for the Dolby Vision output modes. It reports 24 Hz
// with the nominal 23.976 Hz rrx1k and 59.94 Hz with 60 Hz.
struct HdmiVicFormat
{
  NvU16 width;
  NvU16 height;
  NvU16 hFrontPorch;
  NvU16 hSyncWidth;
  NvU16 hTotal;
  NvU16 vFrontPorch;
  NvU16 vSyncWidth;
  NvU16 vTotal;
  NvU32 pclk;
  NvU32 rrx1k;
  unsigned numerator;
  unsigned denominator;
};

constexpr HdmiVicFormat HDMI_VIC_FORMATS[] = {
    {3840, 2160, 1276, 88, 5500, 8, 10, 2250, 29670, 23976, 24000, 1001},
    {3840, 2160, 1276, 88, 5500, 8, 10, 2250, 29700, 23976, 24, 1},
    {3840, 2160, 1056, 88, 5280, 8, 10, 2250, 29700, 25000, 25, 1},
    {3840, 2160, 176, 88, 4400, 8, 10, 2250, 29670, 29970, 30000, 1001},
    {3840, 2160, 1056, 88, 5280, 8, 10, 2250, 59400, 50000, 50, 1},
    {3840, 2160, 176, 88, 4400, 8, 10, 2250, 59341, 60000, 60000, 1001},
    {3840, 2160, 176, 88, 4400, 8, 10, 2250, 59400, 60000, 60, 1},
    {1920, 1080, 638, 44, 2750, 4, 5, 1125, 7418, 23976, 24000, 1001},
    {1920, 1080, 638, 44, 2750, 4, 5, 1125, 7425, 23976, 24, 1},
    {1920, 1080, 528, 44, 2640, 4, 5, 1125, 7425, 25000, 25, 1},
    {1920, 1080, 88, 44, 2200, 4, 5, 1125, 7418, 29970, 30000, 1001},
    {1920, 1080, 528, 44, 2640, 4, 5, 1125, 14850, 50000, 50, 1},
    {1920, 1080, 88, 44, 2200, 4, 5, 1125, 14835, 60000, 60000, 1001},
    {1920, 1080, 88, 44, 2200, 4, 5, 1125, 14850, 60000, 60, 1},
};

// Low 16 bits of the timing status: subtype 0x1b (the driver's HDMI_VIC timing)
// with HDMI_VIC 0. Current drivers only put the HDMI OUI in the mode-7 vendor
// InfoFrame for an HDMI_VIC timing; with this tag they send the classic Dolby
// Vision InfoFrame (format 0x20, HDMI_VIC byte 0xfe), which names no mode.
// Reverting the timing ends Dolby Vision output. The value is inferred from
// the driver, not documented by NVIDIA.
constexpr NvU32 DOLBY_VISION_TIMING_STATUS = 0x1b00;
} // namespace

struct DX::CDolbyVisionOutput::Impl
{
  HWND window{};
  bool candidate{};
  bool enableAttempted{};
  bool carrierSwapChain{};
  // Kodi's format of an exclusive fullscreen swapchain used as the carrier.
  DXGI_FORMAT exclusiveFormat{DXGI_FORMAT_UNKNOWN};
  bool suspended{};
  bool pausedForFocus{};
  bool loggedGui{};
  double fps{};
  int width{};
  int height{};
  Microsoft::WRL::ComPtr<ID3D11Texture2D> packedFrame;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> guiFrame;
  std::function<bool(ID3D11Texture2D*, ID3D11Texture2D*)> compositeGui;

  NvU32 displayId{};
  LUID targetAdapter{};
  UINT32 targetId{};
  NV_HDR_COLOR_DATA originalHdr{};
  // The output colour to restore when entering changed it.
  std::optional<NV_COLOR_DATA> originalColour;
  bool initialized{};
  bool hdmiVicTiming{};
  bool mmioActive{};
  bool active{};
  std::string mode;
};

namespace
{
std::mutex refusalLock;
std::string lastRefusal;

void LogRefusal(int level, const std::string& reason)
{
  std::unique_lock guard(refusalLock);
  if (reason == lastRefusal)
    return;
  lastRefusal = reason;
  CLog::Log(level, "Dolby Vision output: {}", reason);
}

void ClearRefusal()
{
  std::unique_lock guard(refusalLock);
  lastRefusal.clear();
}

const HdmiVicFormat* FormatFor(long width, long height, double refresh)
{
  for (const HdmiVicFormat& format : HDMI_VIC_FORMATS)
  {
    if (format.width == width && format.height == height &&
        std::abs(refresh - static_cast<double>(format.numerator) / format.denominator) < 0.01)
      return &format;
  }
  return nullptr;
}

struct ActiveDisplay
{
  const HdmiVicFormat* format{nullptr};
  double refresh{0.0};
  bool hdmi{false};
};

// A display output. GDI source names can move to another display when modes
// change on a multi-monitor desktop; the adapter and target ID stay.
struct DisplayTarget
{
  LUID adapterId{};
  UINT32 id{};
};

bool QueryActivePaths(std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
                      std::vector<DISPLAYCONFIG_MODE_INFO>& modes)
{
  UINT32 pathCount = 0;
  UINT32 modeCount = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) !=
          ERROR_SUCCESS ||
      pathCount == 0)
    return false;
  paths.resize(pathCount);
  modes.resize(modeCount);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                         modes.data(), nullptr) != ERROR_SUCCESS)
    return false;
  paths.resize(pathCount);
  modes.resize(modeCount);
  return true;
}

// The output currently showing the GDI source gdiName.
std::optional<DisplayTarget> FindTarget(const std::wstring& gdiName)
{
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  if (!QueryActivePaths(paths, modes))
    return {};
  for (const DISPLAYCONFIG_PATH_INFO& path : paths)
  {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source),
                     path.sourceInfo.adapterId, path.sourceInfo.id};
    if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS &&
        _wcsicmp(source.viewGdiDeviceName, gdiName.c_str()) == 0)
      return DisplayTarget{path.targetInfo.adapterId, path.targetInfo.id};
  }
  return {};
}

ActiveDisplay GetActiveDisplay(const DisplayTarget& target)
{
  ActiveDisplay display;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  if (!QueryActivePaths(paths, modes))
    return display;
  for (const DISPLAYCONFIG_PATH_INFO& path : paths)
  {
    if (path.targetInfo.id != target.id ||
        path.targetInfo.adapterId.LowPart != target.adapterId.LowPart ||
        path.targetInfo.adapterId.HighPart != target.adapterId.HighPart)
      continue;

    display.hdmi = path.targetInfo.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    if (path.targetInfo.modeInfoIdx >= modes.size() ||
        modes[path.targetInfo.modeInfoIdx].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_TARGET)
      return display;

    const DISPLAYCONFIG_VIDEO_SIGNAL_INFO& signal =
        modes[path.targetInfo.modeInfoIdx].targetMode.targetVideoSignalInfo;
    display.refresh = path.targetInfo.refreshRate.Denominator == 0
                          ? 0.0
                          : static_cast<double>(path.targetInfo.refreshRate.Numerator) /
                                path.targetInfo.refreshRate.Denominator;
    display.format = FormatFor(signal.activeSize.cx, signal.activeSize.cy, display.refresh);
    return display;
  }
  return display;
}

ActiveDisplay GetActiveDisplay(const std::wstring& gdiName)
{
  const std::optional<DisplayTarget> target = FindTarget(gdiName);
  return target ? GetActiveDisplay(*target) : ActiveDisplay{};
}

// The tunnel is TV-led ("standard") Dolby Vision. Version 0 and 1 Dolby VSVDBs
// always include it; version 2 sinks state it in the interface field, where 0
// and 1 mean low-latency (player-led) only.
std::optional<NvU32> FindStandardDolbyVisionDisplay(const std::wstring& gdiName, bool log = true)
{
  NvU32 displayId = 0;
  std::string name; // GDI display names are ASCII (\\.\DISPLAYn)
  for (wchar_t c : gdiName)
    name += static_cast<char>(c);
  const NvAPI_Status idStatus = NvAPI_DISP_GetDisplayIdByDisplayName(name.c_str(), &displayId);
  if (idStatus != NVAPI_OK)
  {
    if (log)
      CLog::Log(LOGWARNING, "Dolby Vision output: {} is not an NVIDIA display (status {}).", name,
                static_cast<int>(idStatus));
    return {};
  }

  NV_HDR_CAPABILITIES capabilities{};
  capabilities.version = NV_HDR_CAPABILITIES_VER;
  const NvAPI_Status hdrStatus = NvAPI_Disp_GetHdrCapabilities(displayId, &capabilities);
  if (hdrStatus != NVAPI_OK || !capabilities.isDolbyVisionSupported)
  {
    if (log)
      CLog::Log(LOGWARNING,
                "Dolby Vision output: {} does not report Dolby Vision support (status {}, "
                "flag {}).",
                name, static_cast<int>(hdrStatus),
                static_cast<unsigned>(capabilities.isDolbyVisionSupported));
    return {};
  }
  const auto& dv = capabilities.dv_static_metadata;
  const bool standard = dv.VSVDB_version < 2 || dv.interface_supported_by_sink >= 2;
  if (log)
    CLog::Log(standard ? LOGDEBUG : LOGWARNING,
              "Dolby Vision output: {} reports Dolby VSVDB version {}, interface {}{}.", name,
              static_cast<unsigned>(dv.VSVDB_version),
              static_cast<unsigned>(dv.interface_supported_by_sink),
              standard ? "" : "; only low-latency Dolby Vision, which the tunnel does not send");
  if (!standard)
    return {};
  return displayId;
}

// Initializes NVAPI and finds Kodi's display; on success NVAPI stays
// initialized for the caller.
std::optional<NvU32> OpenDisplay(const std::wstring& gdiName, bool log = true)
{
  const NvAPI_Status init = NvAPI_Initialize();
  if (init != NVAPI_OK)
  {
    if (log)
      CLog::Log(LOGWARNING, "Dolby Vision output: NVAPI initialization failed (status {}).",
                static_cast<int>(init));
    return {};
  }
  const std::optional<NvU32> displayId = FindStandardDolbyVisionDisplay(gdiName, log);
  if (!displayId)
    NvAPI_Unload();
  return displayId;
}

std::optional<NV_HDR_COLOR_DATA> GetHdr(NvU32 displayId)
{
  NV_HDR_COLOR_DATA hdr{};
  hdr.version = NV_HDR_COLOR_DATA_VER;
  hdr.cmd = NV_HDR_CMD_GET;
  if (NvAPI_Disp_HdrColorControl(displayId, &hdr) != NVAPI_OK)
    return {};
  return hdr;
}

NvAPI_Status SetHdrStatus(NvU32 displayId, NV_HDR_COLOR_DATA hdr)
{
  hdr.version = NV_HDR_COLOR_DATA_VER;
  hdr.cmd = NV_HDR_CMD_SET;
  return NvAPI_Disp_HdrColorControl(displayId, &hdr);
}

std::optional<NV_COLOR_DATA> GetColour(NvU32 displayId)
{
  NV_COLOR_DATA colour{};
  colour.version = NV_COLOR_DATA_VER;
  colour.size = sizeof(colour);
  colour.cmd = NV_COLOR_CMD_GET;
  if (NvAPI_Disp_ColorControl(displayId, &colour) != NVAPI_OK)
    return {};
  return colour;
}

NvAPI_Status SetColour(NvU32 displayId, NV_COLOR_DATA colour)
{
  colour.version = NV_COLOR_DATA_VER;
  colour.size = sizeof(colour);
  colour.cmd = NV_COLOR_CMD_SET;
  return NvAPI_Disp_ColorControl(displayId, &colour);
}

// The output colour is a persistent driver setting, so the one to restore is
// also kept on disk for a session that ends during Dolby Vision output.
constexpr char SAVED_COLOUR_FILE[] = "special://temp/dolbyvision-colour.bin";

void SaveColour(const NV_COLOR_DATA& colour)
{
  XFILE::CFile file;
  if (!file.OpenForWrite(SAVED_COLOUR_FILE, true) ||
      file.Write(&colour, sizeof(colour)) != static_cast<ssize_t>(sizeof(colour)))
    CLog::Log(LOGWARNING, "Dolby Vision output: saving the output colour failed.");
}

std::optional<NV_COLOR_DATA> LoadSavedColour()
{
  XFILE::CFile file;
  NV_COLOR_DATA colour{};
  if (!file.Open(SAVED_COLOUR_FILE) ||
      file.Read(&colour, sizeof(colour)) != static_cast<ssize_t>(sizeof(colour)) ||
      colour.version != NV_COLOR_DATA_VER)
    return {};
  return colour;
}

void RestoreColour(NvU32 displayId, const NV_COLOR_DATA& colour)
{
  const NvAPI_Status status = SetColour(displayId, colour);
  if (status != NVAPI_OK)
  {
    CLog::Log(LOGWARNING, "Dolby Vision output: restoring the output colour failed ({}).",
              static_cast<int>(status));
    return;
  }
  XFILE::CFile::Delete(SAVED_COLOUR_FILE);
}

constexpr wchar_t EXIT_HELPER_ARGUMENT[] = L"--dv-exit-helper";
constexpr wchar_t EXIT_HELPER_PARTNER[] = L"partner";

// Starts this executable as an exit helper and waits for it to exit.
bool RunExitHelperProcess(NvU32 displayId, bool partner)
{
  wchar_t path[MAX_PATH]{};
  if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0)
    return false;
  std::wstring commandLine = L"\"" + std::wstring(path) + L"\" " + EXIT_HELPER_ARGUMENT + L" " +
                             std::to_wstring(displayId);
  if (partner)
    commandLine += std::wstring(L" ") + EXIT_HELPER_PARTNER;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(path, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      nullptr, &startup, &process))
    return false;
  CloseHandle(process.hThread);
  const bool exited = WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0;
  DWORD exitCode = 1;
  if (exited)
    GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(process.hProcess);
  return exited && exitCode == 0;
}

// The driver ignores an HDR mode OFF request on current Windows, so its Dolby
// Vision output state would stay. Its process-exit cleanup ends that state
// when two processes have set an HDR mode and the later one exits first.
bool EndDriverDolbyVision(NvU32 displayId)
{
  if (RunExitHelperProcess(displayId, false))
  {
    for (unsigned attempt = 0; attempt < 10; ++attempt)
    {
      const std::optional<NV_HDR_COLOR_DATA> hdr = GetHdr(displayId);
      if (hdr && hdr->hdrMode == NV_HDR_MODE_OFF)
        return true;
      Sleep(50);
    }
  }
  CLog::Log(LOGWARNING, "Dolby Vision output: the driver did not end its Dolby Vision state.");
  return false;
}

bool ReportedSdr(NvU32 displayId)
{
  NV_DISPLAY_OUTPUT_MODE mode{};
  return NvAPI_Disp_GetOutputMode(displayId, &mode) == NVAPI_OK &&
         mode == NV_DISPLAY_OUTPUT_MODE_SDR;
}

std::optional<NV_TIMING> GetTiming(NvU32 displayId,
                                   NV_TIMING_OVERRIDE type,
                                   NvU32 width = 0,
                                   NvU32 height = 0,
                                   float refresh = 0.0f)
{
  NV_TIMING_INPUT input{};
  input.version = NV_TIMING_INPUT_VER;
  input.type = type;
  if (type != NV_TIMING_OVERRIDE_CURRENT)
  {
    input.width = width;
    input.height = height;
    input.rr = refresh;
  }
  NV_TIMING timing{};
  if (NvAPI_DISP_GetTiming(displayId, &input, &timing) != NVAPI_OK)
    return {};
  return timing;
}

bool IsMovieTiming(const NV_TIMING& t, const HdmiVicFormat& format)
{
  // Drivers report 1000/1001 rates either at the nominal rate or the exact one;
  // the pixel clock tells those formats apart.
  const NvU32 exactRrx1k = (format.numerator * 1000 + format.denominator / 2) / format.denominator;
  const auto within1 = [](NvU32 value, NvU32 expected)
  { return value + 1 >= expected && value <= expected + 1; };
  return t.HVisible == format.width && t.VVisible == format.height && t.HBorder == 0 &&
         t.VBorder == 0 && t.HTotal == format.hTotal && t.VTotal == format.vTotal &&
         within1(t.pclk, format.pclk) && t.HFrontPorch == format.hFrontPorch &&
         t.HSyncWidth == format.hSyncWidth && t.VFrontPorch == format.vFrontPorch &&
         t.VSyncWidth == format.vSyncWidth && t.HSyncPol == 0 && t.VSyncPol == 0 &&
         t.interlaced == 0 &&
         (within1(t.etc.rrx1k, format.rrx1k) || within1(t.etc.rrx1k, exactRrx1k));
}

std::string DescribeTiming(const std::optional<NV_TIMING>& t)
{
  if (!t)
    return "none";
  return StringUtils::Format(
      "{}x{} total {}x{} front porch {}/{} sync {}/{} polarity {}/{} pclk {} rrx1k {}{}",
      t->HVisible, t->VVisible, t->HTotal, t->VTotal, t->HFrontPorch, t->VFrontPorch,
      t->HSyncWidth, t->VSyncWidth, t->HSyncPol, t->VSyncPol, t->pclk, t->etc.rrx1k,
      t->interlaced ? " interlaced" : "");
}

bool HasHdmiVicTiming(NvU32 displayId, const HdmiVicFormat& format)
{
  const std::optional<NV_TIMING> current = GetTiming(displayId, NV_TIMING_OVERRIDE_CURRENT);
  const bool tagged = current && IsMovieTiming(*current, format) &&
                      (current->etc.status & 0xffff) == DOLBY_VISION_TIMING_STATUS;
  if (!tagged)
    CLog::Log(LOGERROR, "Dolby Vision output: the HDMI_VIC 0 timing is missing (status {:#x}).",
              current ? current->etc.status : 0);
  return tagged;
}

bool ApplyHdmiVicTiming(NvU32 displayId, const HdmiVicFormat& format)
{
  // The EDID lookup can return the 1000/1001 variant; the current mode is the
  // one Kodi switched to.
  const std::optional<NV_TIMING> edid =
      GetTiming(displayId, NV_TIMING_OVERRIDE_EDID, format.width, format.height,
                static_cast<float>(format.numerator) / format.denominator);
  std::optional<NV_TIMING> timing = edid;
  if (!timing || !IsMovieTiming(*timing, format))
    timing = GetTiming(displayId, NV_TIMING_OVERRIDE_CURRENT);
  if (!timing || !IsMovieTiming(*timing, format))
  {
    CLog::Log(
        LOGERROR, "Dolby Vision output: the EDID {}x{}/{:.3f} timing is not the expected one.",
        format.width, format.height, static_cast<double>(format.numerator) / format.denominator);
    CLog::Log(LOGERROR, "Dolby Vision output: EDID timing: {}; current timing: {}.",
              DescribeTiming(edid), DescribeTiming(timing));
    return false;
  }
  timing->etc.status = (timing->etc.status & 0xffff0000u) | DOLBY_VISION_TIMING_STATUS;
  NV_CUSTOM_DISPLAY custom{};
  custom.version = NV_CUSTOM_DISPLAY_VER;
  custom.width = format.width;
  custom.height = format.height;
  custom.depth = 32;
  custom.colorFormat = NV_FORMAT_A8R8G8B8;
  custom.srcPartition = {0.0f, 0.0f, 1.0f, 1.0f};
  custom.xRatio = 1.0f;
  custom.yRatio = 1.0f;
  custom.timing = *timing;
  // A trial timing is never saved to the display's mode list.
  NvU32 id = displayId;
  const NvAPI_Status status = NvAPI_DISP_TryCustomDisplay(&id, 1, &custom);
  if (status != NVAPI_OK)
  {
    CLog::Log(LOGERROR, "Dolby Vision output: HDMI_VIC 0 custom timing rejected ({}).",
              static_cast<int>(status));
    return false;
  }
  // The driver applies the timing with a mode set.
  for (unsigned attempt = 0; attempt < 20; ++attempt)
  {
    const std::optional<NV_TIMING> current = GetTiming(displayId, NV_TIMING_OVERRIDE_CURRENT);
    if (current && (current->etc.status & 0xffff) == DOLBY_VISION_TIMING_STATUS)
      return true;
    Sleep(100);
  }
  CLog::Log(LOGERROR, "Dolby Vision output: the HDMI_VIC 0 timing was not applied.");
  return false;
}

void RevertHdmiVicTiming(NvU32 displayId)
{
  NvU32 id = displayId;
  const NvAPI_Status status = NvAPI_DISP_RevertCustomDisplayTrial(&id, 1);
  if (status != NVAPI_OK)
    CLog::Log(LOGWARNING, "Dolby Vision output: reverting the HDMI_VIC timing failed ({}).",
              static_cast<int>(status));
}

struct EntryState
{
  const HdmiVicFormat* format{};
  DisplayTarget target{};
  NvU32 displayId{};
  NV_HDR_COLOR_DATA hdr{};
  NV_TIMING timing{};
  bool retainedReplay{};
};

// The read-only conditions for entering Dolby Vision output for a width x height
// stream at fps. On success NVAPI stays initialized for the caller. A query
// (not the entry itself) accepts a Windows HDR desktop that Kodi will switch.
std::optional<EntryState> CheckEntry(const std::wstring& gdiName,
                                     int width,
                                     int height,
                                     double fps,
                                     bool query,
                                     bool* hdrStillOn = nullptr)
{
  if (!DX::CDolbyVisionOutput::IsAllowedBySettings())
    return {};
  if (CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
          CSettings::SETTING_VIDEOPLAYER_ADJUSTREFRESHRATE) == ADJUST_REFRESHRATE_OFF)
  {
    LogRefusal(LOGWARNING, "requires \"Adjust display refresh rate\".");
    return {};
  }
  if (fps <= 0.0)
  {
    LogRefusal(LOGDEBUG, "the stream's frame rate is not known yet.");
    return {};
  }

  EntryState entry;
  const DX::CDolbyVisionOutput::OutputMode mode =
      DX::CDolbyVisionOutput::SelectedMode(width, height, fps);
  entry.format = FormatFor(mode.width, mode.height, mode.refresh);
  if (!entry.format)
  {
    LogRefusal(LOGWARNING,
               StringUtils::Format("no supported display mode for a {}x{} stream ({}x{} {:.3f} Hz "
                                   "selected).",
                                   width, height, mode.width, mode.height, mode.refresh));
    return {};
  }

  const std::optional<DisplayTarget> target = FindTarget(gdiName);
  const ActiveDisplay current = target ? GetActiveDisplay(*target) : ActiveDisplay{};
  if (!current.hdmi)
  {
    LogRefusal(LOGWARNING, "requires an HDMI connection.");
    return {};
  }
  entry.target = *target;

  const std::optional<NvU32> displayId = OpenDisplay(gdiName);
  if (!displayId)
    return {};
  entry.displayId = *displayId;
  const std::optional<NV_HDR_COLOR_DATA> hdr = GetHdr(entry.displayId);
  const std::optional<NV_TIMING> timing = GetTiming(entry.displayId, NV_TIMING_OVERRIDE_CURRENT);
  if (!hdr || !timing)
  {
    LogRefusal(LOGWARNING, StringUtils::Format("NVAPI could not read the display's {}.",
                                               hdr ? "timing" : "HDR state"));
    NvAPI_Unload();
    return {};
  }
  entry.hdr = *hdr;
  entry.timing = *timing;

  const NV_HDR_MODE entryMode = entry.hdr.hdrMode;
  // The driver keeps reporting mode 7 when ending its Dolby Vision state failed.
  // That is accepted on an SDR desktop whose current timing is an untagged movie
  // timing; it can still be the desktop rate while Kodi switches to the stream's.
  entry.retainedReplay = entryMode == NV_HDR_MODE_DOLBY_VISION && ReportedSdr(entry.displayId) &&
                         current.format && IsMovieTiming(entry.timing, *current.format) &&
                         (entry.timing.etc.status & 0xffff) != DOLBY_VISION_TIMING_STATUS;
  const HDR_STATUS windowsHdr = CWIN32Util::GetWindowsHDRStatus();
  // Kodi's renderer turns Windows HDR off for a Dolby Vision stream when it
  // may switch HDR, and back on at the end.
  const bool kodiSwitchesHdr = query && windowsHdr == HDR_STATUS::HDR_ON &&
                               CServiceBroker::GetWinSystem()->IsHDRDisplaySettingEnabled();
  if (!kodiSwitchesHdr &&
      ((entryMode != NV_HDR_MODE_OFF && !entry.retainedReplay) || windowsHdr != HDR_STATUS::HDR_OFF))
  {
    if (hdrStillOn)
      *hdrStillOn = true;
    LogRefusal(LOGWARNING,
               StringUtils::Format("requires an SDR desktop with Windows HDR off, or \"Adjust "
                                   "display HDR mode\" to turn it off (NVAPI mode {}, Windows HDR "
                                   "{}).",
                                   static_cast<int>(entryMode), static_cast<int>(windowsHdr)));
    NvAPI_Unload();
    return {};
  }
  ClearRefusal();
  return entry;
}

// Recreating an exclusive fullscreen swapchain leaves exclusive fullscreen,
// and DXGI then restores the desktop mode, so its format is changed in place.
bool ResizeExclusiveSwapChain(DX::DeviceResources& resources, DXGI_FORMAT format)
{
  IDXGISwapChain1* swapChain = resources.GetSwapChain();
  DXGI_SWAP_CHAIN_DESC1 desc{};
  if (!swapChain || FAILED(swapChain->GetDesc1(&desc)))
    return false;
  resources.ReleaseBackBuffer();
  const HRESULT hr = swapChain->ResizeBuffers(desc.BufferCount, desc.Width, desc.Height, format,
                                              DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
  resources.CreateBackBuffer();
  if (FAILED(hr))
    CLog::Log(LOGERROR, "Dolby Vision output: resizing the exclusive swapchain failed ({:#x}).",
              static_cast<unsigned long>(hr));
  return SUCCEEDED(hr);
}
} // namespace

DX::CDolbyVisionOutput::CDolbyVisionOutput() : m_impl(std::make_unique<Impl>())
{
}

DX::CDolbyVisionOutput::~CDolbyVisionOutput()
{
  Restore();
}

bool DX::CDolbyVisionOutput::IsAllowedBySettings()
{
  return CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
      CSettings::SETTING_VIDEOPLAYER_DOLBYVISIONOUTPUT);
}

DX::CDolbyVisionOutput::OutputMode DX::CDolbyVisionOutput::SelectedMode(int width,
                                                                        int height,
                                                                        double fps)
{
  const RESOLUTION_INFO target = CDisplaySettings::GetInstance().GetResolutionInfo(
      CResolutionUtils::ChooseBestResolution(static_cast<float>(fps), width, height, false));
  return {static_cast<unsigned>(target.iScreenWidth), static_cast<unsigned>(target.iScreenHeight),
          target.fRefreshRate};
}

bool DX::CDolbyVisionOutput::IsSupportedDisplay(const std::wstring& gdiName)
{
  auto check = [&gdiName](bool log)
  {
    if (!IsNvidiaDisplay(gdiName))
      return KODI::WINDOWING::WINDOWS::CDVBridgeStateWin::GetInstance().SupportsStandardDolbyVision();
    if (!GetActiveDisplay(gdiName).hdmi)
    {
      if (log)
        CLog::Log(LOGWARNING, "Dolby Vision output: {} is not an active HDMI output.",
                  KODI::PLATFORM::WINDOWS::FromW(gdiName));
      return false;
    }
    if (!OpenDisplay(gdiName, log))
      return false;
    NvAPI_Unload();
    return true;
  };

  // Capability queries are frequent (settings, info labels), so the answer is
  // kept for a while and its reason is logged only when it changes.
  static std::mutex cacheLock;
  static std::wstring cachedName;
  static int cachedResult = -1;
  static std::chrono::steady_clock::time_point checkedAt;
  std::unique_lock lock(cacheLock);
  const auto now = std::chrono::steady_clock::now();
  if (gdiName == cachedName && cachedResult >= 0 && now - checkedAt < std::chrono::seconds(10))
    return cachedResult != 0;

  const bool supported = check(false);
  if (gdiName != cachedName || static_cast<int>(supported) != cachedResult)
    check(true);
  cachedName = gdiName;
  cachedResult = supported;
  checkedAt = now;
  return supported;
}

bool DX::CDolbyVisionOutput::CanEnableOn(const std::wstring& gdiName,
                                         int width,
                                         int height,
                                         double fps)
{
  if (IsNvidiaDisplay(gdiName))
  {
    if (!CheckEntry(gdiName, width, height, fps, true))
      return false;
    NvAPI_Unload();
    return true;
  }

  if (!IsAllowedBySettings())
    return false;
  if (fps <= 0.0)
  {
    LogRefusal(LOGDEBUG, "the stream's frame rate is not known yet.");
    return false;
  }
  auto& winDv = KODI::WINDOWING::WINDOWS::CDVBridgeStateWin::GetInstance();
  if (!winDv.SupportsStandardDolbyVision())
  {
    LogRefusal(LOGWARNING, "display EDID does not support standard TV-led Dolby Vision.");
    return false;
  }
  const OutputMode mode = SelectedMode(width, height, fps);
  const bool validMode =
      (mode.width == 3840 && mode.height == 2160) || (mode.width == 1920 && mode.height == 1080);
  if (!validMode)
  {
    LogRefusal(LOGWARNING,
               StringUtils::Format("no supported display mode for a {}x{} stream ({}x{} {:.3f} Hz "
                                   "selected).",
                                   width, height, mode.width, mode.height, mode.refresh));
    return false;
  }
  const HDR_STATUS windowsHdr = CWIN32Util::GetWindowsHDRStatus();
  const bool kodiSwitchesHdr =
      windowsHdr == HDR_STATUS::HDR_ON && CServiceBroker::GetWinSystem()->IsHDRDisplaySettingEnabled();
  if (windowsHdr != HDR_STATUS::HDR_OFF && !kodiSwitchesHdr)
  {
    LogRefusal(LOGWARNING,
               "requires an SDR desktop with Windows HDR off, or \"Adjust display HDR mode\" to "
               "turn it off.");
    return false;
  }
  ClearRefusal();
  return true;
}

bool DX::CDolbyVisionOutput::Enable(const std::wstring& gdiName,
                                    int width,
                                    int height,
                                    double fps,
                                    const std::function<bool()>& prepareOutput)
{
  if (IsNvidiaDisplay(gdiName))
  {
    // The driver and Windows report a Windows HDR switch made for this stream
    // with a delay.
  std::optional<EntryState> entry;
  for (unsigned attempt = 0; attempt < 30; ++attempt)
  {
    bool hdrStillOn = false;
    entry = CheckEntry(gdiName, width, height, fps, false, &hdrStillOn);
    if (entry || !hdrStillOn)
      break;
    Sleep(100);
  }
  if (!entry)
    return false;
  m_impl->initialized = true;
  m_impl->displayId = entry->displayId;
  m_impl->targetAdapter = entry->target.adapterId;
  m_impl->targetId = entry->target.id;
  m_impl->originalHdr = entry->hdr;
  const HdmiVicFormat& format = *entry->format;
  if (entry->retainedReplay)
    m_impl->originalHdr.hdrMode = NV_HDR_MODE_OFF;

  // A colour depth set in the NVIDIA Control Panel stays in force in Dolby
  // Vision mode, and above 8 bpc the display misreads the 8-bit tunnel.
  const std::optional<NV_COLOR_DATA> colour = GetColour(m_impl->displayId);
  if (colour && colour->data.colorSelectionPolicy == NV_COLOR_SELECTION_POLICY_USER &&
      (colour->data.bpc != NV_BPC_8 || colour->data.colorFormat != NV_COLOR_FORMAT_RGB))
  {
    NV_COLOR_DATA tunnel = *colour;
    tunnel.data.colorFormat = NV_COLOR_FORMAT_RGB;
    tunnel.data.dynamicRange = NV_DYNAMIC_RANGE_VESA;
    tunnel.data.bpc = NV_BPC_8;
    SaveColour(*colour);
    const NvAPI_Status status = SetColour(m_impl->displayId, tunnel);
    if (status != NVAPI_OK)
    {
      CLog::Log(LOGERROR, "Dolby Vision output: setting 8 bpc RGB output failed ({}).",
                static_cast<int>(status));
      XFILE::CFile::Delete(SAVED_COLOUR_FILE);
      Restore();
      return false;
    }
    m_impl->originalColour = *colour;
    CLog::Log(LOGINFO,
              "Dolby Vision output: switched the output colour (format {}, bpc {}) to 8 bpc RGB.",
              static_cast<int>(colour->data.colorFormat), static_cast<int>(colour->data.bpc));
  }

  m_impl->hdmiVicTiming = true;
  if (!ApplyHdmiVicTiming(m_impl->displayId, format))
  {
    Restore();
    return false;
  }

  NV_HDR_COLOR_DATA dolbyVision = m_impl->originalHdr;
  dolbyVision.hdrMode = NV_HDR_MODE_DOLBY_VISION;
  dolbyVision.hdrColorFormat = NV_COLOR_FORMAT_RGB;
  dolbyVision.hdrDynamicRange = NV_DYNAMIC_RANGE_VESA;
  dolbyVision.hdrBpc = NV_BPC_8;

  // Windows lists the HDMI_VIC timing at the driver's nominal rate; the
  // timing checks below still require the exact pixel clock.
  unsigned numerator = format.numerator;
  unsigned denominator = format.denominator;
  if (std::abs(static_cast<double>(numerator) / denominator - format.rrx1k / 1000.0) >= 0.01)
  {
    numerator = format.rrx1k;
    denominator = 1000;
  }
  // Wait for Kodi's refresh-rate switch to reach the display.
  bool ready = false;
  double refresh = 0.0;
  if (prepareOutput())
  {
    for (unsigned attempt = 0; attempt < 20 && !ready; ++attempt)
    {
      const ActiveDisplay display = GetActiveDisplay(entry->target);
      refresh = display.refresh;
      ready =
          display.format && std::abs(refresh - static_cast<double>(numerator) / denominator) < 0.01;
      if (!ready)
        Sleep(100);
    }
  }
  if (!ready)
  {
    CLog::Log(LOGERROR, "Dolby Vision output: output preparation failed (refresh {:.3f}Hz).",
              refresh);
    Restore();
    return false;
  }

  const NvAPI_Status setStatus = SetHdrStatus(m_impl->displayId, dolbyVision);
  const std::optional<NV_HDR_COLOR_DATA> observed = GetHdr(m_impl->displayId);
  m_impl->active = true;
  if (setStatus != NVAPI_OK || !observed || observed->hdrMode != NV_HDR_MODE_DOLBY_VISION ||
      observed->hdrColorFormat != NV_COLOR_FORMAT_RGB ||
      observed->hdrDynamicRange != NV_DYNAMIC_RANGE_VESA || observed->hdrBpc != NV_BPC_8)
  {
    CLog::Log(
        LOGERROR,
        "Dolby Vision output: NVAPI rejected or did not verify mode 7 (set {}, get {}, mode {}).",
        static_cast<int>(setStatus), observed ? "ok" : "failed",
        observed ? static_cast<int>(observed->hdrMode) : -1);
    Restore();
    return false;
  }
  if (!HasHdmiVicTiming(m_impl->displayId, format))
  {
    Restore();
    return false;
  }
  m_impl->mode = StringUtils::Format("{}x{} {:.3f} Hz", format.width, format.height,
                                     static_cast<double>(numerator) / denominator);
  CLog::Log(LOGINFO, "Dolby Vision output: entered at {}.", m_impl->mode);
  return true;
  }

  for (unsigned attempt = 0; attempt < 30; ++attempt)
  {
    if (CWIN32Util::GetWindowsHDRStatus() == HDR_STATUS::HDR_OFF)
      break;
    Sleep(100);
  }
  if (CWIN32Util::GetWindowsHDRStatus() != HDR_STATUS::HDR_OFF)
  {
    LogRefusal(LOGWARNING, "Windows HDR stayed active.");
    return false;
  }

  auto& winDv = KODI::WINDOWING::WINDOWS::CDVBridgeStateWin::GetInstance();
  if (!winDv.SupportsStandardDolbyVision() || !winDv.EnsureMmioInitialized())
  {
    LogRefusal(LOGWARNING, "MMIO Dolby Vision hardware transport unavailable.");
    return false;
  }

  if (!prepareOutput())
  {
    CLog::Log(LOGERROR, "Dolby Vision output: preparing the 8-bit carrier swapchain failed.");
    Restore();
    return false;
  }

  const OutputMode mode = SelectedMode(width, height, fps);
  const uint8_t vic = KODI::WINDOWING::WINDOWS::CDVBridgeStateWin::ComputeCtaVic(
      mode.width, mode.height, static_cast<float>(mode.refresh));
  m_impl->mmioActive = true;
  winDv.SetWantsNativeDvTunnel(true);
  winDv.SetDvTunnelActive(true, vic);
  if (!winDv.IsDvTunnelActive())
  {
    CLog::Log(LOGERROR, "Dolby Vision output: activating MMIO transport failed.");
    Restore();
    return false;
  }

  m_impl->active = true;
  m_impl->mode = StringUtils::Format("{}x{} {:.3f} Hz", mode.width, mode.height, mode.refresh);
  ClearRefusal();
  CLog::Log(LOGINFO, "Dolby Vision output: entered via MMIO at {} (VIC {}).", m_impl->mode, vic);
  return true;
}

void DX::CDolbyVisionOutput::Restore()
{
  if (m_impl->mmioActive)
  {
    auto& winDv = KODI::WINDOWING::WINDOWS::CDVBridgeStateWin::GetInstance();
    winDv.SetDvTunnelActive(false);
    winDv.SetWantsNativeDvTunnel(false);
    m_impl->mmioActive = false;
    m_impl->active = false;
  }
  if (!m_impl->initialized)
    return;

  if (m_impl->active)
  {
    const NvAPI_Status off = SetHdrStatus(m_impl->displayId, m_impl->originalHdr);
    if (off != NVAPI_OK)
      CLog::Log(LOGERROR, "Dolby Vision output: restoring the HDR mode failed ({}).",
                static_cast<int>(off));
    const auto start = std::chrono::steady_clock::now();
    if (EndDriverDolbyVision(m_impl->displayId))
      CLog::Log(LOGINFO, "Dolby Vision output: the driver ended its Dolby Vision state ({} ms).",
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count());
    m_impl->active = false;
  }
  // Reverting the HDMI_VIC timing stops the Dolby Vision InfoFrame.
  if (m_impl->hdmiVicTiming)
  {
    RevertHdmiVicTiming(m_impl->displayId);
    // Reverting puts back the Windows registry mode, which can differ from the mode Kodi set.
    const double refresh =
        GetActiveDisplay(DisplayTarget{m_impl->targetAdapter, m_impl->targetId}).refresh;
    if (refresh > 0.0 && DX::Windowing())
    {
      CLog::Log(LOGDEBUG, "Dolby Vision output: {:.3f} Hz after reverting the HDMI_VIC timing.",
                refresh);
      DX::Windowing()->SetCurrentRefreshRate(static_cast<float>(refresh));
    }
  }
  m_impl->hdmiVicTiming = false;
  if (m_impl->originalColour)
    RestoreColour(m_impl->displayId, *m_impl->originalColour);
  m_impl->originalColour.reset();
  NvAPI_Unload();
  m_impl->initialized = false;
}

std::optional<int> DX::CDolbyVisionOutput::RunExitHelper()
{
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv)
    return {};
  const bool helper = argc >= 3 && wcscmp(argv[1], EXIT_HELPER_ARGUMENT) == 0;
  const NvU32 displayId = helper ? static_cast<NvU32>(wcstoul(argv[2], nullptr, 10)) : 0;
  const bool partner = helper && argc >= 4 && wcscmp(argv[3], EXIT_HELPER_PARTNER) == 0;
  LocalFree(argv);
  if (!helper)
    return {};

  if (NvAPI_Initialize() != NVAPI_OK)
    return 1;
  NV_HDR_COLOR_DATA hdr{};
  hdr.hdrMode = NV_HDR_MODE_SDR;
  bool ok = SetHdrStatus(displayId, hdr) == NVAPI_OK;
  if (ok && !partner)
    ok = RunExitHelperProcess(displayId, true);
  NvAPI_Unload();
  return ok ? 0 : 1;
}

bool DX::CDolbyVisionOutput::IsActive() const
{
  return m_impl->active;
}

std::string DX::CDolbyVisionOutput::ActiveMode() const
{
  return m_impl->active ? m_impl->mode : std::string{};
}

bool DX::CDolbyVisionOutput::SwitchedColour() const
{
  return m_impl->active && m_impl->originalColour.has_value();
}

std::string DX::CDolbyVisionOutput::LastRefusal()
{
  std::unique_lock guard(refusalLock);
  // Without the log message's full stop.
  return lastRefusal.ends_with('.') ? lastRefusal.substr(0, lastRefusal.size() - 1) : lastRefusal;
}

void DX::CDolbyVisionOutput::RemoveLeftoverOutput(const std::wstring& gdiName)
{
  if (!IsNvidiaDisplay(gdiName))
    return;
  // A Kodi that ended during Dolby Vision playback leaves the driver's Dolby
  // Vision state and the tagged trial timing.
  const std::optional<NvU32> display = OpenDisplay(gdiName, false);
  if (!display)
    return;
  const NvU32 displayId = *display;
  const std::optional<NV_HDR_COLOR_DATA> hdr = GetHdr(displayId);
  if (hdr && hdr->hdrMode == NV_HDR_MODE_DOLBY_VISION)
  {
    CLog::Log(LOGWARNING, "Dolby Vision output: ending a Dolby Vision state left by a previous "
                          "session.");
    EndDriverDolbyVision(displayId);
  }
  const std::optional<NV_TIMING> timing = GetTiming(displayId, NV_TIMING_OVERRIDE_CURRENT);
  if (timing && (timing->etc.status & 0xffff) == DOLBY_VISION_TIMING_STATUS)
  {
    CLog::Log(LOGWARNING, "Dolby Vision output: removing a Dolby Vision timing left by a previous "
                          "session.");
    RevertHdmiVicTiming(displayId);
  }
  if (const std::optional<NV_COLOR_DATA> colour = LoadSavedColour())
  {
    CLog::Log(LOGWARNING, "Dolby Vision output: restoring the output colour a previous session "
                          "changed.");
    RestoreColour(displayId, *colour);
  }
  NvAPI_Unload();
}

std::wstring DX::CDolbyVisionOutput::DisplayName() const
{
  MONITORINFOEXW monitor{};
  monitor.cbSize = sizeof(monitor);
  if (!GetMonitorInfoW(MonitorFromWindow(m_impl->window, MONITOR_DEFAULTTONEAREST), &monitor))
    return {};
  return monitor.szDevice;
}

void DX::CDolbyVisionOutput::SetWindow(HWND window)
{
  m_impl->window = window;
  const std::wstring display = DisplayName();
  if (!display.empty())
    RemoveLeftoverOutput(display);
}

bool DX::CDolbyVisionOutput::CanEnable(int width, int height, double fps) const
{
  return IsActive() || CanEnableOn(DisplayName(), width, height, fps);
}

void DX::CDolbyVisionOutput::SetCandidate(bool candidate, double fps, int width, int height)
{
  Impl& d = *m_impl;
  if (candidate && !d.candidate)
  {
    d.enableAttempted = false;
    d.fps = fps;
    d.width = width;
    d.height = height;
    d.suspended = false;
    d.pausedForFocus = false;
    d.loggedGui = false;
  }
  d.candidate = candidate;
  // Renderer replacement can set a new candidate before the next present.
  if (!candidate)
    d.compositeGui = {};
}

bool DX::CDolbyVisionOutput::IsCandidate() const
{
  return m_impl->candidate;
}

void DX::CDolbyVisionOutput::SetFrame(
    ID3D11Texture2D* frame, std::function<bool(ID3D11Texture2D*, ID3D11Texture2D*)> compositeGUI)
{
  if (!m_impl->candidate || !frame)
    return;
  m_impl->packedFrame = frame;
  m_impl->compositeGui = std::move(compositeGUI);
}

bool DX::CDolbyVisionOutput::UsesCarrierSwapChain() const
{
  return m_impl->carrierSwapChain;
}

bool DX::CDolbyVisionOutput::PrepareCarrierSwapChain(DeviceResources& resources,
                                                     unsigned width,
                                                     unsigned height)
{
  BOOL exclusive = FALSE;
  DXGI_SWAP_CHAIN_DESC1 desc{};
  if (resources.GetSwapChain())
  {
    resources.GetSwapChain()->GetFullscreenState(&exclusive, nullptr);
    resources.GetSwapChain()->GetDesc1(&desc);
  }

  m_impl->carrierSwapChain = true;
  if (exclusive)
  {
    m_impl->exclusiveFormat = desc.Format;
    if (!ResizeExclusiveSwapChain(resources, DXGI_FORMAT_B8G8R8A8_UNORM))
      return false;
  }
  else
  {
    resources.RecreateSwapChain();
  }
  CD3DTexture& backBuffer = resources.GetBackBuffer();
  if (!resources.GetSwapChain() || !backBuffer.Get())
    return false;

  // Kodi's refresh-rate switching and the NVAPI timing set the display mode; the
  // 8-bit swapchain only needs to cover it so Windows flips it unchanged.
  BOOL fullscreen = FALSE;
  resources.GetSwapChain()->GetFullscreenState(&fullscreen, nullptr);
  if (fullscreen != exclusive || backBuffer.GetFormat() != DXGI_FORMAT_B8G8R8A8_UNORM ||
      backBuffer.GetWidth() != width || backBuffer.GetHeight() != height)
  {
    CLog::Log(LOGERROR,
              "Dolby Vision output: {} {}x{} BGRA8 swapchain unavailable (exclusive {}, {}x{} "
              "format {}).",
              exclusive ? "exclusive" : "borderless", width, height, fullscreen != FALSE,
              backBuffer.GetWidth(), backBuffer.GetHeight(),
              static_cast<int>(backBuffer.GetFormat()));
    return false;
  }

  CLog::Log(LOGDEBUG, "Dolby Vision output: {} {}x{} BGRA8 swapchain prepared.",
            exclusive ? "exclusive" : "borderless", width, height);
  return true;
}

void DX::CDolbyVisionOutput::Leave(DeviceResources* resources)
{
  Impl& d = *m_impl;
  d.compositeGui = {};
  d.guiFrame.Reset();
  d.packedFrame.Reset();
  Restore();
  if (d.carrierSwapChain)
  {
    d.carrierSwapChain = false;
    const DXGI_FORMAT exclusiveFormat = std::exchange(d.exclusiveFormat, DXGI_FORMAT_UNKNOWN);
    if (resources)
    {
      // Without the Dolby Vision signal the last carrier frame shows as false
      // colours; blank it while the display resynchronizes.
      CD3DTexture& backBuffer = resources->GetBackBuffer();
      if (backBuffer.Get() && resources->GetSwapChain())
      {
        constexpr float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        resources->GetImmediateContext()->ClearRenderTargetView(backBuffer.GetRenderTarget(),
                                                                black);
        resources->GetSwapChain()->Present(0, 0);
      }
      BOOL exclusive = FALSE;
      if (resources->GetSwapChain())
        resources->GetSwapChain()->GetFullscreenState(&exclusive, nullptr);
      if (!exclusive || exclusiveFormat == DXGI_FORMAT_UNKNOWN ||
          !ResizeExclusiveSwapChain(*resources, exclusiveFormat))
        resources->RecreateSwapChain();
    }
  }
}

void DX::CDolbyVisionOutput::Reset()
{
  Leave(nullptr);
  m_impl->candidate = false;
}

bool DX::CDolbyVisionOutput::Present(DeviceResources& resources)
{
  Impl& d = *m_impl;
  const bool foreground = GetForegroundWindow() == d.window;
  if (d.suspended && foreground)
  {
    CLog::Log(LOGINFO, "Dolby Vision output: Kodi has the foreground again; re-entering.");
    d.suspended = false;
    d.enableAttempted = false;
  }

  if (d.candidate && d.packedFrame && !IsActive() && !d.enableAttempted && !d.suspended)
  {
    d.enableAttempted = true;
    D3D11_TEXTURE2D_DESC frameDesc{};
    d.packedFrame->GetDesc(&frameDesc);
    if (!Enable(DisplayName(), d.width, d.height, d.fps, [this, &resources, &frameDesc]
                { return PrepareCarrierSwapChain(resources, frameDesc.Width, frameDesc.Height); }))
    {
      CLog::Log(LOGWARNING,
                "Dolby Vision output: entry failed; falling back to Kodi's HDR renderer.");
      SetCandidate(false);
    }
    if (d.pausedForFocus)
    {
      d.pausedForFocus = false;
      CServiceBroker::GetAppMessenger()->PostMsg(TMSG_MEDIA_UNPAUSE);
    }
  }

  if (!d.candidate && (IsActive() || d.carrierSwapChain))
  {
    Leave(&resources);
    return false;
  }

  if (d.candidate && IsActive() && d.packedFrame)
  {
    if (!foreground)
    {
      Suspend(resources);
      return false;
    }
    PresentFrame(resources);
  }
  return true;
}

// Windows composes other windows into the carrier, which the TV shows as black,
// and without Dolby Vision processing a Profile 5 picture shows false colours. So
// playback pauses and the output is left until Kodi has the foreground again.
void DX::CDolbyVisionOutput::Suspend(DeviceResources& resources)
{
  Impl& d = *m_impl;
  DWORD process = 0;
  wchar_t windowClass[64]{};
  const HWND foreground = GetForegroundWindow();
  GetWindowThreadProcessId(foreground, &process);
  GetClassNameW(foreground, windowClass, 64);
  CLog::Log(LOGINFO,
            "Dolby Vision output: Kodi lost the foreground to process {} ({}); pausing and "
            "leaving the output.",
            process, KODI::PLATFORM::WINDOWS::FromW(windowClass));

  const auto& player = CServiceBroker::GetAppComponents().GetComponent<CApplicationPlayer>();
  if (player->IsPlaying() && !player->IsPaused())
  {
    d.pausedForFocus = true;
    CServiceBroker::GetAppMessenger()->PostMsg(TMSG_MEDIA_PAUSE_IF_PLAYING);
  }

  // Re-entering needs a packed frame, and a paused renderer may not render again.
  const Microsoft::WRL::ComPtr<ID3D11Texture2D> frame = d.packedFrame;
  auto compositeGui = d.compositeGui;
  Leave(&resources);
  d.packedFrame = frame;
  d.compositeGui = std::move(compositeGui);
  d.suspended = true;
}

std::optional<unsigned> DX::CDolbyVisionOutput::DisplayPeakPq() const
{
  const std::optional<unsigned> pq =
      KODI::WINDOWING::WINDOWS::CDVBridgeStateWin::GetInstance().GetDisplayPeakPq();
  if (pq)
    CLog::Log(LOGINFO, "Dolby Vision output: display peak PQ {} (from EDID VSVDB).", *pq);
  return pq;
}

bool DX::CDolbyVisionOutput::IsGuiVisible()
{
  return CServiceBroker::GetGUI()->GetWindowManager().HasVisibleControls() ||
         CServiceBroker::GetAppComponents().GetComponent<CApplicationPlayer>()->HasVisibleOverlay();
}

void DX::CDolbyVisionOutput::PresentFrame(DeviceResources& resources)
{
  Impl& d = *m_impl;
  CD3DTexture& backBuffer = resources.GetBackBuffer();
  D3D11_TEXTURE2D_DESC frameDesc{};
  d.packedFrame->GetDesc(&frameDesc);
  if (!backBuffer.Get() || frameDesc.Width != backBuffer.GetWidth() ||
      frameDesc.Height != backBuffer.GetHeight())
  {
    CLog::Log(LOGERROR,
              "Dolby Vision output: packed frame does not match the 8-bit swapchain dimensions.");
    d.candidate = false;
    return;
  }

  ID3D11DeviceContext* context = resources.GetImmediateContext();
  resources.GetD3DContext()->OMSetRenderTargets(0, nullptr, nullptr);
  context->OMSetRenderTargets(0, nullptr, nullptr);
  // Fullscreen video usually draws no GUI or subtitles; compositing an empty layer
  // would cost a full-frame copy and pass on every present.
  bool guiReady = false;
  if (d.compositeGui && IsGuiVisible())
  {
    if (!d.guiFrame)
    {
      D3D11_TEXTURE2D_DESC desc = frameDesc;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      desc.CPUAccessFlags = desc.MiscFlags = 0;
      resources.GetD3DDevice()->CreateTexture2D(&desc, nullptr, &d.guiFrame);
    }
    if (d.guiFrame)
    {
      context->CopyResource(d.guiFrame.Get(), backBuffer.Get());
      guiReady = true;
    }
  }
  context->CopyResource(backBuffer.Get(), d.packedFrame.Get());
  if (guiReady)
  {
    const bool composited = d.compositeGui(d.guiFrame.Get(), backBuffer.Get());
    if (!composited)
    {
      if (!d.loggedGui)
        CLog::Log(LOGWARNING, "Dolby Vision output: source-domain GUI composition failed.");
    }
    else if (!d.loggedGui)
    {
      CLog::Log(LOGDEBUG, "Dolby Vision output: source-domain GUI composition active.");
    }
    d.loggedGui = true;
  }
  if (d.mmioActive)
    KODI::WINDOWING::WINDOWS::CDVBridgeStateWin::GetInstance().RefreshHwTransportIfNeeded();
}
