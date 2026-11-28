# LibreELEC-x86-DV (`Kodi 22` + `CB1` + `DVBridge`) Native Windows Port

This directory (`/home/aguan/dv-build/win-dv`) contains the complete Windows port of **`CroqueMr/libreelec-x86-DV`** (`CB1` / `DVBridge`), enabling both **Hardware TV-Led Dolby Vision (8-bit RGB 4:4:4 HDMI Tunneling per ETSI GS CCM 001 V1.1.1)** and **CB1 Master Reference HDR10 Reconstruction (CM v2.9 / v4.0 + FEL + L1/L2/L3/L8)** on 64-bit Windows 10/11.

---

## 1. Directory Structure & Built Artifacts

```text
/home/aguan/dv-build/win-dv/
├── include/
│   ├── dvbridge_win_mmio.h          # Intel Gen9-14 & AMD DCN 1.0-4.01 HDMI VSIF/AVI & Dither MMIO header
│   ├── dvbridge_metadata.h          # ETSI GS CCM 001 V1.1.1 Clauses 6.2 & 6.3 wire format & CRC-32
│   ├── cb1_transport_glsl.h         # ETSI GS CCM 001 V1.1.1 Clause 6.4 RGB8 carrier & 3x parity shader
│   ├── drm_dvbridge.h               # Cross-platform DVBridge transport uAPI structures
│   ├── drm_dvbridge_edid.h          # CEA-861 Dolby Vision VSVDB (OUI 0x00D046 v0/v1/v2) parser
│   └── dvbridge_gl_frame.h          # OpenGL ES 3.0 / ANGLE P010/NV12/Y416 frame importer
├── src/
│   ├── dvbridge_win_mmio.c          # Intel & AMD DCN MMIO register programmer implementation
│   ├── dvbridge_golden_frame.c      # C11 ETSI GS CCM 001 V1.1.1 Golden Frame & MMIO/EDID verification suite
│   ├── dvbridge_core.c              # CB1 core policy, metadata, and transport orchestrator
│   └── WinDvTool.cs                 # Native 64-bit Windows CLI & Golden Frame generator source
├── cb1-win/
│   ├── cb1_win_compat.h             # POSIX/Linux -> Win32 compatibility shims for CB1
│   ├── dvbridge_dmabuf_win.c        # Windows P010 / D3D11va replacement for Linux VAAPI/DRM-Prime
│   ├── dvbridge_fel_win.c           # Windows Profile 7 FEL dual-layer compositor backend
│   ├── dvbridge_fel_vpp_win.c       # Windows FEL VPP upsampler backend
│   ├── DVBridgeStateWin.h           # Windows CDVBridgeStateWin class (replaces Linux GBM DVBridgeState.h)
│   ├── DVBridgeStateWin.cpp         # SetupAPI EDID reader, DXGI GPU detector, and MMIO state manager
│   └── CMakeLists.windows.txt       # CMake build script for Kodi 22 Windows (WINSystemWin32DX + ANGLE)
└── bin/
    ├── test_win_dv                  # Compiled C11 verification binary (5/5 test suites PASS)
    ├── win-dv-tool.exe              # Native 64-bit Windows executable (C:\Users\Public\win-dv\win-dv-tool.exe)
    ├── win_dv_golden_3840x2160.png  # Generated 3840x2160 ETSI GS CCM 001 V1.1.1 Dolby Vision Golden Frame
    ├── DVBridgeStateWin.o           # Verified C++20 object for Kodi Windows windowing
    ├── dvbridge_dmabuf_win.o        # Verified C11 object for CB1 Windows frame import
    ├── dvbridge_fel_win.o           # Verified C11 object for CB1 Windows FEL
    └── dvbridge_fel_vpp_win.o       # Verified C11 object for CB1 Windows FEL VPP
```

---

## 2. How the Two Dolby Vision Modes Work on Windows

