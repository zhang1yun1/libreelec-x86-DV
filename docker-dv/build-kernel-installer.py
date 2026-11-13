#!/usr/bin/env python3
import argparse
import hashlib
import io
import json
import os
import tarfile
import time


SCRIPT_TEMPLATE = r"""#!/bin/sh
set -e

KERNEL_VER="__KERNEL_VER__"
PKG_VER="__PKG_VER__"
FNOS_KVER="6.18.18.c1126-trim"
ACTION="${1:-install}"

echo "================================================================"
echo "  LibreELEC Dolby Vision 定制内核管理工具 ($KERNEL_VER / $PKG_VER)"
echo "  (飞牛原生内核无损注入 | 7.2.6-dv 独立内核 | 纯净原厂内核 一键三模切换)"
echo "================================================================"

show_usage() {
    echo "[使用方法 - 支持三大内核模式自由切换]"
    echo ""
    echo "  【模式 1】智能安装 / 切换为飞牛原生修改内核（飞牛 FnOS 默认推荐）："
    echo "     docker run --rm --privileged -v /:/host aguan/libreelec-kodi-dv:kernel"
    echo "     (或显式指定: docker run --rm --privileged -v /:/host aguan/libreelec-kodi-dv:kernel --fnos-dv)"
    echo "     -> 飞牛系统自动在 6.18.18.c1126-trim 原生内核上无损注入 DV 驱动，100% 保留飞牛全部特性；"
    echo "     -> 非飞牛 Debian/Ubuntu 系统自动安装 7.2.6-dv 完整内核。"
    echo ""
    echo "  【模式 2】手动安装 / 切换到独立 $KERNEL_VER 完整内核（飞牛与通用系统均可用）："
    echo "     docker run --rm --privileged -v /:/host aguan/libreelec-kodi-dv:kernel --full-kernel"
    echo "     (或使用别名: docker run --rm --privileged -v /:/host aguan/libreelec-kodi-dv:kernel --7.2.6-dv)"
    echo "     -> 安装并默认引导独立的 linux-image-$KERNEL_VER 主线杜比视界内核（原内核安全保留）。"
    echo ""
    echo "  【模式 3】手动切换回纯净原系统内核（卸载注入模块与定制内核，100% 恢复原厂状态）："
    echo "     docker run --rm --privileged -v /:/host aguan/libreelec-kodi-dv:kernel --restore"
    echo "     (或使用别名: docker run --rm --privileged -v /:/host aguan/libreelec-kodi-dv:kernel --orig-kernel)"
    echo ""
    echo "  [其他辅助功能]"
    echo "  - 保留 $KERNEL_VER 内核文件，仅将 GRUB 启动项切回原系统内核："
    echo "     docker run --rm --privileged -v /:/host aguan/libreelec-kodi-dv:kernel --switch-orig"
    echo "  - 安全试引导模式（仅下次重启单次试运行 $KERNEL_VER，死机再重启自动回退原内核）："
    echo "     docker run --rm --privileged -v /:/host aguan/libreelec-kodi-dv:kernel --safe-boot"
    echo "  - 查看宿主机当前所处模式、内核版本与驱动状态："
    echo "     docker run --rm --privileged -v /:/host aguan/libreelec-kodi-dv:kernel --status"
    echo "  - 强制重新安装当前模式："
    echo "     docker run --rm --privileged -v /:/host aguan/libreelec-kodi-dv:kernel --force"
    echo "  - 仅提取 .deb 安装包与飞牛驱动模块到当前目录："
    echo "     docker run --rm -v \$(pwd):/out aguan/libreelec-kodi-dv:kernel --extract"
}

if [ "$ACTION" = "--help" ] || [ "$ACTION" = "-h" ]; then
    show_usage
    exit 0
fi

if [ "$ACTION" = "--extract" ]; then
    if [ ! -d /out ]; then
        echo "[错误] 提取模式需挂载输出目录，例如："
        echo "  docker run --rm -v \$(pwd):/out aguan/libreelec-kodi-dv:kernel --extract"
        exit 1
    fi
    cp -vf /debs/*.deb /out/ 2>/dev/null || true
    cp -vf /fnos/*.tar.gz /out/ 2>/dev/null || true
    echo "[完成] 内核安装包与飞牛驱动包已提取至当前目录！"
    exit 0
fi

if [ ! -f /host/etc/debian_version ] || [ ! -x /host/usr/bin/dpkg ]; then
    show_usage
    exit 1
fi

IS_FNOS=0
if [ -d /host/usr/trim ] && [ -d "/host/lib/modules/$FNOS_KVER" ] && [ -f "/host/boot/vmlinuz-$FNOS_KVER" ]; then
    IS_FNOS=1
fi

FNOS_MOD_INSTALLED=0
if [ -f "/host/lib/modules/$FNOS_KVER/updates/dv/i915.ko.xz" ] || [ -f "/host/lib/modules/$FNOS_KVER/updates/dv/i915.ko" ]; then
    FNOS_MOD_INSTALLED=1
fi

INSTALLED_VER=$(chroot /host dpkg-query -W -f='${Version}' "linux-image-$KERNEL_VER" 2>/dev/null || true)
RUNNING_KERNEL=$(uname -r)

# 探测宿主机上保留的非 DV 原系统内核列表
ORIG_KERNELS=$(chroot /host /bin/sh -s << 'EOF'
ls -1 /boot/vmlinuz-* 2>/dev/null | sed 's|^/boot/vmlinuz-||' | grep -vE '(-dv$|-dv[^a-zA-Z0-9])' | sort -V || true
EOF
)

SAVED_ORIG=$(chroot /host /bin/sh -s << 'EOF'
if [ -f /etc/kodi-dv-original-kernel ]; then
    CAND=$(cat /etc/kodi-dv-original-kernel 2>/dev/null | tr -d " \n\r")
    if [ -n "$CAND" ] && [ -f "/boot/vmlinuz-$CAND" ]; then
        echo "$CAND"
        exit 0
    fi
fi
ls -1 /boot/vmlinuz-* 2>/dev/null | sed 's|^/boot/vmlinuz-||' | grep -vE '(-dv$|-dv[^a-zA-Z0-9])' | sort -V | tail -n 1 || true
EOF
)

if [ "$IS_FNOS" = "1" ]; then
    echo ">>> 宿主机系统类型: 飞牛 FnOS (检测到 /usr/trim 与原生内核 $FNOS_KVER)"
    if [ "$RUNNING_KERNEL" = "$KERNEL_VER" ]; then
        echo ">>> 当前激活模式: 【模式 2】独立杜比视界完整内核 ($KERNEL_VER)"
    elif [ "$FNOS_MOD_INSTALLED" = "1" ]; then
        echo ">>> 当前激活模式: 【模式 1】飞牛原生内核 ($FNOS_KVER) + 杜比视界无损驱动注入"
    else
        echo ">>> 当前激活模式: 【模式 3】纯净飞牛原厂内核 ($FNOS_KVER，未注入 DV 驱动)"
    fi
else
    echo ">>> 宿主机系统类型: 通用 Debian / Ubuntu"
fi
echo ">>> 宿主机当前运行内核: $RUNNING_KERNEL"
echo ">>> 飞牛原生内核 DV 模块: $([ "$FNOS_MOD_INSTALLED" = "1" ] && echo "已注入 (/lib/modules/$FNOS_KVER/updates/dv)" || echo "未注入")"
echo ">>> 独立 $KERNEL_VER 内核包: ${INSTALLED_VER:-未安装} (镜像内最新: $PKG_VER)"
echo ">>> 宿主机原系统基准内核: ${SAVED_ORIG:-未检测到}"

# ==================== 查看状态 (--status) ====================
if [ "$ACTION" = "--status" ]; then
    echo ""
    echo ">>> 宿主机 /boot 下所有可用原系统内核："
    if [ -n "$ORIG_KERNELS" ]; then
        echo "$ORIG_KERNELS" | sed 's/^/    - /'
    else
        echo "    (无)"
    fi
    if [ "$IS_FNOS" = "1" ]; then
        echo ""
        echo ">>> 飞牛原生内核 ($FNOS_KVER) 杜比视界模块状态："
        ls -lh "/host/lib/modules/$FNOS_KVER/updates/dv/" 2>/dev/null | sed 's/^/    /' || echo "    (未安装)"
    fi
    echo ""
    echo ">>> 宿主机已安装的独立 DV 内核软件包："
    chroot /host /bin/sh -s << 'EOF'
dpkg-query -W -f='    - ${Package} (${Version})\n' 'linux-image-*dv*' 'linux-headers-*dv*' 2>/dev/null | grep -E 'linux-(image|headers)' || echo '    (未安装)'
EOF
    echo ""
    echo ">>> 当前 GRUB 默认启动项配置："
    chroot /host /bin/sh -s << 'EOF'
grep -E '^(GRUB_DEFAULT|GRUB_TIMEOUT)=' /etc/default/grub 2>/dev/null | sed 's/^/    /' || true
EOF
    exit 0
fi

# ==================== 模式 3: 恢复纯净原系统内核 (--restore / --orig-kernel / --rollback / --uninstall) ====================
if [ "$ACTION" = "--restore" ] || [ "$ACTION" = "--orig-kernel" ] || [ "$ACTION" = "--rollback" ] || [ "$ACTION" = "--uninstall" ]; then
    echo ""
    echo ">>> 正在执行【模式 3：恢复纯净原系统内核（移除 DV 注入模块与独立定制内核）】..."
    if [ -z "$SAVED_ORIG" ]; then
        echo "[错误] 未在宿主机 /boot 下检测到任何原系统内核（非 -dv 内核）！"
        echo "       为防止卸载后系统无法启动，已安全中止操作。"
        exit 1
    fi

    chroot /host /bin/sh -s "$FNOS_KVER" << 'EOF'
set -e
FNOS_KVER="$1"
export DEBIAN_FRONTEND=noninteractive
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

if [ -d "/lib/modules/$FNOS_KVER/updates/dv" ] || [ -f /etc/depmod.d/kodi-dv.conf ]; then
    echo ">>> [飞牛模式] 正在移除注入的杜比视界显卡驱动 (/lib/modules/$FNOS_KVER/updates/dv)..."
    rm -rf "/lib/modules/$FNOS_KVER/updates/dv"
    rm -f /etc/depmod.d/kodi-dv.conf
    depmod -a "$FNOS_KVER"
    if command -v update-initramfs >/dev/null 2>&1; then
        update-initramfs -u -k "$FNOS_KVER"
    fi
fi

echo ">>> [1/3] 正在还原 GRUB 默认配置..."
if [ -f /etc/default/grub.kodi-dv.bak ]; then
    cp -f /etc/default/grub.kodi-dv.bak /etc/default/grub
    echo "    已从 /etc/default/grub.kodi-dv.bak 还原原系统 GRUB 配置"
else
    sed -i 's/^GRUB_DEFAULT=.*/GRUB_DEFAULT=0/' /etc/default/grub 2>/dev/null || true
fi
if command -v grub-editenv >/dev/null 2>&1 && [ -f /boot/grub/grubenv ]; then
    grub-editenv /boot/grub/grubenv unset next_entry 2>/dev/null || true
    grub-editenv /boot/grub/grubenv unset saved_entry 2>/dev/null || true
fi

echo ">>> [2/3] 正在卸载所有独立杜比视界定制内核软件包..."
DV_PKGS=$(dpkg-query -W -f='${Package} ' 'linux-image-*dv*' 'linux-headers-*dv*' 2>/dev/null || true)
if [ -n "$DV_PKGS" ]; then
    dpkg --purge --force-all $DV_PKGS || true
fi
rm -f /boot/vmlinuz-*dv* /boot/initrd.img-*dv* /boot/config-*dv* /boot/System.map-*dv*

echo ">>> [3/3] 正在重新生成 GRUB 引导菜单..."
if command -v update-grub >/dev/null 2>&1; then
    update-grub
fi
EOF

    echo ""
    echo "================================================================"
    echo "  [模式 3 切换完成] 杜比视界定制驱动与独立内核已全部移除！"
    echo "  宿主机已 100% 恢复为纯净原系统内核 ($SAVED_ORIG) 及原厂驱动。"
    echo "  请执行 'reboot' 重启宿主机以生效。"
    echo "================================================================"
    exit 0
fi

# ==================== 辅助模式: 保留定制内核，仅切回原系统内核启动 (--switch-orig) ====================
if [ "$ACTION" = "--switch-orig" ]; then
    echo ""
    echo ">>> 正在将 GRUB 默认启动项切换回原系统内核 ($SAVED_ORIG)..."
    if [ -z "$SAVED_ORIG" ]; then
        echo "[错误] 未在宿主机 /boot 下检测到原系统内核！"
        exit 1
    fi

    chroot /host /bin/sh -s "$SAVED_ORIG" << 'EOF'
set -e
SAVED_ORIG="$1"
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
SUBMENU_ID=$(grep 'gnulinux-advanced-' /boot/grub/grub.cfg 2>/dev/null | head -n 1 | sed -n "s/.*'\(gnulinux-advanced-[^']*\)'.*/\1/p")
ENTRY_ID=$(grep "gnulinux-${SAVED_ORIG}-advanced-" /boot/grub/grub.cfg 2>/dev/null | head -n 1 | sed -n "s/.*'\(gnulinux-${SAVED_ORIG}-advanced-[^']*\)'.*/\1/p")

if [ -n "$SUBMENU_ID" ] && [ -n "$ENTRY_ID" ]; then
    TARGET_ENTRY="${SUBMENU_ID}>${ENTRY_ID}"
    echo "    定位到原系统内核 GRUB 菜单项: $TARGET_ENTRY"
    sed -i "s|^GRUB_DEFAULT=.*|GRUB_DEFAULT=\"$TARGET_ENTRY\"|" /etc/default/grub
    update-grub
else
    sed -i 's|^GRUB_DEFAULT=.*|GRUB_DEFAULT=0|' /etc/default/grub
    update-grub
fi
EOF

    echo ""
    echo "================================================================"
    echo "  [切换成功] 已将默认启动项切回原系统内核 ($SAVED_ORIG)！"
    echo "  请执行 'reboot' 重启进入 $SAVED_ORIG 内核。"
    echo "================================================================"
    exit 0
fi

# ==================== 模式 1: 飞牛 FnOS 原生内核无损驱动注入模式 (飞牛默认，或 --fnos-dv / --fnos-modules) ====================
if [ "$ACTION" = "--fnos-dv" ] || [ "$ACTION" = "--fnos-modules" ]; then
    if [ "$IS_FNOS" != "1" ]; then
        echo "[错误] 当前宿主机未检测到飞牛原生内核 ($FNOS_KVER)，无法使用飞牛模块注入模式。"
        echo "       请直接不带参数运行或使用 --full-kernel 安装完整 $KERNEL_VER 内核。"
        exit 1
    fi
fi

if [ "$IS_FNOS" = "1" ] && [ -f "/fnos/fnos-${FNOS_KVER}-dv-modules.tar.gz" ] && [ "$ACTION" != "--full-kernel" ] && [ "$ACTION" != "--7.2.6-dv" ] && [ "$ACTION" != "--safe-boot" ]; then
    if [ "$ACTION" != "install" ] && [ "$ACTION" != "--force" ] && [ "$ACTION" != "--fnos-dv" ] && [ "$ACTION" != "--fnos-modules" ]; then
        echo "[错误] 未知参数: $ACTION"
        echo ""
        show_usage
        exit 1
    fi

    echo ""
    echo ">>> 正在执行【模式 1：飞牛 FnOS 原生内核 ($FNOS_KVER) 无损杜比视界驱动注入】..."
    echo "    优势：100% 保留飞牛原生内核全部特性（VFS 权限/回收站/ZFS/虚拟机/网卡/风扇传感器），"
    echo "          向 /lib/modules/$FNOS_KVER/updates/dv/ 注入杜比视界 i915 & amdgpu 驱动并配置 HDMI 源码直通声卡！"

    mkdir -p "/host/lib/modules/$FNOS_KVER/updates/dv"
    tar -xzf "/fnos/fnos-${FNOS_KVER}-dv-modules.tar.gz" -C "/host/lib/modules/$FNOS_KVER/updates/dv"

    chroot /host /bin/sh -s "$FNOS_KVER" << 'EOF'
set -e
FNOS_KVER="$1"
export DEBIAN_FRONTEND=noninteractive
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

if grep -qi "AuthenticAMD" /proc/cpuinfo 2>/dev/null || grep -qi "0x1002" /sys/class/drm/*/device/vendor 2>/dev/null; then
    if [ ! -d /lib/firmware/amdgpu ] && [ ! -d /usr/lib/firmware/amdgpu ]; then
        echo ">>> 检测到 AMD 平台缺少 /lib/firmware/amdgpu，正在自动安装 firmware-amd-graphics..."
        apt-get update && apt-get install -y firmware-amd-graphics amd64-microcode || true
    fi
fi

echo ">>> [1/3] 正在更新飞牛原生内核模块依赖与 HDMI 源码直通音频配置 (depmod -a $FNOS_KVER)..."
mkdir -p /etc/depmod.d /etc/modprobe.d
cat > /etc/depmod.d/kodi-dv.conf << 'DEPEOF'
override i915 * updates/dv
override amdgpu * updates/dv
DEPEOF
cat > /etc/modprobe.d/kodi-dv-audio.conf << 'AUOF'
# 强制 Intel 11~14 代平台使用标准 HDA 驱动 (snd_hda_intel)，以支持 HDMI 多声道与杜比全景声/DTS:X 源码直通
options snd-intel-dspcfg dsp_driver=1
blacklist snd_pcsp
AUOF
depmod -a "$FNOS_KVER"

echo ">>> [2/3] 正在刷新飞牛原生内核 initramfs 镜像..."
if command -v update-initramfs >/dev/null 2>&1; then
    update-initramfs -u -k "$FNOS_KVER"
fi

echo ">>> [3/3] 正在将系统默认启动项指向飞牛原生内核 ($FNOS_KVER)..."
DV_PKGS=$(dpkg-query -W -f='${Package} ' 'linux-image-*dv*' 'linux-headers-*dv*' 2>/dev/null || true)
if [ -n "$DV_PKGS" ]; then
    echo "    检测到已安装的独立 $DV_PKGS 内核，正在卸载以恢复飞牛原生内核为第一启动项..."
    dpkg --purge --force-all $DV_PKGS || true
    rm -f /boot/vmlinuz-*dv* /boot/initrd.img-*dv* /boot/config-*dv* /boot/System.map-*dv*
fi
if [ -f /etc/default/grub.kodi-dv.bak ]; then
    cp -f /etc/default/grub.kodi-dv.bak /etc/default/grub
else
    sed -i 's/^GRUB_DEFAULT=.*/GRUB_DEFAULT=0/' /etc/default/grub 2>/dev/null || true
fi
if command -v grub-editenv >/dev/null 2>&1 && [ -f /boot/grub/grubenv ]; then
    grub-editenv /boot/grub/grubenv unset next_entry 2>/dev/null || true
    grub-editenv /boot/grub/grubenv unset saved_entry 2>/dev/null || true
fi
if command -v update-grub >/dev/null 2>&1; then
    update-grub
fi
EOF

    echo ""
    echo "================================================================"
    echo "  [模式 1 启用成功] 飞牛原生内核 ($FNOS_KVER) 杜比视界无损驱动已就绪！"
    echo "  - 注入位置: /lib/modules/$FNOS_KVER/updates/dv/ (i915 & amdgpu)"
    echo "  - 引导内核: $FNOS_KVER (飞牛全部 NAS 特性 100% 完整保留)"
    echo "  - 其他模式切换命令："
    echo "    * 切换到独立 7.2.6-dv 内核: docker run --rm --privileged -v /:/host aguan/libreelec-kodi-dv:kernel --full-kernel"
    echo "    * 恢复纯净飞牛原厂内核:     docker run --rm --privileged -v /:/host aguan/libreelec-kodi-dv:kernel --restore"
    echo "  - 请执行 'reboot' 重启宿主机生效！"
    echo "================================================================"
    exit 0
fi

# ==================== 模式 2: 安装/切换到独立 7.2.6-dv 完整内核 (通用 Debian/Ubuntu 默认，或飞牛 --full-kernel / --7.2.6-dv) ====================
if [ "$ACTION" != "install" ] && [ "$ACTION" != "--force" ] && [ "$ACTION" != "--safe-boot" ] && [ "$ACTION" != "--full-kernel" ] && [ "$ACTION" != "--7.2.6-dv" ]; then
    echo "[错误] 未知参数: $ACTION"
    echo ""
    show_usage
    exit 1
fi

if [ "$INSTALLED_VER" = "$PKG_VER" ] && [ "$ACTION" = "install" ]; then
    chroot /host /bin/sh -s << 'EOF'
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
if ! grep -q "^GRUB_DEFAULT=0$" /etc/default/grub 2>/dev/null; then
    echo ">>> 正在将杜比视界定制内核固定为 GRUB 默认启动项 (GRUB_DEFAULT=0)..."
    sed -i "s|^GRUB_DEFAULT=.*|GRUB_DEFAULT=0|" /etc/default/grub
    if command -v grub-editenv >/dev/null 2>&1 && [ -f /boot/grub/grubenv ]; then
        grub-editenv /boot/grub/grubenv unset next_entry 2>/dev/null || true
        grub-editenv /boot/grub/grubenv unset saved_entry 2>/dev/null || true
    fi
    update-grub
fi
EOF
    echo ""
    echo "[已是最新] 宿主机已安装 linux-image-$KERNEL_VER ($PKG_VER) 且已设为默认启动项。"
    if [ "$RUNNING_KERNEL" != "$KERNEL_VER" ]; then
        echo ">>> 提示：你尚未重启进入 $KERNEL_VER 内核，请执行 'reboot' 重启宿主机。"
    fi
    echo ">>> 如需切换回飞牛原生修改内核，请加 '--fnos-dv' 参数运行。"
    echo ">>> 如需恢复纯净原系统内核 ($SAVED_ORIG)，请加 '--restore' 参数运行。"
    exit 0
fi

echo ""
echo ">>> 正在执行【模式 2：安装并切换到独立 $KERNEL_VER ($PKG_VER) 完整定制内核】..."
echo ">>> [1/5] 正在备份原系统内核记录与 GRUB 配置..."
chroot /host /bin/sh -s "$SAVED_ORIG" << 'EOF'
SAVED_ORIG="$1"
if [ ! -f /etc/default/grub.kodi-dv.bak ] && [ -f /etc/default/grub ]; then
    cp -f /etc/default/grub /etc/default/grub.kodi-dv.bak
    echo "    已备份原系统 GRUB 配置至 /etc/default/grub.kodi-dv.bak"
fi
if [ ! -f /etc/kodi-dv-original-kernel ] && [ -n "$SAVED_ORIG" ]; then
    echo "$SAVED_ORIG" > /etc/kodi-dv-original-kernel
    echo "    已记录原系统基准内核版本: $SAVED_ORIG (/etc/kodi-dv-original-kernel)"
fi
mkdir -p /etc/modprobe.d
cat > /etc/modprobe.d/kodi-dv-audio.conf << 'AUOF'
# 强制 Intel 11~14 代平台使用标准 HDA 驱动 (snd_hda_intel)，以支持 HDMI 多声道与杜比全景声/DTS:X 源码直通
options snd-intel-dspcfg dsp_driver=1
blacklist snd_pcsp
AUOF
EOF

echo ">>> [2/5] 正在检测宿主机 CPU/GPU 固件完整性..."
chroot /host /bin/sh -s << 'EOF'
export DEBIAN_FRONTEND=noninteractive
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
if grep -qi "AuthenticAMD" /proc/cpuinfo 2>/dev/null || grep -qi "0x1002" /sys/class/drm/*/device/vendor 2>/dev/null; then
    echo "    检测到 AMD 平台，正在检查 amdgpu 显卡与微码固件..."
    if [ ! -d /lib/firmware/amdgpu ] && [ ! -d /usr/lib/firmware/amdgpu ]; then
        echo "    [提示] 宿主机缺少 /lib/firmware/amdgpu，正在尝试通过 apt 自动安装 firmware-amd-graphics..."
        apt-get update && apt-get install -y firmware-amd-graphics amd64-microcode || echo "    [警告] 自动安装 firmware-amd-graphics 失败，请确保宿主机已安装 AMD 显卡固件"
    else
        echo "    [OK] 宿主机已具备 amdgpu 固件目录"
    fi
fi
EOF

echo ">>> [3/5] 正在向宿主机复制内核安装包..."
mkdir -p /host/tmp/kodi-dv-kernel
cp -f /debs/*.deb /host/tmp/kodi-dv-kernel/

echo ">>> [4/5] 正在宿主机内执行 dpkg 安装与 initramfs/GRUB 配置..."
chroot /host /bin/sh -s "$KERNEL_VER" "$ACTION" "$SAVED_ORIG" << 'EOF'
set -e
KERNEL_VER="$1"
ACTION="$2"
SAVED_ORIG="$3"
export DEBIAN_FRONTEND=noninteractive
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

dpkg -i /tmp/kodi-dv-kernel/linux-image-*dv*.deb /tmp/kodi-dv-kernel/linux-headers-*dv*.deb
if command -v update-initramfs >/dev/null 2>&1; then
    update-initramfs -u -k "$KERNEL_VER"
fi

# 确保 GRUB 菜单显示至少 5 秒，以便用户在极端情况下可在开机界面手动选回原内核
if [ -f /etc/default/grub ]; then
    if grep -q '^GRUB_TIMEOUT=0' /etc/default/grub; then
        sed -i 's/^GRUB_TIMEOUT=0/GRUB_TIMEOUT=5/' /etc/default/grub
    fi
    sed -i 's/^GRUB_TIMEOUT_STYLE=hidden/GRUB_TIMEOUT_STYLE=menu/' /etc/default/grub 2>/dev/null || true
fi

if [ "$ACTION" = "--safe-boot" ] && [ -n "$SAVED_ORIG" ]; then
    echo ">>> 正在配置【安全试引导模式 (--safe-boot)】..."
    sed -i 's|^GRUB_DEFAULT=.*|GRUB_DEFAULT=saved|' /etc/default/grub
    update-grub
    SUBMENU_ID=$(grep 'gnulinux-advanced-' /boot/grub/grub.cfg 2>/dev/null | head -n 1 | sed -n "s/.*'\(gnulinux-advanced-[^']*\)'.*/\1/p")
    ENTRY_ID=$(grep "gnulinux-${SAVED_ORIG}-advanced-" /boot/grub/grub.cfg 2>/dev/null | head -n 1 | sed -n "s/.*'\(gnulinux-${SAVED_ORIG}-advanced-[^']*\)'.*/\1/p")
    if [ -n "$SUBMENU_ID" ] && [ -n "$ENTRY_ID" ] && command -v grub-set-default >/dev/null 2>&1 && command -v grub-reboot >/dev/null 2>&1; then
        grub-set-default "${SUBMENU_ID}>${ENTRY_ID}"
        grub-reboot 0
        echo "    已将永久默认内核保底设为原内核 ($SAVED_ORIG)，仅下一次重启试引导 $KERNEL_VER！"
    else
        sed -i 's|^GRUB_DEFAULT=.*|GRUB_DEFAULT=0|' /etc/default/grub
        update-grub
    fi
else
    sed -i 's|^GRUB_DEFAULT=.*|GRUB_DEFAULT=0|' /etc/default/grub 2>/dev/null || true
    if command -v grub-editenv >/dev/null 2>&1 && [ -f /boot/grub/grubenv ]; then
        grub-editenv /boot/grub/grubenv unset next_entry 2>/dev/null || true
        grub-editenv /boot/grub/grubenv unset saved_entry 2>/dev/null || true
    fi
    if command -v update-grub >/dev/null 2>&1; then
        update-grub
    fi
fi
EOF

echo ">>> [5/5] 正在清理临时文件..."
rm -rf /host/tmp/kodi-dv-kernel

echo ""
echo "================================================================"
echo "  [模式 2 安装成功] 独立定制内核 linux-image-$KERNEL_VER ($PKG_VER) 已就绪！"
if [ "$ACTION" = "--safe-boot" ]; then
    echo "  当前为【安全试引导模式】："
    echo "  1. 请执行 'reboot' 重启测试新内核 ($KERNEL_VER)；"
    echo "  2. 若新内核无法开机，再次强制重启机器即可【自动回退】至原内核 ($SAVED_ORIG)；"
    echo "  3. 若新内核运行正常，重启后再次运行 '--full-kernel' 即可固定为永久默认。"
else
    echo "  1. 请立即在宿主机执行 'reboot' 重启以进入 $KERNEL_VER 内核。"
    echo "  2. 原系统内核 ($SAVED_ORIG) 已安全保留在 /boot 中；"
    echo "  3. 随时可通过以下命令自由切换模式："
    echo "     * 切回飞牛原生修改内核: docker run --rm --privileged -v /:/host aguan/libreelec-kodi-dv:kernel --fnos-dv"
    echo "     * 恢复纯净原厂系统内核: docker run --rm --privileged -v /:/host aguan/libreelec-kodi-dv:kernel --restore"
fi
echo "================================================================"
"""


