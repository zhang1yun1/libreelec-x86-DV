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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>
#include <setupapi.h>
#include <shellapi.h>
#include <wrl/client.h>

#pragma comment(lib, "setupapi.lib")

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

// ---------------------------------------------------------------------------
// Minimal AMD ADLX (amdadlx64.dll) & ADL2 (atiadlxx.dll) ABI definitions
// Dynamically loaded without bundling proprietary SDK headers (matches NVAPI).
// ---------------------------------------------------------------------------
constexpr uint32_t PCI_VENDOR_AMD = 0x1002u;
constexpr uint32_t PCI_VENDOR_INTEL = 0x8086u;

using adlx_int = int;
using adlx_uint = unsigned int;
using adlx_long = long;
using adlx_uint64 = unsigned long long;
using adlx_size = size_t;
using adlx_bool = unsigned char;

enum ADLX_RESULT : int
{
  ADLX_OK = 0,
  ADLX_NOT_SUPPORTED = 12,
};

enum ADLX_COLOR_DEPTH : int
{
  ADLX_BPC_UNKNOWN = 0,
  ADLX_BPC_6 = 1,
  ADLX_BPC_8 = 2,
  ADLX_BPC_10 = 3,
  ADLX_BPC_12 = 4,
  ADLX_BPC_14 = 5,
  ADLX_BPC_16 = 6,
};

enum ADLX_PIXEL_FORMAT : int
{
  ADLX_FORMAT_UNKNOWN = 0,
  ADLX_RGB_444_FULL = 1,
  ADLX_YCBCR_444 = 2,
  ADLX_YCBCR_422 = 3,
  ADLX_RGB_444_LIMITED = 4,
  ADLX_YCBCR_420 = 5,
};

struct IADLXSystem;
struct IADLXDisplayServices;
struct IADLXDisplayList;
struct IADLXDisplay;
struct IADLXDisplayColorDepth;
struct IADLXDisplayPixelFormat;

struct IADLXSystemVtbl
{
  ADLX_RESULT(__stdcall* GetHybridGraphicsType)(IADLXSystem*, int*);
  ADLX_RESULT(__stdcall* GetGPUs)(IADLXSystem*, void**);
  ADLX_RESULT(__stdcall* QueryInterface)(IADLXSystem*, const wchar_t*, void**);
  ADLX_RESULT(__stdcall* GetDisplaysServices)(IADLXSystem*, IADLXDisplayServices**);
};
struct IADLXSystem
{
  IADLXSystemVtbl* lpVtbl;
};

struct IADLXDisplayServicesVtbl
{
  adlx_long(__stdcall* Acquire)(IADLXDisplayServices*);
  adlx_long(__stdcall* Release)(IADLXDisplayServices*);
  ADLX_RESULT(__stdcall* QueryInterface)(IADLXDisplayServices*, const wchar_t*, void**);
  ADLX_RESULT(__stdcall* GetNumberOfDisplays)(IADLXDisplayServices*, adlx_uint*);
  ADLX_RESULT(__stdcall* GetDisplays)(IADLXDisplayServices*, IADLXDisplayList**);
  ADLX_RESULT(__stdcall* Get3DLUT)(IADLXDisplayServices*, IADLXDisplay*, void**);
  ADLX_RESULT(__stdcall* GetGamut)(IADLXDisplayServices*, IADLXDisplay*, void**);
  ADLX_RESULT(__stdcall* GetGamma)(IADLXDisplayServices*, IADLXDisplay*, void**);
  ADLX_RESULT(__stdcall* GetDisplayChangedHandling)(IADLXDisplayServices*, void**);
  ADLX_RESULT(__stdcall* GetFreeSync)(IADLXDisplayServices*, IADLXDisplay*, void**);
  ADLX_RESULT(__stdcall* GetVirtualSuperResolution)(IADLXDisplayServices*, IADLXDisplay*, void**);
  ADLX_RESULT(__stdcall* GetGPUScaling)(IADLXDisplayServices*, IADLXDisplay*, void**);
  ADLX_RESULT(__stdcall* GetScalingMode)(IADLXDisplayServices*, IADLXDisplay*, void**);
  ADLX_RESULT(__stdcall* GetIntegerScaling)(IADLXDisplayServices*, IADLXDisplay*, void**);
  ADLX_RESULT(__stdcall* GetColorDepth)(IADLXDisplayServices*, IADLXDisplay*, IADLXDisplayColorDepth**);
  ADLX_RESULT(__stdcall* GetPixelFormat)(IADLXDisplayServices*, IADLXDisplay*, IADLXDisplayPixelFormat**);
};
struct IADLXDisplayServices
{
  IADLXDisplayServicesVtbl* lpVtbl;
};

struct IADLXDisplayListVtbl
{
  adlx_long(__stdcall* Acquire)(IADLXDisplayList*);
  adlx_long(__stdcall* Release)(IADLXDisplayList*);
  ADLX_RESULT(__stdcall* QueryInterface)(IADLXDisplayList*, const wchar_t*, void**);
  adlx_uint(__stdcall* Size)(IADLXDisplayList*);
  adlx_bool(__stdcall* Empty)(IADLXDisplayList*);
  adlx_uint(__stdcall* Begin)(IADLXDisplayList*);
  adlx_uint(__stdcall* End)(IADLXDisplayList*);
  ADLX_RESULT(__stdcall* At)(IADLXDisplayList*, adlx_uint, void**);
  ADLX_RESULT(__stdcall* Clear)(IADLXDisplayList*);
  ADLX_RESULT(__stdcall* Remove_Back)(IADLXDisplayList*);
  ADLX_RESULT(__stdcall* Add_Back)(IADLXDisplayList*, void*);
  ADLX_RESULT(__stdcall* At_DisplayList)(IADLXDisplayList*, adlx_uint, IADLXDisplay**);
  ADLX_RESULT(__stdcall* Add_Back_DisplayList)(IADLXDisplayList*, IADLXDisplay*);
};
struct IADLXDisplayList
{
  IADLXDisplayListVtbl* lpVtbl;
};

struct IADLXDisplayVtbl
{
  adlx_long(__stdcall* Acquire)(IADLXDisplay*);
  adlx_long(__stdcall* Release)(IADLXDisplay*);
  ADLX_RESULT(__stdcall* QueryInterface)(IADLXDisplay*, const wchar_t*, void**);
  ADLX_RESULT(__stdcall* ManufacturerID)(IADLXDisplay*, adlx_uint*);
  ADLX_RESULT(__stdcall* DisplayType)(IADLXDisplay*, int*);
  ADLX_RESULT(__stdcall* ConnectorType)(IADLXDisplay*, int*);
  ADLX_RESULT(__stdcall* Name)(IADLXDisplay*, const char**);
  ADLX_RESULT(__stdcall* EDID)(IADLXDisplay*, const char**);
  ADLX_RESULT(__stdcall* NativeResolution)(IADLXDisplay*, adlx_int*, adlx_int*);
  ADLX_RESULT(__stdcall* RefreshRate)(IADLXDisplay*, double*);
  ADLX_RESULT(__stdcall* PixelClock)(IADLXDisplay*, adlx_uint*);
  ADLX_RESULT(__stdcall* ScanType)(IADLXDisplay*, int*);
  ADLX_RESULT(__stdcall* GetGPU)(IADLXDisplay*, void**);
  ADLX_RESULT(__stdcall* UniqueId)(IADLXDisplay*, adlx_size*);
};
struct IADLXDisplay
{
  IADLXDisplayVtbl* lpVtbl;
};

struct IADLXDisplayColorDepthVtbl
{
  adlx_long(__stdcall* Acquire)(IADLXDisplayColorDepth*);
  adlx_long(__stdcall* Release)(IADLXDisplayColorDepth*);
  ADLX_RESULT(__stdcall* QueryInterface)(IADLXDisplayColorDepth*, const wchar_t*, void**);
  ADLX_RESULT(__stdcall* IsSupported)(IADLXDisplayColorDepth*, adlx_bool*);
  ADLX_RESULT(__stdcall* GetValue)(IADLXDisplayColorDepth*, ADLX_COLOR_DEPTH*);
  ADLX_RESULT(__stdcall* SetValue)(IADLXDisplayColorDepth*, ADLX_COLOR_DEPTH);
  ADLX_RESULT(__stdcall* IsSupportedColorDepth)(IADLXDisplayColorDepth*, ADLX_COLOR_DEPTH, adlx_bool*);
};
struct IADLXDisplayColorDepth
{
  IADLXDisplayColorDepthVtbl* lpVtbl;
};

struct IADLXDisplayPixelFormatVtbl
{
  adlx_long(__stdcall* Acquire)(IADLXDisplayPixelFormat*);
  adlx_long(__stdcall* Release)(IADLXDisplayPixelFormat*);
  ADLX_RESULT(__stdcall* QueryInterface)(IADLXDisplayPixelFormat*, const wchar_t*, void**);
  ADLX_RESULT(__stdcall* IsSupported)(IADLXDisplayPixelFormat*, adlx_bool*);
  ADLX_RESULT(__stdcall* GetValue)(IADLXDisplayPixelFormat*, ADLX_PIXEL_FORMAT*);
  ADLX_RESULT(__stdcall* SetValue)(IADLXDisplayPixelFormat*, ADLX_PIXEL_FORMAT);
  ADLX_RESULT(__stdcall* IsSupportedPixelFormat)(IADLXDisplayPixelFormat*, ADLX_PIXEL_FORMAT, adlx_bool*);
};
struct IADLXDisplayPixelFormat
{
  IADLXDisplayPixelFormatVtbl* lpVtbl;
};

constexpr int ADL_OK_STATUS = 0;
constexpr int ADL_MAX_PATH_LEN = 256;
constexpr int ADL_MAX_EDID_SIZE = 256;
constexpr int ADL_DISPLAY_CONNECTED = 0x00000001;
constexpr int ADL_DISPLAY_MAPPED = 0x00000002;
constexpr int ADL_COLORDEPTH_888_VAL = 2;
constexpr int ADL_PF_RGB_FULL = (1 << 0);
constexpr int ADL_PF_YCBCR444 = (1 << 1);
constexpr int ADL_PF_YCBCR422 = (1 << 2);
constexpr int ADL_PF_RGB_LIMITED = (1 << 3);
constexpr int ADL_PF_YCBCR420 = (1 << 4);

struct ADLAdapterInfo
{
  int iSize;
  int iAdapterIndex;
  char strUDID[ADL_MAX_PATH_LEN];
  int iBusNumber;
  int iDeviceNumber;
  int iFunctionNumber;
  int iVendorID;
  char strAdapterName[ADL_MAX_PATH_LEN];
  char strDisplayName[ADL_MAX_PATH_LEN];
  int iPresent;
  int iExist;
  char strDriverPath[ADL_MAX_PATH_LEN];
  char strDriverPathExt[ADL_MAX_PATH_LEN];
  char strPNPString[ADL_MAX_PATH_LEN];
  int iOSDisplayIndex;
};

struct ADLDisplayID
{
  int iDisplayLogicalIndex;
  int iDisplayPhysicalIndex;
  int iDisplayLogicalAdapterIndex;
  int iDisplayPhysicalAdapterIndex;
};

struct ADLDisplayInfo
{
  ADLDisplayID displayID;
  int iDisplayControllerIndex;
  char strDisplayName[ADL_MAX_PATH_LEN];
  char strDisplayManufacturerName[ADL_MAX_PATH_LEN];
  int iDisplayType;
  int iDisplayOutputType;
  int iDisplayConnector;
  int iDisplayInfoMask;
  int iDisplayInfoValue;
};

struct ADLDisplayEDIDData
{
  int iSize;
  int iFlag;
  int iEDIDSize;
  int iBlockIndex;
  char cEDIDData[ADL_MAX_EDID_SIZE];
  int iReserved[4];
};

