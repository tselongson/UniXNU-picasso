# WiFi (WCN3990) 项目交接文档 — 2026-11-11 03:25（v9：WiFi 可开机进系统；设置侧待刷 Data）

> **⚠️ 本文件已归档。当前权威状态源是
> `handoff/wifi-wcn3990-handoff-2026-11-11.md`（v10，2026-11-11 14:00）。**
> v10 修正了本文对「设置里 Wi-Fi 必须刷 5.87 G userdata」的结论：
> 改 `runtime_enabled_jobs` 可以保持 Data 身份不变，只刷 boot 即可。
>
> 以下为 v9 原文，仅作历史参考。
> **本文件是当前唯一权威状态源。** 其它 `docs/*.md`、`README.md` 里的阶段状态均为历史归档，
> 与本文冲突时以本文为准。
> 前序：`wifi-wcn3990-handoff-2026-11-09.md`（v4）、本文 v5/v6（20:45，已被本版取代）、
> `handoff/2026-11-10-project-history-digest.md`（11 份日志通读摘要）。
> **v8 变更**：chain-580 零参数开机完成握手并把真实 ARP/IPv4/IPv6 帧交给
> Darwin 协议栈（`msdu=21 bad=0`，`en0` up）；chain-581 单变量证伪
> "logo-hold 导致停 logo"，真因是 `IOMobileFramebufferLegacy` 的 provider
> 不是 `AppleARMIODevice`。模块已合并到上游最新分支。
> 目标仍是**开机即联网、可日用**，但"设置里联网"还隔着显示与 wifid 两条线。
> 事实来源：`.workbuddy-ai/memory/2026-11-09.md` / `2026-11-10.md`、`out/full-flash/logs/*.cap`、
> `work/chain-5*.log`、本地 `work/refs/ath10k/` 与 `work/trogdor/port/WlanRadio.c`。
> 判据口径见 `wifi-wcn3990/docs/diagnostic-charter.md`：**checkpoint 明文，不是「0 panic」**。

## 0. 一句话现状

```
firmware startup ✅ scan ✅ auth ✅ assoc ✅ VDEV_UP ✅ EAPOL 帧 ✅
  ── WPA2 四次握手 ✅ 装 PTK/GTK ✅ PEER_AUTHORIZE ✅
  ── 开机零参数自动联网（驱动侧）✅ chain-580：msdu=21 bad=0，en0 up
  ── 真实 RX/TX 到 Darwin 协议栈 ✅（ARP/IPv4/IPv6 已交付）
  ── DHCP 地址 ⏳（configd 尚未对 en0 发起）
  ── 桌面/设置 ⛔（framebuffer provider 不匹配，见 §11）
  ── 用户态 wifid ⬜
```

**驱动侧的"开机自动连上 AP 并收发真实帧"已经打通；"从设置联网"还差
显示与 wifid。**
chain-573 卡 M3 的真因是**凭据与 SSID 不配对**（密码属于
`ChinaUnicom-33E6TD`，实验却选了 `TP-LINK_chang`），不是代码 bug。
chain-580 用只带 `wifiauto=1` + `wpass` 的镜像完成：周期重扫 → 选中正确
AP → auth/assoc/VDEV_UP → M1/M2/M3/M4 → 装 PTK/GTK → PEER_AUTHORIZE，
随后 `msdu=3→21`、`bad=31→0`，`en0` attach 并 link up。
当前 HEAD `5187ba4`（kd439-pool）；另有合并分支
`wifi-wcn3990-stage12` @ `2cb67cd` 基于上游最新 `00ea4a0`，CI 已 success。

## 1. 验收链逐 checkpoint

| checkpoint | 状态 | 实机证据 |
|---|---|---|
| firmware startup（MPSS/WLFW/CE/HTC/WMI READY） | ✅ | CE/HTC/WMI 全通，SMMU 流 `0xc0` |
| scan（真实 BSS 列表） | ✅ | chain-531：`httidx=1023` → 10+ HEARD BEACON、8 SSID；k1 对照 `mgmtev=0` |
| auth | ✅ | `auth resp status=0`，MGMT_TX `status=0` |
| assoc | ✅ | `assoc resp status=0 aid=N`（chain-535 起） |
| VDEV_UP 不崩 | ✅ | chain-564/n1：`assoc=0 → vdev up sent → 0 PANIC`，活满 220s。根因是 CE 寄存器 un-clocked 读 |
| 数据面 / EAPOL 帧到达 | ✅ | chain-572 v3/v7：`peer mapped`、HTT `type=3`/`type=18`、`WMI PEER_ASSOC_CONF 0x6006`、`ord/msdu/eapol=4~5`、`httidx` 1023→1027 |
| EAPOL M1 正确解析 / M2 发出且被 ACK | ✅ | chain-573 w1：`ver=1 type=3 info=008a`、`msg1 seen`×4、TX_COMPL status=0 |
| **WPA2 四次握手 + 装 PTK/GTK + PEER_AUTHORIZE** | ✅ | **chain-574 x0**：`M3 info=13ca` → M4 131B → `install pairwise/group key` → `msg3 seen, msg4 sent, keys installed, peer authorized`，AP 未 deauth |
| **真实 RX/TX 数据帧交给协议栈** | ✅ | **chain-580**：`ord=20 msdu=21 bad=0`，`en0 attached ... link up`，解密的 ARP/IPv4/IPv6 已交 `hakios_net_input()` |
| 开机零参数自动联网（驱动侧） | ✅ | chain-580：args 只剩 `wifiauto=1` + `wpass`，`rescan`×3 后选中正确 AP 并完成握手 |
| DHCP 地址 / 用户态 wifid | ⏳/⬜ | `configd` pid=34 在跑但未对 en0 发起 DHCP；wifid 三处本地改动未提交 |