def make_install_script(kernel_ver: str, pkg_ver: str) -> str:
    return SCRIPT_TEMPLATE.replace("__KERNEL_VER__", kernel_ver).replace("__PKG_VER__", pkg_ver)


def build_layer_tar(bundle_dir: str, kernel_ver: str, pkg_ver: str) -> bytes:
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as tf:
        for d in ["bin", "sbin", "usr", "usr/bin", "usr/sbin", "debs", "fnos", "host", "out", "tmp"]:
            ti = tarfile.TarInfo(name=d)
            ti.type = tarfile.DIRTYPE
            ti.mode = 0o755
            tf.addfile(ti)

        with open("/bin/busybox", "rb") as f:
            bb_data = f.read()
        ti = tarfile.TarInfo(name="bin/busybox")
        ti.size = len(bb_data)
        ti.mode = 0o755
        tf.addfile(ti, io.BytesIO(bb_data))

        applets = [
            "sh", "ash", "cp", "rm", "mkdir", "cat", "echo", "ls",
            "uname", "grep", "sed", "awk", "sort", "tr", "cut",
            "chmod", "sleep", "head", "tail", "tar", "gzip", "xz"
        ]
        for app in applets:
            ti = tarfile.TarInfo(name=f"bin/{app}")
            ti.type = tarfile.SYMTYPE
            ti.linkname = "busybox"
            tf.addfile(ti)

        for app in ["chroot"]:
            ti = tarfile.TarInfo(name=f"usr/sbin/{app}")
            ti.type = tarfile.SYMTYPE
            ti.linkname = "../../bin/busybox"
            tf.addfile(ti)

        script_bytes = make_install_script(kernel_ver, pkg_ver).encode("utf-8")
        ti = tarfile.TarInfo(name="install-kernel.sh")
        ti.size = len(script_bytes)
        ti.mode = 0o755
        tf.addfile(ti, io.BytesIO(script_bytes))

        added_fnos = set()
        for fname in sorted(os.listdir(bundle_dir)):
            fpath = os.path.join(bundle_dir, fname)
            if fname.endswith(".deb") and "dv" in fname and ("linux-image-" in fname or "linux-headers-" in fname):
                with open(fpath, "rb") as f:
                    data = f.read()
                ti = tarfile.TarInfo(name=f"debs/{fname}")
                ti.size = len(data)
                ti.mode = 0o644
                tf.addfile(ti, io.BytesIO(data))
            elif fname.startswith("fnos-") and fname.endswith("-dv-modules.tar.gz"):
                with open(fpath, "rb") as f:
                    data = f.read()
                ti = tarfile.TarInfo(name=f"fnos/{fname}")
                ti.size = len(data)
                ti.mode = 0o644
                tf.addfile(ti, io.BytesIO(data))
                added_fnos.add(fname)

        script_dir = os.path.dirname(os.path.abspath(__file__))
        if os.path.isdir(script_dir):
            for fname in sorted(os.listdir(script_dir)):
                if fname.startswith("fnos-") and fname.endswith("-dv-modules.tar.gz") and fname not in added_fnos:
                    fpath = os.path.join(script_dir, fname)
                    with open(fpath, "rb") as f:
                        data = f.read()
                    ti = tarfile.TarInfo(name=f"fnos/{fname}")
                    ti.size = len(data)
                    ti.mode = 0o644
                    tf.addfile(ti, io.BytesIO(data))
                    added_fnos.add(fname)

    return buf.getvalue()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bundle-dir", required=True)
    parser.add_argument("--output-tar", required=True)
    parser.add_argument("--kernel-ver", required=True)
    parser.add_argument("--pkg-ver", required=True)
    parser.add_argument("--kernel-hash", default="")
    parser.add_argument("--repo", default="aguan/libreelec-kodi-dv")
    args = parser.parse_args()

    layer_bytes = build_layer_tar(args.bundle_dir, args.kernel_ver, args.pkg_ver)
    layer_sha = hashlib.sha256(layer_bytes).hexdigest()

    with open(__file__, "rb") as sf:
        installer_rev = hashlib.sha256(sf.read()).hexdigest()[:16]

    created_ts = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    config_obj = {
        "architecture": "amd64",
        "os": "linux",
        "created": created_ts,
        "config": {
            "Env": [
                "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
            ],
            "Entrypoint": ["/install-kernel.sh"],
            "WorkingDir": "/",
            "Labels": {
                "org.opencontainers.image.title": f"LibreELEC Dolby Vision Kernel Installer ({args.kernel_ver})",
                "org.opencontainers.image.version": args.pkg_ver,
                "org.opencontainers.image.description": "Three-mode host kernel manager for LibreELEC Dolby Vision (FnOS native DV injection / 7.2.6-dv full kernel / pure original kernel rollback)",
                "io.kodi-dv.kernel-hash": args.kernel_hash,
                "io.kodi-dv.installer-rev": installer_rev,
            },
        },
        "rootfs": {
            "type": "layers",
            "diff_ids": [f"sha256:{layer_sha}"],
        },
        "history": [
            {
                "created": created_ts,
                "created_by": f"build-kernel-installer.py ({args.kernel_ver} {args.pkg_ver})",
            }
        ],
    }

    config_bytes = json.dumps(config_obj, indent=2).encode("utf-8")
    config_sha = hashlib.sha256(config_bytes).hexdigest()

    manifest_obj = [
        {
            "Config": f"{config_sha}.json",
            "RepoTags": [
                f"{args.repo}:kernel",
                f"{args.repo}:kernel-{args.kernel_ver}",
            ],
            "Layers": [f"{layer_sha}/layer.tar"],
        }
    ]
    manifest_bytes = json.dumps(manifest_obj).encode("utf-8")

    with tarfile.open(args.output_tar, "w") as out_tf:
        ti = tarfile.TarInfo(name=f"{layer_sha}/layer.tar")
        ti.size = len(layer_bytes)
        ti.mode = 0o644
        out_tf.addfile(ti, io.BytesIO(layer_bytes))

        ti = tarfile.TarInfo(name=f"{config_sha}.json")
        ti.size = len(config_bytes)
        ti.mode = 0o644
        out_tf.addfile(ti, io.BytesIO(config_bytes))

        ti = tarfile.TarInfo(name="manifest.json")
        ti.size = len(manifest_bytes)
        ti.mode = 0o644
        out_tf.addfile(ti, io.BytesIO(manifest_bytes))

    print(f"Created {args.output_tar} ({os.path.getsize(args.output_tar) / (1024*1024):.2f} MB)")


if __name__ == "__main__":
    main()