static_assert(sizeof(ADLAdapterInfo) == 1572, "ADLAdapterInfo layout mismatch");
static_assert(sizeof(ADLDisplayEDIDData) == 288, "ADLDisplayEDIDData layout mismatch");

constexpr uint32_t SAVED_AMD_COLOUR_MAGIC = 0x414D4443u; // "AMDC"
constexpr uint32_t SAVED_AMD_COLOUR_VERSION = 1u;

struct AmdSavedColour
{
  uint32_t magic{SAVED_AMD_COLOUR_MAGIC};
  uint32_t version{SAVED_AMD_COLOUR_VERSION};
  wchar_t gdiName[32]{};
  uint32_t edidLength{0};
  uint8_t edid[512]{};
  uint8_t hasColorDepth{0};
  uint8_t hasPixelFormat{0};
  uint8_t reserved[2]{};
  // Stored using canonical ADLX enum values (CD: 1..6 matches ADL2; PF mapped to/from ADL2 masks).
  int32_t colorDepth{ADLX_BPC_UNKNOWN};
  int32_t pixelFormat{ADLX_FORMAT_UNKNOWN};
};

// ---------------------------------------------------------------------------
// Minimal Intel IGCL (ControlLib.dll) C ABI definitions
// Binary layout verified against intel/drivers.gpu.control-library igcl_api.h.
// ---------------------------------------------------------------------------
constexpr uint32_t CTL_MAKE_VER(uint32_t major, uint32_t minor)
{
  return (major << 16) | (minor & 0xFFFFu);
}

using ctl_result_t = uint32_t;
constexpr ctl_result_t CTL_RESULT_SUCCESS = 0x00000000u;
constexpr ctl_result_t CTL_RESULT_SUCCESS_STILL_OPEN_BY_ANOTHER_CALLER = 0x00000001u;
constexpr ctl_result_t CTL_RESULT_ERROR_UNSUPPORTED_VERSION = 0x40000009u;

constexpr uint32_t CTL_OUTPUT_BPC_FLAG_6BPC = (1u << 0);
constexpr uint32_t CTL_OUTPUT_BPC_FLAG_8BPC = (1u << 1);
constexpr uint32_t CTL_OUTPUT_BPC_FLAG_10BPC = (1u << 2);
constexpr uint32_t CTL_OUTPUT_BPC_FLAG_12BPC = (1u << 3);

constexpr uint32_t CTL_DISPLAY_CONFIG_FLAG_DISPLAY_ACTIVE = (1u << 0);
constexpr uint32_t CTL_DISPLAY_CONFIG_FLAG_DISPLAY_ATTACHED = (1u << 1);

constexpr int32_t CTL_WIRE_FORMAT_COLOR_MODEL_RGB = 0;
constexpr int32_t CTL_WIRE_FORMAT_COLOR_MODEL_YCBCR_420 = 1;
constexpr int32_t CTL_WIRE_FORMAT_COLOR_MODEL_YCBCR_422 = 2;
constexpr int32_t CTL_WIRE_FORMAT_COLOR_MODEL_YCBCR_444 = 3;

constexpr int32_t CTL_WIRE_FORMAT_OPERATION_TYPE_GET = 0;
constexpr int32_t CTL_WIRE_FORMAT_OPERATION_TYPE_SET = 1;
constexpr uint32_t CTL_MAX_WIREFORMAT_COLOR_MODELS_SUPPORTED = 4;

constexpr uint32_t CTL_DISPLAY_SETTING_FLAG_QUANTIZATION_RANGE = (1u << 3);
constexpr int32_t CTL_DISPLAY_SETTING_QUANTIZATION_RANGE_DEFAULT = 0;
constexpr int32_t CTL_DISPLAY_SETTING_QUANTIZATION_RANGE_LIMITED_RANGE = 1;
constexpr int32_t CTL_DISPLAY_SETTING_QUANTIZATION_RANGE_FULL_RANGE = 2;

constexpr int32_t CTL_EDID_MANAGEMENT_OPTYPE_READ_EDID = 1;
constexpr int32_t CTL_EDID_TYPE_CURRENT = 1;

using ctl_api_handle_t = void*;
using ctl_device_adapter_handle_t = void*;
using ctl_display_output_handle_t = void*;

struct CtlApplicationId
{
  uint32_t Data1;
  uint16_t Data2;
  uint16_t Data3;
  uint8_t Data4[8];
};

struct CtlInitArgs
{
  uint32_t Size;
  uint8_t Version;
  uint32_t AppVersion;
  uint32_t flags;
  uint32_t SupportedVersion;
  CtlApplicationId ApplicationUID;
};

struct CtlGenericVoidDatatype
{
  void* pData;
  uint32_t size;
};

union CtlOsDisplayEncoderIdentifier
{
  uint32_t WindowsDisplayEncoderID;
  CtlGenericVoidDatatype DisplayEncoderID;
};

struct CtlRevisionDatatype
{
  uint8_t major_version;
  uint8_t minor_version;
  uint8_t revision_version;
};

struct CtlDisplayTiming
{
  uint32_t Size;
  uint8_t Version;
  uint64_t PixelClock;
  uint32_t HActive;
  uint32_t VActive;
  uint32_t HTotal;
  uint32_t VTotal;
  uint32_t HBlank;
  uint32_t VBlank;
  uint32_t HSync;
  uint32_t VSync;
  float RefreshRate;
  int32_t SignalStandard;
  uint8_t VicId;
};

struct CtlDisplayProperties
{
  uint32_t Size;
  uint8_t Version;
  CtlOsDisplayEncoderIdentifier Os_display_encoder_handle;
  int32_t Type;
  int32_t AttachedDisplayMuxType;
  int32_t ProtocolConverterOutput;
  CtlRevisionDatatype SupportedSpec;
  uint32_t SupportedOutputBPCFlags;
  uint32_t ProtocolConverterType;
  uint32_t DisplayConfigFlags;
  uint32_t FeatureEnabledFlags;
  uint32_t FeatureSupportedFlags;
  uint32_t AdvancedFeatureEnabledFlags;
  uint32_t AdvancedFeatureSupportedFlags;
  CtlDisplayTiming Display_Timing_Info;
  uint32_t ReservedFields[16];
};

struct CtlWireFormat
{
  uint32_t Size;
  uint8_t Version;
  int32_t ColorModel;
  uint32_t ColorDepth;
};

struct CtlGetSetWireFormatConfig
{
  uint32_t Size;
  uint8_t Version;
  int32_t Operation;
  CtlWireFormat SupportedWireFormat[CTL_MAX_WIREFORMAT_COLOR_MODELS_SUPPORTED];
  CtlWireFormat WireFormat;
};

struct CtlDisplaySettings
{
  uint32_t Size;
  uint8_t Version;
  bool Set;
  uint32_t SupportedFlags;
  uint32_t ControllableFlags;
  uint32_t ValidFlags;
  int32_t LowLatency;
  int32_t SourceTM;
  int32_t ContentType;
  int32_t QuantizationRange;
  uint32_t SupportedPictureAR;
  uint32_t PictureAR;
  int32_t AudioSettings;
  uint32_t Reserved[25];
};

struct CtlEdidManagementArgs
{
  uint32_t Size;
  uint8_t Version;
  int32_t OpType;
  int32_t EdidType;
  uint32_t EdidSize;
  uint8_t* pEdidBuf;
  uint32_t OutFlags;
};

static_assert(sizeof(CtlInitArgs) == 36, "CtlInitArgs size mismatch");
static_assert(sizeof(CtlOsDisplayEncoderIdentifier) == 16,
              "CtlOsDisplayEncoderIdentifier size mismatch");
static_assert(sizeof(CtlDisplayTiming) == 64, "CtlDisplayTiming size mismatch");
static_assert(sizeof(CtlDisplayProperties) == 200 &&
                  offsetof(CtlDisplayProperties, DisplayConfigFlags) == 48,
              "CtlDisplayProperties layout mismatch");
static_assert(sizeof(CtlWireFormat) == 16 && offsetof(CtlWireFormat, ColorModel) == 8 &&
                  offsetof(CtlWireFormat, ColorDepth) == 12,
              "CtlWireFormat layout mismatch");
static_assert(sizeof(CtlGetSetWireFormatConfig) == 92 &&
                  offsetof(CtlGetSetWireFormatConfig, WireFormat) == 76,
              "CtlGetSetWireFormatConfig layout mismatch");
static_assert(sizeof(CtlDisplaySettings) == 148 && offsetof(CtlDisplaySettings, Set) == 5 &&
                  offsetof(CtlDisplaySettings, QuantizationRange) == 32,
              "CtlDisplaySettings layout mismatch");
static_assert(sizeof(CtlEdidManagementArgs) == 40 &&
                  offsetof(CtlEdidManagementArgs, pEdidBuf) == 24,
              "CtlEdidManagementArgs layout mismatch");

constexpr uint32_t SAVED_INTEL_COLOUR_MAGIC = 0x494E5443u; // "INTC"
constexpr uint32_t SAVED_INTEL_COLOUR_VERSION = 1u;

struct IntelSavedColour
{
  uint32_t magic{SAVED_INTEL_COLOUR_MAGIC};
  uint32_t version{SAVED_INTEL_COLOUR_VERSION};
  wchar_t gdiName[32]{};
  uint32_t targetId{0};
  uint32_t edidLength{0};
  uint8_t edid[512]{};
  uint8_t hasWireFormat{0};
  uint8_t hasQuantRange{0};
  uint8_t reserved[2]{};
  int32_t colorModel{CTL_WIRE_FORMAT_COLOR_MODEL_RGB};
  uint32_t colorDepth{0};
  int32_t quantRange{CTL_DISPLAY_SETTING_QUANTIZATION_RANGE_DEFAULT};
};
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
  std::optional<AmdSavedColour> amdOriginalColour;
  std::optional<IntelSavedColour> intelOriginalColour;
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
  unsigned width{0};
  unsigned height{0};
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
    display.width = signal.activeSize.cx;
    display.height = signal.activeSize.cy;
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

constexpr char SAVED_AMD_COLOUR_FILE[] = "special://temp/dolbyvision-amd-colour.bin";
constexpr char SAVED_INTEL_COLOUR_FILE[] = "special://temp/dolbyvision-intel-colour.bin";

template <typename T>
void SaveTunnelColourFile(const char* path, const char* vendorTag, const T& colour)
{
  XFILE::CFile file;
  if (!file.OpenForWrite(path, true) ||
      file.Write(&colour, sizeof(colour)) != static_cast<ssize_t>(sizeof(colour)))
    CLog::Log(LOGWARNING, "Dolby Vision output: saving the {} output colour failed.", vendorTag);
}

template <typename T>
std::optional<T> LoadSavedTunnelColourFile(const char* path, uint32_t magic, uint32_t version)
{
  XFILE::CFile file;
  T colour{};
  if (!file.Open(path) ||
      file.Read(&colour, sizeof(colour)) != static_cast<ssize_t>(sizeof(colour)) ||
      colour.magic != magic || colour.version != version ||
      colour.edidLength > sizeof(colour.edid))
    return {};
  return colour;
}

void SaveAmdColour(const AmdSavedColour& colour)
{
  SaveTunnelColourFile(SAVED_AMD_COLOUR_FILE, "AMD", colour);
}

std::optional<AmdSavedColour> LoadSavedAmdColour()
{
  return LoadSavedTunnelColourFile<AmdSavedColour>(SAVED_AMD_COLOUR_FILE, SAVED_AMD_COLOUR_MAGIC,
                                                   SAVED_AMD_COLOUR_VERSION);
}

void SaveIntelColour(const IntelSavedColour& colour)
{
  SaveTunnelColourFile(SAVED_INTEL_COLOUR_FILE, "Intel", colour);
}

