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
        raise RuntimeError(f"Expected pattern not found during {label}:\n{old}")
    return text.replace(old, new)


def integrate(le_repo_dir: str, kodi_dir: str) -> None:
    le_repo_dir = os.path.abspath(le_repo_dir)
    kodi_dir = os.path.abspath(kodi_dir)

    print(f"=== Integrating CB1 + DVBridge Windows Port into {kodi_dir} ===")

    # 1. Populate C:\kodi-src\cb1\src with CB1 common sources + Windows headers
    cb1_dst = os.path.join(kodi_dir, "cb1", "src")
    os.makedirs(cb1_dst, exist_ok=True)

    cb1_src = os.path.join(le_repo_dir, "packages", "mediacenter", "kodi", "cb1", "src")
    for name in os.listdir(cb1_src):
        src_path = os.path.join(cb1_src, name)
        if os.path.isfile(src_path):
            shutil.copy2(src_path, os.path.join(cb1_dst, name))

    win_inc = os.path.join(le_repo_dir, "win-dv", "include")
    for name in os.listdir(win_inc):
        src_path = os.path.join(win_inc, name)
        if os.path.isfile(src_path):
            shutil.copy2(src_path, os.path.join(cb1_dst, name))

    # 2. Apply FFmpeg 9.0.2 CB1 + 3D MVC patches to tools/depends/target/ffmpeg/
    ffmpeg_patch_src = os.path.join(le_repo_dir, "projects", "Generic", "patches", "ffmpeg")
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
            with open(buildhelpers, "w", encoding="utf-8", newline="\n") as f:
                f.write(bh_txt)
            print("Updated tools/buildsteps/windows/buildhelpers.sh to reuse CB1-patched FFmpeg build")

    # 3. Apply Kodi 22 CB1 patches (kodi-9990..9994)
    kodi_patch_dir = os.path.join(le_repo_dir, "projects", "Generic", "patches", "kodi")
    kodi_patches = [
        "kodi-9990-cb1-video.patch",
        "kodi-9991-cb1-audio.patch",
        "kodi-9992-cb1-display.patch",
        "kodi-9993-cb1-interface.patch",
        "kodi-9994-cb1-integration.patch",
    ]
    for patch_name in kodi_patches:
        patch_path = os.path.join(kodi_patch_dir, patch_name)
        run_cmd(["git", "apply", "--whitespace=nowarn", patch_path], cwd=kodi_dir)
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
        '    DVBRIDGE::StreamSnapshot::Output out{};\n'
        '    const bool hasVsvdb = winDv.ActiveSink().dvCaps.status == DV_EDID_STANDARD;\n'
        '    out.flags = (hasVsvdb ? CDVPlaybackInfo::HDMI : CDVPlaybackInfo::HDR10) |\n'
        '                (picture.strDVELType == "FEL" ? CDVPlaybackInfo::FEL : 0);\n'
        '    out.bits = hasVsvdb ? 12 : 10;\n'
        '    out.target = 1000.0f;\n'
        '    out.effective = hasVsvdb ? 1 : 5;\n'
        '    out.width = picture.iWidth;\n'
        '    out.height = picture.iHeight;\n'
        '    out.stream = picture.doviSourceInfo.informationGeneration;\n'
        '    DVBRIDGE::StreamTelemetry::NativePresented(picture.doviSourceInfo, picture.doviMetadata, out);\n'
        '    DVBRIDGE::StreamTelemetry::PublishNativeInfo(picture.doviSourceInfo.informationGeneration);\n'
        '  }\n'
        '#endif',
        "RenderManager.cpp AddVideoPicture()",
    )

    with open(render_mgr, "w", encoding="utf-8", newline="\n") as f:
        f.write(rm_txt)
    print("Hooked CDVBridgeStateWin + StreamTelemetry + SetVideoSourceSideData into RenderManager.cpp")
    print("=== Integration Completed Successfully! ===")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <le-repo-dir> <kodi-src-dir>")
        sys.exit(1)
    integrate(sys.argv[1], sys.argv[2])
