# WiFi (WCN3990) 项目交接文档 — 2026-11-09 23:40（v4：join 里程碑 + panic 定位）
<!-- ARCHIVE-NOTICE-v6 -->
> **历史归档（v6 状态更新时加注）**：本文是 2026-11-09 23:40 的 v4 WiFi 交接，其后项目已大幅推进。
> 本文的阶段状态、分支名与 CI 命令写于当时，**不代表当前进度**。
> 当前唯一权威状态源是 [`handoff/wifi-wcn3990-handoff-2026-11-10.md`](wifi-wcn3990-handoff-2026-11-10.md)（v6，2026-11-10 20:45）；
> 逐轮实验事实见 `.workbuddy-ai/memory/2026-11-09.md` 与 `2026-11-10.md`。
> 本正文保留作为历史与推理记录。

> 面向下一个接手的人/会话。所有结论均有日志或脚本佐证；
> 按 `wifi-wcn3990/docs/diagnostic-charter.md` 与 UniXNU porting/network 章程执行。

## 0. 一句话现状

**scan → auth → assoc 三个 checkpoint 已实机闭环**（`assoc resp status=0 aid=N`，
AP 侧接受关联）；之后固件在 **`vdev_up` 之后约 20 秒**发 `0x401` ep0 消息
（尾字节 01→02）并使 XNU panic——已证明与 once 块/HTT 消费/peer-param 无关，
是 vdev_up 的伴随事件，五次无一例外。固件崩溃转储已定位到正确地址
（`0x8ba00000`，WLFW MSA_INFO 报告），取证链 chain-541 需重跑，预期拿到明文断言。

## 1. 验收链进度（官方口径逐 checkpoint）

| checkpoint | 状态 | 证据 |
|---|---|---|
| firmware startup | ✅ | MPSS/WLFW/CE/HTC/WMI READY，0 panic |
| scan（真实 BSS） | ✅ | chain-531/532c：15-18 beacon，8+ SSID/BSSID，0 panic |
| **auth（MGMT_TX）** | ✅ | chain-534/537/540：`mgmt tx compl status=0`、`auth resp status=0` |
| **assoc** | ✅ | chain-535：`assoc resp status=0 aid=1`；537：`aid=8` |
| **（vdev_up 后）** | ⛔ | **固件 `0x401` early-crash → 1 次 XNU kernel panic**（535/536/537/540-h3/h4 五次全部如此） |
| EAPOL/四次握手 | ⬜ | 未实现；**需要目标 AP 密码** |
| address/DHCP | ⬜ | `picasso_wlan_transmit()` 仍伪造 DHCP/ARP |
| rx/tx 数据面 | ⬜ | RX 排水/refill/分类已嫁接（47a2b0f）但固件从未投递帧（`ind=0 ord=0`），TX 未做 |
| 用户态 wifid | ⬜ | 本地已改未推送（见 §6） |

## 2. 本日 commit 链（全部已推送 fork kd439-pool，abi-check 0 HARD）

```
7eb7ae4 join=2：VDEV_START(0x5003)+PEER_CREATE(0x6001)+等 HTT peer map → auth
81b44ce join>=2 跳过严格 RSN 门槛 + peer-map 超时诊断降级
7514187 assoc 填标准 CCMP/PSK RSN IE（修 status=10）
8fa38ce 删掉参考实现中不存在的 PEER_SET_PARAM 255（= PEER_DUMMY_VAR）
47a2b0f HTT RX 路径整段嫁接（wr_post/wr_rx_ind/wr_in_ord/wr_msdu，诊断版）
bcad449 wj_begin() once 块九条参数命令（TXPOWER/NSS/TX_ENCAP/RTS/STA_PS×4）
588e58d joinonce=<n> bitmask（逐命令门控，二分用）
bed4002 msadump 扫描固件自己的 MSA 区域（0x8ba00000，不只是 DTS 的 0x8b500000）
（另有 4c44e3e HTT RX fill 转正默认、3260e8d join 全序列嫁接）
```