std::optional<IntelSavedColour> LoadSavedIntelColour()
{
  return LoadSavedTunnelColourFile<IntelSavedColour>(
      SAVED_INTEL_COLOUR_FILE, SAVED_INTEL_COLOUR_MAGIC, SAVED_INTEL_COLOUR_VERSION);
}

template <typename T>
struct AdlxPtr
{
  T* ptr{nullptr};
  AdlxPtr() = default;
  ~AdlxPtr() { Reset(); }
  AdlxPtr(const AdlxPtr&) = delete;
  AdlxPtr& operator=(const AdlxPtr&) = delete;
  void Reset()
  {
    if (ptr)
    {
      ptr->lpVtbl->Release(ptr);
      ptr = nullptr;
    }
  }
  T* operator->() const { return ptr; }
  explicit operator bool() const { return ptr != nullptr; }
};

struct ScopedAlteringWindow
{
  bool active{false};
  bool previous{false};
  ScopedAlteringWindow()
  {
    if (auto* win = DX::Windowing())
    {
      previous = win->IsAlteringWindow();
      win->SetAlteringWindow(true);
      active = true;
    }
  }
  ~ScopedAlteringWindow()
  {
    if (active)
    {
      if (auto* win = DX::Windowing())
        win->SetAlteringWindow(previous);
    }
  }
  ScopedAlteringWindow(const ScopedAlteringWindow&) = delete;
  ScopedAlteringWindow& operator=(const ScopedAlteringWindow&) = delete;
};

struct ScopedRegistryModeSync
{
  std::wstring gdiName;
  DEVMODEW origRegistry{};
  bool pinned{false};

  explicit ScopedRegistryModeSync(const std::wstring& name) : gdiName(name)
  {
    if (gdiName.empty())
      return;
    DEVMODEW cur{};
    cur.dmSize = sizeof(cur);
    origRegistry.dmSize = sizeof(origRegistry);
    if (!EnumDisplaySettingsW(gdiName.c_str(), ENUM_CURRENT_SETTINGS, &cur) ||
        !EnumDisplaySettingsW(gdiName.c_str(), ENUM_REGISTRY_SETTINGS, &origRegistry))
      return;
    const bool differs =
        cur.dmPelsWidth != origRegistry.dmPelsWidth ||
        cur.dmPelsHeight != origRegistry.dmPelsHeight ||
        cur.dmDisplayFrequency != origRegistry.dmDisplayFrequency ||
        (cur.dmDisplayFlags & DM_INTERLACED) != (origRegistry.dmDisplayFlags & DM_INTERLACED);
    if (!differs)
      return;

    // Pin Kodi's volatile CDS_FULLSCREEN playback mode in the registry so ADLX/ADL2
    // does not re-commit the desktop refresh rate when changing colour depth or format.
    cur.dmSize = sizeof(cur);
    cur.dmDriverExtra = 0;
    cur.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_DISPLAYFLAGS;
    if (ChangeDisplaySettingsExW(gdiName.c_str(), &cur, nullptr,
                                 CDS_UPDATEREGISTRY | CDS_NORESET, nullptr) ==
        DISP_CHANGE_SUCCESSFUL)
      pinned = true;
  }

  ~ScopedRegistryModeSync()
  {
    if (!pinned)
      return;
    origRegistry.dmSize = sizeof(origRegistry);
    origRegistry.dmDriverExtra = 0;
    origRegistry.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_DISPLAYFLAGS;
    ChangeDisplaySettingsExW(gdiName.c_str(), &origRegistry, nullptr,
                             CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
  }

  ScopedRegistryModeSync(const ScopedRegistryModeSync&) = delete;
  ScopedRegistryModeSync& operator=(const ScopedRegistryModeSync&) = delete;
};

bool ApplyAmdTargetDisplayMode(const std::wstring& gdiName,
                               unsigned targetWidth,
                               unsigned targetHeight,
                               double targetRefresh,
                               const DisplayTarget& target)
{
  if (gdiName.empty() || targetWidth == 0 || targetHeight == 0 || targetRefresh <= 0.0)
    return false;

  DEVMODEW bestMode{};
  bool foundBest = false;
  double bestDiff = 0.1;
  bool bestProgressive = false;
  for (DWORD idx = 0;; ++idx)
  {
    DEVMODEW cand{};
    cand.dmSize = sizeof(cand);
    if (!EnumDisplaySettingsW(gdiName.c_str(), idx, &cand))
      break;
    if (cand.dmBitsPerPel != 32 || cand.dmPelsWidth != targetWidth ||
        cand.dmPelsHeight != targetHeight)
      continue;
    const DWORD f = cand.dmDisplayFrequency;
    const double candRefresh = ((f + 1U) % 24U == 0U || (f + 1U) % 30U == 0U)
                                   ? static_cast<double>(f + 1U) / 1.001
                                   : static_cast<double>(f);
    const double diff = std::abs(candRefresh - targetRefresh);
    const bool progressive = (cand.dmDisplayFlags & DM_INTERLACED) == 0;
    if (diff < bestDiff ||
        (foundBest && std::abs(diff - bestDiff) < 0.001 && progressive && !bestProgressive))
    {
      bestMode = cand;
      bestDiff = diff;
      bestProgressive = progressive;
      foundBest = true;
    }
  }

  if (!foundBest)
  {
    bestMode.dmSize = sizeof(bestMode);
    bestMode.dmPelsWidth = targetWidth;
    bestMode.dmPelsHeight = targetHeight;
    bestMode.dmDisplayFrequency = static_cast<DWORD>(targetRefresh);
    bestMode.dmDisplayFlags = 0;
  }
  bestMode.dmSize = sizeof(bestMode);
  bestMode.dmDriverExtra = 0;
  bestMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_DISPLAYFLAGS;

  DEVMODEW regMode{};
  regMode.dmSize = sizeof(regMode);
  const bool haveReg =
      EnumDisplaySettingsW(gdiName.c_str(), ENUM_REGISTRY_SETTINGS, &regMode) != FALSE;

  ScopedAlteringWindow altering;
  if (auto* win = DX::Windowing())
    win->SuppressDisplayChangeFor(std::chrono::milliseconds(2000));

  bool switched = false;
  if (haveReg &&
      ChangeDisplaySettingsExW(gdiName.c_str(), &bestMode, nullptr,
                               CDS_UPDATEREGISTRY | CDS_NORESET, nullptr) ==
          DISP_CHANGE_SUCCESSFUL)
  {
    switched = ChangeDisplaySettingsExW(gdiName.c_str(), nullptr, nullptr, CDS_FULLSCREEN,
                                        nullptr) == DISP_CHANGE_SUCCESSFUL;
    regMode.dmSize = sizeof(regMode);
    regMode.dmDriverExtra = 0;
    regMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_DISPLAYFLAGS;
    ChangeDisplaySettingsExW(gdiName.c_str(), &regMode, nullptr,
                             CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
  }
  if (!switched)
  {
    switched = ChangeDisplaySettingsExW(gdiName.c_str(), &bestMode, nullptr, CDS_FULLSCREEN,
                                        nullptr) == DISP_CHANGE_SUCCESSFUL;
  }
  if (!switched)
  {
    CLog::Log(LOGWARNING,
              "Dolby Vision output: restoring {}x{} {:.3f} Hz playback mode failed.",
              targetWidth, targetHeight, targetRefresh);
    return false;
  }

  if (auto* win = DX::Windowing())
    win->SetCurrentRefreshRate(static_cast<float>(targetRefresh));

  for (unsigned attempt = 0; attempt < 20; ++attempt)
  {
    const ActiveDisplay live = GetActiveDisplay(target);
    if (live.width == targetWidth && live.height == targetHeight &&
        std::abs(live.refresh - targetRefresh) < 0.1)
      return true;
    Sleep(50);
  }
  return true;
}

std::vector<uint8_t> HexToBytes(const char* hex)
{
  std::vector<uint8_t> out;
  if (!hex)
    return out;
  auto nibble = [](char c) -> int
  {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; hex[i] && hex[i + 1]; i += 2)
  {
    const int hi = nibble(hex[i]);
    const int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0)
      break;
    out.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return out;
}

bool EdidBytesEqual(const uint8_t* a, size_t aLen, const uint8_t* b, size_t bLen)
{
  if (!a || !b || aLen < 128 || bLen < 128)
    return false;
  const size_t cmpLen = (aLen >= 256 && bLen >= 256) ? 256 : 128;
  return std::memcmp(a, b, cmpLen) == 0;
}

struct AmdPfMapEntry
{
  ADLX_PIXEL_FORMAT adlx;
  int adl;
};

constexpr AmdPfMapEntry AMD_PF_MAP[] = {
    {ADLX_RGB_444_FULL, ADL_PF_RGB_FULL},
    {ADLX_YCBCR_444, ADL_PF_YCBCR444},
    {ADLX_YCBCR_422, ADL_PF_YCBCR422},
    {ADLX_RGB_444_LIMITED, ADL_PF_RGB_LIMITED},
    {ADLX_YCBCR_420, ADL_PF_YCBCR420},
};

int AdlxPfToAdlMask(ADLX_PIXEL_FORMAT pf)
{
  for (const auto& e : AMD_PF_MAP)
  {
    if (e.adlx == pf)
      return e.adl;
  }
  return 0;
}

ADLX_PIXEL_FORMAT AdlMaskToAdlxPf(int mask)
{
  for (const auto& e : AMD_PF_MAP)
  {
    if (e.adl == mask)
      return e.adlx;
  }
  return ADLX_FORMAT_UNKNOWN;
}

std::vector<uint8_t> ReadTargetEdid(const DisplayTarget& target)
{
  std::vector<uint8_t> result;
  DISPLAYCONFIG_TARGET_DEVICE_NAME tgtName{};
  tgtName.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(tgtName), target.adapterId,
                    target.id};
  if (DisplayConfigGetDeviceInfo(&tgtName.header) != ERROR_SUCCESS ||
      tgtName.monitorDevicePath[0] == L'\0')
    return result;

  HDEVINFO devInfo = SetupDiCreateDeviceInfoList(nullptr, nullptr);
  if (devInfo == INVALID_HANDLE_VALUE)
    return result;

  SP_DEVICE_INTERFACE_DATA ifData{};
  ifData.cbSize = sizeof(ifData);
  if (SetupDiOpenDeviceInterfaceW(devInfo, tgtName.monitorDevicePath, 0, &ifData))
  {
    SP_DEVINFO_DATA devData{};
    devData.cbSize = sizeof(devData);
    DWORD reqSize = 0;
    SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &reqSize, nullptr);
    if (reqSize >= sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
    {
      std::vector<uint8_t> buf(reqSize, 0);
      auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
      detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
      if (SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, reqSize, nullptr, &devData))
      {
        HKEY hKey = SetupDiOpenDevRegKey(devInfo, &devData, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (hKey != INVALID_HANDLE_VALUE)
        {
          DWORD type = 0;
          DWORD dataSize = 0;
          if (RegQueryValueExW(hKey, L"EDID", nullptr, &type, nullptr, &dataSize) == ERROR_SUCCESS &&
              type == REG_BINARY && dataSize >= 128 && dataSize <= 4096)
          {
            result.resize(dataSize);
            if (RegQueryValueExW(hKey, L"EDID", nullptr, &type, result.data(), &dataSize) ==
                ERROR_SUCCESS)
              result.resize(dataSize);
            else
              result.clear();
          }
          RegCloseKey(hKey);
        }
      }
    }
  }
  SetupDiDestroyDeviceInfoList(devInfo);
  return result;
}

uint32_t GetDisplayVendorId(const std::wstring& gdiName)
{
  const std::optional<DisplayTarget> target = FindTarget(gdiName);
  if (target)
  {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                     reinterpret_cast<void**>(factory.GetAddressOf()))) &&
        factory)
    {
      Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
      for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) !=
                       DXGI_ERROR_NOT_FOUND;
           ++i)
      {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
            desc.AdapterLuid.LowPart == target->adapterId.LowPart &&
            desc.AdapterLuid.HighPart == target->adapterId.HighPart)
        {
          return desc.VendorId;
        }
      }
    }
  }
  return KODI::WINDOWING::WINDOWS::CDVBridgeStateWin::GetInstance().ActiveGpu().vendorId;
}