## 2. 今天定位并解决的根因（按发现倒序）

### 2.1 零数据面 = WMI INIT `ast_skid_limit=0`（chain-572，最大突破）
`work/trogdor/port/WlanRadio.c wm_send_init()` 的 `cfg[46]` 里 `ast_skid_limit=16`，
我们 `g_wmi_cfg_full[44]` 的 `cfg[6]=0`。WCN3990 full-RX-reorder 固件没有 AST 表
就不给 peer 建数据路径。四组 `initfix=0/1/3/7` 单变量二分：

| 组 | initfix | 结果 |
|---|---|---|
| v0 | 0（现状） | `peer map timeout`、`ind=ord=msdu=eapol=0`、NO DATAPATH |
| v1 | 1（offload+reorder 4/4） | 同上，无效 |
| **v3** | **3（+ast=16）** | **PASS**：`peer mapped`×2、HTT type=3 ×2、type=18(RX_IN_ORD) ×4、0x6006、EAPOL×4 |
| v7 | 7（+wds=2） | PASS，与 v3 无实质差别 → wds 非必需 |

`initfix=2`（单独 ast）未单独跑，但 v1 vs v3 已把元凶锁在 `cfg[6]`。
**`initfix` 目前仍是 opt-in boot-arg，尚未转正默认。**

### 2.2 EAPOL M1 解析偏移（804f651 / `rxfix`，待验证）
v3 收到 4 个 EAPOL 帧但帧头是垃圾 `ver=247 type=28 code=96`，M1 被
`hif_wpa_handle_eapol()` 的 `e[1] != 3` 丢弃，AP 等不到 M2 发 `deauth reason 6`。
逐行对照 wlan-wip `wr_msdu()`：decap==1 时参考把以太帧折到
`frame+32-14 = frame+18`，EAPOL payload 在 `frame+32`（24B 802.11 头 + 8B LLC/SNAP）；
我们的 `eth` 留在 `frame+0`，`eth+14` 落在 802.11 头中段，**早了 18 字节**。
`rxfix` bit0 = 采用参考布局，0 保留旧行为作单变量对照。
TX 侧（`hif_wpa_reply` 帧构造、`hif_wpa_tx_frame` 的 HTT TX_FRM 头 peer=0xffff/freq=0）
与 `wj_tx_put`/`wj_tx_send` 逐字一致，无需改。

### 2.3 VDEV_UP SError（62572ba，已实机闭环）
1ms 轮询无条件读 CE 主体寄存器（`CE_CURRENT_DRRI 0x48` 等），撞上固件把空闲 CE
置入低功耗的窗口 → CPU 侧异步外部中止 → ARM64 SError（`arm64_platform_error`，
ESR EC=0x2F，SMMU gfsr=0 干净）。上游逐字背书：Douglas Anderson
"reading from their address space will cause a full system crash"（WCN3990 SNOC 实测）、
`ce.c:1237-1248`、Govind Singh SRRI/DRRI→DDR。修复 = 运行期零 CE 主体寄存器读、
CE_STATUS ACK 收进 `nbytes!=0` 分支、repost 走 always-on shadow doorbell。
**v5 交接文档说它「尚未实施」，已作废。**

### 2.4 HTT index-page 扫描 bug（be91f28，真实但非主因）
`htt_rx.c:151/209` 证明 `g_dma->htt_alloc_idx` 是 **host refill 指针**（`fw_idx_shadow_reg_paddr`
只是它的物理地址，`htt_tx.c:940`），固件不写它。旧 `hif_service_htt_ring()` 拿它当固件进度，
第一次调用把 `rd` 推到 1022 后 `rd==fw_idx` 恒成立、循环永不进入。
chain-571 u0=`ring=0/1023`、u1（attention-driven 修复）=`ring=0/0`：
**修好后固件仍写一个 MSDU，干净证伪「host 没看」是零帧主因**，但 bug 确实存在并已修。

### 2.5 更早（531→545，已转正默认或已解决）
`fw_idx_init_val=0` → HTT fill=1023（chain-531）；START_SCAN ABI 100B/8B MAC、
TLV tag 16/17/18/19、REGDOMAIN 24B、WMI INIT 44 字段；VDEV_START + PEER_CREATE
前置序列（chain-533/534）；assoc RSN IE 全零导致 `status=10`（7514187）；
RSN group cipher 导致 `status=41`（5c80c69）。

## 3. 已判决与在跑的实验

### 3.1 chain-573（已判决）：M1/M2 通，M3 不来是凭据错配
- w0 `rxfix=0`：帧头仍垃圾 `ver=51 type=232`；w1 `rxfix=1`：
  `ver=1 type=3 len=005f desc=2 info=008a`、`WPA: msg1 seen`×4、
  `WPA: tx frame 153B`×4、`HTT type=7` ×4（`07 00 01 00 <slot> 00 ff ff`），
  **无 M3**，最后 deauth reason 6。