## 3. 实验结论时间线（531→541，全部单变量 A/B）

| 轮 | 变量/改动 | 结论 |
|---|---|---|
| 531 | k1/k2：`fw_idx_init_val` 0→1023 | scan 从零信标→真实 AP；修复转正（4c44e3e） |
| 532b | BASE 漏 scanfix | **教训**：转正一个 bit ≠ 整套 ABI 默认化；逐 token 对照才能发现 |
| 532c | 补 scanfix=63 | scan 完整跑通，join=2 进入 auth → **status=3** |
| 533 | TAG z1/z2 修正版 | z1 对照复现 status=3；z2 被 RSN 门槛挡住 |
| 534 | 跳过 RSN 门槛 | auth status=0 ×8、**assoc status=10**（坏 RSN IE） |
| 535 | 填标准 CCMP/PSK RSN | **assoc status=0 aid=1** → vdev_up → peer param 255 → panic |
| 536 | 删 peer param 255 | panic 依旧 → 假设证伪；vdev up 后存活 ~20s |
| 537 | HTT RX 嫁接 | `ind=0 ord=0`：固件**从未投递 HTT 帧** → HTT 假设证伪 |
| 538 | once 块九条 | **0 panic**（首次）；但 auth 回退 status=3 ×8 |
| 540 | joinonce 四组 | **h1 auth=0**（证伪 once→status=3）、h4 panic×2（证伪 once→panic）；panic 与 vdev_up 强绑定五次无一例外；h1 assoc **status=41**（AP：稍后再试） |
| 539 | MSA 0x8b500000 | 全零；WLFW MSA_INFO 揭示固件实际区域是 **0x8ba00000**（DTS 的 0x8b500000 它不写） |
| 541 | msadump 双区 | **运行中被中断，待跑**（CI 42645509485 完成后 3 分钟即可） |

## 4. 已证伪/已排除清单（勿重查）

- SMMU 全线（0x20/0xc0 路由、USFCFG、bank 切换）、CE3 race（CVE 已修）、
  TZ 内存授权、HTC credit、WMI event id、MGMT 解析器、vdev 状态、
  3 处 MAC TLV ABI、HTT RX ring 不消费、PEER_SET_PARAM 255、
  once 块九条命令、peer map 等待（固件不发 peer map，降级后照常成功）、
  **「0x401 不回导致死」（domain_search：任何公开源码无 0x401 定义；
  ath10k 对 ep0 未知消息即丢弃不回，上游真机也不回）**。
- `0x401` 消息语义**仍未解**：payload `01 04 00 03 01 0{1,2} d0`，尾字节
  01→02 有规律；条数随实验 12→20；无 panic 时也持续出现——只当异常指示观测。

## 5. panic 现状与取证通道（最高优先级）

- panic 形态：`vdev_up` 后 ~20s，`HTC: control message 1025 (7 bytes)
  01 04 00 03 01 0{1,2} d0` ×12-20 → `=== PICASSO CDC PANIC BEGIN ===`，
  panic 文本在 `caller=0x` 处被 CDC 截断，1 次 kernel panic/次启动。
- **取证通道（不用猜死因）**：WLFW `MSA_INFO` 报告固件自己的区域是
  `0x8ba00000 +0x10000 (secure=1)` + `0x8ba10000 +0x1f0000 (secure=0)`；
  MSA 是普通 DDR，XBL 只清 IMEM 页，**dump 跨 boot 保留**；固件崩溃的明文
  断言形如 `PDM: service 'wlan_process' crash: 'EX:wlan_process:0x1:
  WLAN RT:0x2076:PC=...'`（2026-04 ath10k 上游线程，WCN3990/WLAN.HL.3.2 同款）。
- `msadump=1`（现成开关）现扫描两块区域，时机在 modem 线程重载固件之前。
- **chain-541 被中断**，需重跑：`bash work/chain-541.sh`（CI 42645509485
  完成后运行；若 run 已完成可跳过等 CI 直接下载）。