bool IsAmdDisplay(const std::wstring& gdiName)
{
  return GetDisplayVendorId(gdiName) == PCI_VENDOR_AMD;
}

bool IsIntelDisplay(const std::wstring& gdiName)
{
  return GetDisplayVendorId(gdiName) == PCI_VENDOR_INTEL;
}

class AdlxSession
{
public:
  ~AdlxSession() { Close(); }

  bool Open()
  {
    m_dll = LoadLibraryW(L"amdadlx64.dll");
    if (!m_dll)
      return false;

    using PFN_QueryFullVer = ADLX_RESULT(__cdecl*)(adlx_uint64*);
    using PFN_Init = ADLX_RESULT(__cdecl*)(adlx_uint64, IADLXSystem**);
    auto queryFullVer =
        reinterpret_cast<PFN_QueryFullVer>(GetProcAddress(m_dll, "ADLXQueryFullVersion"));
    auto init = reinterpret_cast<PFN_Init>(GetProcAddress(m_dll, "ADLXInitialize"));
    m_terminate = reinterpret_cast<PFN_Terminate>(GetProcAddress(m_dll, "ADLXTerminate"));

    adlx_uint64 ver = 0;
    if (!queryFullVer || !init || !m_terminate || queryFullVer(&ver) != ADLX_OK ||
        init(ver, &m_sys) != ADLX_OK || !m_sys)
    {
      Close();
      return false;
    }
    m_initialized = true;
    return true;
  }

  void Close()
  {
    m_sys = nullptr;
    if (m_initialized && m_terminate)
      m_terminate();
    m_initialized = false;
    m_terminate = nullptr;
    if (m_dll)
    {
      FreeLibrary(m_dll);
      m_dll = nullptr;
    }
  }

  bool FindDisplayByEdid(const uint8_t* targetEdid,
                         size_t targetEdidLen,
                         AdlxPtr<IADLXDisplayServices>& outSvc,
                         AdlxPtr<IADLXDisplay>& outDisp) const
  {
    outSvc.Reset();
    outDisp.Reset();
    if (!m_initialized || !m_sys || !targetEdid || targetEdidLen < 128)
      return false;

    AdlxPtr<IADLXDisplayServices> svc;
    if (m_sys->lpVtbl->GetDisplaysServices(m_sys, &svc.ptr) != ADLX_OK || !svc)
      return false;

    AdlxPtr<IADLXDisplayList> list;
    if (svc->lpVtbl->GetDisplays(svc.ptr, &list.ptr) != ADLX_OK || !list)
      return false;

    const adlx_uint count = list->lpVtbl->Size(list.ptr);
    for (adlx_uint i = 0; i < count; ++i)
    {
      AdlxPtr<IADLXDisplay> disp;
      if (list->lpVtbl->At_DisplayList(list.ptr, i, &disp.ptr) != ADLX_OK || !disp)
        continue;

      const char* edidHex = nullptr;
      if (disp->lpVtbl->EDID(disp.ptr, &edidHex) == ADLX_OK && edidHex)
      {
        const std::vector<uint8_t> dispEdid = HexToBytes(edidHex);
        if (EdidBytesEqual(targetEdid, targetEdidLen, dispEdid.data(), dispEdid.size()))
        {
          outSvc.ptr = svc.ptr;
          svc.ptr = nullptr;
          outDisp.ptr = disp.ptr;
          disp.ptr = nullptr;
          return true;
        }
      }
    }
    return false;
  }

private:
  using PFN_Terminate = ADLX_RESULT(__cdecl*)();
  HMODULE m_dll{nullptr};
  IADLXSystem* m_sys{nullptr};
  PFN_Terminate m_terminate{nullptr};
  bool m_initialized{false};
};

void* __stdcall AdlAllocCallback(int size)
{
  return std::malloc(size > 0 ? static_cast<size_t>(size) : 1);
}

class Adl2Session
{
public:
  using PFN_Create = int(__stdcall*)(void*(__stdcall*)(int), int, void**);
  using PFN_Destroy = int(__stdcall*)(void*);
  using PFN_NumAdaptersGet = int(__stdcall*)(void*, int*);
  using PFN_AdapterInfoGet = int(__stdcall*)(void*, ADLAdapterInfo*, int);
  using PFN_DispInfoGet = int(__stdcall*)(void*, int, int*, ADLDisplayInfo**, int);
  using PFN_EdidGet = int(__stdcall*)(void*, int, int, ADLDisplayEDIDData*);
  using PFN_IntGet = int(__stdcall*)(void*, int, int, int*);
  using PFN_IntSet = int(__stdcall*)(void*, int, int, int);

  ~Adl2Session() { Close(); }

  bool Open()
  {
    m_dll = LoadLibraryW(L"atiadlxx.dll");
    if (!m_dll)
      return false;

    auto gp = [this](auto& fn, const char* sym)
    { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(m_dll, sym)); };
    gp(m_create, "ADL2_Main_Control_Create");
    gp(m_destroy, "ADL2_Main_Control_Destroy");
    gp(m_numAdp, "ADL2_Adapter_NumberOfAdapters_Get");
    gp(m_adpInfo, "ADL2_Adapter_AdapterInfo_Get");
    gp(m_dispInfo, "ADL2_Display_DisplayInfo_Get");
    gp(m_edidGet, "ADL2_Display_EdidData_Get");
    gp(m_cdGet, "ADL2_Display_ColorDepth_Get");
    gp(m_cdSet, "ADL2_Display_ColorDepth_Set");
    gp(m_cdSup, "ADL2_Display_SupportedColorDepth_Get");
    gp(m_pfGet, "ADL2_Display_PixelFormat_Get");
    gp(m_pfSet, "ADL2_Display_PixelFormat_Set");
    gp(m_pfSup, "ADL2_Display_SupportedPixelFormat_Get");

    if (!m_create || !m_destroy || !m_numAdp || !m_adpInfo || !m_dispInfo ||
        m_create(AdlAllocCallback, 1, &m_ctx) != ADL_OK_STATUS || !m_ctx)
    {
      Close();
      return false;
    }
    return true;
  }

  void Close()
  {
    if (m_ctx && m_destroy)
      m_destroy(m_ctx);
    m_ctx = nullptr;
    if (m_dll)
    {
      FreeLibrary(m_dll);
      m_dll = nullptr;
    }
  }

  bool FindDisplay(const uint8_t* targetEdid,
                   size_t targetEdidLen,
                   const std::wstring& gdiName,
                   int& outAdpIdx,
                   int& outDispIdx) const
  {
    if (!m_ctx)
      return false;
    int nAdp = 0;
    if (m_numAdp(m_ctx, &nAdp) != ADL_OK_STATUS || nAdp <= 0)
      return false;

    std::vector<ADLAdapterInfo> adps(static_cast<size_t>(nAdp));
    for (auto& a : adps)
    {
      std::memset(&a, 0, sizeof(a));
      a.iSize = sizeof(ADLAdapterInfo);
    }
    if (m_adpInfo(m_ctx, adps.data(), static_cast<int>(adps.size() * sizeof(ADLAdapterInfo))) !=
        ADL_OK_STATUS)
      return false;

    const std::string gdiUtf8 = KODI::PLATFORM::WINDOWS::FromW(gdiName);
    int gdiMatchAdp = -1;
    int gdiMatchDisp = -1;

    for (const auto& a : adps)
    {
      if (a.iPresent == 0 || a.iExist == 0)
        continue;
      int nDisp = 0;
      ADLDisplayInfo* rawDisp = nullptr;
      if (m_dispInfo(m_ctx, a.iAdapterIndex, &nDisp, &rawDisp, 0) != ADL_OK_STATUS || !rawDisp ||
          nDisp <= 0)
      {
        std::free(rawDisp);
        continue;
      }
      std::unique_ptr<ADLDisplayInfo, decltype(&std::free)> dispInfos(rawDisp, &std::free);

      for (int d = 0; d < nDisp; ++d)
      {
        const auto& di = dispInfos.get()[d];
        const int activeMask = ADL_DISPLAY_CONNECTED | ADL_DISPLAY_MAPPED;
        if ((di.iDisplayInfoValue & activeMask) != activeMask ||
            di.displayID.iDisplayLogicalAdapterIndex != a.iAdapterIndex)
          continue;

        if (m_edidGet && targetEdid && targetEdidLen >= 128)
        {
          ADLDisplayEDIDData ed{};
          ed.iSize = sizeof(ed);
          if (m_edidGet(m_ctx, a.iAdapterIndex, di.displayID.iDisplayLogicalIndex, &ed) ==
                  ADL_OK_STATUS &&
              ed.iEDIDSize >= 128)
          {
            const size_t sz = std::min<size_t>(static_cast<size_t>(ed.iEDIDSize), sizeof(ed.cEDIDData));
            if (EdidBytesEqual(targetEdid, targetEdidLen,
                               reinterpret_cast<const uint8_t*>(ed.cEDIDData), sz))
            {
              outAdpIdx = a.iAdapterIndex;
              outDispIdx = di.displayID.iDisplayLogicalIndex;
              return true;
            }
          }
        }
        if (gdiMatchAdp < 0 && !gdiUtf8.empty() && _stricmp(a.strDisplayName, gdiUtf8.c_str()) == 0)
        {
          gdiMatchAdp = a.iAdapterIndex;
          gdiMatchDisp = di.displayID.iDisplayLogicalIndex;
        }
      }
    }

    if (gdiMatchAdp >= 0)
    {
      outAdpIdx = gdiMatchAdp;
      outDispIdx = gdiMatchDisp;
      return true;
    }
    return false;
  }

  void* Ctx() const { return m_ctx; }
  PFN_IntGet CdGet() const { return m_cdGet; }
  PFN_IntSet CdSet() const { return m_cdSet; }
  PFN_IntGet CdSup() const { return m_cdSup; }
  PFN_IntGet PfGet() const { return m_pfGet; }
  PFN_IntSet PfSet() const { return m_pfSet; }
  PFN_IntGet PfSup() const { return m_pfSup; }

private:
  HMODULE m_dll{nullptr};
  void* m_ctx{nullptr};
  PFN_Create m_create{nullptr};
  PFN_Destroy m_destroy{nullptr};
  PFN_NumAdaptersGet m_numAdp{nullptr};
  PFN_AdapterInfoGet m_adpInfo{nullptr};
  PFN_DispInfoGet m_dispInfo{nullptr};
  PFN_EdidGet m_edidGet{nullptr};
  PFN_IntGet m_cdGet{nullptr};
  PFN_IntSet m_cdSet{nullptr};
  PFN_IntGet m_cdSup{nullptr};
  PFN_IntGet m_pfGet{nullptr};
  PFN_IntSet m_pfSet{nullptr};
  PFN_IntGet m_pfSup{nullptr};
};

