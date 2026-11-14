# Picasso 项目日志通读摘要（2026-10-26 → 2026-11-09）
<!-- ARCHIVE-NOTICE-v6 -->
> **历史归档（v6 状态更新时加注）**：本文是 2026-11-10 00:10 的 11 份日志通读摘要，其后项目已大幅推进。
> 本文的阶段状态、分支名与 CI 命令写于当时，**不代表当前进度**。
> 当前唯一权威状态源是 [`handoff/wifi-wcn3990-handoff-2026-11-10.md`](wifi-wcn3990-handoff-2026-11-10.md)（v6，2026-11-10 20:45）；
> 逐轮实验事实见 `.workbuddy-ai/memory/2026-11-09.md` 与 `2026-11-10.md`。
> 本正文保留作为历史与推理记录。

> 2026-11-10 00:10 通读全部 11 份工作日志（`.workbuddy-ai/memory/`，约 300KB）后的
> 压缩摘要。目的：让后续会话不必重读全文，同时保留每条"用代价换来的"教训。
> 原始日志仍为最终事实来源；本摘要不替代日志。

## 一、项目主线时间线

| 日期 | 阶段目标 | 关键突破 | 代价/教训 |
|---|---|---|---|
| 10-26 | iOS 移植起步（UFS 根挂载） | UFS 数据通路打通；根设备发布；APFS 挂载 | 绑定符号漏加 prelink slide（`nm` 类型 `A`）导致 `OSTypeAlloc` 永不返回 |
| 10-29 | USB CDC 串口 | COM5 打通，首次拿到完整内核日志 | `picasso_usb_hold=1` 是调试模式（故意停在 bsd_init）；`IODelay` 在本板不返回 |
| 10-31 | 换 silenzio stage7 基线 | 构建链跑通 | `unixnu tree` 会覆盖 tree 内脚本，要改设备仓库 |
| 11-02 | stage12-lock | 构建链修复 | 交接文档 §3.1 结论有误（bootstrap 失败是预期） |
| 11-03 | 首次真机进系统 | 用户态、GPU、Data 1GiB | —— |
| 11-04 | MPSS 启动 | **PAS pas_id=4**（不是 1）、metadata=ELF 头；MPSS started | 自写 super.img 解析器错，lpunpack 一次就对（"不该造轮子"） |
| 11-05 | WCN3990 栈移植 | GLINK/QRTR/WLFW 打通；en0 attach | k20/hakios 的"WiFi"是假的（硬编码 6 BSSID + 伪造 DHCP） |
| 11-06 | CE 悬崖 → iOS16 基线 | SEC 预绑 SMR 让 CE 首次取描述符成功；nbytes=2048 修复 | 多次"静默死"被误判；UFS stall 与 WiFi 混淆 |
| 11-07 | CE 门铃死亡根因 | **WLAN 流是 0xc0 不是 0x20**；S2CR/SMR 实测表 | 三天在 0x20 上白忙；`CB_FSR=0x400` 是 LPAY format 不是故障 |
| 11-08 | 控制面全通 | 481：0xc0 有人路由后 CE→HTC→WMI 全通；487：WMI INIT 44 字段修好 VDEV_CREATE 崩溃 | 每次"前进"都出现新的 SError（固件开始真实工作） |
| 11-09 | 真扫描 → 真信标 | ec5673e tag 修复（16/17/18/19）；a9bcc66 ABI（100B/8B MAC）；chain-526 首次真扫描；**chain-531 HTT fill=1023 → 真信标**；join 序列嫁接；scan/auth/assoc 全通 | VDEV_UP 后立即 SError（当前未解） |

## 二、WiFi 验收链与状态

> 本节原本记录 2026-11-10 00:10 的状态（当时 VDEV_UP 必崩）。**该阻塞已解决**，
> 下方保留原始记录作为历史；**当前状态见 v6 交接文档 §1**。

### 当前状态（2026-11-10 20:45，摘要）