## 6. 用户态链路（本地已改，**未提交未推送**，单变量纪律）

`git status` 三个文件：
- `board.toml`：`offline_disabled_jobs` 移除 `com.apple.wifid`
- `patches/picasso-runtime-jobs-xnu.patch`：两个 SHA 已按移除 wifid 后的
  plist 重算（`24358e02…`/`0e38d793…`，plistlib 复算验证一致）
- `wifi-wcn3990/source/hakios.wlan.cpp`：`IONetworkInterface` 抽象类 alloc
  NULL → 候选类降级（IO80211Interface→IOEthernetInterface→原类兜底）
- **派 CI 必须带 `-f create_data_seed=true`**（board.toml 改动会变 recipe）
- 验收判据：串口出现 wifid 的 `SCAN_REQ`/`SCAN_RESULT` 且返回真实 BSSID

## 7. 下一步决策树（优先级）

1. **重跑 chain-541** 拿固件断言文本（3 分钟上机）→ 按 assert 修 vdev_up 后的死因。
2. **WPA2 四次握手 + 密钥**：`wk_sha1/wk_hmac/wk_prf/wk_pmk/wj_eapol/wj_send_key`
   （wlan-wip L3231-3383/L3965/L4316）整段照抄；`join=2` 后在 assoc OK 时若
   `secure` 进入 WJ_KEYS 等价状态。**硬依赖：目标 AP 的 SSID+密码**（外部信息）。
3. **数据面 TX**（`wj_tx_put/wj_tx_send/wj_tx_drain/wj_tx_compl` L3997-4092）
   —— EAPOL 帧发出需要它；RX 已有。
4. 去伪造：删 `picasso_wlan_transmit()` 的伪造 DHCP/ARP，接真实数据面，
   按 scan→auth→assoc→address→rx/tx 验收。
5. 用户态链路推送（§6）+ Settings 可见性。
6. 清理：`acbcf84..c4f2105` 无效 SMMU 诊断代码；`0x401` 语义（低优先）。

## 8. 铁律与坑（全部踩过）

- **CI 必须 `gh workflow run compose-bootimg.yml -f fast=true`**：非 fast 会死在
  `Check iOS 16 framebuffer and IOSurface adapters`，compose 被跳过、无 boot.img。
- **cmdline 硬上限 995 B**：`mk-args.py` 等长替换；放不下时删显式 `=0` 冗余 token
  （`msadump`/`vdevup`/`initarr` 等缺省即 0）。
- **实验 TAG 换新名**：旧 `j1/j2/r1/r2` 文件被 Windows 锁住（PermissionError），
  `mk-args` 写失败会留下陈旧日志造成假结果——生成前先 `rm -f`，判读前查
  `.cap` 的 mtime/字节数。
- **单变量纪律**：一次只动一个 boot-arg/一个 bit；`scanfix`/`joinonce`/`join`
  三个 bitmask 已覆盖全部开关，不需要重建即可表达组合。
- 抓取窗口 ≥170s（WLAN 初始化延迟 70s）；判据是 checkpoint 明文
  （`auth resp status=0` 等），**「不崩」「COMPLETED」「BSS_CHANNEL」都不算**。
- 11-09 日志前半的「0x20 无 SMMU 路由」段已撤回，不可引用。
- 用户可中断后台链（Agent 会被平台清理，普通 Bash 不会）；被中断的链
  用 `work/chain-5NN.sh` 原样重跑即可。
- commit message **不得写未实机验证的结果**（用「Device verification pending」）。

## 9. 关键文件索引