### Mode A: CB1 Master Reference / HDR10 PQ Reconstruction (Zero Driver Modification, All GPUs)
- **Supported GPUs**: AMD Radeon, Intel Arc/UHD/Iris, NVIDIA GeForce
- **Supported Displays**: Any HDR10-capable monitor or TV (e.g., `MyASUS_Splendid SDC4172` OLED, `Mi TV XMDF076`)
- **Pipeline**:
  1. `CB1` (`dvbridge_core.c` + `cb1_master_glsl.h` + `mpv_dvbridge_cm4.h`) executes full Dolby Vision RPU/DM reshaping (12-bit polynomial/MMR, Profile 7 FEL reconstruction, ICtCp/IPT-PQ ->LMS -> BT.2020 PQ, L1 dynamic scene tone-mapping, L2/L8 creative trims, L3/L9 mastering display adjustments) inside Kodi's OpenGL ES 3.0 (`ANGLE` -> Direct3D 11) renderer.
  2. `WINSystemWin32DX` presents a 10-bit `DXGI_FORMAT_R10G10B10A2_UNORM` swapchain with `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` and `IDXGISwapChain4::SetHDRMetaData`.

### Mode B: Native Hardware TV-Led Dolby Vision (8-bit RGB 4:4:4 HDMI Tunnel)
- **Supported GPUs**:
  - **AMD DCN 1.0 – 4.01** (including current host `AMD Radeon(TM) Graphics PCI\VEN_1002&DEV_1681`, Rembrandt DCN 3.1.2)
  - **Intel Gen9 – Gen14** (Skylake through Arrow Lake / Lunar Lake)
- **Pipeline**:
  1. **User-Space Pixel & Metadata Packing (`cb1_transport_glsl.h` + `dvbridge_metadata.h`)**:
     - Packs the 71-byte DM base header + extension blocks (`L1`, `L2`, `L5`, `L254`) into 128-byte packets with MSB-first IEEE 802.3 CRC-32 (`dvbridge_dv_crc(pkt, 128) == 0`).
     - Repeats each 1024-bit packet **3 times** (`3072` pixels per packet) on scanline 0, scrambling each metadata bit with the 12-bit parity of `(C[11:1] << 1) ^ Y[11:0]` (`0x6996u` polynomial) into `C[0]`.
     - Packs 12-bit `(Y, C)` into 8-bit `RGB8` via `R = C[11:4]`, `G = Y[11:4]`, `B = (C[3:0] << 4) | Y[3:0]` on a `DXGI_FORMAT_R8G8B8A8_UNORM` fullscreen flip-model swapchain (`DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709`).
  2. **Hardware MMIO Register Programming (`dvbridge_win_mmio.c` + `CDVBridgeStateWin`)**:
     - **AMD DCN 3.1.2 (`0x1002:0x1681`)**:
       - `MMIO[0x13404]` (`FMT0_FMT_BIT_DEPTH_CONTROL`) `&= ~0x0001FF33` (disables truncation & spatial/temporal dithering so `B` LSBs survive bit-exact).
       - `MMIO[0x154A0 / 0x154A4]` (`VPG0_GENERIC_PACKET_ACCESS_CTRL / DATA`): writes RGB Full 8bpc AVI InfoFrame to Slot 0 and Dolby Vision VSIF (`DW0=0x001B0181, DW1=0x00D0464B, DW2=0x00000002`) to Slot 1.
       - `MMIO[0x154AC]` (`VPG0_GSP_IMMEDIATE_UPDATE_CTRL`) `= 0x00000002` and `MMIO[0x15568 / 0x15578]` (`DIG0_HDMI_GENERIC_PACKET_CONTROL0/1`) enables continuous VBI line-2 transmission.
     - **Intel Gen9–14 (`0x8086`)**:
       - `MMIO[0x70030]` (`PIPE_MISC`) clears `PIPE_MISC_DITHER_ENABLE`.
       - `MMIO[0x60200]` (`HSW_TVIDEO_DIP_CTL`) + `MMIO[0x602A0]` (`HSW_TVIDEO_DIP_VS_DATA`) programs the 9-dword Dolby Vision VSIF and enables continuous transmission.
