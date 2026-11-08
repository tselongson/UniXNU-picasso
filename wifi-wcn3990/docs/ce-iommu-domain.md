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

`work/chain-485.sh` 在同一镜像上跑 `s0/s1/s2/s3` 四组各 150 秒。**实测结果**：

| 组 | WMI READY 之后发的命令 | panic | 存活 |
|---|---|---|---|
| `s0` | PDEV_SET_REGDOMAIN + VDEV_CREATE | SError @ t+94.3s | 否 |
| `s1` | 仅 VDEV_CREATE | **同样的 SError** | 否 |
| `s2` | 仅 PDEV_SET_REGDOMAIN | **无** | 99.6 s，HIF 心跳 1000→5000，rx=12 |
| `s3` | 都不发 | **无** | 正常 |

所以 **VDEV_CREATE 是唯一触发点**，与 regdomain 无关。

**SMMU 故障转储（485 新增的同步 panic 转储）证明不是 IOMMU 故障**：
`gfsr=0`、`gfsynr0/1/2=0`、`smr7=800100c0`、`s2cr7=0000003d`（0xc0 正确绑到 CB61），
`cb61_fsr=0x400`、`cb4_fsr=0x400`、`cb61_far=37943e81`、`cb4_far=1e4bd6cc`。
对照 Linux `arm-smmu.h`：`CB_FSR` 的故障位是 bit1/2/3/4/5/6/7/8/30/31，
**bit10 属于 `FSR_FORMAT[10:9]`，是 LPAE 上下文格式标志，不是故障**；
`FSR_FAULT` 掩码完全不含 0x400。FAR 是残留值。因此这个 SError 不是
SMMU 翻译/权限错误，而是别处的总线错误（最可能是固件在 vdev 起来后
开始使用我们从未配置的数据通路而崩掉）。

**对照原系统**：Linux `ath10k_core_start()` 在 WMI READY 之后会发
`WMI_PDEV_SET_BASE_MACADDR`，接着 `ath10k_htt_connect()`（第二个 HTC 服务）、
`ath10k_htt_setup()`（HTT 版本协商 + FRAG_DESC_BANK_CFG + RX_RING_CFG +
AGGR_CFG），而 `VDEV_CREATE` 是很久以后 `mac_op_add_interface()` 里才发的。
这几步本移植全跳过了。486 用 `vdevpre` / `hifstop=5/6` / `hifstop=4`
分别测「补发 base MAC」「延迟 vdev」「干脆不要 vdev 能否扫描」。

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

## 486：触发点修正 —— 不是某条命令，是「固件一用资源池就死」

同一镜像五组（`boot-486.img`，均带 8 槽 CE3 缓冲轮换）：

| 组 | WMI READY 之后 | 结果 |
|---|---|---|
| `v0`  `hifstop=0 vdevpre=0` | regdomain + VDEV_CREATE | SError |
| `d1`  `hifstop=5` | regdomain + 延迟 1000 polls 再 VDEV_CREATE | SError |
| `d3`  `hifstop=6` | regdomain + 延迟 5000 polls 再 VDEV_CREATE | SError |
| `p1`  `vdevpre=1` | base MAC + regdomain + VDEV_CREATE | SError |
| `n4`  `hifstop=4` | regdomain + **START_SCAN，无 vdev** | SError |

五组 panic caller 全部 `0xfffffff007cb580c`，SMMU 转储全干净
（`gfsr=0`、`gfsynr0/1/2=0`、`cb61_fsr=cb4_fsr=0x400` 只是 FORMAT 位）。
对照 485 `hifstop=2`（只发 regdomain）：**不 panic**，活 99.6 s，
HIF 心跳到 5000、modem 正常工作。

所以 485 的「VDEV_CREATE 是唯一触发点」要修正：延迟发送、补 base MAC
都没用；连跳过 vdev 只发 START_SCAN 也崩。共同点是固件**开始实际射频/
数据面工作**（建 vdev 或扫描），而 485 的 s2/s3 让固件一直闲着。
→ 不是命令内容的问题，是固件动用某块资源时崩。