// Restores previously saved AMD colour settings: PixelFormat first, then ColorDepth.
// Returns true if restored, already matching, or unsupported at current mode (and deletes
// SAVED_AMD_COLOUR_FILE); returns false if display wasn't matched/controllable right now.
bool RestoreAmdColour(const AmdSavedColour& saved)
{
  const std::wstring gdiName(saved.gdiName);
  const size_t edidLen = std::min<size_t>(saved.edidLength, sizeof(saved.edid));
  const uint8_t* edidPtr = edidLen >= 128 ? saved.edid : nullptr;

  bool restoredOrVerified = false;
  bool anyError = false;

  {
    AdlxSession adlx;
    AdlxPtr<IADLXDisplayServices> svc;
    AdlxPtr<IADLXDisplay> disp;
    if (adlx.Open() && edidPtr && adlx.FindDisplayByEdid(edidPtr, edidLen, svc, disp))
    {
      AdlxPtr<IADLXDisplayPixelFormat> pf;
      AdlxPtr<IADLXDisplayColorDepth> cd;
      adlx_bool pfSup = 0;
      adlx_bool cdSup = 0;
      const bool havePfIface = svc->lpVtbl->GetPixelFormat(svc.ptr, disp.ptr, &pf.ptr) == ADLX_OK &&
                               pf && pf->lpVtbl->IsSupported(pf.ptr, &pfSup) == ADLX_OK && pfSup;
      const bool haveCdIface = svc->lpVtbl->GetColorDepth(svc.ptr, disp.ptr, &cd.ptr) == ADLX_OK &&
                               cd && cd->lpVtbl->IsSupported(cd.ptr, &cdSup) == ADLX_OK && cdSup;

      if (havePfIface || haveCdIface)
      {
        restoredOrVerified = true;
        ScopedAlteringWindow altering;

        if (saved.hasPixelFormat && saved.pixelFormat != ADLX_FORMAT_UNKNOWN && havePfIface)
        {
          const auto targetPf = static_cast<ADLX_PIXEL_FORMAT>(saved.pixelFormat);
          adlx_bool supVal = 0;
          ADLX_PIXEL_FORMAT curPf = ADLX_FORMAT_UNKNOWN;
          if (pf->lpVtbl->IsSupportedPixelFormat(pf.ptr, targetPf, &supVal) == ADLX_OK && supVal)
          {
            if (pf->lpVtbl->GetValue(pf.ptr, &curPf) != ADLX_OK || curPf != targetPf)
            {
              const ADLX_RESULT r = pf->lpVtbl->SetValue(pf.ptr, targetPf);
              if (r == ADLX_OK)
                Sleep(200);
              else
              {
                anyError = true;
                CLog::Log(LOGWARNING,
                          "Dolby Vision output: ADLX restoring pixel format {} failed ({}).",
                          saved.pixelFormat, static_cast<int>(r));
              }
            }
          }
          else
          {
            CLog::Log(LOGINFO,
                      "Dolby Vision output: saved AMD pixel format {} is unsupported at current "
                      "display mode; skipping restore.",
                      saved.pixelFormat);
          }
        }

        if (saved.hasColorDepth && saved.colorDepth != ADLX_BPC_UNKNOWN && haveCdIface)
        {
          const auto targetCd = static_cast<ADLX_COLOR_DEPTH>(saved.colorDepth);
          adlx_bool supVal = 0;
          ADLX_COLOR_DEPTH curCd = ADLX_BPC_UNKNOWN;
          if (cd->lpVtbl->IsSupportedColorDepth(cd.ptr, targetCd, &supVal) == ADLX_OK && supVal)
          {
            if (cd->lpVtbl->GetValue(cd.ptr, &curCd) != ADLX_OK || curCd != targetCd)
            {
              const ADLX_RESULT r = cd->lpVtbl->SetValue(cd.ptr, targetCd);
              if (r == ADLX_OK)
                Sleep(200);
              else
              {
                anyError = true;
                CLog::Log(LOGWARNING,
                          "Dolby Vision output: ADLX restoring colour depth {} failed ({}).",
                          saved.colorDepth, static_cast<int>(r));
              }
            }
          }
          else
          {
            CLog::Log(LOGINFO,
                      "Dolby Vision output: saved AMD colour depth {} is unsupported at current "
                      "display mode; skipping restore.",
                      saved.colorDepth);
          }
        }
      }
    }
  }

  if (!restoredOrVerified || anyError)
  {
    Adl2Session adl2;
    int adpIdx = -1;
    int dispIdx = -1;
    if (adl2.Open() && adl2.FindDisplay(edidPtr, edidLen, gdiName, adpIdx, dispIdx))
    {
      bool adlErr = false;
      bool adlHandled = false;
      ScopedAlteringWindow altering;

      if (saved.hasPixelFormat && saved.pixelFormat != ADLX_FORMAT_UNKNOWN && adl2.PfGet() &&
          adl2.PfSet())
      {
        const int targetMask = AdlxPfToAdlMask(static_cast<ADLX_PIXEL_FORMAT>(saved.pixelFormat));
        int supMask = 0;
        int curMask = 0;
        const bool haveSup = adl2.PfSup() && adl2.PfSup()(adl2.Ctx(), adpIdx, dispIdx, &supMask) ==
                                                 ADL_OK_STATUS;
        if (targetMask != 0 && (!haveSup || (supMask & targetMask) != 0) &&
            adl2.PfGet()(adl2.Ctx(), adpIdx, dispIdx, &curMask) == ADL_OK_STATUS)
        {
          adlHandled = true;
          if (curMask != targetMask)
          {
            const int r = adl2.PfSet()(adl2.Ctx(), adpIdx, dispIdx, targetMask);
            if (r >= 0)
              Sleep(200);
            else
            {
              adlErr = true;
              CLog::Log(LOGWARNING,
                        "Dolby Vision output: ADL2 restoring pixel format 0x{:X} failed ({}).",
                        targetMask, r);
            }
          }
        }
      }

      if (saved.hasColorDepth && saved.colorDepth != ADLX_BPC_UNKNOWN && adl2.CdGet() &&
          adl2.CdSet())
      {
        int supMask = 0;
        int curCd = 0;
        const bool haveSup = adl2.CdSup() && adl2.CdSup()(adl2.Ctx(), adpIdx, dispIdx, &supMask) ==
                                                 ADL_OK_STATUS;
        if ((!haveSup || (supMask & (1 << saved.colorDepth)) != 0) &&
            adl2.CdGet()(adl2.Ctx(), adpIdx, dispIdx, &curCd) == ADL_OK_STATUS)
        {
          adlHandled = true;
          if (curCd != saved.colorDepth)
          {
            const int r = adl2.CdSet()(adl2.Ctx(), adpIdx, dispIdx, saved.colorDepth);
            if (r >= 0)
              Sleep(200);
            else
            {
              adlErr = true;
              CLog::Log(LOGWARNING,
                        "Dolby Vision output: ADL2 restoring colour depth {} failed ({}).",
                        saved.colorDepth, r);
            }
          }
        }
      }

      if (adlHandled)
      {
        restoredOrVerified = true;
        anyError = adlErr;
      }
    }
  }

  if (restoredOrVerified && !anyError)
  {
    CLog::Log(LOGINFO, "Dolby Vision output: restored AMD output colour (cd={}, pf={}).",
              saved.hasColorDepth ? saved.colorDepth : 0,
              saved.hasPixelFormat ? saved.pixelFormat : 0);
    XFILE::CFile::Delete(SAVED_AMD_COLOUR_FILE);
    return true;
  }
  return false;
}

// Switches an AMD display to 8 bpc + RGB 4:4:4 Full before MMIO carrier creation.
// Sets outChanged=true (and populates outSaved) only when driver colour state was modified.
bool ApplyAmdTunnelColour(const std::wstring& gdiName,
                          AmdSavedColour& outSaved,
                          bool& outChanged,
                          bool saveBackup = true)
{
  outSaved = AmdSavedColour{};
  outChanged = false;

  const std::optional<DisplayTarget> target = FindTarget(gdiName);
  const std::vector<uint8_t> edid = target ? ReadTargetEdid(*target) : std::vector<uint8_t>{};

  auto fillSaved = [&](int32_t cd, bool haveCd, int32_t pf, bool havePf)
  {
    outSaved = AmdSavedColour{};
    wcsncpy_s(outSaved.gdiName, gdiName.c_str(), _TRUNCATE);
    outSaved.edidLength =
        static_cast<uint32_t>(std::min<size_t>(edid.size(), sizeof(outSaved.edid)));
    if (outSaved.edidLength > 0)
      std::memcpy(outSaved.edid, edid.data(), outSaved.edidLength);
    outSaved.hasColorDepth = haveCd ? 1 : 0;
    outSaved.colorDepth = cd;
    outSaved.hasPixelFormat = havePf ? 1 : 0;
    outSaved.pixelFormat = pf;
  };

  // 1. Primary path: ADLX (amdadlx64.dll) matched by active monitor EDID.
  {
    AdlxSession adlx;
    AdlxPtr<IADLXDisplayServices> svc;
    AdlxPtr<IADLXDisplay> disp;
    if (adlx.Open() && !edid.empty() && adlx.FindDisplayByEdid(edid.data(), edid.size(), svc, disp))
    {
      AdlxPtr<IADLXDisplayColorDepth> cd;
      AdlxPtr<IADLXDisplayPixelFormat> pf;
      adlx_bool cdSup = 0;
      adlx_bool pfSup = 0;
      ADLX_COLOR_DEPTH curCd = ADLX_BPC_UNKNOWN;
      ADLX_PIXEL_FORMAT curPf = ADLX_FORMAT_UNKNOWN;

      const bool haveCd = svc->lpVtbl->GetColorDepth(svc.ptr, disp.ptr, &cd.ptr) == ADLX_OK && cd &&
                          cd->lpVtbl->IsSupported(cd.ptr, &cdSup) == ADLX_OK && cdSup &&
                          cd->lpVtbl->GetValue(cd.ptr, &curCd) == ADLX_OK;
      const bool havePf = svc->lpVtbl->GetPixelFormat(svc.ptr, disp.ptr, &pf.ptr) == ADLX_OK && pf &&
                          pf->lpVtbl->IsSupported(pf.ptr, &pfSup) == ADLX_OK && pfSup &&
                          pf->lpVtbl->GetValue(pf.ptr, &curPf) == ADLX_OK;

      if (haveCd || havePf)
      {
        const bool needCd = haveCd && curCd != ADLX_BPC_8;
        const bool needPf = havePf && curPf != ADLX_RGB_444_FULL;
        if (!needCd && !needPf)
        {
          CLog::Log(LOGINFO, "Dolby Vision output: AMD ADLX colour is already 8 bpc RGB Full.");
          return true;
        }

        fillSaved(curCd, haveCd, curPf, havePf);
        if (saveBackup)
          SaveAmdColour(outSaved);

        bool switchOk = true;
        {
          ScopedAlteringWindow altering;
          if (auto* win = DX::Windowing())
            win->SuppressDisplayChangeFor(std::chrono::milliseconds(2000));
          ScopedRegistryModeSync regSync(gdiName);
          // Reduce colour depth BEFORE enforcing RGB 4:4:4 Full so HDMI 2.0 18 Gbps stays valid.
          if (needCd)
          {
            const ADLX_RESULT r = cd->lpVtbl->SetValue(cd.ptr, ADLX_BPC_8);
            if (r == ADLX_OK)
              Sleep(250);
            else
            {
              CLog::Log(LOGERROR, "Dolby Vision output: ADLX SetValue(BPC_8) failed ({}).",
                        static_cast<int>(r));
              switchOk = false;
            }
          }
          if (switchOk && needPf)
          {
            const ADLX_RESULT r = pf->lpVtbl->SetValue(pf.ptr, ADLX_RGB_444_FULL);
            if (r == ADLX_OK)
              Sleep(250);
            else
            {
              CLog::Log(LOGERROR, "Dolby Vision output: ADLX SetValue(RGB_444_FULL) failed ({}).",
                        static_cast<int>(r));
              switchOk = false;
            }
          }
        }

        if (switchOk)
        {
          ADLX_COLOR_DEPTH nowCd = ADLX_BPC_UNKNOWN;
          ADLX_PIXEL_FORMAT nowPf = ADLX_FORMAT_UNKNOWN;
          if (haveCd &&
              (cd->lpVtbl->GetValue(cd.ptr, &nowCd) != ADLX_OK || nowCd != ADLX_BPC_8))
            switchOk = false;
          if (havePf &&
              (pf->lpVtbl->GetValue(pf.ptr, &nowPf) != ADLX_OK || nowPf != ADLX_RGB_444_FULL))
            switchOk = false;
        }

        if (!switchOk)
        {
          cd.Reset();
          pf.Reset();
          disp.Reset();
          svc.Reset();
          adlx.Close();
          if (saveBackup)
            RestoreAmdColour(outSaved);
          return false;
        }

        outChanged = true;
        CLog::Log(LOGINFO,
                  "Dolby Vision output: AMD ADLX switched output colour (cd={}->{}, pf={}->{}) to "
                  "8 bpc RGB Full.",
                  static_cast<int>(curCd), static_cast<int>(ADLX_BPC_8), static_cast<int>(curPf),
                  static_cast<int>(ADLX_RGB_444_FULL));
        return true;
      }
    }
  }

  // 2. Fallback path: ADL2 (atiadlxx.dll).
  {
    Adl2Session adl2;
    int adpIdx = -1;
    int dispIdx = -1;
    if (adl2.Open() && adl2.FindDisplay(edid.empty() ? nullptr : edid.data(), edid.size(), gdiName,
                                        adpIdx, dispIdx))
    {
      int curCd = 0;
      int curPfMask = 0;
      const bool haveCd =
          adl2.CdGet() && adl2.CdSet() &&
          adl2.CdGet()(adl2.Ctx(), adpIdx, dispIdx, &curCd) == ADL_OK_STATUS && curCd != 0;
      const bool havePf =
          adl2.PfGet() && adl2.PfSet() &&
          adl2.PfGet()(adl2.Ctx(), adpIdx, dispIdx, &curPfMask) == ADL_OK_STATUS &&
          curPfMask != 0;

      if (haveCd || havePf)
      {
        const bool needCd = haveCd && curCd != ADL_COLORDEPTH_888_VAL;
        const bool needPf = havePf && curPfMask != ADL_PF_RGB_FULL;
        if (!needCd && !needPf)
        {
          CLog::Log(LOGINFO, "Dolby Vision output: AMD ADL2 colour is already 8 bpc RGB Full.");
          return true;
        }

        const ADLX_PIXEL_FORMAT origPfCanonical = AdlMaskToAdlxPf(curPfMask);
        fillSaved(curCd, haveCd, origPfCanonical, havePf && origPfCanonical != ADLX_FORMAT_UNKNOWN);
        if (saveBackup)
          SaveAmdColour(outSaved);

        bool switchOk = true;
        {
          ScopedAlteringWindow altering;
          if (auto* win = DX::Windowing())
            win->SuppressDisplayChangeFor(std::chrono::milliseconds(2000));
          ScopedRegistryModeSync regSync(gdiName);
          if (needCd)
          {
            const int r = adl2.CdSet()(adl2.Ctx(), adpIdx, dispIdx, ADL_COLORDEPTH_888_VAL);
            if (r >= 0)
              Sleep(250);
            else
            {
              CLog::Log(LOGERROR, "Dolby Vision output: ADL2 ColorDepth_Set(8bpc) failed ({}).", r);
              switchOk = false;
            }
          }
          if (switchOk && needPf)
          {
            const int r = adl2.PfSet()(adl2.Ctx(), adpIdx, dispIdx, ADL_PF_RGB_FULL);
            if (r >= 0)
              Sleep(250);
            else
            {
              CLog::Log(LOGERROR, "Dolby Vision output: ADL2 PixelFormat_Set(RGB_FULL) failed ({}).",
                        r);
              switchOk = false;
            }
          }
        }

        if (switchOk)
        {
          int nowCd = 0;
          int nowPf = 0;
          if (haveCd && (adl2.CdGet()(adl2.Ctx(), adpIdx, dispIdx, &nowCd) != ADL_OK_STATUS ||
                         nowCd != ADL_COLORDEPTH_888_VAL))
            switchOk = false;
          if (havePf && (adl2.PfGet()(adl2.Ctx(), adpIdx, dispIdx, &nowPf) != ADL_OK_STATUS ||
                         nowPf != ADL_PF_RGB_FULL))
            switchOk = false;
        }

        if (!switchOk)
        {
          adl2.Close();
          if (saveBackup)
            RestoreAmdColour(outSaved);
          return false;
        }

        outChanged = true;
        CLog::Log(LOGINFO,
                  "Dolby Vision output: AMD ADL2 switched output colour (cd={}->{}, pf=0x{:X}->0x1) "
                  "to 8 bpc RGB Full.",
                  curCd, ADL_COLORDEPTH_888_VAL, curPfMask);
        return true;
      }
    }
  }

  CLog::Log(LOGDEBUG,
            "Dolby Vision output: AMD ADLX/ADL2 colour control unavailable on this output; "
            "continuing with current driver colour.");
  return true;
}

