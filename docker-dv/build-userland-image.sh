#!/usr/bin/env bash
set -euo pipefail

RELEASE_TAG="${1:?Usage: build-userland-image.sh <RELEASE_TAG> <ASSET_URL> <DOCKERHUB_REPO>}"
ASSET_URL="${2:?Missing ASSET_URL}"
DOCKERHUB_REPO="${3:?Missing DOCKERHUB_REPO}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK_DIR="$(mktemp -d /tmp/le-userland.XXXXXX)"
trap 'sudo rm -rf "$WORK_DIR"' EXIT

echo "=== [1/4] 正在下载上游发布包: ${RELEASE_TAG} ==="
echo "URL: ${ASSET_URL}"
if [[ "${ASSET_URL}" == *.tar ]]; then
    curl -fL --retry 5 -o "${WORK_DIR}/release.tar" "${ASSET_URL}"
    mkdir -p "${WORK_DIR}/extracted"
    tar -xf "${WORK_DIR}/release.tar" -C "${WORK_DIR}/extracted" --wildcards '*/target/SYSTEM'
    SYSTEM_FILE=$(find "${WORK_DIR}/extracted" -name "SYSTEM" -type f | head -n 1)
elif [[ "${ASSET_URL}" == *.img.gz ]]; then
    curl -fL --retry 5 -o "${WORK_DIR}/release.img.gz" "${ASSET_URL}"
    gunzip "${WORK_DIR}/release.img.gz"
    7z x "${WORK_DIR}/release.img" -o"${WORK_DIR}/img_parts" >/dev/null
    7z x "${WORK_DIR}/img_parts/0.system.fat" -o"${WORK_DIR}/fat_parts" >/dev/null
    SYSTEM_FILE="${WORK_DIR}/fat_parts/SYSTEM"
else
    echo "Unsupported asset format: ${ASSET_URL}"
    exit 1
fi

if [[ ! -f "${SYSTEM_FILE}" ]]; then
    echo "Failed to locate SYSTEM SquashFS file!"
    exit 1
fi

echo "=== [2/4] 正在解包 SquashFS 根文件系统 ==="
sudo unsquashfs -d "${WORK_DIR}/rootfs" "${SYSTEM_FILE}"
sudo install -m 0755 "${SCRIPT_DIR}/start-kodi.sh" "${WORK_DIR}/rootfs/usr/bin/start-kodi.sh"

echo "=== [3/4] 正在构建 Docker 镜像: ${DOCKERHUB_REPO}:${RELEASE_TAG} ==="
sudo tar -C "${WORK_DIR}/rootfs" -cf - . | docker import \
    --change 'ENV PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin' \
    --change 'ENV HOME=/storage' \
    --change 'ENV KODI_HOME=/usr/share/kodi' \
    --change 'ENV KODI_TEMP=/storage/.kodi/temp' \
    --change 'ENV LIBVA_DRIVER_NAME=auto' \
    --change 'WORKDIR /storage' \
    --change 'ENTRYPOINT ["/usr/bin/start-kodi.sh"]' \
    --change "LABEL org.opencontainers.image.title=\"LibreELEC x86_64 Dolby Vision (${RELEASE_TAG})\"" \
    --change "LABEL org.opencontainers.image.version=\"${RELEASE_TAG}\"" \
    --change "LABEL org.opencontainers.image.source=\"https://github.com/CroqueMr/libreelec-x86-DV\"" \
    - "${DOCKERHUB_REPO}:${RELEASE_TAG}"

docker tag "${DOCKERHUB_REPO}:${RELEASE_TAG}" "${DOCKERHUB_REPO}:latest"
docker tag "${DOCKERHUB_REPO}:${RELEASE_TAG}" "${DOCKERHUB_REPO}:kodi22-cb1-0.5"

echo "=== [4/4] 正在推送用户态镜像至 Docker Hub ==="
docker push "${DOCKERHUB_REPO}:${RELEASE_TAG}"
docker push "${DOCKERHUB_REPO}:latest"
docker push "${DOCKERHUB_REPO}:kodi22-cb1-0.5"
echo "=== 用户态镜像发布完成: ${DOCKERHUB_REPO}:${RELEASE_TAG} ==="