**据此找到的具体缺陷**：`hif_send_wmi_init()` 里
`struct wmi_tlv_resource_config` 有 44 个 u32，本机只填了 12 个，
其余全 0。对照 `wmi-tlv.c:1895-1965 gen_init()` + `hw.h TARGET_TLV_*`：

| 字段 | 本机 | Linux WCN3990 |
|---|---|---|
| `num_vdevs` / `num_peers` / `num_tids` | 2 / 16 / 16 | 4 / 33 / 66 |
| `rx_timeout_pri[4]` | 全 0 | 100/100/100/40 |
| `rx_decap_mode` | 0（RAW，注释却写 native wifi） | 1（NATIVE_WIFI） |
| `num_msdu_desc` | 128 | 1056 |
| `max_frag_entries` | 0 | 2 |
| `tx/rx_chain_mask` | 3 / 3 | 7 / 7 |
| `bmiss`/`roam`/`gtk`/`beacon` offload max vdev | 0 | 4/4/2/2 |
| `host_capab` | 0 | `BIT(9)` = 0x200 |

这些池正是 vdev_create / scan 会去分配的。ABI `ns2/ns3` 在 Linux 也是 0，
不是问题。

## 487（进行中）：把 WMI INIT 补成 Linux 的样子

`358bc6b` 新增 `g_wmi_cfg_full[44]`（逐字段按上表），
`wmiinit=1` 启用，`=0` 保持旧配置逐字节不变 —— 同一镜像单变量 A/B。
`work/make-487.py` 用等长槽位 `wmiinit=0` / `hifstop=0` /
`wcn3990-hif-tx3=1`，`work/chain-487.sh` 跑 `i0` / `i1` / `i2`：
`i2` 额外用 `hifstop=4` 强制无 vdev 自动扫描，看能否出 `HEARD BEACON`。

判读：`i1` 若不再 panic 且能继续输出 → 资源池根因闭环；
若三组同样 SError → 此假设证伪，下一步按 Linux
`ath10k_core_start()` 补 HTT 服务连接 + `ath10k_htt_setup()`
（VERSION_REQ / FRAG_DESC_BANK_CFG / RX_RING_CFG / AGGR_CFG），
那也是数据面的前提。

**仍未完成**：扫描结果上报虽已接线（`WMI_MGMT_RX → parse_beacon →
heard()`），但从未在硬件上见过 `HEARD BEACON`；`picasso_sta_join()`
只打印握手成功并编造 BSSID/RSSI，未发任何 WMI 关联/密钥命令；
`picasso_wlan_transmit()` 会主动伪造 DHCP Offer/ACK，HTT 数据面完全没有。
控制面通 ≠ WiFi 可用，不拿「能 attach en0」冒称修好。

## 487：**SError 根因闭环** —— WMI INIT 资源池没配

`boot-487.img` 三组，同一镜像单变量：

| 组 | `wmiinit` | `hifstop` | panic | 采集 | 结果 |
|---|---|---|---|---|---|
| `i0` | 0（旧 12 字段） | 0 | **有** | 94.5 s 断 | 基准，VDEV_CREATE 后 SError |
| `i1` | **1（完整 44 字段）** | 0 | **0** | **满 99.6 s** | WMI READY → VDEV_CREATE → 无崩溃 |
| `i2` | 1 | 4 | **有** | 94.5 s 断 | 跳过 vdev 却强制 START_SCAN |

`i1` 计数：`CDC PANIC BEGIN` 0、`PICASSO PANIC caller` 0、
`Unhandled implementation specific` 0；HIF 心跳持续到 5000 poll、
`rx=13`、GPU hangcheck / CPUFREQ / TOUCH / nvram sync / modem tftp 全部
正常，`state=5`（HIF_READY）、`en0 ATTACHED`。

**所以 post-WMI SError 的因就是 `hif_send_wmi_init()` 只填了
`wmi_tlv_resource_config` 44 个 u32 中的 12 个。** 补齐 Linux
`ath10k_wmi_tlv_op_gen_init()` 的值后，VDEV_CREATE 不再杀死机器。
这是三天来第一个「CE → HTC → WMI → vdev 之后系统继续活着」的构建。