```
firmware startup ✅  scan ✅  auth ✅  assoc ✅  VDEV_UP ✅
  ── 真实 EAPOL 帧 ✅（chain-572） ── WPA2 四次握手 ⏳（chain-573 待判）
  ── 装密钥 / DHCP / rx-tx / 用户态 wifid ⬜
```

- VDEV_UP SError 根因 = 1ms 轮询里对 CE 寄存器的 un-clocked 读，`62572ba` 修复，
  chain-564/n1 起 0 PANIC 活满 220s。
- 零数据面根因 = WMI INIT `ast_skid_limit=0`，chain-572 四组 `initfix` 二分钉死；
  `initfix=3` 起固件投递 peer map / `PEER_ASSOC_CONF 0x6006` / `RX_IN_ORD` / EAPOL。
- 当前卡点 = EAPOL 以太帧 fold 偏移早 18 字节使 M1 被丢弃，`804f651`（`rxfix`）已修、
  chain-573 正在实机判决。

### 原始记录（00:10，已过时）

```
firmware startup ✅  scan ✅  auth ✅  assoc ✅  ── vdev_up ⛔(SError) ── EAPOL ⬜  address ⬜  rx/tx ⬜
```

- 扫描：chain-531 单变量确证 `fw_idx_init_val=0` 是零帧根因，fill=1023 后
  170s 收到 15 信标 / 10+ AP（`4c44e3e` 转正）。
- join：`3260e8d` 嫁接 MGMT_TX/auth/assoc/peer_assoc/VDEV_UP，`join=<n>` 门控。
- 关联：chain-535 `assoc resp status=0 aid=N`，真实 AP 接受关联。
- 当时的阻塞：`vdev_up sent` 后 1–2 秒内 SError（`Unhandled implementation
  specific error`），5 次实验（2 个不同 AP）100% 复现；SMMU 转储全干净
  （gfsr=0，cb*_fsr=0x400 是 LPAE format 不是故障）。
- 当时已排除：once 块九条、HTT RX 嫁接、PEER_SET_PARAM 255、AGGR_CFG（chain-542）、
  0x401（扫描期间也正常出现，是噪声）、AP 差异（h3=TP-LINK 也崩）。
- 已证伪：MSA 转储跨 boot 保留（chain-541 实测全零）。

## 三、反复踩过的坑（按类别，全部有日志证据）

### 平台/硬件类
1. `IODelay` 在本板不返回（UFS 时代）；所有等待用有限自旋/计数，不用时间假设。
2. `bzero`/`dc zva` 对 device memory 非法（UFS、DMA 窗口）；用 volatile 逐字写。
3. CE_HOST_IE(0x2c)/CE_MISC_IE(0x34) 在本机**只写**，读会挂死 AHB。
4. MMIO 访问未上电/未投票的块 → NoC error → TZ 转成整机复位（"静默死"真因之一）。
5. `kmem_alloc_contig` 的 min/max 是虚拟地址边界，不是物理边界。
6. 该板时间基准/时钟不可信，任何"超时"都要打折扣。
7. 抓取窗口 ≥170–200s（WLAN 初始化延迟 70s）；CDC 常 t+51s 才枚举。

### 方法论类（最贵的一课）
1. **没有控制组的测量回答没人问的问题**（HakiOS air.md）。11-09 十连全灭
   就是因为每组都带 `vdevup=1`/`scanshp=0` 共享变量。
2. **"不崩"不等于成功**：判据必须是 checkpoint 明文（STARTED/COMPLETED/
   auth resp/beacon），不是"没 panic"。
3. **"静默"不等于"卡死"**：先确认超时机制/日志通道本身是否可靠。
4. **"地址有段支撑"不等于地址正确**：空页也有段支撑（slide bug）。
5. **参考实现要挑同芯片同总线**：sheng/pong/wcn7860 全是 PCIe/MHI，方法不可移植；
   本硬件唯一 ground truth 是 ath10k SNOC（Linux）与 wlan-wip WlanRadio.c。
6. 单一变量纪律：一次只改一个 boot-arg/一个 bit；修复转正要与实验变量分开。
7. commit message 不得写未实机验证的结果（写 "Device verification pending"）。
8. 证据文件先看字节数/mtime，避免陈旧日志冒充新结果。

