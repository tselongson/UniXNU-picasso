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
（480 同类 panic 输出了 121 KB，说明输出通路本身可用）。

**482 已把这个 panic 定性：它不是软件 bug，是 ARM64 SError（异步外部中止）。**
`ctl482` 的同步首行：

```
PICASSO PANIC caller=0xfffffff007cb4d94 reason=0x0000000000000000
fmt: Unhandled implementation specific error
```

用 `work/symbolize.py` 对 482 的 `kernel.macho`（134663 符号、slide=0）解析：

```
0xfffffff007cb4d94  _arm64_platform_error.cold.1 + 0x0
0xfffffff007cb4d90  _sleh_serror + 0x23c
```

即 `sleh_serror()` 的默认分支，CPU 收到一笔没有处理程序的外部中止。
这与「总线/DMA 事务被拒」一致，也解释了为什么此前控制台只会突然静音、
`gfsr` 干净却整屏变灰。它不是空指针、不是 `kmem_alloc`、不是看门狗。

**`wifi=0` 对照（用现成 481 镜像原地改参数，无需重建）已定性一半**：
`out/full-flash/logs/console-exp-off481.log` 163,256 B，HIF 行 0，
`CDC PANIC BEGIN` **0 次**，采集窗口内（attach 后 99 s，即设定的 150 s 结尾）
控制台持续输出 GPU hangcheck / CPUFREQ / UFS cache 统计，系统正常跑到用户态。
关键是这组**仍然执行了 0xc0 绑定**（`wlan stream c0 slot=7 ... domain=0`），
只是 WLAN/modem 线程没启动、CE 没跑。所以：

- 那个 panic 不是系统固有故障，也不是 0xc0 绑定本身，而是 **CE/WMI 真正跑起来之后**的路径引入的；
- 三天里「控制台断流 + 整屏变灰 + 重启」也由此闭环到 WLAN 路径，不是 USB 或显示的独立问题。

`wifi=0` 同时关掉了 modem 子系统，所以它只能圈定范围到 `PicassoModem`/WLAN
初始化路径，不能单独指向 HIF。串口还读到 `time_ms=2626837`（约 44 分钟连续
运行），排除了「150 s 窗口里恰好没崩」。

## 482 三组：因果闭环完成

| 组 | 0xc0 | HIF 行 | 结果 |
|---|---|---|---|
| `ctl482` | 绑定 bank 61 | 78 | `posted 63` → `CE rings live` → `HTC READY` → `WMI READY` → `scan vdev created` → SError panic |
| `nb482` (`wlansmmu=2`) | **故意不绑** | 45 | 停在 `posted 63`，无 `CE rings live`、无 HTC/WMI、无 panic marker —— **精确复现历史全部实验** |
| `off482` (`wifi=0`) | 绑定 bank 61 | 0 | 无 HIF、无 panic，正常进桌面 |

`nb482` 与 `ctl482` 是**同一镜像、只差 0xc0 一个 SMR/S2CR 条目**，
因此「0xc0 未注册是 CE 卡死的根因」不再是跨内核的相关性推断，而是单变量
A/B。同时它也说明：0xc0 未注册时的表现是**静音**（无 panic），注册后 CE 真跑
起来才会触发 SError —— 两个不同故障，之前一直被混为一谈。

**483 结果：TZ 授权假设证伪**（同一镜像 `=0/1/2/3` 四组）：

| 组 | SCM 授权 | 结果 |
|---|---|---|
| `a0` | 不授权 | 完整 WMI → 同样的 SError |
| `a1` | `VMID_WLAN_CE` | `hyp_assign rc -2`，TZ 拒绝，HIF 保持 inert |
| `a2` | `+VMID_WLAN` | 两级都 `rc -2`，HIF 保持 inert |
| `a3` | `VMID_MSS_MSA` | **`rc 0` 授权成功**，完整 WMI → **仍同样的 SError** |

所以问题不在内存归属。（副产品：本机只有 `VMID_MSS_MSA` 能授成功，
24/25 两个 VMID 被 TZ 拒绝。）

## 484：CE3 缓冲区复用是**真 bug，但不是 SError 的因**（假设已证伪）

网上检索命中 **CVE-2022-50700 / Linux `wifi: ath10k: Delay the unmapping
of the buffer`**：WCN3990 的 CE3（WMI 命令通道）copy-complete 到达时硬件
可能仍在读源缓冲，host 立即解映射/复用会触发 SMMU fault + kernel panic。
代码里确实存在更严重的同型 bug：`hif_wmi_command()` 把每条 WMI 命令都写进
同一个静态 DMA 缓冲 `s_tx3`，于是 CE3 每个描述符都指向**同一个物理地址**。

修复（`d00b0b8`）保留：`tx3` 改成 8 个 2 KiB 槽、按 `hif_ce_send()` 用的
同一个 `g_wr3` 索引取槽；编译期 `hif_dma_window_check` 保证 512 KiB 窗口
仍装得下；`wcn3990-hif-tx3=0/1` 单变量 A/B；`wcn3990-hif-autoscan=1`
在 `HIF_READY` 后自动发 `START_SCAN`。

**但 484 三组实测把这个假设证伪了**：