struct WindowsDisplayColourState
{
  bool valid{false};
  UINT32 bpc{0};
  DISPLAYCONFIG_COLOR_ENCODING encoding{DISPLAYCONFIG_COLOR_ENCODING_RGB};
  bool advancedColorEnabled{false};
};

WindowsDisplayColourState GetWindowsDisplayColourState(const DisplayTarget& target)
{
  WindowsDisplayColourState st{};
  DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info{};
  info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
  info.header.size = sizeof(info);
  info.header.adapterId = target.adapterId;
  info.header.id = target.id;
  if (DisplayConfigGetDeviceInfo(&info.header) == ERROR_SUCCESS)
  {
    st.valid = true;
    st.bpc = info.bitsPerColorChannel;
    st.encoding = info.colorEncoding;
    st.advancedColorEnabled = info.advancedColorEnabled != 0;
  }
  return st;
}

const char* WindowsEncodingName(DISPLAYCONFIG_COLOR_ENCODING enc)
{
  switch (enc)
  {
  case DISPLAYCONFIG_COLOR_ENCODING_RGB:
    return "RGB";
  case DISPLAYCONFIG_COLOR_ENCODING_YCBCR444:
    return "YCbCr 4:4:4";
  case DISPLAYCONFIG_COLOR_ENCODING_YCBCR422:
    return "YCbCr 4:2:2";
  case DISPLAYCONFIG_COLOR_ENCODING_YCBCR420:
    return "YCbCr 4:2:0";
  default:
    return "Unknown";
  }
}

class IgclSession
{
public:
  using PFN_Init = ctl_result_t(__cdecl*)(CtlInitArgs*, ctl_api_handle_t*);
  using PFN_Close = ctl_result_t(__cdecl*)(ctl_api_handle_t);
  using PFN_EnumDevices =
      ctl_result_t(__cdecl*)(ctl_api_handle_t, uint32_t*, ctl_device_adapter_handle_t*);
  using PFN_EnumDisplays = ctl_result_t(__cdecl*)(ctl_device_adapter_handle_t,
                                                  uint32_t*,
                                                  ctl_display_output_handle_t*);
  using PFN_GetDisplayProps =
      ctl_result_t(__cdecl*)(ctl_display_output_handle_t, CtlDisplayProperties*);
  using PFN_GetSetWireFormat =
      ctl_result_t(__cdecl*)(ctl_display_output_handle_t, CtlGetSetWireFormatConfig*);
  using PFN_GetSetDisplaySettings =
      ctl_result_t(__cdecl*)(ctl_display_output_handle_t, CtlDisplaySettings*);
  using PFN_EdidManagement =
      ctl_result_t(__cdecl*)(ctl_display_output_handle_t, CtlEdidManagementArgs*);

  ~IgclSession() { Close(); }

