#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Applies the LibreELEC-x86-DV (Kodi 22 Piers + CB1 + DVBridge + 3D MVC) patch stack
and Windows native hardware/telemetry bridge (CDVBridgeStateWin + dvbridge_win_mmio)
to an upstream xbmc/xbmc checkout for Windows x64 compilation.
"""

import os
import re
import shutil
import subprocess
import sys


def run_cmd(cmd, cwd=None):
    print(f"[CMD] {' '.join(cmd)}")
    res = subprocess.run(cmd, cwd=cwd, text=True, capture_output=True)
    if res.stdout:
        print(res.stdout, end="")
    if res.stderr:
        print(res.stderr, end="", file=sys.stderr)
    if res.returncode != 0:
        raise RuntimeError(f"Command failed with exit code {res.returncode}: {' '.join(cmd)}")


def replace_required(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        if new in text:
            return text
        raise RuntimeError(f"Expected pattern not found during {label}:\n{old}")
    return text.replace(old, new)


def integrate(le_repo_dir: str, kodi_dir: str) -> None:
    le_repo_dir = os.path.abspath(le_repo_dir)
    kodi_dir = os.path.abspath(kodi_dir)

    print(f"=== Integrating CB1 + DVBridge Windows Port into {kodi_dir} ===")

    # 1. Populate C:\kodi-src\cb1\src with CB1 common sources + Windows headers
    cb1_dst = os.path.join(kodi_dir, "cb1", "src")
    os.makedirs(cb1_dst, exist_ok=True)

    cb1_src = os.path.join(le_repo_dir, "win-dv", "base", "cb1-src")
    for name in os.listdir(cb1_src):
        src_path = os.path.join(cb1_src, name)
        if os.path.isfile(src_path):
            shutil.copy2(src_path, os.path.join(cb1_dst, name))

    win_inc = os.path.join(le_repo_dir, "win-dv", "include")
    for name in os.listdir(win_inc):
        src_path = os.path.join(win_inc, name)
        if os.path.isfile(src_path):
            shutil.copy2(src_path, os.path.join(cb1_dst, name))

    # 1b. Copy CB1 media assets (DialogPlayerProcessInfo.xml, IncludesDVProcessInfo.xml,
    #     DialogPlaybackModes.xml, cb1-white.png, cb1-panel.png, cb1-tick.png) into media/
    le_media_dir = os.path.join(le_repo_dir, "win-dv", "base", "media")
    if os.path.isdir(le_media_dir):
        shutil.copytree(le_media_dir, os.path.join(kodi_dir, "media"), dirs_exist_ok=True)
        print("Installed CB1 media assets and XML dialogs into media/")

    # 2. Apply FFmpeg 9.0.2 CB1 + 3D MVC patches to tools/depends/target/ffmpeg/
    ffmpeg_patch_src = os.path.join(le_repo_dir, "win-dv", "base", "patches-ffmpeg")
    ffmpeg_patch_dst = os.path.join(kodi_dir, "tools", "depends", "target", "ffmpeg")

    conflict_patch = os.path.join(
        ffmpeg_patch_dst, "008-ffmpeg-all-pgssubdec-use-caller-colorspace.patch"
    )
    if os.path.exists(conflict_patch):
        os.remove(conflict_patch)
        print(f"Removed superseded patch: {conflict_patch}")

    ffmpeg_patch_map = [
        ("ffmpeg-9900-00-libavcodec__dovi_rpudec.c.patch", "9900-00-ffmpeg-all-cb1-dovi-rpudec.patch"),
        ("ffmpeg-9900-01-libavformat__isom_tags.c.patch", "9900-01-ffmpeg-all-cb1-isom-tags.patch"),
        ("ffmpeg-9900-02-libavutil__dovi_meta.h.patch", "9900-02-ffmpeg-all-cb1-dovi-meta.patch"),
        ("ffmpeg-9900-03-source-dovi-raw.patch", "9900-03-ffmpeg-all-cb1-source-dovi-raw.patch"),
        ("ffmpeg-9900-04-ts-dovi-layer.patch", "9900-04-ffmpeg-all-cb1-ts-dovi-layer.patch"),
        ("ffmpeg-9900-05-matroska-dovi-association.patch", "9900-05-ffmpeg-all-cb1-matroska-dovi.patch"),
        ("ffmpeg-9900-06-hdr10plus-country.patch", "9900-06-ffmpeg-all-cb1-hdr10plus-country.patch"),
        ("ffmpeg-9900-06-hevc-picture-clock.patch", "9900-07-ffmpeg-all-cb1-hevc-picture-clock.patch"),
        ("ffmpeg-9900-07-mov-dovi-hevc-entry.patch", "9900-08-ffmpeg-all-cb1-mov-dovi.patch"),
        ("ffmpeg-9900-08-pgs-authoring.patch", "9900-09-ffmpeg-all-cb1-pgs-authoring.patch"),
        ("ffmpeg-9910-mvc.patch", "9910-ffmpeg-all-mvc.patch"),
    ]

    for src_name, dst_name in ffmpeg_patch_map:
        shutil.copy2(
            os.path.join(ffmpeg_patch_src, src_name),
            os.path.join(ffmpeg_patch_dst, dst_name),
        )
        print(f"Installed FFmpeg patch: {dst_name}")

    # Update cmake/modules/FindFFMPEG.cmake so CMake doesn't try to copy removed 008 patch
    find_ffmpeg = os.path.join(kodi_dir, "cmake", "modules", "FindFFMPEG.cmake")
    if os.path.exists(find_ffmpeg):
        with open(find_ffmpeg, "r", encoding="utf-8") as f:
            content = f.read()
        old_line = (
            "                    COMMAND ${CMAKE_COMMAND} -E copy "
            "${CMAKE_SOURCE_DIR}/tools/depends/target/ffmpeg/"
            "008-ffmpeg-all-pgssubdec-use-caller-colorspace.patch <SOURCE_DIR>"
        )
        new_lines = [
            f"                    COMMAND ${{CMAKE_COMMAND}} -E copy "
            f"${{CMAKE_SOURCE_DIR}}/tools/depends/target/ffmpeg/{dst_name} <SOURCE_DIR>"
            for _, dst_name in ffmpeg_patch_map
        ]
        if old_line in content:
            content = content.replace(old_line, "\n".join(new_lines))
            with open(find_ffmpeg, "w", encoding="utf-8", newline="\n") as f:
                f.write(content)
            print("Updated cmake/modules/FindFFMPEG.cmake with CB1 FFmpeg patches")

    # Prevent BuildSetup.bat (Release) from recompiling FFmpeg a second time after make-mingwlibs.bat
    # Also normalize all .sh scripts under tools/buildsteps/windows/ to LF so MSYS2 sh works on core.autocrlf=true checkouts
    win_steps_dir = os.path.join(kodi_dir, "tools", "buildsteps", "windows")
    if os.path.isdir(win_steps_dir):
        for root_d, _, files_d in os.walk(win_steps_dir):
            for fname in files_d:
                if fname.endswith(".sh"):
                    sh_p = os.path.join(root_d, fname)
                    with open(sh_p, "r", encoding="utf-8") as f:
                        sh_txt = f.read()
                    with open(sh_p, "w", encoding="utf-8", newline="\n") as f:
                        f.write(sh_txt)

    buildhelpers = os.path.join(kodi_dir, "tools", "buildsteps", "windows", "buildhelpers.sh")
    if os.path.exists(buildhelpers):
        with open(buildhelpers, "r", encoding="utf-8") as f:
            bh_txt = f.read()
        bh_old = (
            '  #no optims in release builds!\n'
            '  if [ "$Configuration" == "Release" ]\n'
            '  then\n'
            '    echo "1"\n'
            '    return\n'
            '  fi'
        )
        bh_new = (
            '  if [ -f "$1/include/libavutil/dovi_meta.h" ] && grep -q "dvbridge_raw_magic" "$1/include/libavutil/dovi_meta.h"; then\n'
            '    echo "0"\n'
            '    return\n'
            '  fi'
        )
        if bh_old in bh_txt:
            bh_txt = bh_txt.replace(bh_old, bh_new)
        bh_dl_old = '  if [ ! -d "$LOCALSRCDIR" ] || [ -z $( ls -A "$LOCALSRCDIR" ) ]; then'
        bh_dl_new = '  if [ ! -d "$LOCALSRCDIR" ] || [ ! -f "$LOCALSRCDIR/configure" ]; then'
        if bh_dl_old in bh_txt:
            bh_txt = bh_txt.replace(bh_dl_old, bh_dl_new)
        with open(buildhelpers, "w", encoding="utf-8", newline="\n") as f:
            f.write(bh_txt)
        print("Updated tools/buildsteps/windows/buildhelpers.sh to reuse CB1-patched FFmpeg build")

    localized_msvc_patch = os.path.join(
        ffmpeg_patch_dst, "008-ffmpeg-windows-configure-detect-localized-msvc.patch"
    )
    localized_msvc_patch_content = """diff --git a/configure b/configure