- 按 wlan-wip `wj_tx_compl()` + 本地 `htt.h:471`（10.4 `TX_COMPL_IND=0x7`）：
  `p[1]&7=0` 即 **ACK**，M2 确实发出。w1.fi 官方 `wpa_common.h` 与本地
  hashlib 复算都证明 MIC/PMK/PTK 实现与标准一致；双方 `wk_reply`/`hif_wpa_reply`
  逐字相同 ⇒ 不是代码 bug。
- **真因**：`2026-11-09.md:964` 记着凭据是 `ChinaUnicom-33E6TD` / `REDACTED`，
  而 `joinsel=4` 选中 `TP-LINK_chang`。密码错 → M2 MIC 错 → AP 静默丢弃。

### 3.2 chain-574（已判决）：四次握手闭环 ★
- 同镜像 804f651，单变量 `joinsel=4`(x0) ↔ `joinsel=1`(x1)，995 B 等长，各 220s。
- **x0** 恰好扫到 `ChinaUnicom-33E6TD`：M1 → M2 → **M3(193B, info=13ca)** →
  M4(131B) → `install pairwise key idx=0 len=16 cipher=4` →
  `install group key idx=2 len=32 cipher=2` → `JOIN: peer param 3=1` →
  `WPA: msg3 seen, msg4 sent, keys installed, peer authorized`。AP 未踢人。
- x1 无效（那轮只 6 个信标，严格等待 ChinaUnicom 没等到）。
- 剩余：`ord=22 msdu=2 bad=21` —— 装 key 后加密帧被 `hif_wr_msdu()` 拒收。

### 3.3 chain-575（已判决，AP 环境无效但取到关键证据）
- 两组 `joinsel=4` 都选中 `1902_WIFI_CLOCK_k4_Ga0cea5`（密码不匹配）；
  y0 没 M3、y1 auth 后零 EAPOL ⇒ `netpath` 未被验证。
- **`dpdiag` 抓到真实 bug**：同一 M1 被重复投递 14 次（ANonce 全 `818a13a5`），
  `DP: bad r=3 attn=82300043`，hexdump 显示 `aa aa 03` 被 BSSID 覆盖。
  固件把同一个 DMA buffer 通过多条 RX_IN_ORD 重复投递，而我们的
  `rxfix=1` 折叠**原地修改 `buf+360`**，第二次读到损坏的 802.11 头。
  参考 `wr_msdu()` 是先 `memcpy` 到静态缓冲再折叠 ⇒ `b7e350e` 已修。
  ⇒ `bad=N` 是重复投递污染，**不是加密帧解密失败**。
- `wpadiag` 也验证有效：M1/M2 全帧 hexdump + PMK/KCK/MIC 指纹可离线复算。

### 3.4 chain-576（已判决）：开机零参数**失败在两个真 bug**，已修
- args 成功剥到只剩 `wpass=REDACTED wifiauto=1`（995 B）。
- 日志：`JOIN: connect requested, ssid_len=18 passlen=0` ⇒ **密码被清空**。
  根因：`wifiauto` 把 `g_wpa_pass` 同时当源传给 `wcn3990_hif_connect()`，
  函数第一句 `memset(g_wpa_pass,...)` 把源也擦了。`bf0e5cf` 先存局部再清。
- `CE3 tx buffers: single (legacy)`；`autoscan: WMI ready, issuing START_SCAN`
  和 100B 命令都打了，但 `beacons=0 mgmtev=0 htt=1` ⇒ START_SCAN 被后续
  WMI 命令覆盖（CVE-2022-50700 同型）。`97a55ce` 把 `g_tx3_rotate=1` 并入
  wifiauto 默认。
- 这两个都是"日用默认集"缺项，不是链路本身倒退。

### 3.5 chain-579/580/581（已判决）：驱动侧数据面打通，显示真因定案
- **chain-579**（`bf0e5cf`，args 只剩 `wifiauto=1` + `wpass`）：
  `connect requested ssid_len=18 passlen=9`、选中 `ChinaUnicom-33E6TD`、
  auth/assoc/VDEV_UP、M1→M4、装 PTK/GTK、`peer authorized` 全通；
  但 `bad=31`，`dpdiag` 显示 `r=1 attn=90300047`，帧是**已解密的广播
  ARP/IPv6**，fatal 位（27/29/30/16/17）全清 ⇒ bit28 是 TKIP 组密钥特有，
  不该当错误。`5187ba4` 在真实数据面忽略 bit28（`netpath=0` 保留旧掩码）。
- **chain-580**（`5187ba4`）：零参数开机，`wifiauto: rescan`×3 等到目标 AP，
  握手全通；心跳 **`msdu=3→21`、`bad=31→0`**、`ord=20`、`ring=1/1`；
  `en0 attached ... link up`、`configd` pid=34 在跑。`REAL DATA PLANE`/DHCP
  未出现 ⇒ `configd` 还没对 en0 发起 DHCP（用户态，不是驱动）。