| 文件 | 内容 |
|---|---|
| `wifi-wcn3990/source/Wcn3990Hif.cpp`（~3900 行） | 主驱动：g_scan_fix/g_join/joinonce 全部开关；join 状态机；HTT RX wr_* |
| `wifi-wcn3990/docs/diagnostic-charter.md` | 证据规则 |
| `work/chain-531..541.sh` | 每轮实验链（可复用模板） |
| `work/abi-check.py` | 提交前门禁（必须 0 HARD） |
| `work/mk-args.py` / `make-503.py` | 等长改 args / CI 产物转本地镜像 |
| `work/refs/ath10k/` | Linux ath10k 参考树（本地，勿外网重拉） |
| `wlan-wip` 分支 `WlanRadio.c`（5627 行） | **同芯片 XNU 完整参考**；只摘 state≥20 函数 |
| `handoff/wifi-wcn3990-handoff-2026-11-09.md` | 本文档（v4） |
| `.workbuddy-ai/memory/2026-11-09.md` | 本日全部实验数据与裁决 |

## 10. wlan-wip 嫁接注记

三个历史启动 panic 坑（OSMetaClass 重复注册 / device memory `dc zva` /
SMP2P 时序）全在 state<20 固件加载段，与 L2900-5250 的 mgmt/HTT/join 段
**完全解耦**——只摘 state≥20 函数无时序风险。嫁接清单（行号已核对）：
RX `wr_msdu/wr_post/wr_in_ord/wr_rx_ind` L4169-4270（**已嫁接**）；
TX `wj_tx_put/wj_tx_send/wj_tx_drain/wj_tx_compl` L3997-4092（待）；
EAPOL/crypto `wk_sha1/wk_hmac/wk_prf/wk_pmk/wj_eapol/wj_send_key/wj_classify`
L3231-3383/L3965/L4125-4316（待）。

---

## 11. 2026-11-10 00:40 状态增量（接力必读）

### 新确证
- **VDEV_UP 是 SError 的唯一触发点**（chain-543 干净控制组）：`joinskip=2`
  （assoc 成功、不发 vdev_up）→ 0 panic、活满 170s；历史崩组与它只差这一条命令。
- 崩溃与 AP 无关（h3=TP-LINK ht=1、h4/o3=legacy 11b 都崩）；AGGR_CFG 无关
  （chain-542 o3 仍崩）；`0x401` 是噪声（扫描期也出现）；MSA 转储跨 boot 不保留
  （chain-541 实测全零，msadump 路线作废）。
- panic 符号化：`_sleh_serror` / `_arm64_platform_error` → 真实硬件 SError；
  SMMU dump 里 `cb*_fsr=0x400` 是 LPAE format 标志，不是故障（11-08 已核实）。

### 新代码
- `d6c799d`：`joinskip` bitmask（bit1 跳过 PEER_ASSOC、bit2 跳过 VDEV_UP）+ 删
  open-AP `peer_param(3,1)` 重复块。
- `a685e13`：补 HTT 三连第一条 **FRAG_DESC_BANK_CFG**（`httring` bit2=4），
  64B 布局与 wlan-wip `ws_htt_next` step0 逐字节一致。**实机验证被用户取消，
  待跑**：CI 产物 `out/run-42654759830`，A/B = `httring=5`（frag+rx）vs
  `httring=1`（rx only，已知崩）。

### 凭据（本地，绝不提交）
- `work/wifi-credentials.txt`：ChinaUnicom-33E6TD(-5G)，两频段同密码。
- `work/make-545.py` 已备好 wpa=1 / wpass=<本地> 槽位。
- join 选择待加 `joinsel=1`（SSID 前缀 ChinaUnicom），当前 join=<n> 常选错 AP。

### 进行中
- wpa-graft worker：WPA2 四次握手（PMK/PTK/EAPOL/装 key/authorize）+ HTT TX
  gather 数据面，`wpa=1` 门控，完成前不要并发改 `Wcn3990Hif.cpp`。

## 12. 2026-11-10 02:40 状态增量（vdev_up 攻坚中）

### 已证伪（vdev_up 崩溃）
- `AGGR_CFG`（542）：加发仍崩。
- `FRAG_DESC_BANK_CFG`（547）：注册成功（bank=…, n=1056, desc=72）仍崩。
- 与 AP 无关：时钟 AP assoc=0 后 vdev_up 即崩（547，caller=…cc0344）。
- `0x401` 在扫描期也出现，是噪声；MSA 转储不跨 boot（541 全零）。

