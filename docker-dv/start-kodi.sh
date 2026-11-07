#!/bin/sh
set -e

# 创建 LibreELEC 运行 Kodi 所需的必要运行时目录
mkdir -p /storage/.kodi/temp /storage/.kodi/userdata /storage/.cache /var/run /var/log /run/libreelec

# 初始化 LibreELEC SSL 根证书（修复容器内 HTTPS 插件源与刮削器请求）
if [ -f /etc/ssl/cacert.pem.system ]; then
    cp -f /etc/ssl/cacert.pem.system /run/libreelec/cacert.pem
fi

# 自动识别 Intel / AMD 核显并匹配正确的 VAAPI 驱动（兼容用户未修改 LIBVA_DRIVER_NAME 的情况）
HAS_INTEL=0
HAS_AMD=0
for vfile in /sys/class/drm/renderD*/device/vendor; do
    if [ -f "$vfile" ]; then
        vendor=$(cat "$vfile" 2>/dev/null || true)
        case "$vendor" in
            0x8086) HAS_INTEL=1 ;;
            0x1002) HAS_AMD=1 ;;
        esac
    fi
done

if [ -z "$LIBVA_DRIVER_NAME" ] || [ "$LIBVA_DRIVER_NAME" = "auto" ]; then
    if [ "$HAS_AMD" -eq 1 ] && [ "$HAS_INTEL" -eq 0 ]; then
        export LIBVA_DRIVER_NAME=radeonsi
    elif [ "$HAS_INTEL" -eq 1 ]; then
        export LIBVA_DRIVER_NAME=iHD
    fi
elif [ "$LIBVA_DRIVER_NAME" = "iHD" ] && [ "$HAS_AMD" -eq 1 ] && [ "$HAS_INTEL" -eq 0 ]; then
    echo "[start-kodi] 检测到纯 AMD 显卡环境 (0x1002)，自动将 LIBVA_DRIVER_NAME 从 iHD 切换为 radeonsi"
    export LIBVA_DRIVER_NAME=radeonsi
fi

# 清理可能残留的崩溃锁文件
rm -f /storage/.kodi/temp/*.lock

# 以 GBM 独立无桌面模式 + ALSA 源码直通后端启动 Kodi
exec /usr/lib/kodi/kodi.bin --standalone -fs --audio-backend=alsa "$@"