- **chain-581**（同 `boot-580.img`，单变量 `picasso-boot-logo-hold=1↔0`）：

  | 组 | verify_swap failed | provider 错误 | GX | SpringBoard | WiFi |
  |---|---|---|---|---|---|
  | h8 hold=1 | 2021 | `provider was not AppleARMIODevice` | not powered | 在跑 | join=5 msdu=22 bad=0 |
  | h9 hold=0 | 2037 | 同上 | 同上 | 在跑 | join=5 msdu=22 bad=0 |

  两组逐项几乎相同 ⇒ **`picasso-boot-logo-hold` 根本没轮到生效**，
  `verify_swap` 在 hold 逻辑之前就失败。真因是苹果
  `IOMobileFramebufferLegacy::start` 要求 provider 是 `AppleARMIODevice`，
  我们的显示节点不匹配 → start 失败 → 每帧 swap 校验失败 → 无帧上屏。
  `hold=0` 组可能擦掉 logo（用户反馈"没 Logo"），但同样没有 UI。

> **CI 并发陷阱**：同一 workflow 同一 ref 连续派单会把上一个 pending run
> `cancelled`（`42831388403`/chain-577 因此作废）。一次只派一个 CI，
> 等它 completed 再派下一个。

## 4. 启动参数速查（接手第一件事先看这里）

**已知良好基线**（chain-572 v3 打通数据面的配置）：

```
chanlist=1 httsetup=1 httconn=1 wcn3990-hif-autoscan=1 scanfix=127
join=2 httring=7 joinsel=4 joinskip=0 wpa=1 dpfix=5 authz=0 initfix=3
rxfix=<0|1>        # chain-573 的唯一变量
```

| 门控 | 含义 | 已验证结论 |
|---|---|---|
| `scanfix=127` | WMI/HTT ABI 修复位；bit6=0x40 是 7 条 PDEV_SET_PARAM | 必需；上限 127 |
| `httring=7` | HTT 三连 FRAG_BANK/RX_RING/AGGR_CFG | 必需 |
| `initfix=3` | bit0 offload+reorder(4/4)、bit1 ast=16、bit2 wds=2 | **bit1 是零数据面元凶**；尚未转正默认 |
| `dpfix=5` | bit0 index-page 收 MSDU、bit2 attention-driven 排水 | bit2 修真实 bug 但非主因 |
| `rxfix=1` | EAPOL 以太帧 fold 到 frame+18 | chain-573 w1 已验证（默认已转正） |
| `netpath=1` | RX 交给 `hakios_net_input()`、TX 走 HTT TX_FRM | chain-575 待判；0 保留伪造 DHCP/ARP 对照 |
| `wpadiag=1` | M1/M2 hexdump + PMK/KCK/MIC 指纹（不含明文密码） | 离线复算 MIC 用 |
| `dpdiag=1` | 打印前 12 个被拒帧的 attn/start/len/decap | 定位 x0 的 `bad=21` |
| `wifiauto=1` | 已验证门控全部转默认 + 开机自动发连接请求 | chain-576 待判；显式 boot-arg 仍可覆盖 |
| `authz=1` | VDEV_UP 后提前 PEER_AUTHORIZE | chain-570 t1 因 `assoc status=17` 无效，**未验证** |
| `joinsel=4` | 任意可见 2.4G AP（避免依赖单一路由器） | 572 选中 `TP-LINK_chang`；1=ChinaUnicom、2=时钟 AP、3=5G |
| `wpa=1` / `wpass=` | WPA2 状态机与密码 | `wpass=REDACTED` 在默认槽位，**对应哪个 SSID 未确认** |
| `join=N` | 1-based BSS 表条目；≥2 走完整前置序列 | `join=1` 旧路径对照 |

**cmdline 是硬上限 995 B**，改参数必须等长替换：
`python work/make-545.py <CI img> <slotted img>` 打槽 →
`python work/mk-args.py <slotted> <out img> "<tokens>"` 重排。
新 CI 镜像带 board.toml 原始装饰 token，**必须先打槽**；旧镜像已打过槽，再打会
1105>995。两条流水线槽位不同，跨构建 A/B 要逐字节复制 995B args 区。

## 5. 实验链索引（531→573）

| 链 | 变量 | 结论 |
|---|---|---|
| 531 | `scanfix=31/63` | HTT fill=1023 → 首次真信标；单变量 A/B 成立 |
| 532c | `join=0/1` | 补 `scanfix=63` 后配置修正，join 状态机跑通 |
| 533–535 | VDEV_START/PEER_CREATE、RSN IE | auth/assoc 打通；`status=10` 修复 |
| 536–540 | once 块九条参数 | 证伪 once 与 panic 无关；auth status=3/0 非确定；assoc=41 是 AP 会话残留 |
| 561–563 | ESR dump / AP 冷却 | AP 静默（TXFILT=3）导致多组无效实验 |
| 564 | 构建（62572ba） | **VDEV_UP SError 消除**，0 PANIC 活满 220s |
| 566/567 | 7 条 PDEV 参数 + AGGR_CFG、dpfix | 参数补齐证伪；d1 证明 HTT ring 8 个 buffer attn 全 0 |
| 570 | `authz=0/1` | t0 走到 vdev_up 但零帧；t1 assoc=17 无效；`srri3 25→29` 证明 PEER_CREATE 被消费 |
| 571 | `dpfix=1/5` | `ring=0/1023` vs `0/0`，证伪 host 收包为主因；`SVC_AVAIL` bit143=1 |
| **572** | `initfix=0/1/3/7` | **ast_skid_limit 是元凶；v3/v7 首次收到 EAPOL 帧** |
| 573 | `rxfix=0/1` | w1 M1 解析成功、M2 被 ACK，但 AP 不回 M3（凭据/SSID 错配） |
| **574** | `joinsel=4/1` | **x0 四次握手 + 装 PTK/GTK + PEER_AUTHORIZE 闭环**；x1 AP 不在场无效 |
| 575 | `netpath=0/1` | 两组都选中时钟 AP（密码不匹配），无效；`dpdiag` 抓到 `r=3` 重复投递 |
| 576 | `wifiauto`（剥全部实验参数） | `passlen=0`（密码自赋值）+ `tx3=0`（START_SCAN 被覆盖），两个真 bug |
| 579 | `wifiauto`（含修复） | 握手全通；`bad=31`，`r=1` 帧是已解密 ARP/IPv6（bit28） |
| **580** | `wifiauto` + bit28 修复 | **`msdu=21 bad=0`，`en0` up，真实帧进协议栈**；DHCP 待用户态 |
| **581** | `logo-hold=1/0` | **两组几乎相同 → hold 证伪**；真因 `IOMobileFramebufferLegacy` provider 不匹配 |