### 待验证（受 AP 可用性阻塞）
- `joinskip` 隔离（bit1 跳过 PEER_ASSOC、bit2 跳过 VDEV_UP、bit4 跳过
  PEER_CREATE，commit 0815be3）**尚未拿到有效数据**：
  - 时钟热点（1902_WIFI_CLOCK）已关闭：auth 不再应答（s7/s5 均 no answer）。
  - ChinaUnicom 2.4G assoc 稳定回 status=41；5G（joinsel=3, 5061f23）未测。
- `vdevpre=1`（PDEV_SET_BASE_MACADDR，从未在 vdev 前发过）候选未测（chain-552）。
- 新候选：**延后 vdev_up 到四次握手之后**（joinskip=2 + wpa=1）——若固件在
  peer 未授权时 vdev_up 必崩，这个顺序可能绕过（wlan-wip 从未在真机验证）。

### WPA2（f6968a4 + ff33732）
- SHA1/HMAC/PRF/PBKDF2/AES-unwrap + EAPOL 状态机 + HTT TX（CE4 gather，
  含 bit63 gather 位）+ WMI 0x5009 装 key + PEER_AUTHORIZE 全部实现。
- `wpa=1`/`wpass=` boot-arg（密码只进镜像 cmdline，不进源码/仓库）。
- **尚未跑过一次 EAPOL**（每次都先崩在 vdev_up）。

### 运维
- 磁盘 0 字节死锁已解：删除回收站里 65,105,920 字节的 $R 文件释放 2.3GB
  （未碰用户旧文件）。清理本项目产物要走永久删除，别再堆回收站。

## 13. 2026-11-10 03:30 状态增量（RSN 突破 + vdev_up 仍是唯一阻塞）

### 新突破：assoc status=41 的根因 = RSN 的 group cipher
- AP 的 RSN（chain-554 dump）：`group cipher TKIP`、pairwise TKIP+CCMP、AKM PSK。
- `join>=2` 旧代码把 RSN 覆盖成 plain CCMP → AP 回 **status 41 = invalid group
  cipher**（802.11-2020）。
- `5c80c69` 改为按 AP 的 group cipher 构造 RSN（pairwise 仍 CCMP）→
  **`assoc resp status=0 aid=16`，中国联通 2.4G 首次关联成功**。

### 新事实
- **不开 VDEV_UP 就没有数据面**：h5（joinskip=2）assoc=0、活满 240s、0 panic，
  但 20 分钟 `eapol=0 ind=0 ord=0 msdu=0` —— 固件一个数据帧都不投递。
  因此「先握手再 vdev_up」不可行（没 vdev_up 收不到 EAPOL）。
- h7（正常顺序 + wpa）assoc=0 → vdev_up → **仍立即 panic**（第三次跨 AP 复现）。
- 5G BSS 的 MGMT_TX 被固件拒（status=1/3）→ 5G 暂不可用；joinsel 已改为优先 2.4G。
- 35 次 "peer map timeout"、0 次 peer mapped：固件从不发 HTT peer map；
  frag bank/q_state 按 Linux 原生 builder 对照无错（q_state 仅 PEER_FLOW_CONTROL
  有效，info=0 正确）。

### 待验证（构建/实验中）
- `96cb208`/`8427730`：joinsel 2.4G 优先、vdwait、vdevpre、assoc 前补
  SLOT_TIME/PREAMBLE vdev 参数（7/8）。
- chain-559：`joinskip=1`（无 peer_assoc）/`joinskip=7`（无 peer 命令）隔离，
  均发 vdev_up —— 这次 assoc 能成功，矩阵首次有效。
- chain-552：`vdevpre=1`（PDEV_SET_BASE_MACADDR，Linux core_start 必发，
  我们从未发过）。
- chain-556（defer vdev_up）**已被 h5 证伪**，不要再跑。