--- a/configure
+++ b/configure
@@ -1696,3 +1696,3 @@ check_pkg_config_header_only(){
 test_exec(){
-    test_ld "cc" "$@" && { enabled cross_compile || $TMPE >> $logfile 2>&1; }
+    test_ld "cc" "$@"
 }
@@ -5449,13 +5449,13 @@ probe_cc(){
-    elif VSLANG=1033 $_cc -nologo- 2>&1 | grep -q ^Microsoft || { $_cc -v 2>&1 | grep -q clang && $_cc -? > /dev/null 2>&1; }; then
+    elif VSLANG=1033 $_cc -nologo- 2>&1 | grep -q "Microsoft (R)" || { $_cc -v 2>&1 | grep -q clang && $_cc -? > /dev/null 2>&1; }; then
         _type=msvc
-        if VSLANG=1033 $_cc -nologo- 2>&1 | grep -q ^Microsoft; then
+        if VSLANG=1033 $_cc -nologo- 2>&1 | grep -q "Microsoft (R)"; then
             # Depending on the tool (cl.exe or link.exe), the version number
             # is printed on the first line of stderr or stdout
-            _ident=$(VSLANG=1033 $_cc 2>&1 | grep ^Microsoft | head -n1 | tr -d '\\r')
+            _ident=$(VSLANG=1033 $_cc 2>&1 | grep "Microsoft (R)" | head -n1 | tr -d '\\r')
         else
             _ident=$($_cc --version 2>/dev/null | head -n1 | tr -d '\\r')
         fi
-        if [ -x "$(command -v wslpath)" ]; then
+        if false && [ -x "$(command -v wslpath)" ]; then
             _DEPCMD='$(DEP$(1)) $(DEP$(1)FLAGS) $($(1)DEP_FLAGS) $< 2>&1 | awk '\\''/including/ { sub(/^.*file: */, ""); if (!match($$0, / /)) { print $$0 } }'\\'' | xargs -r -d\\\\n -n1 wslpath -u | awk '\\''BEGIN { printf "%s:", "$@" }; { sub(/\\r/,""); printf " %s", $$0 }; END { print "" }'\\'' > $(@:.o=.d)'

         else
@@ -5570,7 +5570,7 @@ if [ -z "$CC_DEPFLAGS" ] && [ "$dep_cc" != "$cc" ]; then
 fi

-if VSLANG=1033 $ar 2>&1 | grep -q ^Microsoft; then
+if VSLANG=1033 $ar 2>&1 | grep -q "Microsoft (R)"; then
     arflags="-nologo"
     ar_o='-out:$@'
 elif $ar 2>&1 | grep -q "\\[D\\] "; then
"""
    with open(localized_msvc_patch, "w", encoding="utf-8", newline="\n") as f:
        f.write(localized_msvc_patch_content)

    # 3. Apply Kodi 22 CB1 patches (kodi-9990..9994)
    kodi_patch_dir = os.path.join(le_repo_dir, "win-dv", "base", "patches-kodi")
    kodi_patches = [
        "kodi-9990-cb1-video.patch",
        "kodi-9991-cb1-audio.patch",
        "kodi-9992-cb1-display.patch",
        "kodi-9993-cb1-interface.patch",
        "kodi-9994-cb1-integration.patch",
    ]
    for patch_name in kodi_patches:
        patch_path = os.path.join(kodi_patch_dir, patch_name)
        rev_chk = subprocess.run(
            ["git", "apply", "--check", "--reverse", "--ignore-space-change", "--ignore-whitespace", patch_path],
            cwd=kodi_dir,
            capture_output=True,
        )
        if rev_chk.returncode == 0 or (
            patch_name == "kodi-9994-cb1-integration.patch"
            and os.path.exists(os.path.join(kodi_dir, "xbmc", "utils", "DVRenderUsage.h"))
        ):
            print(f"Already applied Kodi patch: {patch_name}")
            continue
        run_cmd(
            ["git", "apply", "--whitespace=nowarn", "--ignore-space-change", "--ignore-whitespace", patch_path],
            cwd=kodi_dir,
        )
        print(f"Applied Kodi patch: {patch_name}")

    # 4. Update root CMakeLists.txt so DVBRIDGE_SOURCE_DIR defaults to ${CMAKE_SOURCE_DIR}/cb1/src
    #    and is always in include_directories for DVDVideoCodecFFmpeg, RenderManager, and Windows windowing
    root_cmake = os.path.join(kodi_dir, "CMakeLists.txt")
    with open(root_cmake, "r", encoding="utf-8") as f:
        cmake_txt = f.read()

    cmake_txt = cmake_txt.replace(
        'set(DVBRIDGE_SOURCE_DIR "" CACHE PATH "DVBridge common source directory")',
        'set(DVBRIDGE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/cb1/src" CACHE PATH "DVBridge common source directory")\n'
        'include_directories("${DVBRIDGE_SOURCE_DIR}")',
    )
    with open(root_cmake, "w", encoding="utf-8", newline="\n") as f:
        f.write(cmake_txt)
    print("Configured default DVBRIDGE_SOURCE_DIR=${CMAKE_SOURCE_DIR}/cb1/src in CMakeLists.txt")

    # 5. Install CDVBridgeStateWin & dvbridge_win_mmio into xbmc/windowing/windows/
    win_windowing_dir = os.path.join(kodi_dir, "xbmc", "windowing", "windows")
    shutil.copy2(
        os.path.join(le_repo_dir, "win-dv", "cb1-win", "DVBridgeStateWin.h"),
        os.path.join(win_windowing_dir, "DVBridgeStateWin.h"),
    )

    with open(
        os.path.join(le_repo_dir, "win-dv", "cb1-win", "DVBridgeStateWin.cpp"),
        "r",
        encoding="utf-8",
    ) as f:
        state_cpp = f.read()
    # Adjust relative "../include/..." to direct header names (since cb1/src is in include_directories)
    state_cpp = state_cpp.replace('../include/', '')
    # Include dvbridge_win_mmio.c inside extern "C" so it shares the C++ PCH cleanly in MSVC
    state_cpp += '\nextern "C" {\n#include "dvbridge_win_mmio.c"\n}\n'
    with open(
        os.path.join(win_windowing_dir, "DVBridgeStateWin.cpp"),
        "w",
        encoding="utf-8",
        newline="\n",
    ) as f:
        f.write(state_cpp)

    with open(
        os.path.join(win_windowing_dir, "DVBridgeStateWin.h"),
        "r",
        encoding="utf-8",
    ) as f:
        state_h = f.read()
    state_h = state_h.replace('../include/', '')
    with open(
        os.path.join(win_windowing_dir, "DVBridgeStateWin.h"),
        "w",
        encoding="utf-8",
        newline="\n",
    ) as f:
        f.write(state_h)

    with open(
        os.path.join(le_repo_dir, "win-dv", "src", "dvbridge_win_mmio.c"),
        "r",
        encoding="utf-8",
    ) as f:
        mmio_c = f.read()
    mmio_c = mmio_c.replace('../include/', '')
    with open(
        os.path.join(win_windowing_dir, "dvbridge_win_mmio.c"),
        "w",
        encoding="utf-8",
        newline="\n",
    ) as f:
        f.write(mmio_c)

    win_cmake = os.path.join(win_windowing_dir, "CMakeLists.txt")
    with open(win_cmake, "r", encoding="utf-8") as f:
        win_cmake_txt = f.read()
    win_cmake_txt = replace_required(
        win_cmake_txt,
        "set(SOURCES VideoSyncD3D.cpp",
        "set(SOURCES DVBridgeStateWin.cpp\n            VideoSyncD3D.cpp",
        "xbmc/windowing/windows/CMakeLists.txt SOURCES",
    )
    win_cmake_txt = replace_required(
        win_cmake_txt,
        "set(HEADERS VideoSyncD3D.h",
        "set(HEADERS DVBridgeStateWin.h\n            VideoSyncD3D.h",
        "xbmc/windowing/windows/CMakeLists.txt HEADERS",
    )
    with open(win_cmake, "w", encoding="utf-8", newline="\n") as f:
        f.write(win_cmake_txt)
    print("Added DVBridgeStateWin.cpp (with dvbridge_win_mmio.c) to xbmc/windowing/windows/CMakeLists.txt")

    # 5b. Hook GetDisplayHDRCapabilities() and IsHDRDisplay() in WinSystemWin32DX.h/.cpp
    #     so Windows Kodi reports HDR10, HLG, HDR10+, and Dolby Vision in System Info -> Video
    win_sys_h = os.path.join(win_windowing_dir, "WinSystemWin32DX.h")
    with open(win_sys_h, "r", encoding="utf-8") as f:
        ws_h_txt = f.read()
    ws_h_txt = replace_required(
        ws_h_txt,
        "  bool IsHDRDisplay() override;",
        "  bool IsHDRDisplay() override;\n  CHDRCapabilities GetDisplayHDRCapabilities() const override;",
        "WinSystemWin32DX.h GetDisplayHDRCapabilities",
    )
    with open(win_sys_h, "w", encoding="utf-8", newline="\n") as f:
        f.write(ws_h_txt)

    win_sys_cpp = os.path.join(win_windowing_dir, "WinSystemWin32DX.cpp")
    with open(win_sys_cpp, "r", encoding="utf-8") as f:
        ws_cpp_txt = f.read()
    ws_cpp_txt = replace_required(
        ws_cpp_txt,
        '#include "WinSystemWin32DX.h"',
        '#include "WinSystemWin32DX.h"\n#include "DVBridgeStateWin.h"',
        "WinSystemWin32DX.cpp include DVBridgeStateWin.h",
    )
    ws_cpp_txt = replace_required(
        ws_cpp_txt,
        "bool CWinSystemWin32DX::IsHDRDisplay()\n{\n  return (CWIN32Util::GetWindowsHDRStatus() != HDR_STATUS::HDR_UNSUPPORTED);\n}",
        "bool CWinSystemWin32DX::IsHDRDisplay()\n"
        "{\n"
        "  auto& winDv = KODI::WINDOWING::WINDOWS::CDVBridgeStateWin::GetInstance();\n"
        "  return (CWIN32Util::GetWindowsHDRStatus() != HDR_STATUS::HDR_UNSUPPORTED) || winDv.HasHDRDisplay();\n"
        "}\n\n"
        "CHDRCapabilities CWinSystemWin32DX::GetDisplayHDRCapabilities() const\n"
        "{\n"
        "  CHDRCapabilities caps;\n"
        "  auto& winDv = KODI::WINDOWING::WINDOWS::CDVBridgeStateWin::GetInstance();\n"
        "  if ((CWIN32Util::GetWindowsHDRStatus() != HDR_STATUS::HDR_UNSUPPORTED) || winDv.SupportsHDR10() || winDv.SupportsDolbyVision())\n"
        "    caps.SetHDR10();\n"
        "  if (winDv.SupportsHLG())\n"
        "    caps.SetHLG();\n"
        "  if (winDv.SupportsHDR10Plus())\n"
        "    caps.SetHDR10Plus();\n"
        "  if (winDv.SupportsDolbyVision())\n"
        "    caps.SetDolbyVision();\n"
        "  return caps;\n"
        "}",
        "WinSystemWin32DX.cpp IsHDRDisplay & GetDisplayHDRCapabilities",
    )
    with open(win_sys_cpp, "w", encoding="utf-8", newline="\n") as f:
        f.write(ws_cpp_txt)
    print("Updated WinSystemWin32DX.h/.cpp with EDID-backed GetDisplayHDRCapabilities() (HDR10/HLG/HDR10+/Dolby Vision)")

    # 5c. Enable CB1 Dolby Vision UI, Settings, Playback Mode Selector, and Enhanced Process Info OSD on Windows
    dvbridge_win_files = [
        "xbmc/cores/VideoPlayer/DVDCodecs/Video/DVOutputSettings.h",
        "xbmc/cores/VideoPlayer/DVDCodecs/Video/DVOutputSelection.h",
        "xbmc/cores/VideoPlayer/DVDCodecs/Video/DVPlaybackNotification.h",
        "xbmc/dialogs/DVPlaybackModes.h",
        "xbmc/dialogs/DVPlaybackModes.cpp",
        "xbmc/dialogs/GUIDialogPlayerProcessInfo.h",
        "xbmc/dialogs/GUIDialogPlayerProcessInfo.cpp",
        "xbmc/dialogs/GUIDialogSelect.h",
        "xbmc/dialogs/GUIDialogSelect.cpp",
        "xbmc/settings/SettingConditions.h",
        "xbmc/settings/SettingConditions.cpp",
        "xbmc/settings/Settings.cpp",
        "xbmc/settings/SubtitlesSettings.cpp",
        "xbmc/settings/windows/GUIControlSettings.cpp",
        "xbmc/video/PlayerController.cpp",
        "xbmc/video/dialogs/GUIDialogVideoSettings.cpp",
    ]
    for rel_path in dvbridge_win_files:
        full_path = os.path.join(kodi_dir, *rel_path.split("/"))
        with open(full_path, "r", encoding="utf-8") as f:
            txt = f.read()
        txt = replace_required(
            txt,
            "#ifdef HAVE_DVBRIDGE",
            "#if defined(HAVE_DVBRIDGE) || defined(TARGET_WINDOWS)",
            f"{rel_path} HAVE_DVBRIDGE",
        )
        with open(full_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(txt)

    dialogs_cmake = os.path.join(kodi_dir, "xbmc", "dialogs", "CMakeLists.txt")
    with open(dialogs_cmake, "r", encoding="utf-8") as f:
        dcmake_txt = f.read()
    dcmake_txt = replace_required(
        dcmake_txt,
        "if(ENABLE_DVBRIDGE)\n  list(APPEND SOURCES DVPlaybackModes.cpp)",
        "if(ENABLE_DVBRIDGE OR WIN32)\n  list(APPEND SOURCES DVPlaybackModes.cpp)",
        "xbmc/dialogs/CMakeLists.txt ENABLE_DVBRIDGE",
    )
    with open(dialogs_cmake, "w", encoding="utf-8", newline="\n") as f:
        f.write(dcmake_txt)

    setting_cond_cpp = os.path.join(kodi_dir, "xbmc", "settings", "SettingConditions.cpp")
    with open(setting_cond_cpp, "r", encoding="utf-8") as f:
        sc_txt = f.read()
    sc_txt = replace_required(
        sc_txt,
        '#include "windowing/gbm/WinSystemGbmGLESContext.h"',
        '#if defined(TARGET_WINDOWS)\n'
        '#include "windowing/windows/DVBridgeStateWin.h"\n'
        '#else\n'
        '#include "windowing/gbm/WinSystemGbmGLESContext.h"\n'
        '#endif',
        "SettingConditions.cpp include DVBridgeStateWin.h",
    )
    sc_txt = replace_required(
        sc_txt,
        "  auto* window=dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(CServiceBroker::GetWinSystem());\n"
        "  const auto available=window ? window->GetDVOutputCapabilities(nullptr, true) : DVBRIDGE::OutputCapabilities{};",
        "#if defined(TARGET_WINDOWS)\n"
        "  const auto available=KODI::WINDOWING::WINDOWS::CDVBridgeStateWin::GetInstance().GetDVOutputCapabilities(false);\n"
        "#else\n"
        "  auto* window=dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(CServiceBroker::GetWinSystem());\n"
        "  const auto available=window ? window->GetDVOutputCapabilities(nullptr, true) : DVBRIDGE::OutputCapabilities{};\n"
        "#endif",
        "SettingConditions.cpp ActiveDVOutputCapabilities",
    )
    with open(setting_cond_cpp, "w", encoding="utf-8", newline="\n") as f:
        f.write(sc_txt)

    vid_settings_cpp = os.path.join(
        kodi_dir, "xbmc", "video", "dialogs", "GUIDialogVideoSettings.cpp"
    )
    with open(vid_settings_cpp, "r", encoding="utf-8") as f:
        vs_txt = f.read()
    vs_txt = replace_required(
        vs_txt,
        'AddButton(groupSaveAsDefault,"dvbridge.playbackchoice",40855,SettingLevel::Basic);',
        'AddButton(groupSaveAsDefault,"dvbridge.playbackchoice",40863,SettingLevel::Basic);',
        "GUIDialogVideoSettings.cpp playbackchoice label 40863",
    )
    with open(vid_settings_cpp, "w", encoding="utf-8", newline="\n") as f:
        f.write(vs_txt)
    print("Enabled CB1 Dolby Vision UI, Settings, Playback Modes, and Enhanced Process Info OSD on Windows")

    # 6. Hook CDVBridgeStateWin + StreamTelemetry + SetVideoSourceSideData into RenderManager.cpp on Windows
    render_mgr = os.path.join(
        kodi_dir, "xbmc", "cores", "VideoPlayer", "VideoRenderers", "RenderManager.cpp"
    )
    with open(render_mgr, "r", encoding="utf-8") as f:
        rm_txt = f.read()

    # Add Windows includes at top of RenderManager.cpp
    rm_txt = replace_required(
        rm_txt,
        '#if defined(HAVE_GBM) && defined(HAS_GLES)\n#include "windowing/gbm/WinSystemGbmGLESContext.h"\n#endif',
        '#if defined(HAVE_GBM) && defined(HAS_GLES)\n#include "windowing/gbm/WinSystemGbmGLESContext.h"\n'
        '#elif defined(TARGET_WINDOWS)\n'
        '#include "cores/DataCacheCore.h"\n'
        '#include "cores/VideoSourceSideData.h"\n'
        '#include "cores/VideoPlayer/DVDCodecs/Video/DVOutputSettings.h"\n'
        '#include "cores/VideoPlayer/DVDCodecs/Video/DVOutputSelection.h"\n'
        '#include "cores/VideoPlayer/DVDCodecs/Video/DVStreamTelemetry.h"\n'
        '#include "windowing/windows/DVBridgeStateWin.h"\n'
        '#endif',
        "RenderManager.cpp includes",
    )

    # Hook Configure() to probe Windows GPU & Monitor EDID VSVDB
    rm_txt = replace_required(
        rm_txt,
        '#ifdef HAVE_DVBRIDGE\n    m_nativeDVRate = DVBRIDGE::WantsNativeOutput(&picture);\n#endif',
        '#ifdef HAVE_DVBRIDGE\n    m_nativeDVRate = DVBRIDGE::WantsNativeOutput(&picture);\n'
        '#elif defined(TARGET_WINDOWS)\n'
        '    KODI::WINDOWING::WINDOWS::CDVBridgeStateWin::GetInstance().EnsureHardwareProbed();\n'
        '#endif',
        "RenderManager.cpp Configure()",
    )

    # Hook Render() to publish VideoSourceSideData on Windows
    rm_txt = replace_required(
        rm_txt,
        '#if defined(HAVE_GBM) && defined(HAS_GLES)\n'
        '    if (auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(\n'
        '            CServiceBroker::GetWinSystem()))\n'
        '      window->StageVideoSource(m_pRenderer->HasRenderedFrame() ?\n'
        '          m_Queue[m_presentsource].sourceSideData : nullptr);\n'
        '#endif',
        '#if defined(HAVE_GBM) && defined(HAS_GLES)\n'
        '    if (auto* window = dynamic_cast<KODI::WINDOWING::GBM::CWinSystemGbmGLESContext*>(\n'
        '            CServiceBroker::GetWinSystem()))\n'
        '      window->StageVideoSource(m_pRenderer->HasRenderedFrame() ?\n'
        '          m_Queue[m_presentsource].sourceSideData : nullptr);\n'
        '#elif defined(TARGET_WINDOWS)\n'
        '    if (m_pRenderer->HasRenderedFrame())\n'
        '      if (const auto& source = m_Queue[m_presentsource].sourceSideData)\n'
        '        CServiceBroker::GetDataCacheCore().SetVideoSourceSideData(source->Generation(), source);\n'
        '#endif',
        "RenderManager.cpp Render()",
    )

    # Hook AddVideoPicture() to pack ETSI GS CCM 001 V1.1.1 metadata and publish StreamTelemetry on Windows
    rm_txt = replace_required(
        rm_txt,
        '  SPresent& m = m_Queue[index];\n  m.sourceSideData = picture.sourceSideData;',
        '  SPresent& m = m_Queue[index];\n  m.sourceSideData = picture.sourceSideData;\n'
        '#if defined(TARGET_WINDOWS)\n'
        '  DVBRIDGE::StreamTelemetry::SourceBits(picture.colorBits, picture.pixelFormat, picture.doviSourceInfo);\n'
        '  if (picture.doviMetadata.Data())\n'
        '  {\n'
        '    auto& winDv = KODI::WINDOWING::WINDOWS::CDVBridgeStateWin::GetInstance();\n'
        '    winDv.ProcessDoviMetadata(picture.doviMetadata.Data(), picture.doviMetadata.Size(), picture.pts);\n'
        '    const auto prefs = DVBRIDGE::FrameOutputSettings();\n'
        '    auto caps = winDv.GetDVOutputCapabilities(true);\n'
        '    const auto selection = DVBRIDGE::SelectOutput(prefs, caps);\n'
        '    const int requested = DVBRIDGE::PolicyCode(prefs.Policy());\n'
        '    const int effective = DVBRIDGE::PolicyCode(selection.policy);\n'
        '    const DVBRIDGE::SourceBinding binding{picture.doviSourceInfo.informationGeneration,\n'
        '                                          picture.doviMetadata.Binding().picture,\n'
        '                                          prefs.revision};\n'
        '    DVBRIDGE::StreamTelemetry::NativeSelection(binding, caps, requested,\n'
        '                                               effective, selection.reason);\n'
        '    DVBRIDGE::StreamSnapshot::Output out{};\n'
        '    if (selection.route != DVBRIDGE::OutputRoute::Vanilla)\n'
        '    {\n'
        '      const bool isHdmiDv = (selection.route == DVBRIDGE::OutputRoute::NativeDV);\n'
        '      out.flags = (isHdmiDv ? CDVPlaybackInfo::HDMI :\n'
        '                   effective == 7 ? (CDVPlaybackInfo::HDR10 | CDVPlaybackInfo::HDR10PLUS) :\n'
        '                   CDVPlaybackInfo::HDR10) |\n'
        '                  (picture.strDVELType == "FEL" ? CDVPlaybackInfo::FEL : 0);\n'
        '      out.bits = isHdmiDv ? 12 : 10;\n'
        '      out.format = isHdmiDv ? 0x34325258u : 0x30335258u;\n'
        '      out.target = prefs.peak > 0 ? static_cast<float>(prefs.peak) : 1000.0f;\n'
        '      out.manualPeak = prefs.peak;\n'
        '      out.displayBlack = prefs.expertBlack;\n'
        '      out.panel = prefs.panel;\n'
        '      out.gamut = prefs.gamut;\n'
        '      out.method = static_cast<int>(prefs.method);\n'
        '      out.requested = requested;\n'
        '      out.effective = effective;\n'
        '      out.reason = selection.reason;\n'
        '      out.fallback = (selection.route == DVBRIDGE::OutputRoute::HDR10 &&\n'
        '                      prefs.output != DVBRIDGE::OutputMode::HDR10);\n'
        '      out.revision = prefs.revision;\n'
        '      out.width = picture.iWidth;\n'
        '      out.height = picture.iHeight;\n'
        '      out.stream = binding.stream;\n'
        '      out.picture = binding.picture;\n'
        '    }\n'
        '    DVBRIDGE::StreamTelemetry::NativePresented(picture.doviSourceInfo, picture.doviMetadata, out);\n'
        '    DVBRIDGE::StreamTelemetry::PublishNativeInfo(picture.doviSourceInfo.informationGeneration);\n'
        '  }\n'
        '#endif',
        "RenderManager.cpp AddVideoPicture()",
    )

    with open(render_mgr, "w", encoding="utf-8", newline="\n") as f:
        f.write(rm_txt)
    print("Hooked CDVBridgeStateWin + StreamTelemetry + SetVideoSourceSideData into RenderManager.cpp")

    # 7. Overlay universal Intel/AMD/NVIDIA DV + FEL patch files and package inpoutx64.dll
    kodi_patch_dir = os.path.join(le_repo_dir, "win-dv", "kodi-patch")
    if os.path.isdir(kodi_patch_dir):
        shutil.copytree(kodi_patch_dir, kodi_dir, dirs_exist_ok=True)
        print("Applied universal Intel/AMD/NVIDIA DV + FEL overlay from win-dv/kodi-patch/")

    inpout_src = os.path.join(le_repo_dir, "win-dv", "inpoutx64.dll")
    inpout_dst = os.path.join(kodi_dir, "project", "Win32BuildSetup", "inpoutx64.dll")
    if os.path.isfile(inpout_src):
        os.makedirs(os.path.dirname(inpout_dst), exist_ok=True)
        shutil.copy2(inpout_src, inpout_dst)
        print("Copied inpoutx64.dll into project/Win32BuildSetup/")

    build_setup_bat = os.path.join(kodi_dir, "tools", "buildsteps", "windows", "BuildSetup.bat")
    if os.path.exists(build_setup_bat):
        with open(build_setup_bat, "r", encoding="utf-8", newline="") as f:
            bat_txt = f.read()
        eol = "\r\n" if "\r\n" in bat_txt else "\n"
        target_line = "md BUILD_WIN32\\application"
        inject_line = (
            "md BUILD_WIN32\\application"
            + eol
            + "  IF EXIST inpoutx64.dll copy /Y inpoutx64.dll BUILD_WIN32\\application\\inpoutx64.dll > NUL"
        )
        if "inpoutx64.dll" not in bat_txt and target_line in bat_txt:
            bat_txt = bat_txt.replace(target_line, inject_line, 1)
            with open(build_setup_bat, "w", encoding="utf-8", newline="") as f:
                f.write(bat_txt)
            print("Patched BuildSetup.bat to bundle inpoutx64.dll into BUILD_WIN32\\application")

    print("=== Integration Completed Successfully! ===")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <le-repo-dir> <kodi-src-dir>")
        sys.exit(1)
    integrate(sys.argv[1], sys.argv[2])