## 6. 铁律与坑（都有日志代价）

1. **「不崩 ≠ 成功」**：本批多个 0 PANIC 组根本没走到 vdev_up（538 auth=3、543/545 assoc=41）；
   判据必须是 checkpoint 明文。
2. **单变量 A/B + 控制组 + 220s 采集**；HIF 约 t+94s 才启动，短抓等于没抓。
3. **触发点 ≠ 根因**：485「VDEV_CREATE 唯一触发点」→ 486b「固件开始实际工作就死」
   → 本轮「VDEV_UP 是充分条件，真因是触发后的主机轮询读」。
4. **实验链 BASE 必须与已知良好点逐 token 对照**：chain-532b 漏 `scanfix=63`
   让两组全崩在旧 ABI SError，白费两次 170s。上机前看镜像实际 args 与 `WMI: send` 长度行。
5. **组名跨日复用**：`boot-exp-*.img` / `console-exp-*.cap` 必须先删，否则拿旧日志误判
   （chain-531 撞过）。`chain-533` 还撞过 Windows 文件锁 `PermissionError`，
   换没用过的 TAG 最省事。
6. **改 patch 文件只能脚本改并本地验 hunk 行数**（两次 CI `patch does not apply`）；
   **同名函数先查全部调用点**（905c667 误判 `hif_send_vdev_up` 后 revert）。
7. **CE_HOST_IE(0x2c)/CE_MISC_IE(0x34) 只写**，读挂死 AHB；已写进
   `diagnostic-charter.md` §5。
8. **CI 必须 `-f fast=true`**：否则 host-check `Check iOS 16 framebuffer and IOSurface adapters`
   失败、compose 被 skip、没有 boot.img（run 42602597855 的教训）。push 不触发 CI，
   要 `gh workflow run compose-bootimg.yml --ref kd439-pool -f fast=true`。
9. **提交前 `python3 work/abi-check.py` 必须 0 HARD**；commit message 未验证就写
   「Device verification pending」，绝不写「已在 chain-N 验证」。
10. **实验后设备不会自己回 fastboot**：`work/ensure-fastboot.sh` 或
    `python work/serial-cmd.py COM5 reboot-fastboot 3`；实验链 `boot-variants.sh` 内已处理。
11. **外网 WebFetch 整份 3000+ 行 Linux 源文件会空返回并阻塞主线**：本地
    `work/refs/ath10k/`（20 文件含 `mac.c`/`htt_rx.c`/`wmi-tlv.c`）、
    `work/refs/ath10k-full/`（`htt.c`/`htt_tx.c`）、`work/trogdor/port/WlanRadio.c`（5627 行同芯片
    真机跑通）就是权威一手资料，优先本地、外网只补缺。
12. C 盘反复满（清理走永久删除，勿堆回收站）；AP 反复关联会静默（TXFILT=3），
    上机前先看 .cap 的 BSS 表。

## 7. 本地可复用资源

| 资源 | 位置 | 用途 |
|---|---|---|
| 同芯片完整栈（ground truth） | `work/trogdor/port/WlanRadio.c` 5627 行、`work/trogdor/coachz_wifi.c` | `wm_send_init`（WMI INIT 44/46 字段）、`ws_htt_next`（HTT 三连）、`wr_msdu`/`wr_in_ord`/`wr_post`、`wj_eapol`/`wk_reply`/`wj_send_key`、`wj_step` 状态机 |
| Linux 主线 | `work/refs/ath10k/`、`work/refs/ath10k-full/` | `mac.c:735 ath10k_peer_create`+`wait_for_peer_created`、`htt_rx.c:151/209` alloc_idx 语义、`htt_tx.c:940` fw_idx_shadow、`wmi-tlv.h` 枚举验算 |
| CE 服务框架 | git 分支 `ce-irq-service` | `hif_ce_service`@1475、`hif_ce_irq`@1399（只写版）、`hif_ce_attach_irqs`@1441；**注意 CI:1168 全扫 fallback 要删** |
| 工具 | `work/abi-check.py`、`work/make-545.py`、`work/mk-args.py`、`work/symbolize.py`、`work/ensure-fastboot.sh`、`work/boot-variants.sh`、`work/serial-cmd.py` | 门禁 / 打槽 / 改参数 / 符号化 / 回 fastboot / 上机抓取 |

