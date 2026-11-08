# CE 第一次 DMA 的根因：stream 0xc0 从未注册（2026-11-08，481 闭环）

## 结论先行

**CE 数据通路已经打通。** 481 的两组日志第一次跑完了整条协议栈：

```
12 个 CE 环全部编程 → posted 63 → doorbell → CE rings live
→ HTC READY → CONNECT_SERVICE_RESP (eid=1) → SETUP_COMPLETE_EX
→ WMI SERVICE_READY (fw 840630314, abi 01000000) → WMI INIT
→ WMI READY → PDEV_SET_REGDOMAIN → VDEV_CREATE
→ "scan vdev created, scanning is available" → en0 attached, link up
```

根因不是「缺 IOMMU domain / 页表」，而是 **WLAN CE 的 stream 0xc0 在 apps-SMMU
里从来没有任何 SMR→S2CR 路由**。XBL 只为固件自己的流（0xc2/0xc3）和 USB（0xe0）
留了条目，0xc0/0xc1 是留给 HLOS 的；Linux 由 `arm-smmu` 在 `qcom,icnss` probe
时注册，这个移植从没做。CE 一敲门铃去取第一个描述符，就是一条 SMMU 不认识的
流，事务在互连上被挂住，USB DWC3 和 DPU 同时失明（控制台断流 + 整屏变灰）。

## 481 实测

| 项 | `ctl481` (`wlansmmu=0`) | `ctx481` (`wlansmmu=1`) |
|---|---|---|
| 日志 | 189,442 B | 187,520 B |
| `kmem_alloc` panic | 0 | 0 |
| `WCN3990-HIF` 行 | 78 | 77 |
| 0xc0 绑定 | `SMR[7]=800100c0 S2CR=0000003d` (bank 61, bypass) | `SMR[7]=800100c0 S2CR=00000004` (bank 4, translated) |
| 上下文 | `OFF`，CB4 无页表、`SCTLR.M=0` | `ENABLED bank 4 sctlr=00000061 ttbr0=cd5d9000 tcr=00800020` |
| HTC / WMI | 完整到 `scanning is available` | 完整到 `scanning is available` |

**`wlansmmu=0` 的旁路组同样跑通** → 上一轮「必须建带页表的翻译上下文」的假设
**证伪**。真正必要的只是让 0xc0 有一条约有效的 S2CR 路由；bypass bank 61
（`SCTLR.M=0`，物理地址直通）就够。这也解释了为什么三天里改 CB60/61、CB0、
DMA 池、CTRL1、RPMh、FD、内核版本都没用：那些全是在「0xc0 无路由」这个
不变的前提下换参数。

## 与历史日志的对照（决定性证据）

`work/keep/` 全部实验日志里 `wlan stream c0` 出现次数 = **0**；`kd439`、`q0`、
`q1`、`kpool`、`kheap`、`vote`、`qp` 全都停在 `stage 3: posted 63 receive
buffers`，`CE rings live` 只在故意不敲真实门铃的 `nodoor` 里出现过一次。
`WMI: READY` 出现次数 = **0**。481 是唯一同时具备「0xc0 已注册」和「跑完 WMI」的构建。

439 旧内核当年「能到 doorbell」并不是 WiFi 能用——所有版本都死在同一个
`posted 63`，只是日志长度差 1–2 KB，之前把「到门铃」误当成了「过门铃」。

## 三轮无效 A/B 的教训（判读纪律）

| 轮次 | 现象 | 真因 |
|---|---|---|
| 479 | 两组 0 字节，手机完全掉线 | 设备脱机，PnP 全 `CM_PROB_PHANTOM`，需手动长按电源回 fastboot |
| 480 | 两组 28–30 万字节，但 `WCN3990-HIF` 行数 0 | 自己加的屏幕探针 `ml_io_map(0xa0000000, 0x9e3400)` 在 `wcn3990_hif_init()` **之前**申请 10,371,072 B 内核 VA 失败，XNU panic |
| 480 附带 | 控制组打印 `wlan stream c0 skipped (taken=1)` | 空闲判断把控制台自己的 bypass bank 当成占用，对照组根本没绑 0xc0 |

480 的 panic 栈用 `work/macho-syms.py` 解析 CI 的 `kernel.macho` 得到
`modem_thread → svc_rx → io_map → kmem_alloc_guard_internal → Assert`，
`10371072 = round_page(0x9e3400)` = 1080×2400 RGBA 帧缓冲。探针已改为
`wlan_fb_probe=1` 才启用。**判读任何 A/B 前先查三件事：日志字节数、
`WCN3990-HIF` 行数、`10371072` panic —— 否则工具故障会伪装成「控制台静音」。**

## 仍未解决的问题

481 两组都在 WMI 完成后约 0.2 s（主机采集 t+94.5 s）panic，位置完全一致，
且 `=== PICASSO CDC PANIC BEGIN ===` 之后**只有 21 字节、没有任何 panic 文本**
（480 同类 panic 输出了 121 KB，说明输出通路本身可用）。因此这个 panic 让 CPU
在常规 printf 路径之前就停住了，暂时无法定性是否与 WLAN 有关。

482（commit `d091a13`）已推出，做三件事：

1. `panic_trap_to_debugger()` 入口同步输出一行
   `PICASSO PANIC caller=0x... reason=0x... fmt: <原始格式串>`，纯手写十六进制
   打印、不用 `vsnprintf`/不加锁/不分配；`panic_caller` 可用 `work/macho-syms.py`
   对 CI `kernel.macho` 符号化，直接定位 panic 源。
2. `wlansmmu=2`：同一镜像**故意不绑 0xc0** 的负对照，把「0xc0 路由是因」从
   跨内核的历史比较升级为真正的单镜像 A/B。预期退回 `posted 63`。
3. `wifi=0`：判断 post-WMI panic 是否由 WLAN 路径引入。

## 距离「完全可用」还差什么

CE/HTC/WMI 控制面已通，但日志里**还没有** `WMI: send START_SCAN`、
`HEARD BEACON` 或任何扫描结果。后续是软件活：

- 触发并解析 `START_SCAN` / `WMI_SCAN_EVENTID`，把 BSS 交给 `picasso_sta_scan()` 的 `ops->heard()`；
- association（`picasso_sta_join`）与数据面 HTT/CE 管道；
- 解决上述 post-WMI panic，否则系统无法稳定停留到用户态发起扫描。

**当前状态：控制面打通、根因闭环到 SMMU 流注册；WiFi 尚未完全修复，不宣称可连网。**