`i2` 不是反例：`hifstop=4` 故意跳过 VDEV_CREATE 又强制发
START_SCAN，而 `hif_send_vdev_create()` 上方的注释已经写明
「A START_SCAN names a vdev_id, and the firmware rejects the scan if that
vdev was never created」。这是人为构造的非法命令序列，不是
`wmiinit` 失效。488 改为**保留 vdev + 打开 autoscan** 再验证扫描。

## 488（进行中）：扫描能不能真的出 AP

不重建：`work/make-488.py` 把 487 镜像的 `wmiinit=0` → `=1`，
并把 `hifstop=0` 的 26 字节槽等长换成 `wcn3990-hif-autoscan=1`
（`hifstop` 随之缺省为 0，即正常创建 vdev）。`work/chain-488.sh`
跑 `s1`（完整配置 + 自动扫描）与 `s0`（旧配置 + 自动扫描，对照）。
判读：`WMI: send START_SCAN` → `WMI: SCAN event` →
`WMI_MGMT_RX` beacon → `HEARD BEACON` → `heard N access points`。

## 488 / 489：START_SCAN 是**第二个独立故障**

`boot-488.img`（=`boot-487.img` 等长换槽：`wmiinit=1` +
`wcn3990-hif-autoscan=1`，`hifstop` 缺省 0 所以 vdev 正常创建）：

| 组 | 配置 | 结果 |
|---|---|---|
| `s1` | wmiinit=1 + vdev + autoscan | 发出 START_SCAN 后 SError |
| `s0` | wmiinit=0 + vdev + autoscan | 同样 SError |
| `m1` | wmiinit=1 + base MAC + vdev + autoscan（489） | 同样 SError |

`m1` 是 `boot-489.img`（把 `offline-hid-auto-home=5` 等长换成
`vdevpre=1`）。三组 panic caller 全 `0xfffffff007cb5978`，SMMU 转储干净，
崩溃前只有两条 `HTC: control message 1025`，**没有任何
`WMI: SCAN event` 或 `WMI_MGMT_RX`** —— 固件没来得及回传扫描结果。

所以现在是**两个独立故障**：

1. VDEV_CREATE → SError：**已闭环修复**（`wmiinit=1`，487 i1 验证）。
2. START_SCAN → SError：与 `wmiinit`、base MAC 无关，未修。

已排除：CE3 缓冲复用、SMMU 故障、TZ VMID 授权、VDEV_CREATE 时序/前置
命令、START_SCAN TLV 结构（逐字段对照 `wmi-tlv.c:1977 gen_start_scan`，
15 u32 common + 6 u32 + mac_addr/mac_mask + 5 个数组 TLV 完全一致）、
CE1/CE5/CE2 接收缓冲投递（各 31/31/63 个）。

**下一步依据**：`ath10k_core_start()` 的顺序是
`ath10k_htt_connect()`(core.c:3108) → `wmi_wait_for_unified_ready`(3213)
→ `pdev_set_base_macaddr`(3225) → **`ath10k_htt_setup()`(3270)**
→ 之后才有扫描。本移植从没连接过 HTT 服务（`ATH10K_HTC_SVC_ID_HTT_DATA_MSG`
= 0x0300），也从没发过 `VERSION_REQ` / `FRAG_DESC_BANK_CFG` /
`RX_RING_CFG` / `AGGR_CFG`。固件扫描时要把结果写回 host 的 HTT RX ring，
没有这块 DMA 内存就是外部中止的典型成因。补齐它同时也是数据面的前提。

## 490 未跑成：CI 是网络故障，不是构建错误

`42464600780` 在 **Prepare authenticated stock touch firmware** 一步以
`urllib.error.HTTPError: HTTP Error 504: Gateway Time-out` 失败，连
`Build kernel + payload` 都没到。`work/ci-fail-42464600780.log` 有原始
堆栈。这是外部下载超时，重跑即可；`work/chain-491.sh` 因此对 CI 加了
最多 4 次重试，并且遇到真正的编译错误（`Undefined symbols` / `fatal
error`）会立刻停止，不把编译错误当网络抖动反复重试。