## 8. 下一步决策树（按 chain-580/581 结果）

1. **chain-575 y1 出现 `REAL DATA PLANE`、真实 DHCP offer/ack、心跳 `msdu` 增长**
   → 真实数据面通，进入 chain-576 的开机零参数验收。
2. **y1 仍 `bad=N` 且无数据帧** → 读 `DP: bad r=<n> attn=... start=... len=...`：
   - `r=1`（attention 错误位，常见 decrypt err / TKIP MIC err）→ 按 wlan-wip
     `wj_step` 的 `WJ_M4` 形态，把装 key + PEER_AUTHORIZE 延后到 **M4 的
     TX_COMPL 确认（或 300ms）之后**；现版本是 M4 一发出就装，是唯一与
     同芯片真机跑通实现不同的时序。
   - `r=3`（LLC/SNAP 检查失败）→ 加密帧是 QoS 头（26B）而代码按 24B 判
     `frame[24]==0xaa`，需按 QoS 偏移折叠。
   - `r=2/4` → 长度或 decap 类型，按 `start` 字段重新核对。
   **取证后再改，不要猜。**
3. **chain-576 z1 完成握手 + 拿到地址** → 把 `wifiauto` 默认改为 1，
   清掉 `PicassoWlan.cpp` 里作为 A/B 对照保留的伪造 DHCP/ARP 分支。
4. 用户态三处本地未提交改动（`board.toml` 放行 `com.apple.wifid` /
   `patches/picasso-runtime-jobs-xnu.patch` 两个 SHA /
   `wifi-wcn3990/source/hakios.wlan.cpp` 的 `IO80211Interface` 候选类降级）仍在
   工作区，**派 CI 必须带 `-f create_data_seed=true`**。验收 = 串口出现 `wifid`
   的 `SCAN_REQ`/`SCAN_RESULT` 且返回真实 BSSID；`picasso_sta_join()` 现已把
   请求真正下发给 HIF。
5. 清理：工作区 5 个 evidence 日志删除（勿动）；`acbcf84..c4f2105` 六次基于已证伪
   SMMU 假设的诊断代码待清理（其中 `a74a4e8` 每次启动发 SCM SMC 调用）。
6. 凭据与 SSID 必须成对：`wcn3990_hif_connect()` 已把两者绑定，
   不要再用 `joinsel=4` 做握手实验（会选到别的 AP 导致 MIC 错）。

## 9. 遗留未完成线（WiFi 之外）

GPU 用户态 `NativeAdrenoMetal.bundle`（`backboardd` 仍 `match=<absent>`）、
持久化 userdata、电池/充电、背光、音频与基带；
`src/YinCQL_UniXNU-picasso` 是另一条停在 APFS 容器探测的移植线。
`wifi=1` 组故意停在 logo（`picasso-boot-logo-hold=1`），`wifi=0` 才走完显示路径；
logo 消失不是版本倒退，是显示管线本来就缺 Metal addon/provider 不匹配。

## 10. commit / CI 速查（11-10）

| commit | 内容 |
|---|---|
| `62572ba` | 停止 1ms 轮询里的 un-clocked CE 寄存器读（VDEV_UP SError 根因） |
| `1ae19a1` | WMI/HTT 前扫描序列对齐 trogdor、钉 2.4G |
| `cab11cd` / `7dee28b` | index-page 收 MSDU；修 repost 空转 |
| `447d45f` | `authz` 提前 PEER_AUTHORIZE + HTT 全量诊断 |
| `be91f28` | attention-driven HTT ring 排水 + 扩展服务位/peer 事件/HTT payload 取证 |
| `82af5d7` | `initfix` 门控 WMI INIT 缺失的 peer/reorder 资源 |
| `804f651` | `rxfix` 修 EAPOL 以太帧 fold 偏移 |
| `a2b20a3` | 转正 `initfix=3`/`rxfix=1`/`dpfix=5`，解码 TX_COMPL_IND + slot 回收，`wpadiag` |
| `6cbea2e` | `netpath=1` 真实 RX/TX 数据面、TX 槽 4→8、`dpdiag` |
| `1373414` | `wifiauto` + `wcn3990_hif_connect()` 请求通道 + `picasso_sta_join()` 不再造假 |
| `b7e350e` | `hif_wr_msdu()` 先 memcpy 出 DMA buffer 再折叠（对齐 `wr_msdu()`，修 `r=3`） |
| `50c0e8b` | wifiauto 目标 AP 未出现时每 ~10s 周期重扫 |
| `97a55ce` | wifiauto 默认 `g_tx3_rotate=1`（CE3 轮换，CVE-2022-50700 同型） |
| **`bf0e5cf`** | **修 `wcn3990_hif_connect()` 的密码自赋值（`passlen=0`）（HEAD）** |

成功 CI：`42762577851`(22a09a5+ESR)、`42784441239`、`42788077607`、
`42792560159`、`42797113739`、`42806630640`(be91f28)、
`42812054681`(82af5d7)、`42817637471`(804f651)。
在跑：`42827775622`(6cbea2e，chain-575)、`42829185358`(1373414，chain-576)。
`42826130797`(a2b20a3) 已取消。
失败参考：`42747390508`(编译错)、`42750125164`/`42750268015`(patch hunk)、
`42751390482`(DISR_EL1 读)。

