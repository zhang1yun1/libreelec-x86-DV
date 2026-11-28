#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL_HASH="${1:-manual}"
DOCKERHUB_REPO="${2:-aguan/libreelec-kodi-dv}"

# 1. 从 packages/linux/package.mk 自动提取当前上游内核版本号 (例如 7.2.6)
KVER=$(awk '/\*\)/,/\;\;/' "${REPO_ROOT}/packages/linux/package.mk" | grep -E 'PKG_VERSION="[0-9]+\.[0-9]+' | head -n 1 | cut -d'"' -f2)
if [[ -z "${KVER}" ]]; then
    KVER="7.2.6"
fi
KMAJOR="${KVER%%.*}"
echo "=== 检测到上游 Linux 内核版本: ${KVER} (v${KMAJOR}.x) ==="

BUILD_ROOT="/tmp/kernel-build"
mkdir -p "${BUILD_ROOT}"
cd "${BUILD_ROOT}"

# 2. 下载并解压官方内核源码
if [[ ! -f "linux-${KVER}.tar.xz" ]]; then
    curl -fL --retry 5 -O "https://cdn.kernel.org/pub/linux/kernel/v${KMAJOR}.x/linux-${KVER}.tar.xz"
fi
rm -rf "linux-${KVER}"
tar -xf "linux-${KVER}.tar.xz"
cd "linux-${KVER}"

# 3. 依次打入 LibreELEC 通用补丁与 Generic 杜比视界/DRM/HDMI 专属补丁
echo "=== 正在打入 LibreELEC 杜比视界内核补丁 ==="
for pdir in \
    "${REPO_ROOT}/packages/linux/patches/default" \
    "${REPO_ROOT}/projects/Generic/patches/linux" \
    "${REPO_ROOT}/projects/Generic/linux/${KVER}"
do
    if [[ -d "${pdir}" ]]; then
        for p in $(find "${pdir}" -maxdepth 1 -name "*.patch" | sort); do
            echo "Applying patch: $(basename "$p")"
            patch -p1 --forward < "$p"
        done
    fi
done

# 4. 载入 LibreELEC x86_64 基准内核配置并融合 Debian/FnOS/Docker 与 Intel+AMD 完整驱动支持
if [[ -f "${REPO_ROOT}/projects/Generic/linux/${KVER}/linux.x86_64.conf" ]]; then
    cp "${REPO_ROOT}/projects/Generic/linux/${KVER}/linux.x86_64.conf" .config
elif [[ -f "${REPO_ROOT}/projects/Generic/linux/linux.x86_64.conf" ]]; then
    cp "${REPO_ROOT}/projects/Generic/linux/linux.x86_64.conf" .config
else
    echo "Cannot find linux.x86_64.conf!"
    exit 1
fi