## 491：静态审计先于实机，找到 4 个格式错误的 WMI 命令

之前把 START_SCAN 的 TLV **逐字段**对照过 `wmi-tlv.c:1977
gen_start_scan`，结论是「结构一致」，但那只比了字段顺序和长度，**没有
比 TLV 之间的填充**。

`wmi-tlv.c:72 ath10k_wmi_tlv_iter()` 的推进规则是：

```c
ptr += sizeof(*tlv);   /* 4 */
ptr += tlv_len;        /* 精确长度，没有 roundup */
```

而本移植的 `hif_put_tlv()` 返回 `p + 4 + ((len + 3) & ~3u)`，把每条
TLV 补齐到 4 字节。凡是长度不是 4 的倍数的 TLV，都被硬塞进 2 个
零字节；这些字节在 target 眼里是下一条 TLV 的前半截。

自己写的离线工具 `work/wmi-tlv-audit.py`（按同一套字段尺寸重建字节流，
再用 Linux 的迭代器走一遍）给出：

| 命令 | 旧布局（`--pad`） | 精确布局 |
|---|---|---|
| WMI_INIT | 216 B OK | 216 B OK |
| PDEV_SET_REGDOMAIN | 24 B OK | 24 B OK |
| PDEV_SET_BASE_MACADDR | **12 B，尾部 2 B 截断** | 10 B OK |
| **VDEV_CREATE** | **24 B，尾部 2 B 截断** | **22 B OK** |
| VDEV_UP（新增） | 20 B 截断 | 18 B OK |
| **START_SCAN(39ch)** | **280 B，多出一条 tag=0 的假 TLV，尾部 2 B 截断** | **278 B OK** |

7 条命令里 4 条在旧构建里无法被 Linux 的解析器读完。这直接给出一个
可证伪的解释：**VDEV_CREATE 被 target 拒绝，vdev 根本不存在**，随后
`START_SCAN` 点名一个不存在的 vdev，固件 assert，经 apps 侧 SError
表现为 `Unhandled implementation specific error`；SMMU 转储干净、
崩溃前没有 `WMI: SCAN event` 也都对得上。

注意：这只是**源码级证据**，不是实机结论。修法是把精确长度设为默认
（`tlvpad=0`），同时保留 `tlvpad=1` 复现旧行为作为同一条链里的负对照。

## 491 的一次构建、七个单变量实验

`cec4d5c` 除修 TLV 外，把 Linux 在 WMI READY 之后、START_SCAN 之前还
做、而本移植没做的步骤全部做成**独立开关**，这样一次 CI 构建就能分别
验证，不用每个假设都重编一次内核：

| 开关 | 对应 Linux |
|---|---|
| `chanlist=1` | `WMI_SCAN_CHAN_LIST`（`mac.c ath10k_update_channel_list()`，`0x3003`），39 个 `STRUCT_CHANNEL` |
| `vdevup=1` | `WMI_VDEV_UP`（`wmi-tlv.c:2283 gen_vdev_up`，`0x5005`），14 B 载荷 |
| `scanlinux=1` | `0xA000` scan/req ID 前缀 + `ath10k_wmi_start_scan_init()` 默认值 + passive flag XOR |
| `httconn=1` | `ath10k_htt_connect()`：SETUP_COMPLETE 之前连接 `HTT_DATA_MSG`(0x0300) |
| `scanchans=N` | 490 留下的信道数二分（39 / 1 / 0） |

`work/make-491.py` 把短参数等长塞进 6 个既有 boot-arg 槽（空格是
`PE_parse_boot_argn` 的分隔符），保持 args 行长度不变；`work/multi-boot.sh`
每次只翻一个 token。判读顺序：`pad`（旧布局，应崩）→ `fix`（精确布局）
→ 其余开关 → `WMI: SCAN event` → `WMI_MGMT_RX` → `HEARD BEACON`。
**结果待 491 链跑完后回填，此处不预先宣称已修复。**