## 11. "开机联网 / 日用" 的验收标准与剩余差距（用户最终目标）

**用户口径**：开机看到苹果标 → 进桌面 → **在「设置」里选 WiFi 并联网**，
这才叫日用；跑通握手不等于可用。

| 环节 | 状态 | 事实 / 阻塞 |
|---|---|---|
| 开机苹果标 | ⚠️ 不可靠 | `hold=1` 时保留 logo；`hold=0` 时可能被擦成黑屏（chain-581 h9）。**它不是启动成功的判据**，串口 `Early boot complete` 才是 |
| 进桌面 / SpringBoard / 设置 | ✅（g10） | **根因是打槽工具**：`work/make-545.py:31` 把 `offline-legacy-validate=1` 当槽位换成 `joinskip=0`，`validateSwap()` 在 `enabled!=1` 时直接 false → 每帧 `verify_swap failed`。补回后 **g10（wifi=1 + wifiauto + validate）`verify_swap failed=0`、SpringBoard 在跑、用户确认有 logo 并进系统**。`provider was not AppleARMIODevice` 在能上屏的 g7 里同样存在，**非致命**；`GX is not powered` 只影响 Metal/GPU 加速 |
| 设置里选 WiFi | ⬜ | 依赖上一行 + `com.apple.wifid` 放行（`board.toml`）、`patches/picasso-runtime-jobs-xnu.patch` 两个 SHA、`hakios.wlan.cpp` 的 `IO80211Interface` 候选类降级（三处本地未提交） |
| 固件 / scan / auth / assoc / VDEV_UP | ✅ | 全部实机闭环 |
| WPA2 四次握手 + 装 PTK/GTK + PEER_AUTHORIZE | ✅ | chain-574 x0；chain-580 零参数复现 |
| 真实 RX/TX 数据帧交给协议栈 | ✅ | chain-580：`ord=20 msdu=21 bad=0`，`en0 attached ... link up`，ARP/IPv4/IPv6 已交 `hakios_net_input()` |
| 开机零参数自动联网（驱动侧） | ✅ | chain-580：args 只剩 `wifiauto=1` + `wpass`，`rescan`×3 后自动握手 |
| DHCP 地址 | ⏳ | `configd` pid=34 在跑，但没对 en0 发起 DHCP；`REAL DATA PLANE` 未出现 |
| 凭据持久化 | ⬜ | 现在 `wpass=REDACTED` 是 boot-arg 明文，日用应由 wifid/系统钥匙串提供 |

**结论**：驱动侧已达到「零参数开机 → 自动连上正确 AP → 四次握手 → 装密钥
→ 真实 ARP/IPv4/IPv6 进入 Darwin 协议栈 → en0 up」。
**还没达到「设置里联网」**，中间隔着两条独立的线：
1. **显示线**（当前最大阻塞）：framebuffer provider 不匹配，SpringBoard
   无法上屏；`GX is not powered` 让 Metal 路径也不可用。
2. **用户态线**：`configd` 对 en0 的 DHCP、`com.apple.wifid` 放行、
   Apple80211 接口类、凭据持久化。
两条都不是 WCN3990 驱动单模块能闭环的；苹果标问题属于第 1 条。

### 11.1 环境依赖（每轮实验前确认）
- `ChinaUnicom-33E6TD` 的 2.4G 必须在场（密码 `REDACTED`）。近期多轮扫描
  它时有时无，导致 574 x1 / 575 两组无效。可用 `wifissid=<ssid> wpass=<pass>`
  换成任意 SSID/密码都已知的 2.4G 热点；50c0e8b 起会每 ~10s 重扫等待它出现。
- 不要用 `joinsel=4` 做握手实验：会选到别的 AP 导致 MIC 错、M3 不来。

## 12A. 2026-11-11 凌晨：显示/CPU/scan 三个根因与当前可用配置

### 12A.1 显示"卡住"的真因：打槽工具吃掉了 offline-legacy-validate
- `work/make-545.py` 原来用 `(offline-legacy-validate=1 -> joinskip=0)` 作槽位，
  把所有实验镜像的**显示校验总开关**换掉了。`NativeLegacyDisplay::validateSwap()`
  在 `enabled!=1` 时直接 `return false` → SpringBoard 每帧 `verify_swap failed`
  → 一帧都上不了屏。chain-546 起全部中招，之前 WiFi 崩得早没暴露。
- 修复：槽位改用 `picasso-buttons=1`；`.gitattributes` 锁 patch 为 LF。
- 对照（同设备）：g7 旧基线 437 + wifi=0 → 进桌面；g8' 当前 580 + wifi=0 无
  validate → 6155 次 verify_swap failed、黑屏；g9 补 validate → 有 logo 能进系统。

### 12A.2 WiFi 开着会饿死 UI：HIF 初始化与合成器争 CPU
- g13（当前内核 + 旧完整 args + wifi=0）：FBUC 120、SpringBoard 226，进系统。
- g14（只改 wifi=1，WiFi 都没连上）：FBUC 33、SpringBoard 34，进不去。
  ⇒ 与数据面无关，是 `wcn3990_hif_poll()` 初始化阶段的 CPU 占用。