./scripts/config \
  --set-str EXTRA_FIRMWARE "" \
  --disable SYSTEM_TRUSTED_KEYS \
  --disable SYSTEM_REVOCATION_KEYS \
  --disable MODULE_SIG \
  --disable DEBUG_INFO \
  --disable DEBUG_INFO_BTF \
  --enable BLK_DEV_INITRD \
  --enable RD_GZIP \
  --enable RD_BZIP2 \
  --enable RD_LZMA \
  --enable RD_XZ \
  --enable RD_LZO \
  --enable RD_LZ4 \
  --enable RD_ZSTD \
  --enable DEVTMPFS \
  --enable CGROUPS \
  --enable CGROUP_FREEZER \
  --enable CGROUP_PIDS \
  --enable CGROUP_DEVICE \
  --enable CGROUP_CPUACCT \
  --enable CGROUP_PERF \
  --enable CGROUP_BPF \
  --enable MEMCG \
  --enable NAMESPACES \
  --enable UTS_NS \
  --enable IPC_NS \
  --enable USER_NS \
  --enable PID_NS \
  --enable NET_NS \
  --enable OVERLAY_FS \
  --module BRIDGE \
  --enable BRIDGE_NETFILTER \
  --module VETH \
  --module VXLAN \
  --module IPVLAN \
  --module MACVLAN \
  --module OPENVSWITCH \
  --enable NETFILTER \
  --enable NETFILTER_ADVANCED \
  --module NF_CONNTRACK \
  --module NF_NAT \
  --module NF_TABLES \
  --module NFT_NAT \
  --module NFT_MASQ \
  --module NFT_COMPAT \
  --module IP_NF_IPTABLES \
  --module IP_NF_FILTER \
  --module IP_NF_NAT \
  --module IP_NF_MANGLE \
  --module NETFILTER_XT_MATCH_ADDRTYPE \
  --module NETFILTER_XT_MATCH_CONNTRACK \
  --module NETFILTER_XT_MARK \
  --enable MD \
  --module BLK_DEV_MD \
  --module MD_LINEAR \
  --module MD_RAID0 \
  --module MD_RAID1 \
  --module MD_RAID10 \
  --module MD_RAID456 \
  --module BLK_DEV_DM \
  --module DM_CRYPT \
  --module DM_SNAPSHOT \
  --module DM_THIN_PROVISIONING \
  --module DM_MIRROR \
  --module DM_RAID \
  --enable DM_UEVENT \
  --module DM_CACHE \
  --module DM_WRITECACHE \
  --module DM_ZERO \
  --module DM_MULTIPATH \
  --enable BTRFS_FS \
  --enable BTRFS_FS_POSIX_ACL \
  --enable EXT4_FS \
  --enable XFS_FS \
  --enable F2FS_FS \
  --enable VMD \
  --module DRM_AMDGPU \
  --enable DRM_AMDGPU_SI \
  --enable DRM_AMDGPU_CIK \
  --enable DRM_AMDGPU_USERPTR \
  --enable DRM_AMD_DC \
  --enable DRM_AMD_DC_FP \
  --enable DRM_AMD_DC_SI \
  --enable HSA_AMD \
  --enable HSA_AMD_SVM \
  --module DRM_RADEON \
  --module DRM_I915 \
  --module DRM_XE \
  --enable SYSFB_SIMPLEFB \
  --enable DRM_SIMPLEDRM \
  --enable CPU_SUP_AMD \
  --enable X86_AMD_PLATFORM_DEVICE \
  --enable X86_MCE \
  --enable X86_MCE_AMD \
  --enable X86_MCE_INTEL \
  --enable X86_MCE_THRESHOLD \
  --enable NUMA \
  --enable AMD_NUMA \
  --enable X86_MEM_ENCRYPT \
  --enable AMD_MEM_ENCRYPT \
  --enable X86_AMD_PSTATE \
  --enable AMD_IOMMU \
  --enable IRQ_REMAP \
  --enable PINCTRL_AMD \
  --module GPIO_AMDPT \
  --module GPIO_AMD_FCH \
  --enable CRYPTO_HW \
  --enable CRYPTO_DEV_CCP \
  --module CRYPTO_DEV_CCP_DD \
  --enable CRYPTO_DEV_SP_CCP \
  --module CRYPTO_DEV_CCP_CRYPTO \
  --enable CRYPTO_DEV_SP_PSP \
  --module TEE \
  --module AMDTEE \
  --module AMD_PMC \
  --module AMD_PMF \
  --module AMD_SFH_HID \
  --module AMD_HSMP_ACPI \
  --module AMD_HSMP_PLAT \
  --module AMD_3D_VCACHE \
  --enable AMD_WBRF \
  --module I2C_AMD756 \
  --module I2C_AMD8111 \
  --module I2C_AMD_MP2 \
  --module I2C_DESIGNWARE_PCI \
  --enable I2C_DESIGNWARE_PLATFORM \
  --enable SPI \
  --enable SPI_MASTER \
  --enable SPI_MEM \
  --module SPI_AMD \
  --enable VIRTUALIZATION \
  --module KVM \
  --module KVM_AMD \
  --enable KVM_AMD_SEV \
  --module KVM_INTEL \
  --enable EDAC \
  --module EDAC_AMD64 \
  --module SENSORS_K10TEMP \
  --module AMD_XGBE \
  --module SND_SOC_AMD_ACP \
  --module SND_SOC_AMD_ACP3x \
  --module SND_SOC_AMD_RENOIR \
  --module SND_SOC_AMD_ACP5x \
  --module SND_SOC_AMD_ACP6x \
  --module SND_SOC_AMD_ACP_COMMON \
  --module SND_SOC_AMD_ACP_PCI \
  --module SND_AMD_ASOC_RENOIR \
  --module SND_AMD_ASOC_REMBRANDT \
  --module SND_AMD_ASOC_ACP70 \
  --module SND_SOC_AMD_PS \
  --module SND_SOC_SOF_AMD_TOPLEVEL \
  --module SND_SOC_SOF_AMD_COMMON \
  --module SND_SOC_SOF_AMD_RENOIR \
  --module SND_SOC_SOF_AMD_REMBRANDT \
  --module VFIO \
  --module VFIO_PCI \
  --module VFIO_IOMMU_TYPE1 \
  --enable VFIO_VIRQFD \
  --module BCACHE \
  --module SENSORS_NCT6775 \
  --module SENSORS_NCT6683 \
  --module SENSORS_IT87 \
  --module SENSORS_F71882FG \
  --module SENSORS_W83627EHF \
  --module SENSORS_CORETEMP \
  --module SENSORS_DRIVETEMP \
  --module SENSORS_JC42 \
  --module SENSORS_LM75 \
  --module SCSI_MPT3SAS \
  --module MEGARAID_SAS \
  --module SCSI_AACRAID \
  --module I40E \
  --module ICE \
  --module IXGBE \
  --module IGC \
  --module E1000E \
  --module R8169 \
  --module ATLANTIC \
  --module MLX4_EN \
  --module MLX5_CORE \
  --enable MLX5_CORE_EN \
  --module BNXT \
  --module TUN \
  --module VHOST_NET \
  --module VHOST_VSOCK \
  --module VHOST_SCSI \
  --enable TCP_CONG_BBR \
  --module NTSYNC || true