  bool Open()
  {
    m_dll = LoadLibraryExW(L"ControlLib.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m_dll)
      return false;

    auto pInit = reinterpret_cast<PFN_Init>(GetProcAddress(m_dll, "ctlInit"));
    m_close = reinterpret_cast<PFN_Close>(GetProcAddress(m_dll, "ctlClose"));
    m_enumDevices =
        reinterpret_cast<PFN_EnumDevices>(GetProcAddress(m_dll, "ctlEnumerateDevices"));
    m_enumDisplays =
        reinterpret_cast<PFN_EnumDisplays>(GetProcAddress(m_dll, "ctlEnumerateDisplayOutputs"));
    m_getDispProps =
        reinterpret_cast<PFN_GetDisplayProps>(GetProcAddress(m_dll, "ctlGetDisplayProperties"));
    m_wireFormat =
        reinterpret_cast<PFN_GetSetWireFormat>(GetProcAddress(m_dll, "ctlGetSetWireFormat"));
    m_dispSettings = reinterpret_cast<PFN_GetSetDisplaySettings>(
        GetProcAddress(m_dll, "ctlGetSetDisplaySettings"));
    m_edidMgmt =
        reinterpret_cast<PFN_EdidManagement>(GetProcAddress(m_dll, "ctlEdidManagement"));

    if (!pInit || !m_close || !m_enumDevices || !m_enumDisplays || !m_getDispProps ||
        !m_wireFormat)
    {
      Close();
      return false;
    }

    auto tryInit = [&](uint32_t appVer) -> ctl_result_t
    {
      CtlInitArgs args{};
      args.Size = sizeof(args);
      args.Version = 0;
      args.AppVersion = appVer;
      args.flags = 0;
      return pInit(&args, &m_api);
    };

    ctl_result_t r = tryInit(CTL_MAKE_VER(1, 0));
    if (r == CTL_RESULT_ERROR_UNSUPPORTED_VERSION)
      r = tryInit(CTL_MAKE_VER(1, 1));
    if ((r != CTL_RESULT_SUCCESS && r != CTL_RESULT_SUCCESS_STILL_OPEN_BY_ANOTHER_CALLER) ||
        !m_api)
    {
      m_api = nullptr;
      Close();
      return false;
    }
    return true;
  }

  void Close()
  {
    if (m_api && m_close)
      m_close(m_api);
    m_api = nullptr;
    m_close = nullptr;
    m_enumDevices = nullptr;
    m_enumDisplays = nullptr;
    m_getDispProps = nullptr;
    m_wireFormat = nullptr;
    m_dispSettings = nullptr;
    m_edidMgmt = nullptr;
    if (m_dll)
    {
      FreeLibrary(m_dll);
      m_dll = nullptr;
    }
  }

  bool FindActiveOutput(uint32_t targetId,
                        const uint8_t* targetEdid,
                        size_t targetEdidLen,
                        ctl_display_output_handle_t& outDisp) const
  {
    outDisp = nullptr;
    if (!m_api || !m_enumDevices || !m_enumDisplays || !m_getDispProps)
      return false;

    uint32_t devCount = 0;
    if (m_enumDevices(m_api, &devCount, nullptr) != CTL_RESULT_SUCCESS || devCount == 0)
      return false;

    std::vector<ctl_device_adapter_handle_t> devices(devCount, nullptr);
    if (m_enumDevices(m_api, &devCount, devices.data()) != CTL_RESULT_SUCCESS)
      return false;

    ctl_display_output_handle_t edidCandidate = nullptr;
    for (uint32_t a = 0; a < devCount; ++a)
    {
      if (!devices[a])
        continue;
      uint32_t dispCount = 0;
      if (m_enumDisplays(devices[a], &dispCount, nullptr) != CTL_RESULT_SUCCESS || dispCount == 0)
        continue;
      std::vector<ctl_display_output_handle_t> outputs(dispCount, nullptr);
      if (m_enumDisplays(devices[a], &dispCount, outputs.data()) != CTL_RESULT_SUCCESS)
        continue;

      for (uint32_t d = 0; d < dispCount; ++d)
      {
        if (!outputs[d])
          continue;
        CtlDisplayProperties props{};
        props.Size = sizeof(props);
        props.Version = 0;
        if (m_getDispProps(outputs[d], &props) != CTL_RESULT_SUCCESS)
          continue;
        constexpr uint32_t kActiveMask =
            CTL_DISPLAY_CONFIG_FLAG_DISPLAY_ACTIVE | CTL_DISPLAY_CONFIG_FLAG_DISPLAY_ATTACHED;
        if ((props.DisplayConfigFlags & kActiveMask) != kActiveMask)
          continue;

        if (targetId != 0 && props.Os_display_encoder_handle.WindowsDisplayEncoderID == targetId)
        {
          outDisp = outputs[d];
          return true;
        }
        if (!edidCandidate && m_edidMgmt && targetEdid && targetEdidLen >= 128)
        {
          CtlEdidManagementArgs edArgs{};
          edArgs.Size = sizeof(edArgs);
          edArgs.OpType = CTL_EDID_MANAGEMENT_OPTYPE_READ_EDID;
          edArgs.EdidType = CTL_EDID_TYPE_CURRENT;
          if (m_edidMgmt(outputs[d], &edArgs) == CTL_RESULT_SUCCESS && edArgs.EdidSize >= 128 &&
              edArgs.EdidSize <= 4096)
          {
            std::vector<uint8_t> buf(edArgs.EdidSize, 0);
            edArgs.pEdidBuf = buf.data();
            if (m_edidMgmt(outputs[d], &edArgs) == CTL_RESULT_SUCCESS &&
                EdidBytesEqual(targetEdid, targetEdidLen, buf.data(), buf.size()))
              edidCandidate = outputs[d];
          }
        }
      }
    }

    if (edidCandidate)
    {
      outDisp = edidCandidate;
      return true;
    }
    return false;
  }

  static CtlGetSetWireFormatConfig InitWireConfig(int32_t op)
  {
    CtlGetSetWireFormatConfig cfg{};
    cfg.Size = sizeof(cfg);
    cfg.Version = 0;
    cfg.Operation = op;
    for (auto& s : cfg.SupportedWireFormat)
    {
      s.Size = sizeof(CtlWireFormat);
      s.Version = 0;
    }
    cfg.WireFormat.Size = sizeof(CtlWireFormat);
    cfg.WireFormat.Version = 0;
    return cfg;
  }

  bool GetWireFormat(ctl_display_output_handle_t disp, CtlGetSetWireFormatConfig& outCfg) const
  {
    outCfg = InitWireConfig(CTL_WIRE_FORMAT_OPERATION_TYPE_GET);
    return m_wireFormat && m_wireFormat(disp, &outCfg) == CTL_RESULT_SUCCESS;
  }

  ctl_result_t SetWireFormat(ctl_display_output_handle_t disp,
                             int32_t colorModel,
                             uint32_t colorDepthFlag) const
  {
    if (!m_wireFormat)
      return CTL_RESULT_ERROR_UNSUPPORTED_VERSION;
    CtlGetSetWireFormatConfig cfg = InitWireConfig(CTL_WIRE_FORMAT_OPERATION_TYPE_SET);
    cfg.WireFormat.ColorModel = colorModel;
    cfg.WireFormat.ColorDepth = colorDepthFlag;
    return m_wireFormat(disp, &cfg);
  }

  bool GetDisplaySettings(ctl_display_output_handle_t disp, CtlDisplaySettings& outDs) const
  {
    outDs = CtlDisplaySettings{};
    outDs.Size = sizeof(outDs);
    outDs.Version = 0;
    outDs.Set = false;
    return m_dispSettings && m_dispSettings(disp, &outDs) == CTL_RESULT_SUCCESS;
  }

  ctl_result_t SetQuantizationRange(ctl_display_output_handle_t disp, int32_t range) const
  {
    if (!m_dispSettings)
      return CTL_RESULT_ERROR_UNSUPPORTED_VERSION;
    CtlDisplaySettings ds{};
    ds.Size = sizeof(ds);
    ds.Version = 0;
    ds.Set = true;
    ds.ValidFlags = CTL_DISPLAY_SETTING_FLAG_QUANTIZATION_RANGE;
    ds.QuantizationRange = range;
    return m_dispSettings(disp, &ds);
  }

private:
  HMODULE m_dll{nullptr};
  ctl_api_handle_t m_api{nullptr};
  PFN_Close m_close{nullptr};
  PFN_EnumDevices m_enumDevices{nullptr};
  PFN_EnumDisplays m_enumDisplays{nullptr};
  PFN_GetDisplayProps m_getDispProps{nullptr};
  PFN_GetSetWireFormat m_wireFormat{nullptr};
  PFN_GetSetDisplaySettings m_dispSettings{nullptr};
  PFN_EdidManagement m_edidMgmt{nullptr};
};

// Restores previously saved Intel colour settings: Quantization Range first, then Wire Format.
// Returns true if restored, already matching, or unsupported (and deletes SAVED_INTEL_COLOUR_FILE).
bool RestoreIntelColour(const IntelSavedColour& saved)
{
  const size_t edidLen = std::min<size_t>(saved.edidLength, sizeof(saved.edid));
  const uint8_t* edidPtr = edidLen >= 128 ? saved.edid : nullptr;
  const std::optional<DisplayTarget> curTarget =
      saved.gdiName[0] != L'\0' ? FindTarget(saved.gdiName) : std::optional<DisplayTarget>{};
  const uint32_t lookupTargetId = saved.targetId != 0 ? saved.targetId
                                                      : (curTarget ? curTarget->id : 0u);

  IgclSession igcl;
  ctl_display_output_handle_t disp = nullptr;
  if (!igcl.Open() || !igcl.FindActiveOutput(lookupTargetId, edidPtr, edidLen, disp))
  {
    CLog::Log(LOGWARNING,
              "Dolby Vision output: could not re-acquire Intel display output for colour restore.");
    return false;
  }

  bool anyError = false;
  bool verified = true;
  {
    ScopedAlteringWindow altering;
    if (auto* win = DX::Windowing())
      win->SuppressDisplayChangeFor(std::chrono::milliseconds(2000));

    if (saved.hasQuantRange)
    {
      CtlDisplaySettings ds{};
      if (igcl.GetDisplaySettings(disp, ds) &&
          (ds.SupportedFlags & CTL_DISPLAY_SETTING_FLAG_QUANTIZATION_RANGE) != 0 &&
          (ds.ControllableFlags & CTL_DISPLAY_SETTING_FLAG_QUANTIZATION_RANGE) != 0 &&
          ds.QuantizationRange != saved.quantRange)
      {
        const ctl_result_t r = igcl.SetQuantizationRange(disp, saved.quantRange);
        if (r == CTL_RESULT_SUCCESS)
          Sleep(100);
        else
        {
          CLog::Log(LOGWARNING,
                    "Dolby Vision output: Intel IGCL QuantizationRange restore failed (0x{:X}).",
                    r);
          anyError = true;
        }
      }
    }

    if (saved.hasWireFormat && saved.colorDepth != 0)
    {
      CtlGetSetWireFormatConfig wf{};
      if (igcl.GetWireFormat(disp, wf))
      {
        if (wf.WireFormat.ColorModel != saved.colorModel ||
            wf.WireFormat.ColorDepth != saved.colorDepth)
        {
          const ctl_result_t r =
              igcl.SetWireFormat(disp, saved.colorModel, saved.colorDepth);
          if (r == CTL_RESULT_SUCCESS)
          {
            Sleep(250);
            CtlGetSetWireFormatConfig verify{};
            if (!igcl.GetWireFormat(disp, verify) ||
                verify.WireFormat.ColorModel != saved.colorModel ||
                verify.WireFormat.ColorDepth != saved.colorDepth)
              verified = false;
          }
          else
          {
            CLog::Log(LOGWARNING,
                      "Dolby Vision output: Intel IGCL WireFormat restore failed (0x{:X}).", r);
            anyError = true;
          }
        }
      }
    }
  }

  if (!anyError && verified)
  {
    XFILE::CFile::Delete(SAVED_INTEL_COLOUR_FILE);
    CLog::Log(LOGINFO,
              "Dolby Vision output: restored Intel output colour (model={}, bpc=0x{:X}, q={}).",
              saved.colorModel, saved.colorDepth, saved.quantRange);
    return true;
  }
  return false;
}

bool ApplyIntelTunnelColour(const std::wstring& gdiName,
                            IntelSavedColour& outSaved,
                            bool& outChanged,
                            bool saveBackup = true)
{
  outSaved = IntelSavedColour{};
  outChanged = false;

  const std::optional<DisplayTarget> target = FindTarget(gdiName);
  const std::vector<uint8_t> edid = target ? ReadTargetEdid(*target) : std::vector<uint8_t>{};
  const WindowsDisplayColourState initialWin =
      target ? GetWindowsDisplayColourState(*target) : WindowsDisplayColourState{};

  auto isWin8BpcRgb = [](const WindowsDisplayColourState& st) -> bool
  {
    return !st.valid ||
           (st.bpc == 8 && st.encoding == DISPLAYCONFIG_COLOR_ENCODING_RGB);
  };

  IgclSession igcl;
  ctl_display_output_handle_t disp = nullptr;
  if (igcl.Open() && igcl.FindActiveOutput(target ? target->id : 0u,
                                           edid.empty() ? nullptr : edid.data(), edid.size(),
                                           disp))
  {
    CtlGetSetWireFormatConfig wf{};
    if (igcl.GetWireFormat(disp, wf))
    {
      const int32_t curModel = wf.WireFormat.ColorModel;
      const uint32_t curDepth = wf.WireFormat.ColorDepth;
      const bool needWire =
          curModel != CTL_WIRE_FORMAT_COLOR_MODEL_RGB || curDepth != CTL_OUTPUT_BPC_FLAG_8BPC;

      bool hasAnySupportedTable = false;
      bool supportsRgb8 = !needWire;
      for (const auto& entry : wf.SupportedWireFormat)
      {
        if (entry.ColorDepth != 0)
        {
          hasAnySupportedTable = true;
          if (entry.ColorModel == CTL_WIRE_FORMAT_COLOR_MODEL_RGB &&
              (entry.ColorDepth & CTL_OUTPUT_BPC_FLAG_8BPC) != 0)
            supportsRgb8 = true;
        }
      }
      if (needWire && hasAnySupportedTable && !supportsRgb8)
      {
        CLog::Log(LOGWARNING,
                  "Dolby Vision output: Intel display output does not report 8 bpc RGB wire "
                  "format support (cur model={}, bpc=0x{:X}).",
                  curModel, curDepth);
        return false;
      }

      CtlDisplaySettings ds{};
      const bool haveDs = igcl.GetDisplaySettings(disp, ds);
      const bool canQuant =
          haveDs && (ds.SupportedFlags & CTL_DISPLAY_SETTING_FLAG_QUANTIZATION_RANGE) != 0 &&
          (ds.ControllableFlags & CTL_DISPLAY_SETTING_FLAG_QUANTIZATION_RANGE) != 0;
      const bool needQuant =
          canQuant && ds.QuantizationRange != CTL_DISPLAY_SETTING_QUANTIZATION_RANGE_FULL_RANGE;

      if (!needWire && !needQuant)
      {
        if (!isWin8BpcRgb(initialWin))
        {
          CLog::Log(LOGWARNING,
                    "Dolby Vision output: Windows still reports {} bpc {} on Intel display despite "
                    "8 bpc RGB wire format (disable Windows Auto Color Management / set 8 bpc RGB).",
                    initialWin.bpc, WindowsEncodingName(initialWin.encoding));
          return false;
        }
        CLog::Log(LOGINFO, "Dolby Vision output: Intel IGCL colour is already 8 bpc RGB Full.");
        return true;
      }

      outSaved = IntelSavedColour{};
      wcsncpy_s(outSaved.gdiName, gdiName.c_str(), _TRUNCATE);
      outSaved.targetId = target ? target->id : 0u;
      outSaved.edidLength =
          static_cast<uint32_t>(std::min<size_t>(edid.size(), sizeof(outSaved.edid)));
      if (outSaved.edidLength > 0)
        std::memcpy(outSaved.edid, edid.data(), outSaved.edidLength);
      outSaved.hasWireFormat = needWire ? 1 : 0;
      outSaved.colorModel = curModel;
      outSaved.colorDepth = curDepth;
      outSaved.hasQuantRange = 0;
      outSaved.quantRange = ds.QuantizationRange;

      if (saveBackup && needWire)
        SaveIntelColour(outSaved);

      bool switchOk = true;
      bool wireChanged = false;
      bool quantChanged = false;
      {
        ScopedAlteringWindow altering;
        if (auto* win = DX::Windowing())
          win->SuppressDisplayChangeFor(std::chrono::milliseconds(2000));
        ScopedRegistryModeSync regSync(gdiName);

        if (needWire)
        {
          const ctl_result_t r = igcl.SetWireFormat(disp, CTL_WIRE_FORMAT_COLOR_MODEL_RGB,
                                                    CTL_OUTPUT_BPC_FLAG_8BPC);
          if (r == CTL_RESULT_SUCCESS)
          {
            Sleep(250);
            CtlGetSetWireFormatConfig verify{};
            if (igcl.GetWireFormat(disp, verify) &&
                verify.WireFormat.ColorModel == CTL_WIRE_FORMAT_COLOR_MODEL_RGB &&
                verify.WireFormat.ColorDepth == CTL_OUTPUT_BPC_FLAG_8BPC)
            {
              wireChanged = true;
            }
            else
            {
              CLog::Log(LOGERROR,
                        "Dolby Vision output: Intel IGCL WireFormat(8bpc RGB) did not verify "
                        "(got model={}, bpc=0x{:X}).",
                        verify.WireFormat.ColorModel, verify.WireFormat.ColorDepth);
              switchOk = false;
            }
          }
          else
          {
            CLog::Log(LOGERROR,
                      "Dolby Vision output: Intel IGCL SetWireFormat(8bpc RGB) failed (0x{:X}).",
                      r);
            switchOk = false;
          }
        }

        if (switchOk && needQuant)
        {
          const ctl_result_t r =
              igcl.SetQuantizationRange(disp, CTL_DISPLAY_SETTING_QUANTIZATION_RANGE_FULL_RANGE);
          if (r == CTL_RESULT_SUCCESS)
          {
            outSaved.hasQuantRange = 1;
            quantChanged = true;
            Sleep(100);
          }
          else
          {
            CLog::Log(LOGDEBUG,
                      "Dolby Vision output: Intel IGCL Full-Range quantization set skipped (0x{:X}); "
                      "MMIO PIPE_CSC_MODE bypass + AVI Full-Range remains active.",
                      r);
          }
        }
      }

      if (!switchOk)
      {
        if (saveBackup && (wireChanged || quantChanged))
          RestoreIntelColour(outSaved);
        else if (saveBackup)
          XFILE::CFile::Delete(SAVED_INTEL_COLOUR_FILE);
        return false;
      }

      if (wireChanged || quantChanged)
      {
        outChanged = true;
        if (saveBackup)
          SaveIntelColour(outSaved);
      }

      if (target && wireChanged)
      {
        const WindowsDisplayColourState afterWin = GetWindowsDisplayColourState(*target);
        if (!isWin8BpcRgb(afterWin))
        {
          CLog::Log(LOGWARNING,
                    "Dolby Vision output: Windows still reports {} bpc {} after Intel IGCL switch; "
                    "aborting Dolby Vision tunnel.",
                    afterWin.bpc, WindowsEncodingName(afterWin.encoding));
          if (saveBackup)
            RestoreIntelColour(outSaved);
          return false;
        }
      }

      if (outChanged)
      {
        CLog::Log(LOGINFO,
                  "Dolby Vision output: Intel IGCL switched output colour (model={}->0, "
                  "bpc=0x{:X}->0x2, q={}->{}) to 8 bpc RGB Full.",
                  curModel, curDepth, ds.QuantizationRange,
                  quantChanged ? CTL_DISPLAY_SETTING_QUANTIZATION_RANGE_FULL_RANGE
                               : ds.QuantizationRange);
      }
      return true;
    }
  }

  // Fallback when IGCL ControlLib.dll or wire-format control is unavailable (e.g. legacy drivers/PCON).
  if (!isWin8BpcRgb(initialWin))
  {
    CLog::Log(LOGWARNING,
              "Dolby Vision output: Intel output is currently {} bpc {} and IGCL automatic "
              "colour switch is unavailable; disable Windows Auto Color Management or switch to "
              "8 bpc RGB in Intel Graphics Software.",
              initialWin.bpc, WindowsEncodingName(initialWin.encoding));
    return false;
  }

  CLog::Log(LOGDEBUG,
            "Dolby Vision output: Intel output is already 8 bpc RGB (IGCL ControlLib unavailable); "
            "continuing with MMIO Full-Range bypass.");
  return true;
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

  const bool adjustRefreshRate =
      CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
          CSettings::SETTING_VIDEOPLAYER_ADJUSTREFRESHRATE) != ADJUST_REFRESHRATE_OFF;
  const OutputMode mode = SelectedMode(width, height, fps);
  const std::optional<DisplayTarget> target = FindTarget(gdiName);
  if (target && adjustRefreshRate)
  {
    for (unsigned attempt = 0; attempt < 10; ++attempt)
    {
      const ActiveDisplay display = GetActiveDisplay(*target);
      if (display.width == mode.width && display.height == mode.height &&
          std::abs(display.refresh - mode.refresh) < 0.1)
        break;
      Sleep(50);
    }
  }