- `netpoll=6`（k8）无效；`wifiauto=0`（k9/k10）仍饿死（HIF 没 READY 也重）。
- **`wlan_delay_ms` 实测**：120 s 仍饿死（FBUC 29）；**150 s 正常（FBUC 160）**。
  `HakiosCompat.cpp` 默认已改 150000。
- k13（150 s + wifiauto，400 s）：UI 正常、扫描 873 beacon、`WPA: msg3 seen`；
  但用户反馈"完全没反应"。

### 12A.3 "完全没反应"的代码级根因：scan 在 ioctl 里睡 3 秒
- `PicassoWlan.cpp picasso_sta_scan()` 末尾 `for (i<30) IOSleep(100);`，
  在调用者 ioctl 上下文里阻塞 **3 秒**；`hakios_sta_ask_scan()` 是同步的。
  设置里一点 Wi-Fi/扫描就挂住 SpringBoard/configd；`wifiauto` 也走同一路径，
  所以 k13/k14 卡、k12（无 wifiauto）不卡。`PICASSO_IPC SpringBoard` 积压是后果。
- 修复 `9ead7fc`：等待改 5×100 ms（500 ms），扫描在 modem 线程继续。

### 12A.4 当前可用配置（k12，用户确认能进系统）
```
wifi=1  picasso-boot-logo-hold=1  offline-legacy-validate=1
sheng-vsync=60  sheng-banner=0          # wifiauto 关闭，避免 scan 阻塞
（wlan_delay_ms 默认 150000）
```
镜像：`out/full-flash/boot-583.img`（15da938，旧 Data 匹配）。

### 12A.5 设置里 Wi-Fi：已提交但今晚无法验证（磁盘硬约束）
- `45d577a`：board.toml 移除 `com.apple.wifid`、runtime-jobs SHA、
  `hakios.wlan.cpp` 用具体 IONetworkInterface 子类。
- 但 `offline_disabled_jobs` 变了 ⇒ boot 的 `scratch-data.img`（seed）与旧 Data
  不匹配，只 boot 会 `offline scratch Data mount failed` panic（m1 实测）。
- 必须同时刷 **`build_userdata=true` 生成的 userdata-root.sparse.img**：
  artifact **5.87 GB**，`gh run download` 需要 zip+解压约 12 GB 峰值；
  本机 C 盘清理后仅 6.3 GB，**今晚无法完成**。
- CI `42875987953`（9ead7fc + create_data_seed + build_userdata）已 success，
  产物 `picasso-bootimg-579` / `picasso-userdata-579` / `picasso-data-seed-579`
  都在 GitHub 上；磁盘够时直接下载刷写即可。

### 12A.6 明天/下一轮
1. 腾出 ≥15 GB 或换盘，下载 `picasso-userdata-579`（5.87 GB）。
2. `bash work/flash-userdata.sh` 前先把 artifact 放到
   `out/userdata/userdata-root.sparse.img`；刷完再 boot `picasso-bootimg-579`
   （打槽后）。
3. 验证：设置里 Wi-Fi 开关能开、扫描不再卡（9ead7fc 的 500 ms）、连接/DHCP。

## 12. 模块已合并到上游最新分支（2026-11-10 23:30）

- 上游最新 = `silenzio111/UniXNU-picasso` 的 `stage12/picasso-ios16`
  @ `00ea4a0`（2026-11-06）。`kd439-pool` 正是基于它（领先 142、落后 0）。
- fork 新分支：**`tselongson/UniXNU-picasso:wifi-wcn3990-stage12`**
  （本地 worktree `upstream/merge-wifi`），两个 commit：
  - `22832f9` 导入 `wifi-wcn3990/` + 4 个宿主接入点
  - `2cb67cd` 第 5 个接入点：`Adreno620Cp.cpp` 的 `rpmhVoteCryptoPath`
    加 `extern "C"`（CE 门铃前的互连投票，不能空实现）
- 合并内容**只有**：`wifi-wcn3990/` 整目录 + 5 个宿主文件改动
  （`board.toml` 仅 `wifi=1`、`scripts/build-arm-driver.py`、
  `scripts/build-xnu-4k-baseline.sh`、`drivers/NativeUfs/NativeUfs.cpp`、
  `drivers/NativeAdrenoPicasso/Adreno620Cp.cpp`）。GPU 逻辑/触摸/显示/
  CI workflow/Data 几何**一概不带**；`wifi=0` 时整包 inert。
- 5 个 `integration/0*.patch` 已按 `00ea4a0` 重新生成并反向校验；
  `INTEGRATION.md` / `USAGE.txt` / `README.md` / `manifest.json` 基线全部更新。
- `.gitattributes` 新增 `wifi-wcn3990/integration/*.patch text eol=lf`
  （本机 `core.autocrlf=true`，不锁会让 `git apply` 静默失败）。
- CI `42842132006` **success**（18m17s），产物 `picasso-bootimg-575` /
  `picasso-data-seed-575`；Data seed 为**复用**。
- **约定**：以后派 CI 默认只带 `-f fast=true`，复用 Data seed，
  **不要**带 `-f create_data_seed=true`（除非明确要重建 Data identity）。
- 上游无推送权限；如需 PR：
  `https://github.com/silenzio111/UniXNU-picasso/compare/stage12/picasso-ios16...tselongson:wifi-wcn3990-stage12`
  （未主动创建，等用户决定）。
