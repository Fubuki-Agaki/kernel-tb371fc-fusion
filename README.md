# TB371FC Fusion Kernel — 4.19.325-perf++

联想小新 Pad Pro 12.7 2021（**TB371FC**，高通 SM8250 / kona）的自定义内核。

本仓库（**fusion**，`Fubuki-Agaki/kernel-tb371fc-fusion`）是一个集成开发 fork：
以 [`smith-dog/kernel-tb371fc`](https://github.com/smith-dog/kernel-tb371fc) 的 **v2.2 谱系**为基线
（联想 GPL 公开源码 + linux-stable **v4.19.157 → v4.19.325** 完整采纳 + 全量 bring-up 补丁集），
合入 [`bampudding/linux_kernel_lenovo_tb371fc`](https://github.com/bampudding/linux_kernel_lenovo_tb371fc)
的 TB371FC 修复，并在此之上**新增 MGLRU / BBRv3 等上游回移与本机原创修复**。

> 基线功能（扬声器全链路、相机录像、指纹、充电保护、双击亮屏、OTG、VINTF、deep suspend、
> KernelSU-as-LKM 等）继承自上游 v2.2，详见下方「代码来源」。本 README 重点说明 fusion 的
> 新增内容与**全部代码出处**。最近一批改动逐条说明见 [CHANGES-20261117.md](CHANGES-20261117.md)。

---

## 功能 / Features

### 基线功能（继承 smith-dog v2.2）
- **音视频**：4×TFA9894 功放 + CLO 音频核心栈全内建（功放固件烤入内核，冷启动出声更快）、相机录像、venus 硬解码
- **充电**：充电保护（40–60% 电量保持）、电池养护开关、充电状态反馈死循环修复
- **外设**：指纹供电轨修复、WiFi/蓝牙、双击亮屏（DT2W）、USB-C OTG 自动主机模式
- **系统兼容**：VINTF 匹配（消除开机“设备内部出现问题”弹窗）、deep suspend
- **Root**：KernelSU（backslashxx fork，v3.3.1+）以 **LKM** 运行，与内核镜像解耦，升级无需重编内核
- **Docker**：iptables 全套 + xt_addrtype
- **零依赖**：WiFi / 音频 / rmnet / gspca 全部内建，`lsmod` 仅 KernelSU 一行

### Fusion 新增
| 功能 | 说明 | 提交 |
|---|---|---|
| **Multi-Gen LRU (MGLRU)** | 上游分代 LRU 老化/回收框架回移到 4.19，`CONFIG_LRU_GEN=y`（默认开启 + STATS），改善内存超分与回收效率 | `ae701d9ef34a` |
| **TCP BBRv3** | 上游 BBRv3 拥塞控制（`BBR_VERSION 3`）回移，设为默认拥塞控制（`CONFIG_DEFAULT_BBR=y`），含 4.19 兼容头与协议栈配套 | `c0a9b6cb5470` |
| **dm-verity 调优** | kverityd 校验工作队列改 `WQ_HIGHPRI`、去 `WQ_UNBOUND`，降低 arm64 big.LITTLE 上的校验调度延迟 | `d936a991331c` |
| **TFA9894 爆音修复** | 播放温重启（音量变化）爆音：① p269 同值 chan_sel 短路（避免 TDME 0→1 扰动）② 温重启仅保持 CFSM 软静音不切 AMPE；`mute_seq_fix` 参数门控（默认 1） | `c5eaca63c714` |
| **bampudding #6 修复** | GPU 内存 eBPF 记账、dma-buf  teardown 防卫、a6xx 上电寄存器越界检查、触摸 instant-wake + HAL resume 互锁、DSC PPS pic_width 幂等 | 6 个 cherry-pick |
| **vsock loopback** | 回移上游 5.9 loopback transport，使本机 `AF_VSOCK` 可用（无 VMCI/virtio/Hyper-V 总线时协议族此前不注册） | `8c4f94e16347` |

---

## 代码来源 / Provenance & Credits

本树是**派生作品**，绝大部分代码来自下列上游 / 社区项目， fusion 仅在其上做适配与少量原创修复。
按“来源 → 用途”列出，便于追溯与合规（GPL-2.0）。

### 基线 / 驱动来源（继承上游）
| 组成 | 来源 |
|---|---|
| 上游基线项目 | [`smith-dog/kernel-tb371fc`](https://github.com/smith-dog/kernel-tb371fc)（origin，v2.2 谱系；p1~p371 bring-up 补丁见 `tb371fc/scripts/`） |
| 内核基线 | [`lss4/android_kernel_lenovo_paladin`](https://github.com/lss4/android_kernel_lenovo_paladin)（分支 11）——社区整理的联想官方 GPL 包（paladin TB-Q706F/Z，4.19.157，与 TB371FC stock 同版本，含联想板级代码） |
| linux-stable 采纳 | [`gregkh/linux`](https://github.com/gregkh/linux) 分支 `linux-4.19.y`：**v4.19.157 → v4.19.325**（168 个 stable 发布）整体合并；tag `v4.19.157` / `v4.19.325` 指向上游原提交（Greg KH 署名） |
| 显示栈 | CodeLinaro [msm-4.19 @ LA.UM.9.12.r1-18500-SMxx50.QSSI14.0](https://git.codelinaro.org/clo/la/kernel/msm-4.19/-/tree/LA.UM.9.12.r1-18500-SMxx50.QSSI14.0)（techpack/display） |
| 音频核心栈 | 同上 CLO tag 的 [techpack/audio](https://git.codelinaro.org/clo/la/kernel/msm-4.19/-/tree/LA.UM.9.12.r1-18500-SMxx50.QSSI14.0/techpack/audio)（v1.5 起整体 =y 内建） |
| 相机 KMD | 同上 CLO tag 的 [techpack/camera](https://git.codelinaro.org/clo/la/kernel/msm-4.19/-/tree/LA.UM.9.12.r1-18500-SMxx50.QSSI14.0/techpack/camera)（SPECTRA_CAMERA=y） |
| 视频硬解 | [`MiCode/Xiaomi_Kernel_OpenSource`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource) kona 分支 msm_vidc |
| USB 相机 | 内核主线 `drivers/media/usb/gspca` |
| 触摸 / 背光 | [`tem423/android_kernel_lenovo_tb371fc`](https://github.com/tem423/android_kernel_lenovo_tb371fc)（nt36532 SPI 触摸 + ktz8866a/b 双芯片背光） |
| WiFi | 2022 世代 qcacld 三仓（v5.2.0.190I）：qcacld-3.0 + [`arter97-mirror/caf_qca-wifi-host-cmn`](https://github.com/arter97-mirror/caf_qca-wifi-host-cmn)（2022-09-29）+ fw-api [`sonyxperiadev`](https://github.com/sonyxperiadev) 镜像 324cb3d（2022-10-07） |
| 功放 codec | [`raystef66/InfiniR_kernel_alioth`](https://github.com/raystef66/InfiniR_kernel_alioth) 的 tfa98xx codec（NXP TFA98xx 驱动，适配 4.19 与本机 DT） |
| 音频 machine | 本树 `kona.c` 按联想 `audio_machine_kona.ko` 逆向重建（ELF 表级对齐） |
| 功放固件 | 联想原厂 `tfa98xx_QS.cnt` / `tfa98xx_YC.cnt`（提取自 stock /vendor/firmware），经 CONFIG_EXTRA_FIRMWARE 烤入 |
| rmnet 加速 | [`MiCode/vendor_qcom_opensource_data-kernel`](https://github.com/MiCode/vendor_qcom_opensource_data-kernel) alioth-r-oss 分支 `drivers/rmnet/{perf,shs}` |
| KernelSU | [`backslashxx/KernelSU`](https://github.com/backslashxx/KernelSU) master（v3.3.1+ / 32704 = e0183047）；管理器 APK = [`smith-dog/KernelSU`](https://github.com/smith-dog/KernelSU) fork（含 boot v1/v2 修补支持） |

### 回移 / cherry-pick 来源（fusion 合入）
| 组成 | 来源与作者 |
|---|---|
| **Multi-Gen LRU** | 上游 Linux **Yu Zhao（Google）** 的 multi-gen LRU **v14 系列**（后并入主线 v6.1）；回移参考 `mglru-ref/`（v14 mbox + android12-5.4 common-kernel backport）。详见 `Documentation/admin-guide/mm/multigen_lru.rst`。4.19 适配（mm 钩子 / 位域 / bounds / `__assign_bit` 修正）为 fusion 原创 |
| **TCP BBRv3** | 上游 Linux `net/ipv4/tcp_bbr.c`（`BBR_VERSION 3`），**Google BBR 团队：Neal Cardwell、Yuchung Cheng、C. Stephen Gunn、Soheil Hassas Yeganeh 等**（6.x / 2024 快照，见 `bbrv3-ref/`）。模型代码 verbatim；4.19 兼容头 `tcp_bbr_v3_compat.h` 与协议栈配套为 fusion 原创 |
| **vsock loopback** | 上游 Linux **5.9** `net/vmw_vsock/vsock_loopback.c` 回移（本机 vsock core 已含 5.9 期 virtio_transport_common 件） |
| **GPU / 触摸 / 显示修复（#6）** | [`bampudding/linux_kernel_lenovo_tb371fc`](https://github.com/bampudding/linux_kernel_lenovo_tb371fc)（main 2026-10-10 快照）：kgsl gpu_mem eBPF 记账、kgsl dma-buf priv_data 防卫、adreno_a6xx 上电寄存器越界检查、nt36xxx instant-wake、nt36xxx HAL resume 互锁、dsi DSC PPS pic_width 幂等 |

### Fusion 原创
- MGLRU 的 4.19 适配（fork/exit/exec/sched 钩子、`in_lru_fault`、`LRU_GEN_WIDTH`/`__LRU_REFS_WIDTH`、`next_memory_node`、`__assign_bit` 返回值修正）
- BBRv3 的 4.19 兼容头与协议栈配套（`icsk_ca_priv` 104→144、`sk_buff.cb[48]`→`[56]`、rate_sample ECN/loss 增量、`inet_diag` v3 字段、`cong_ops->tso_segs`、精确 ECN-ACK）
- dm-verity kverityd 工作队列调优
- TFA9894 温重启爆音修复①②（`mute_seq_fix`）
- clang-21 全树清洁构建修复、boot header v2 重打包工具 [`tb371fc/tools/repack_boot.py`](tb371fc/tools/repack_boot.py)（继承上游并修正 `dtb_size` quirk）

> 联想未随 GPL dump 公开的部分（如 144Hz 显示驱动、部分面板参数）不在本树，对应功能保持原厂形态。

---

## 自己编译 / Build from source

环境：**Ubuntu clang 21.1.8**（`/usr/lib/llvm-21/bin`）+ aarch64-linux-gnu binutils 2.46。
fusion 构建横幅：`Linux version 4.19.325-perf++ (yuki@yuki) (Ubuntu clang version 21.1.8 (6ubuntu1), GNU ld (GNU Binutils for Ubuntu) 2.46) #8`。

配置：以 `tb371fc/config-v2.2.txt` 为基底，开启 fusion 增量：
`CONFIG_LRU_GEN=y` / `CONFIG_LRU_GEN_ENABLED=y` / `CONFIG_LRU_GEN_STATS=y`、
`CONFIG_TCP_CONG_BBR=y` / `CONFIG_DEFAULT_BBR=y`（`CONFIG_DEFAULT_TCP_CONG="bbr"`）。

```bash
KV=$(pwd)
export PATH=/usr/lib/llvm-21/bin:$PATH
FLAGS="ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- CC=clang CLANG_TRIPLE=aarch64-linux-gnu- AS=aarch64-linux-gnu-as"
KC="KCFLAGS=-Wno-error -Wno-error=strict-prototypes -Wno-error=implicit-int -Wno-error=incompatible-pointer-types -Wno-error=date-time -include $KV/drivers/staging/fw-api/fw/p226_compat.h"
# 内核镜像（DYNAMIC_SINGLE_CHIP 决定 qcacld 取 qca6390 变体）
make -j16 $FLAGS $KC DYNAMIC_SINGLE_CHIP=qca6390 Image
# 模块链 + KernelSU LKM（同一棵树同一份 .config）
make -j16 $FLAGS $KC DYNAMIC_SINGLE_CHIP=qca6390 modules
make -j16 $FLAGS $KC DYNAMIC_SINGLE_CHIP=qca6390 M=drivers/kernelsu modules
# 打包 boot 镜像（保留 v2 头 / ramdisk / DTB 尾）
python3 tb371fc/tools/repack_boot.py <base.img> arch/arm64/boot/Image out/boot.img
```

- WiFi 为 2022 世代 qcacld 三仓（`drivers/staging/` 下 qcacld-3.0 / qca-wifi-host-cmn / fw-api，体积原因不入 git），完整重建见 `tb371fc/scripts/p224-build.sh`。
- `CONFIG_KSU=m` 时 ksu.ko 需本树 `drivers/ksu_sym.c`（非公开符号 EXPORT 垫片，已内建）。
- techpack display/audio/camera/video 源码已全部入库，仅构建产物被忽略。

## 刷入 / Flash

> 前提：Bootloader 已解锁（`fastboot flashing unlock`）。A/B 设备，刷前核对 `fastboot getvar current-slot`。

```bash
adb reboot bootloader
fastboot flash boot out/boot.img      # 持久刷入当前 slot 的 boot 分区
fastboot reboot
```

- **刷前备份**（便于回滚）：`adb shell su -c 'dd if=/dev/block/bootdevice/by-name/boot_a of=/data/local/tmp/boot-backup.img'` 再 `adb pull`。
- **回滚**：`fastboot flash boot boot-backup.img`。本次音频修复落刷前的备份与镜像 md5 见 [CHANGES-20261117.md](CHANGES-20261117.md)。
- 升级 KernelSU 直接在管理器内完成，内核无需重刷（LKM 解耦）。
- 刷过旧载荷包（v1.4 及更早 `tb371fc-dlkm-pkg-fixed.tar.gz`）的，先跑上游 Release v1.5 的 `tb371fc-payload-cleanup.sh`（只移动不删除）。

## 上游 Release / Upstream

基线功能的成品镜像与管理器 APK 见上游 [`smith-dog/kernel-tb371fc` Releases](https://github.com/smith-dog/kernel-tb371fc/releases)（v2.2 等）。
fusion fork 自行构建上述新增功能镜像；逐条变更与验证状态见 [CHANGES-20261117.md](CHANGES-20261117.md)。

## 详细文档
- [CHANGES-20261117.md](CHANGES-20261117.md) — 最近一批补丁（MGLRU / BBRv3 / dm-verity / 音频爆音）逐条说明、构建 #8 与落刷验证、回滚。
- [docs/NOTE-build-and-root.md](docs/NOTE-build-and-root.md) — 管理器修补分步、root 自举原理、开机模块自动化部署、踩坑表。

## License

GPL-2.0（继承 Linux 内核、联想 GPL 发布及上述各上游项目的许可）。
各回移代码保留其原始版权与署名（MGLRU：Yu Zhao / Google；BBRv3：Google BBR 团队；
vsock loopback：上游 5.9；bampudding 修复：bampudding；KernelSU：backslashxx / tiann）。