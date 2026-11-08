# CE 第一次 DMA 为什么让 USB 控制台和屏幕一起死（2026-11-08）

## 现象与已测事实

| 事实 | 来源 |
|---|---|
| 门铃（`CE_DST_WR_INDEX` 由 0 写成 63）之后约 1 ms，USB CDC 控制台断流；SoC 仍活着，能响应重启进 fastboot | `work/keep/console-exp-kd439.log`、`q0`、`q1`，三次一致 |
| 同一时刻整屏变灰 → 15 s 后复位 | 用户观察 + 重启日志 |
| `gfsr=0`、`gfsynr0/1/2=0`，CB61 `fsr=00000400`，没有干净的 SMMU 全局 fault | `stage 3 warm` 日志 |
| WLFW/modem 全流程是通的：`FW_READY_IND` → `WLAN_CFG` → `WLAN_MODE` → `MISSION MODE ATTACH COMPLETE` | `console-exp-q1.log` 2450–2545 行 |
| 不写真实门铃的那次能继续打印到 `CE rings live, poller now servicing every 1 ms` | `console-exp-nodoor.log` |
| 全部 10 余次实验都是 `assign_level=0`（CE 内存从未向 TZ 申请授权），CE 拿到的都是**物理地址** `0xcd......`，走的是 `SCTLR.M=0` 的旁路基 CB60/CB61 | 所有 `CE DMA block ... assign_level=0` 日志 |

结论：不是内核 panic，也不是 watchdog 误触发。CE 作为 apps-SMMU 上的一个 master，第一笔描述符读取
在互连（NoC/XPU）上被挂住/拒绝；USB DWC3 和 DPU 显示都要走同一条互连取数据，于是一个断流、一个灰屏。
SMMU 全局 fault 寄存器干净，正说明事务没走到「正常报错」那一步。

## 原系统怎么做，我们漏了哪一步

Linux 这边，`qcom,icnss` 的 DT 是 `iommus = <&apps_smmu 0xc0 0x1>`、
`qcom,iommu-dma-addr-pool = <0xa0000000 0x10000000>`、`qcom,iommu-dma = "fastmap"`。
`arm-smmu` 在设备 probe 时给 0xc0 **分配一个空闲 context bank → 建页表把 IOVA 窗口映射到 DDR →
S2CR 写成 type 0 翻译到该 bank**，之后 ath10k 才把 IOVA 写进 CE 的 ring base / descriptor。

本机已有的两条硬件验证过的 XNU 实现也是这么做的：

- `drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp` `dpuEnable()`：给显示流所在的 CB2
  装页表（`L1[2] = 0x80000000 | attrs | 1`、`L1[3] = 0xc0000000 | attrs | 1`，1 GiB 恒等块），
  写 TTBR0/TCR2/TCR/MAIR、刷 TLB、开 `SCTLR.M`。这是本 SoC 上已跑通的 SMMU stage-1 配方。
- `work/refs/hakios/drivers/wifi/hakios.wifi.c` `radio_space_on()`：为 wcn6750 的射频流
  找空闲 SMR + 空闲 CB、建页表、把 `0xa0000000` 起的 IOVA 交给射频。它的注释直接写了这个故障：
  > "the radio's first reach for memory, at mission mode, was then a stream the SMMU knows
  > nothing of, and **the phone went down without a word**"

我们三天里改的 S2CR 值、CB60/CB61/CB0、DMA 池地址、CTRL1、RPMh vote、FD/内核版本，
都没有跨过这条分界线：**0xc0 始终没有带页表的翻译上下文，CE 始终拿物理地址**。
XBL 的表里 0xc0/0xc1 本来就是空的（0xc2/0xc3 才是 XBL 给固件自己流预留的），
0xc0 是留给 HLOS 的 arm-smmu 去建的。

## 这次补上的东西（单变量 A/B）

- `patches/picasso-usb-console-xnu.patch`：在 SMMU 还允许写全局寄存器的早期窗口，
  把 0xc0/mask 1 挂到空闲 **CB4**（S2CR type 0 = 翻译），并预先设好 CB4 的 CBAR/CBA2R。
  之后窗口关闭，驱动侧再写 SMR 会被拒（v356 已测）。