| 组 | 配置 | `ce3 send` 物理地址 | 结果 |
|---|---|---|---|
| `t0` | 单缓冲 | `cdc3ae40` ×3 | WMI READY → SError |
| `t1` | 8 槽轮换 | `cdd7ae40` / `cdd7b640` / `cdd7be40` | **同样位置 SError** |
| `t2` | 8 槽 + autoscan | 前三条不同，第四条 `cdd3c640` | 发出 `START_SCAN`（292 B）→ **同样 SError** |

三组 HIF 行 79/79/82，panic 首行全是
`caller=0xfffffff007cb4eb8 fmt: Unhandled implementation specific error`。
缓冲区确实分离了，外部中止没变。**所以 CE3 race 不是本次故障的成因**，
修复只是消除了一个潜在 DMA 竞态。

panic 前的固定序列（484 `t1`）：

```
WMI: READY → send PDEV_SET_REGDOMAIN → send VDEV_CREATE
→ WMI: event 0x401f (1148 bytes)
→ HTC: control message 1025 ×2
→ PICASSO CDC PANIC BEGIN
```

用 ath11k `wmi.h` 的事件表对照，日志里此前不认识的三个事件都是**合法的
固件指示**，不是损坏数据：`0x4009` = `SERVICE_READY_EXT`（980 B）、
`0x4022` = `SERVICE_READY_EXT2`（20 B）、
`0x401f` = `PDEV_COLD_BOOT_CAL_DATA`（1148 B）。

## 485：不再猜，改用硬件寄存器 + 命令二分

网上另命中一条形状**几乎完全一致**的已知问题：WCN3990（WLAN.HL.3.2）
固件在 `ath10k_start()` 的 quiet-mode WMI 命令后崩溃，
「~17 ms after the subsequent vdev_create command, cascades into a full
modem crash」。我们没发 quiet-mode，但崩溃紧跟 `VDEV_CREATE` 这一点一致。

485（`f23e168`）加两件仪器，默认不改任何行为：

1. **panic 同步转储 SMMU 故障寄存器**：USB 控制台记住自己早期的
   apps-SMMU 映射，在 `panic_trap_to_debugger()` 的同步首行里用无锁
   十六进制打印 `scr0/gfsr/gfsynr0..2`、0xc0 所在 slot 的 SMR/S2CR、
   CB61 与 CB4 的 `SCTLR/FSR/FAR/FARHI/FSYNR0`。异步外部中止应当在
   这里留下故障地址；常规 printf 在这个 panic 里根本不执行，这是唯一
   能读到它的地方。
2. **`hifstop=0..3`**：在 WMI READY 之后跳过 `PDEV_SET_REGDOMAIN`
   和/或 `VDEV_CREATE`，把「我们的命令把固件弄崩」和「固件自己把主机
   弄崩」分开。0 = 两条都发（默认），1 = 跳过 regdomain，
   2 = 跳过 vdev_create，3 = 两条都跳过。

`work/chain-485.sh` 在同一镜像上跑 `s0/s1/s2/s3` 四组各 150 秒。

## 距离「完全可用」还差什么（代码摸底结论，2026-11-08）

**已经齐的链路**：用户态 `apple80211` → `hakios_sta_attach(&picasso_sta_ops)`
→ `picasso_sta_scan()` → `wcn3990_hif_start_scan()` → `hif_send_wmi_start_scan()`
（39 信道、TLV、probe request）→ 固件 `WMI_MGMT_RX_EVENTID` →
`hif_handle_mgmt_rx()` → `wcn3990_hif_set_frame_sink(picasso_parse_beacon_frame)`
→ `picasso_add_bss()` → `ops->heard()`。也就是说**扫描代码不用重写**，
只差系统稳得住、用户态真的发起扫描。482 日志里 `apple80211: the station
says: power, 1` 已经出现，但在发出 scan 之前就 SError 了。

**明确还缺的三块**：

1. **消除 post-WMI SError** —— 483 已证伪 TZ 授权、484 已证伪 CE3 缓冲复用；
   485 用 SMMU 故障寄存器转储 + `hifstop` 命令二分继续定位。这是所有后续的前提。
2. **关联是假的** —— `picasso_sta_join()` 只打印
   `"4-way WPA2-PSK handshake authenticated successfully"`，没有发任何
   `WMI_VDEV_START` / `WMI_PEER_CREATE` / 密钥安装命令，找不到 BSS 时甚至
   编造默认 BSSID `24:4b:fe:11:22:33` 和 RSSI `-45`。要真连 AP 必须重写。
3. **数据面 HTT 完全没有** —— `Wcn3990Hif.cpp` 里 CE1/4/5/9/10 的 HTT 环
   只是占位（注释写着 unused），没有 MSDU 收发、没有 `ifnet` 输入输出对接。
   `picasso_sta_power()` 也只是打日志返回 0。

因此「WiFi 开关能打开、能扫出 AP 列表」和「能真正连网」是两个量级，
前者依赖第 1 项，后者还要做完第 2、3 项。

**当前状态：控制面打通、根因闭环到 SMMU 流注册；WiFi 尚未完全修复，不宣称可连网。**
