# clover-windows-uefi

为 **小米平板 4**（`clover`，骁龙 660 / SDM660）编译的、带**可用 GOP 显示驱动**的 Windows UEFI 固件。

把 [UniXNU clover](https://github.com/qine233/uefi-clover) 项目的 `CloverGopDxe` 移植进
[edk2-msm](https://github.com/edk2-porting/edk2-msm) 的 `sdm660` 目标，让固件在 ABL 留下的
`0x9D400000` framebuffer 上发布 `EFI_GRAPHICS_OUTPUT_PROTOCOL`（GOP）。

---

## 当前状态（能做什么 / 不能做什么）

### ✅ 已经能做到

| 能力 | 说明 |
|------|------|
| **屏幕显示画面** | GOP 显示驱动生效，能进 SimpleInit 菜单、UEFI Shell，有完整图形输出 |
| **SD 卡槽驱动** | `CloverSdDxe` 自研驱动，在 `sdhci@c084000`（SD 卡槽）发布 `EFI_BLOCK_IO_PROTOCOL`，UEFI 能读 SD 卡 |
| **编译出可刷机的 boot 镜像** | `boot-clover.img`，通过 GitHub Actions 云端编译 |
| **可复现** | 任何人有这台平板 + 一个 GitHub 账号，就能 fork 本仓库一键编译出同样的固件 |

### ❌ 还没做到（剩余问题）

| 缺失项 | 原因 | 解决难度 |
|--------|------|----------|
| **USB 主机驱动**（读 U 盘/键盘） | sdm660 没有 `XhciDxe`；sdm845 的 `XhciDxe` 是**闭源二进制**（90KB .efi 无源码），且 sdm660 的 UEFI 内存映射里**没有 USB 控制器 MMIO 映射** | 极高（需从 kernel 4.4 挖 DWC3 寄存器 + 自己写 xHCI 驱动） |
| **触摸驱动** | edk2-msm sdm660 无触摸驱动；触摸屏是 FTS（i2c-3 地址 0x38）或 GT9xx（0x5d） | 高 |
| **eMMC 被 Windows 识别** | Windows 侧需 `ACPI\QCOM24BF` 驱动，高通 WoA BSP 不覆盖 SDM660，公开渠道无此驱动 | 死路（5年未解决） |
| **GPU 加速** | Adreno 512 无 Windows/苹果驱动 | 死路 |

> 结论：**「能显示、能进 UEFI、能读 SD 卡」已经做到了**；「跑 Windows PE / 完整系统」卡在 USB 主机驱动 + 输入设备 + eMMC 驱动，前两个是可行但工程量大的方向，eMMC 是死路。

---

## 原理

`CloverGopDxe` 是一个**自包含的 DXE 驱动**，只依赖 `MdePkg`。它**不**重新初始化 MDSS/DSI 或面板——而是**复用 bootloader（ABL）已经留在 `0x9D400000` 的 framebuffer**（1200x1920，像素格式 PixelBlueGreenRedReserved8BitPerColor）。它实现了固定单模式的 `QueryMode`/`SetMode`/`Blt`，加载时把面板刷绿作为 liveness 信号。

关键常量（与这台机器实测一致）：

```
Framebuffer base : 0x9D400000
Panel            : 1200 x 1920（NT51021 + BOE WUXGA）
Pixel format     : BGR8（byte0=Blue）
```

---

## 使用教程（完整流程）

### 前置条件

1. 一台 **Windows 电脑**（装好 `fastboot`）
2. 平板 **已解锁 Bootloader**（`fastboot getvar unlocked` 返回 `yes`）
3. 一个 GitHub 账号

### 第 1 步：编译（GitHub Actions 云端）

1. **Fork 本仓库**（或直接用你自己的）
2. GitHub 网页 → 仓库 → **Actions** 标签 → 选 `build-clover-windows-uefi` → **Run workflow**
3. 等编译完成（约 5-10 分钟）
4. 下载 artifact `clover-windows-uefi`，解压得到 `boot-clover.img`

> 也可以本地编译：clone edk2-msm → `bash patch-gop.sh <edk2-msm路径> <本仓库路径>` → `cd edk2-msm && ./build.sh --device clover`。
> **注意**：不要用 `--skip-rootfs-gen`，否则 `SimpleInitRootFS.inf` 会报 `build/rootfs.c not found`。

### 第 2 步：备份原厂 boot（重要，刷前必做）

平板开机进 Android，用 root 备份：

```sh
adb shell su -c "dd if=/dev/block/by-name/boot of=/data/local/tmp/stock-boot.img"
adb pull /data/local/tmp/stock-boot.img
```

### 第 3 步：刷机

```sh
# 进 fastboot
adb reboot bootloader        # 或关机后 音量- + 电源

# 刷入
fastboot flash boot boot-clover.img
fastboot reboot
```

### 第 4 步：回滚（随时可回 Android）

```sh
fastboot flash boot stock-boot.img
fastboot reboot
```

---

## 常见坑（踩坑记录）

### 1. fastboot 连不上（USB3 口 + `18D1:D00D`）

小米平板 4 的 fastboot 枚举为 `VID_18D1&PID_D00D`，在 **USB3 口**上会报「配置描述符请求失败」。

**解决**：写注册表补丁（管理员运行），让系统跳过 BOS 描述符查询：

```bat
reg add "HKLM\SYSTEM\CurrentControlSet\Control\usbflags\18D1D00D0100" /v "osvc" /t REG_BINARY /d "0000" /f
reg add "HKLM\SYSTEM\CurrentControlSet\Control\usbflags\18D1D00D0100" /v "SkipContainerIdQuery" /t REG_BINARY /d "01000000" /f
reg add "HKLM\SYSTEM\CurrentControlSet\Control\usbflags\18D1D00D0100" /v "SkipBOSDescriptorQuery" /t REG_BINARY /d "01000000" /f
```

驱动用 MiFlash 自带的 `android_winusb.inf`（含 `D00D` 条目 + 小米签名）。

### 2. 编译报 `rootfs.c not found`

见上，去掉 `--skip-rootfs-gen`。

### 3. `boot-clover.img` 没生成

`platform_build_bootimg` 需要 `Platform/Xiaomi/sdm660/FdtBlob_compat/clover.dtb` 和一个空 `ramdisk` 文件。`patch-gop.sh` 已自动放 dtb；ramdisk 用 `touch ramdisk` 创建。

### 4. SimpleInit 启动即崩（Translation fault / nanosvg）

**本地编译必须用 clang-18，不是默认 clang**。Ubuntu 26.04 默认 `clang` 是 clang-21，它会**错误编译 SimpleInit 的 nanosvg（SVG 解析）**，导致固件一进 SimpleInit 菜单就 `Data abort: Translation fault, zeroth level`（即使没加任何自定义驱动也崩）。

**解决**：

```sh
sudo apt install clang-18
sudo update-alternatives --install /usr/bin/clang clang /usr/bin/clang-18 100
sudo update-alternatives --set clang /usr/bin/clang-18
# 清空旧产物重新编译
rm -rf workspace/Build/clover && ./build.sh --device clover
```

> GitHub Actions 的 `ubuntu-24.04` 默认就是 clang-18，所以云端编译没这个问题。

---

## 文件说明

| 文件 | 用途 |
|------|------|
| `CloverGopDxe.c` / `.inf` | GOP 显示驱动（移植自 UniXNU clover） |
| `CloverSdDxe.c` / `.inf` | SD 卡槽 BlockIo 驱动（自研，驱动 sdhci@c084000） |
| `patch-gop.sh` | 把 GOP 驱动 + 设备树打进 edk2-msm sdm660 |
| `patch-sd.sh` | 把 SD 驱动打进 edk2-msm sdm660 |
| `build-local.sh` | WSL 本地一键编译脚本（含 clang-18 要求） |
| `.github/workflows/build-clover-win.yml` | CI 编译流程 |
| `clover.dtb` | 这台机器的设备树（从原厂 boot 镜像拆出） |

> `p-xbl.bin`（厂商 XBL blob）**不包含在仓库里**（版权 + 体积）。如需 `extract-clover-blobs.py` 步骤，从自己设备提取：
> `adb shell su -c "dd if=/dev/block/by-name/xbl of=/data/local/tmp/p-xbl.bin"` 后拉取，放到仓库根目录。

---

## 下一步方向（按可行性排序）

1. **验证从 SD 卡启动** —— `CloverSdDxe` 已发布 BlockIo，往 SD 卡放一个 ARM64 测试 `.efi`（`EFI\BOOT\BOOTAA64.EFI`）看 UEFI 能否加载；或直接在 SD 卡装 Ubuntu/Windows on ARM。
2. **USB 主机驱动（XhciDxe）** —— 最大的工程，需从 sdm660 kernel 4.4 挖 DWC3 寄存器写驱动。这是「跑 PE」的前置。
3. **触摸驱动** —— 已知是 FTS（0x38）/ GT9xx（0x5d），可参考 UniXNU 的 CloverInput。

---

## SD 卡驱动（CloverSdDxe）技术要点

`CloverSdDxe` 是自研的 SD 卡槽 BlockIo 驱动，目标硬件是 `sdhci@c084000`（SDCC2，SD 卡槽）。几个关键坑：

| 坑 | 现象 | 修复 |
|----|------|------|
| **GCC 时钟门控** | 读 SDCC2 寄存器触发 `Synchronous external abort`（FAR 0xC0840FE） | 先开 GCC SDCC2 AHB/APPS 门控（0x14008 / 0x14004）再访问 |
| **cache 一致性** | ADMA 引擎（DMA）绕过 CPU cache，读到过期数据 | 传输前后 `InvalidateDataCacheRange` / `WriteBackDataCacheRange` |
| **CSD 字节序** | SDHCI R2 响应是大端（Resp[0]=CSD[127:96]），容量算错 | CsdStructure 在 `Csd[0]>>30`，C_SIZE 从 Csd[1]/Csd[2] 正确提取 |
| **设备路径** | 自定义 Vendor 路径让 SimpleInit 崩溃 | 用标准 `SD_DEVICE_PATH`（MSG_SD_DP） |

SD 卡在 Android 里的设备节点是 `/dev/block/mmcblk1`（eMMC 是 `mmcblk0`，别搞错）。

---

## Credits

- `CloverGopDxe` —— [UniXNU / qine233](https://github.com/qine233/uefi-clover)（BSD-2-Clause-Patent）
- edk2-msm —— [edk2-porting](https://github.com/edk2-porting/edk2-msm)