  const bool isAmd = IsAmdDisplay(gdiName);
  const bool isIntel = !isAmd && IsIntelDisplay(gdiName);
  if (isAmd || isIntel)
  {
    auto applyVendorColour = [&](bool saveBackup) -> bool {
      if (isAmd)
      {
        AmdSavedColour saved{};
        bool changed = false;
        if (!ApplyAmdTunnelColour(gdiName, saved, changed, saveBackup))
          return false;
        if (changed && !m_impl->amdOriginalColour)
          m_impl->amdOriginalColour = saved;
        return true;
      }
      IntelSavedColour saved{};
      bool changed = false;
      if (!ApplyIntelTunnelColour(gdiName, saved, changed, saveBackup))
        return false;
      if (changed && !m_impl->intelOriginalColour)
        m_impl->intelOriginalColour = saved;
      return true;
    };

    if (!applyVendorColour(true))
    {
      LogRefusal(LOGWARNING, isAmd ? "switching AMD output colour to 8 bpc RGB failed."
                                   : "switching Intel output colour to 8 bpc RGB failed.");
      Restore();
      return false;
    }

    // Switching vendor colour (ADLX/ADL2/IGCL) can revert Kodi's temporary CDS_FULLSCREEN
    // refresh rate back to the desktop registry rate, and Alt+Tab resume starts at that rate.
    if (target && adjustRefreshRate)
    {
      const ActiveDisplay afterColour = GetActiveDisplay(*target);
      if (afterColour.width != mode.width || afterColour.height != mode.height ||
          std::abs(afterColour.refresh - mode.refresh) >= 0.1)
      {
        CLog::Log(LOGINFO,
                  "Dolby Vision output: display timing {}x{} {:.3f} Hz differed from target {}x{} "
                  "{:.3f} Hz; applying playback mode.",
                  afterColour.width, afterColour.height, afterColour.refresh, mode.width,
                  mode.height, mode.refresh);
        if (ApplyAmdTargetDisplayMode(gdiName, mode.width, mode.height, mode.refresh, *target))
        {
          const bool saveBackup =
              isAmd ? !m_impl->amdOriginalColour.has_value()
                    : !m_impl->intelOriginalColour.has_value();
          if (!applyVendorColour(saveBackup))
          {
            LogRefusal(LOGWARNING,
                       isAmd
                           ? "re-verifying AMD output colour after restoring playback refresh failed."
                           : "re-verifying Intel output colour after restoring playback refresh failed.");
            Restore();
            return false;
          }
        }
      }
    }
  }

  OutputMode activeMode = mode;
  if (target)
  {
    const ActiveDisplay live = GetActiveDisplay(*target);
    if (adjustRefreshRate &&
        (live.width != mode.width || live.height != mode.height ||
         std::abs(live.refresh - mode.refresh) >= 0.1))
    {
      LogRefusal(LOGWARNING,
                 StringUtils::Format("display timing {}x{} {:.3f} Hz did not match expected "
                                     "{}x{} {:.3f} Hz.",
                                     live.width, live.height, live.refresh, mode.width, mode.height,
                                     mode.refresh));
      Restore();
      return false;
    }
    if (live.width > 0 && live.height > 0 && live.refresh > 0.0)
      activeMode = {live.width, live.height, live.refresh};
  }

  if (!prepareOutput())
  {
    CLog::Log(LOGERROR, "Dolby Vision output: preparing the 8-bit carrier swapchain failed.");
    Restore();
    return false;
  }

  const uint8_t vic = KODI::WINDOWING::WINDOWS::CDVBridgeStateWin::ComputeCtaVic(
      activeMode.width, activeMode.height, static_cast<float>(activeMode.refresh));
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
  m_impl->mode = StringUtils::Format("{}x{} {:.3f} Hz", activeMode.width, activeMode.height,
                                     activeMode.refresh);
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
  auto restoreVendorColour = [](auto& slot, auto restoreFn) {
    if (!slot)
      return;
    if (auto* win = DX::Windowing())
      win->SuppressDisplayChangeFor(std::chrono::milliseconds(2000));
    const auto saved = *slot;
    restoreFn(saved);
    slot.reset();
    if (saved.gdiName[0] != L'\0')
    {
      const double refresh = GetActiveDisplay(saved.gdiName).refresh;
      if (refresh > 0.0 && DX::Windowing())
        DX::Windowing()->SetCurrentRefreshRate(static_cast<float>(refresh));
    }
  };
  restoreVendorColour(m_impl->amdOriginalColour, RestoreAmdColour);
  restoreVendorColour(m_impl->intelOriginalColour, RestoreIntelColour);
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
  return m_impl->active &&
         (m_impl->originalColour.has_value() || m_impl->amdOriginalColour.has_value() ||
          m_impl->intelOriginalColour.has_value());
}

std::string DX::CDolbyVisionOutput::LastRefusal()
{
  std::unique_lock guard(refusalLock);
  // Without the log message's full stop.
  return lastRefusal.ends_with('.') ? lastRefusal.substr(0, lastRefusal.size() - 1) : lastRefusal;
}

void DX::CDolbyVisionOutput::RemoveLeftoverOutput(const std::wstring& gdiName)
{
  auto recoverVendorColour = [&](const char* vendorName, auto loadFn, auto restoreFn,
                                 const char* backupFile) {
    const auto savedColour = loadFn();
    if (!savedColour)
      return;
    CLog::Log(LOGWARNING,
              "Dolby Vision output: restoring the {} output colour a previous session changed.",
              vendorName);
    if (!restoreFn(*savedColour))
    {
      // Delete stale backup if the saved GDI display now carries a different monitor EDID.
      const std::optional<DisplayTarget> target = FindTarget(gdiName);
      const std::vector<uint8_t> curEdid = target ? ReadTargetEdid(*target) : std::vector<uint8_t>{};
      if (_wcsicmp(savedColour->gdiName, gdiName.c_str()) == 0 && !curEdid.empty() &&
          savedColour->edidLength >= 128 &&
          !EdidBytesEqual(savedColour->edid, savedColour->edidLength, curEdid.data(),
                          curEdid.size()))
      {
        XFILE::CFile::Delete(backupFile);
      }
    }
  };
  recoverVendorColour("AMD", LoadSavedAmdColour, RestoreAmdColour, SAVED_AMD_COLOUR_FILE);
  recoverVendorColour("Intel", LoadSavedIntelColour, RestoreIntelColour, SAVED_INTEL_COLOUR_FILE);
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