- `source/PicassoWlan.cpp` `picasso_wlan_smmu_context()`：`wlansmmu=1` 时按显示驱动那套
  配方给 CB4 装 4 KiB L1 恒等页表并打开翻译；`=0` 时保持 `SCTLR.M=0`，即以前所有构建的旁路状态。
  恒等映射覆盖 `0x80000000..0xffffffff`，所以 HIF 现有的物理地址 `0xcd......` 自动成为合法 IOVA，
  **HIF 一行地址代码都不用改** —— 这正是「只改一个变量」。
- `source/Wcn3990Hif.cpp`：`wlansmmu=1` 时跳过 stage 2.5 那两笔按流 ID 直接索引 S2CR 的旧探测写
  （0xc0 已越出 S2CR 数组，写到 0xf00 保留区了）。
- 修掉 `picasso_wlan_probe_paint()` 的链接域不一致（`extern "C"`），CI 42442990677/42442600730
  两次失败都是 `Undefined symbols: _picasso_wlan_probe_paint`；修完屏幕色块探针才可用。

判读（同一镜像，只改 `wlansmmu=0/1`）：

- `=1` 有 `wlan smmu ctx: ENABLED ... fsr=00000000`，且门铃后控制台继续输出 / 屏幕出现绿色格子
  → 根因确认就是缺 IOMMU domain，后面只剩 HTC/WMI 软件活。
- `=1` 仍在同一处断流，但 CB4 `FSR/FAR` 记到了 fault → 上下文建起来了，问题变成页表属性或
  TZ 内存授权，下一步按 FAR 落地址查。
- `=1` 与 `=0` 完全一样 → 「翻译上下文」这条假设被证伪，转去查 TZ VMID 授权
  （`wcn3990-hif-assign=3`，即把 CE 内存授给 VMID 15，这个调用在 rmtfs 上已验证返回 0）。

## 480 的教训：先排除自己引入的 panic（已修，commit 48597d0）

479 第一次 A/B 两组都是 0 字节、手机掉线（`CM_PROB_PHANTOM`），属于设备/采集问题。
480 设备回来后两组都流了 28–30 万字节，但**都在同一个地方 panic，且都不是 CE**：

```
panic(cpu 7 caller 0xfffffff007c9b2d0):
  kmem_alloc(0x..., 10371072, 0x100021): failed with 3 @vm_kern.c:180
```

用 CI 产出的 `kernel.macho` 符号表（脚本 `work/macho-syms.py`）解析调用栈：

```
Call_continuation
modem_thread +0x2dac
svc_rx       +0x181c
io_map       +0x160
kmem_alloc_guard_internal +0x6b8
kmem_invalid_size_idx      <- Assert/panic
```

`10371072 = round_page(0x9e3400)`，正是 1080×2400 RGBA 帧缓冲大小，对应
`wlan_fb_map()` 里 `ml_io_map(0xa0000000, 0x9e3400)`。这个屏幕色块探针是
11:24 那次提交加的，位置在 `picasso_wlan_ce_init()` 里、**`wcn3990_hif_init()`
之前**；启动后期 zone map 拿不出 10 MB 连续内核 VA，XNU 在 `io_map()` 里直接
panic。所以两组日志里连一行 `WCN3990-HIF` 都没有，CE 根本没被初始化。

修复：探针改成 `wlan_fb_probe=1` 才启用，默认路径不做任何大映射。

同一个 480 日志还暴露了对照组不纯：控制组早期绑定打印
`wlan stream c0 skipped (bank 61 taken=1 slot=7)`。空闲判断把控制台自己的
bypass bank 当成「被占用」，于是控制组压根没绑 0xc0。已修为只有专用 CB4
才做空闲检查。

**方法论**：连续两轮 A/B 无效，原因都不在 WiFi —— 一次设备掉线，一次是我
自己上一轮引入的诊断代码 panic。判读前必须先确认日志里有没有
`WCN3990-HIF` 行和 `kmem_alloc` panic，否则「控制台静音」会被误当成 CE 故障。

## 还没做

- association / 数据面（HTT）仍未实现；扫描结果不等于能连网。
- IOMMU domain 假设的硬件 A/B 仍在跑（481），本文不宣称修复完成。
