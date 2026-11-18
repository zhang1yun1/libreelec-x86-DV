# LibreELEC x86_64 Dolby Vision (Kodi 22 RC1 + CB1 0.5) Docker 镜像

基于 [CroqueMr/libreelec-x86-DV](https://github.com/CroqueMr/libreelec-x86-DV) 构建的容器化 LibreELEC 用户态与定制内核/驱动注入镜像，专为 **飞牛 FnOS** 及 **Debian / Ubuntu** 等 x86_64 NAS 与 HTPC 宿主系统设计。无需重装宿主系统，即可通过 Docker 容器实现 **GBM 无桌面独占显示直通 + VAAPI 硬件解码 + ALSA 杜比全景声/DTS-X 源码直通 + HDMI 杜比视界（Dolby Vision）硬件桥接（DV Bridge）**。

---

## 镜像标签（Tags）说明

| 标签 | 体积 | 说明 |
| :--- | :--- | :--- |
| `aguan/libreelec-kodi-dv:latest` | ~775 MB | LibreELEC 13 用户态镜像（自动跟随上游最新 Release 更新，内置 Intel/AMD 核显自动识别） |
| `aguan/libreelec-kodi-dv:kodi22-cb1-0.5` | ~775 MB | 同 `latest`（固定版本归档标签） |
| `aguan/libreelec-kodi-dv:kernel` | ~50 MB | **宿主机杜比视界驱动/内核一键三模切换工具**（支持 **飞牛原生修改内核** / **`7.2.6-dv` 独立完整内核** / **纯净原系统内核** 自由切换，兼容 Intel 与 AMD 平台） |
| `aguan/libreelec-kodi-dv:kernel-7.2.6-dv` | ~50 MB | 同 `kernel`（固定内核版本归档标签） |

---

## 第一步：宿主机杜比视界内核配置（支持三大模式自由切换）

由于 Docker 容器共享宿主机内核驱动，若需开启完整的 **HDMI 杜比视界硬件隧道直通（TV-Led / Player-Led DV Bridge）与 HDMI 全景声源码直通**，请先拉取内核管理工具：

```bash
docker pull aguan/libreelec-kodi-dv:kernel
```

随后可根据你的系统与偏好，在以下**三种内核模式**之间随时一键切换（切换后执行 `reboot` 重启宿主机生效）：

### 【模式 1】飞牛 FnOS 原生修改内核模式（飞牛系统默认推荐）
在飞牛 FnOS 上直接运行默认命令（或显式加 `--fnos-dv` 参数）：
```bash
docker run --rm --privileged -v /:/host aguan/libreelec-kodi-dv:kernel
# 或显式指定（从 7.2.6-dv 切回飞牛原生修改内核时同样适用）：
docker run --rm --privileged -v /:/host aguan/libreelec-kodi-dv:kernel --fnos-dv
```
- **原理与优势**：**不替换飞牛官方内核**！在飞牛原生 `6.18.18.c1126-trim` 内核之上，向 `/lib/modules/6.18.18.c1126-trim/updates/dv/` 无损注入打好杜比视界补丁的 `i915`（Intel）与 `amdgpu`（AMD）显卡驱动，并自动配置 Intel HDA HDMI 源码直通声卡。
- **100% 完整保留飞牛全部原生特性**：飞牛专属 VFS 文件权限（`trim_acl`）、内核级回收站（`trim_trashbin`）、ZFS 存储池、KVM/VFIO 虚拟机硬件直通、FullCone NAT（`nft_fullcone`）、全型号无线/万兆网卡与主板风扇温控传感器均不受任何影响。
- *(注：若在非飞牛的普通 Debian / Ubuntu 系统上运行上述默认命令，工具会自动识别并安装下方的 `7.2.6-dv` 完整内核。)*

---

### 【模式 2】独立 `7.2.6-dv` 完整内核模式（飞牛手动切换 / Debian & Ubuntu 默认）
如果你希望在飞牛系统上体验最新主线 `7.2.6-dv` 独立内核（或者从模式 1 切换到 `7.2.6-dv`），只需加上 `--full-kernel`（或别名 `--7.2.6-dv`）：
```bash
docker run --rm --privileged -v /:/host aguan/libreelec-kodi-dv:kernel --full-kernel
# 或使用直观别名：
docker run --rm --privileged -v /:/host aguan/libreelec-kodi-dv:kernel --7.2.6-dv
```
- **原理与优势**：安装并默认引导独立的 `linux-image-7.2.6-dv` 完整内核（已集成 KVM/VFIO、Bcache、主板温控芯片 `nct6775`/`it87`、万兆网卡、HBA 阵列卡等 NAS 驱动），同时安全保留飞牛/原系统内核在 `/boot` 中不删除。
- **安全试引导模式（`--safe-boot`）**：若担心新内核不兼容，可使用 `--safe-boot` 仅在下一次重启单次试运行 `7.2.6-dv`，若死机强制重启一次即自动回退原内核：
  ```bash
  docker run --rm --privileged -v /:/host aguan/libreelec-kodi-dv:kernel --safe-boot
  ```

---

### 【模式 3】纯净原系统内核模式（一键卸载还原原厂状态）
无论当前处于模式 1 还是模式 2，随时执行以下命令即可移除所有杜比视界注入模块与独立定制内核，**100% 恢复纯净原厂内核与原厂驱动**：
```bash
docker run --rm --privileged -v /:/host aguan/libreelec-kodi-dv:kernel --restore
# 或使用直观别名：
docker run --rm --privileged -v /:/host aguan/libreelec-kodi-dv:kernel --orig-kernel
```

---

### 其他实用查询与辅助命令
- **查看宿主机当前所处模式、内核与驱动注入状态（`--status`）**：
  ```bash
  docker run --rm --privileged -v /:/host aguan/libreelec-kodi-dv:kernel --status
  ```
- **保留定制内核不卸载，仅将 GRUB 启动项切回原内核（`--switch-orig`）**：
  ```bash
  docker run --rm --privileged -v /:/host aguan/libreelec-kodi-dv:kernel --switch-orig
  ```
- **仅提取 `.deb` 安装包与飞牛驱动模块压缩包到当前目录（`--extract`）**：
  ```bash
  docker run --rm -v $(pwd):/out aguan/libreelec-kodi-dv:kernel --extract
  ```

---

## 第二步：启动 Kodi-DV 容器（Docker Compose）

在宿主机新建目录（例如 `/opt/kodi-dv`），创建 `docker-compose.yml`：

```yaml
services:
  kodi-dv:
    image: aguan/libreelec-kodi-dv:latest
    container_name: kodi-dv
    restart: unless-stopped
    privileged: true
    network_mode: host
    ipc: host
    environment:
      # 默认 auto 会自动识别 Intel (iHD) 与 AMD (radeonsi) 核显
      # 如为 Intel 7代及以前核显，可手动改为 i965
      - LIBVA_DRIVER_NAME=auto
    volumes:
      # 硬件直通（显卡 DRM、声卡 ALSA、输入设备与热插拔监听）
      - /dev:/dev
      - /run/udev:/run/udev:ro
      - /sys:/sys:ro
      # 持久化保存 Kodi 配置、插件与数据库到宿主机空目录即可
      - ./kodi_storage:/storage
      # 按需挂载宿主机本地电影目录（只读保护）
      - /mnt:/var/media/mnt:ro
      - /media:/var/media/media:ro
```

启动容器：

```bash
docker compose up -d
```

> **提示**：若宿主机安装了图形桌面（GDM/LightDM/SDDM），需先释放 DRM 显卡锁：
> ```bash
> systemctl disable --now gdm3 lightdm sddm
> systemctl set-default multi-user.target
> ```

---

## 常用维护命令

```bash
# 查看 Kodi 实时运行日志
tail -f ./kodi_storage/.kodi/temp/kodi.log

# 验证杜比视界内核属性识别状态（应输出 dv_property=present）
grep -i "DVBridge" ./kodi_storage/.kodi/temp/kodi.log

# 停止容器（立即释放电视 HDMI 与声卡独占）
docker compose down
```