make olddefconfig

# 5. 使用 ccache 并行编译 Debian 内核安装包
PKG_REV="$(date -u +%Y%m%d)"
make -j"$(nproc)" CC="ccache gcc" HOSTCC="ccache gcc" bindeb-pkg LOCALVERSION=-dv KDEB_PKGVERSION="${KVER}-${PKG_REV}"

mkdir -p "${BUILD_ROOT}/out"
mv "${BUILD_ROOT}"/linux-image-*dv*.deb "${BUILD_ROOT}"/linux-headers-*dv*.deb "${BUILD_ROOT}/out/"
cp -f "${REPO_ROOT}"/docker-dv/fnos-*-dv-modules.tar.gz "${BUILD_ROOT}/out/" 2>/dev/null || true
ls -lh "${BUILD_ROOT}/out/"

# 6. 构建并推送 :kernel 安装器镜像
python3 "${REPO_ROOT}/docker-dv/build-kernel-installer.py" \
  --bundle-dir "${BUILD_ROOT}/out" \
  --output-tar "${BUILD_ROOT}/libreelec-kodi-dv-kernel.tar" \
  --kernel-ver "${KVER}-dv" \
  --pkg-ver "${KVER}-${PKG_REV}" \
  --kernel-hash "${KERNEL_HASH}" \
  --repo "${DOCKERHUB_REPO}"

skopeo copy --retry-times 5 "docker-archive:${BUILD_ROOT}/libreelec-kodi-dv-kernel.tar" "docker://docker.io/${DOCKERHUB_REPO}:kernel"
skopeo copy --retry-times 5 "docker://docker.io/${DOCKERHUB_REPO}:kernel" "docker://docker.io/${DOCKERHUB_REPO}:kernel-${KVER}-dv"