### 工程/流程类
1. CI 必须 `-f fast=true`（否则死在可选校验、无 boot.img）；push 不触发，
   必须 `gh workflow run`；只能推 fork（origin 是 silenzio111 无权限）。
2. cmdline 硬上限 995B；改参数需 make-NNN.py 预留槽位 + mk-args.py 等长替换。
3. 补丁套补丁：改 `picasso-usb-console-xnu.patch` 必须同步 hunk 行数，
   CRLF 会导致 `git apply` 静默截断；改前跑 `patch-hunk-check.py`。
4. 实验 TAG 换新名；旧 TAG 文件可能被 Windows 锁住，写失败会留下陈旧日志。
5. 设备被抓取结束后**留在系统里**，不会自己回 fastboot；用 `serial-cmd.py
   reboot-fastboot` 送回。
6. 磁盘：C 盘多次被 CI 产物塞满；下载/解压前先看余量。

## 四、WiFi 协议层已确证事实（避免重查）

- WCN3990 = SNOC 集成，固件跑在 modem DSP 上；AP 侧只加载 BDF + firmware-5.bin
  元数据（`11-04`）。
- WMI TLV tag：ARRAY_UINT32=16、BYTE=17、STRUCT=18、FIXED_STRUCT=19（ath10k，
  非 ath11k 的 17/18/19/20）；`ec5673e` 修正。
- `struct wmi_mac_addr` = 8 字节（2×u32 union），不是 6；START_SCAN=100B、
  VDEV_CREATE=20B、VDEV_UP=16B、BASE_MACADDR=8B、REGDOMAIN=24B。
- `abi-check.py` 是提交前门禁（Linux × trogdor × 我们三方 join，0 HARD）。
- beacon/mgmt 走 **WMI_MGMT_RX (0x7001)**，不经 HTT ring；HTT ring 只承载数据帧。
- HTT RX ring 必须预填 `fw_idx_init_val=posted`（1023），否则固件视 host 零 buffer。
- trogdor HTT 三连：FRAG_DESC_BANK_CFG(6,64B) → RX_RING_CFG(2,48B) → AGGR_CFG(5)；
  我们仍缺 FRAG_DESC_BANK_CFG（TX 数据面与 vdev_up 后的 TX 路径可能都需要）。
- 扫描前 Linux **不发 VDEV_UP**；VDEV_UP 在关联成功后（wlan-wip L4471）。
- `0x401`（HTC control msg 1025，payload `01 04 00 03 01 0{1,2} d0`）语义未解，
  公开源码无定义；扫描期也出现，只能当噪声观测。
- 固件 WLAN.HL.3.2 已知坑（2026-04 ath10k 线程）：quiet mode 命令后 ~17ms 崩、
  5GHz 主动扫描不调谐需被动、TxBF on no-peer、CE3 race（已修）。

## 五、当前未解问题与下一步

1. **vdev_up 后 SError**（当前主攻）：chain-543 隔离中；若 vdev_up 定案，
   下一步补 FRAG_DESC_BANK_CFG（HTT 三连最后一条）后复测。
2. EAPOL/四次握手：硬依赖目标 AP 密码（环境里全是 RSN，无 open AP）。
3. 数据面 TX：wj_tx_put/send/drain/compl 未嫁接；RX 已嫁接但固件从未投递帧。
4. 用户态：board.toml 的 wifid 放行 + runtime-jobs SHA + hakios.wlan.cpp 候选类
   降级，本地已改未提交（§6 交接 v4）。
5. 清理：`acbcf84..c4f2105` 无效 SMMU 诊断代码（含每次启动 SCM SMC 调用）。

## 六、日志索引（按主题）

- 平台/UFS/USB/显示：10-26、10-29、10-31、11-02、11-03
- modem/PAS/GLINK/WLFW：11-04、11-05
- CE 悬崖与 SMMU 长战：11-06、11-07、11-08
- 扫描/ABI/tag/HTT fill/join：11-09（顶部有 0x20 结论撤回声明）
