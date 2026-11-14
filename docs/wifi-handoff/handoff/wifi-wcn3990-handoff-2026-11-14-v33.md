# WiFi (WCN3990) 交接文档 — 2026-11-14 v33

**时间**：2026-11-14 15:5x
**HEAD**：`9e4dfe3`（fork `kd439-pool`）
**CI**：`43571493379` SUCCESS（`-f fast=true`，复用 seed-613）
**设备**：chain-614 A/B 已完成（两组均有效，keyfix 证伪）
**唯一权威状态源**：本文件。v32 及更早为历史归档。

---

## 0. 一句话状态

```text
固件/HIF/扫描/关联/WPA2 四次握手/装密钥/PEER_AUTHORIZE/DHCP/DNS/键盘 ✅
  ── 发送方向 ✅（AP 对运行期 89 帧全部 ACK）
  ── 广播/组播接收 ✅（无上限计数 grp=102）
  ── ★ 单播接收 ❌（无上限计数 uni=0，DP: reject=0）← 唯一缺口
  ── ★ 运行期 Data/System 卷 I/O 挂死 → UI 冻结（转圈静止）← 并列必修
```

**本轮没有修好上网。** 三个假设（`rekey` / `keyfix` / `pspoke`）全部被
实机证伪，详见 §2.7 / §2.8 / §2A.5。判据已补严，"单播确实为 0"是铁证。
**最短上网路径见 §2A.7：把路由器改成 WPA2-PSK/仅 AES（SSID 密码不变），
重启设备即可，不需要 CI、不需要改代码。**

---

## 1. 本轮读过的材料

`handoff/` v4→v32 全部、`memory/` 10-26→11-14 共 15 份日志（两路子代理
独立通读 + 自己逐份核）、`console-exp-{rx,kb3,kb2,kb,tg1,rk0,rk1,kf0}.cap`、
Linux `ath10k` 主线源码（`wmi.h` / `mac.c` / `wmi-tlv.c` / `htt_rx.c`）、
同芯片 XNU 参考 `work/trogdor/port/WlanRadio.c`、非同芯片参考
`UniXNU-pong/userland/wifi-bootstrap.c` / `pong-wifi/PongWifiNet.cpp` /
`unixnu-wcn7860/PeachNet.c`、Apple OSS `configd`/`xnu`。

---

## 2. ★ 单播接收：精准定位链（全部实机/源码，非推测）

### 2.1 chain-612 探针（`ebd4c1f`，400 s，0 PANIC）
`hif_rx_trace()` 打印的 64 帧**全部** dst 为 `ff:ff:*` / `01:00:5e:*` /
`33:33:*`，0 单播、`DP: reject #`=0；DHCP bound、dns set=1×3。
配合 `WlanRadio.c:4177-4179` 与 `ath10k htt_rx.c:2003-2005`（解不开的加密帧
仍以 raw 上报并计数），`bad=0/0` ⇒ **固件从未交付单播 MSDU**，不是主机侧
decap/过滤。

### 2.2 修正 v32 的三处判读错误
| v32 说法 | 事实 | 依据 |
|---|---|---|
| `htt=64` 冻结 | **打印上限**，非冻结 | `Wcn3990Hif.cpp:1221-1240`，`g_htt_rx++` 在 `if (g_htt_rx < 64)` 块**内部** |
| `ord/msdu` 冻结 | **极慢增长**（kb3 101→107，rx 74→96），DHCP DISCOVER 重试 5 次 / REQUEST 2 次，广播也在丢 | kb3/rx 心跳序列 |
| `0x5003` = VDEV_START_RESP | **VDEV_INSTALL_KEY_COMPLETE** | `wmi.h:1283-1285`：0x5001=START_RESP、0x5002=STOPPED、0x5003=INSTALL_KEY_COMPLETE。驱动里 0x5003 是*命令* `WMI_VDEV_START_CMDID`，不同命名空间 |

`0x5003` payload 末 4 字节（`00` / `01` / `02`）等于所装 key 的 index，
是**按 key index 的回执，不是失败状态**——三条 key 命令固件都确认收到。

### 2.3 时序硬证据
`console-exp-rk1.cap`：
```
4457 peer mapped id=55  keyed=0     4460 peer mapped id=7 keyed=0
4516/4518/4532 EAPOL frame (单播，装 key 前能收到)
4534 install pairwise key idx=0 len=16 cipher=4
4536 install group key    idx=2 len=32 cipher=2   ← TKIP
4538 peer param 3=1 (PEER_AUTHORIZE)
4543 keys installed, peer authorized, link up
4561 peer mapped id=72 keyed=1       ← 装完密钥后的晚到 PEER_MAP
```
kb3 / rx.cap 同形（id=72 / id=104）。

### 2.4 chain-613：`rekey` 假设 → **实机证伪**
`60417b5`：在 `keyed=1` 后到达的 PEER_MAP 上幂等重发一次 PTK，
`rekey=0/1` 单变量（995 B、仅 1 字节差）。

rk1 **有效轮**（重跑第 1 次即有效）：`WPA: rekey PTK after late PEER_MAP id=72`
执行、`WPA: install pairwise key` 再发、KEY_COMPLETE 回执、`join=5`×55、
DHCP bound、dns set=1×3、89 TX 帧、0 PANIC/0 UFS/0 disk I/O —
**但 64 帧仍全是组播，0 单播**。⇒ **PTK 挂在旧 peer 不是根因，已排除。**

### 2.5 又排除三处
- `PEER_ASSOC flags=00019007` 已含 `WMI_PEER_AUTH(0x1)` / `QOS(0x2)` /
  `NEED_PTK_4_WAY(0x4)`（`wmi.h:6421-6423`）。
- `WMI_KEY_PAIRWISE=0x00` / `WMI_KEY_GROUP=0x01`（`wmi.h:5032-5034`）
  与我们的 `body[4] = pairwise ? 0 : 1` 一致。
- 装 key 前 EAPOL 单播能收 ⇒ 交付路径本身没坏。

### 2.6 ★ chain-614：先补判据漏洞，再修确定性差异
**判据漏洞（我自己的）**：`hif_rx_trace()` 只打前 64 行。rk1.cap 第 64 行在
第 6571 行 / 共 7942 行，末次心跳 `msdu=106` ⇒ **42 帧从未打印**，
所以 chain-613 的"0 单播"严格讲不成立。已修：
- `g_rx_unicast` / `g_rx_group` 按 `dst[0]&1` 统计**每一个**接受帧，不受 64 上限；
  心跳新增 `uni=%u grp=%u`（24 个格式参数已核对齐）。
- 前 8 个单播帧单独打 `RXU: #n`（有界），后期 SYN-ACK 可见。

**确定性差异（逐字节可对，非推测）**：本机 AP 是混合模式
`group=TKIP / pairwise=CCMP`（`rk1.cap:4395-4396`）。
`struct wmi_vdev_install_key_cmd`（`wmi.h:5066-5083`，packed）字段顺序
正好映射我们的 `body[23]`：`body[20]=key_len`、
**`body[21]=key_txmic_len`、`body[22]=key_rxmic_len`**。
ath10k `mac.c:271-276` 对 TKIP 要求 `8/8`，`wmi-tlv.c:2386-2387` 写入命令；
同芯片参考 `WlanRadio.c:3965` 跑**纯 CCMP**，`wj_send_key` 硬编码
`cipher=4` / key 16 B / 写 `0/0/0`，`WlanRadio.c:3705` 明写只应答
"pairwise CCMP, group CCMP"。我们照抄时把 TKIP 分支丢了，对 32 B TKIP GTK
仍写 `0/0`。`keyfix=1` 为 `cipher==2` 写 `8/8`，默认 0 保持原行为 ⇒ 严格单变量。

`9e4dfe3` 同时移除了已证伪的 rekey 门控。改动只在 `wifi-wcn3990/source/`
（不在 `recipe()` 内）⇒ 复用 seed-613，**不刷 userdata、不清 marker**。

### 2.7 ★ chain-614 最终判决（15:58）：keyfix 被实机证伪

| | kf0（keyfix=0） | kf1（keyfix=1） |
|---|---|---|
| VALID | `install`✓ `join=5`×55 `UFSFAIL=0` | 同（attempt 1 即有效） |
| `WPA: TKIP mic lengths 8/8` | 0 | **1（修复确实执行）** |
| **`uni/grp`（无上限计数）** | **0 / 102** | **0 / 90** |
| `RXU:` / `DP: reject` / `PANIC` / `disk I/O` | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| DHCP bound / dns set / SIOCSIFADDR | ✓ / ×3 / ×2 | ✓ / ×3 / ×2 |
| `REAL DATA PLANE` | 93 | 64 |
| `RX: #` 64 行中单播 / `arp op=2` / `fl=12` | 0 / 0 / 0 | 0 / 0 / 0 |

⇒ **补 TKIP MIC 长度 8/8 无效，该假设排除。** kf1 组播计数 90 < kf0 的 102
属正常波动（组播流量随机），单播在两组都严格为 0。
判据这次是铁的：无上限计数覆盖**每一个**接受帧，不受 64 行探针限制。

### 2.8 本轮两个假设均被证伪后的收敛结论（可证伪，非推测）
1. **rekey**（PTK 挂旧 peer）— 证伪（chain-613 rk1 有效轮）。
2. **keyfix**（TKIP MIC 长度）— 证伪（chain-614 kf1 有效轮）。
3. 又排除：`PEER_ASSOC flags=00019007` 已含 `WMI_PEER_AUTH/QOS/NEED_PTK_4_WAY`；
   `ath10k_peer_assoc_h_crypto`（`mac.c:2209-2261`）只额外加
   `need_gtk_2_way` / `pmf`，本机无 WPA IE、无 MFP，不构成差异；
   `WMI_KEY_PAIRWISE/GROUP` 数值正确；`0x5003` 是按 key index 的回执非失败码；
   装 key 前 EAPOL 单播能收（交付路径没坏）。

**剩下的确定性事实**：装 PTK/GTK 之后，固件不再向主机交付**任何单播 MSDU**；
广播/组播（vdev 组钥路径）照常；主机侧 `bad=0/0` 且 `DP: reject=0`，
说明帧没进 `hif_wr_msdu`，是固件侧丢弃或未生成 indication。
**"为什么"仍无实机证据，不下结论。**

### 2.9 下一步（优先级，按成本）
1. **零成本、用户可立即做**：把路由器改成「WPA2-PSK / 仅 AES」或开手机热点
   （纯 CCMP group），让设备连它（需改 `wifissid`/`wpass` 两个等长 boot-arg）。
   若网页立刻能开 ⇒ 一刀锁死是 **TKIP/CCMP 混合模式**的固件行为，
   与单播交付代码无关；若仍打不开 ⇒ 排除 cipher 组合，转 2。
2. 若 1 证伪：加 `WMI_VDEV_INSTALL_KEY` 之外的固件侧探针——
   `WMI_PEER_ASSOC` 后的 `WMI_VDEV_PARAM_*` 隐私/加密位、
   HTT RX indication 里单播 MPDU 的 `peer_id` 与 attention 原始值，
   用有界日志确认"固件是否真的产生了单播 MSDU"。
3. 并行必修：§3 运行期卷 I/O 挂死 → UI 冻结（>20 min 长时采集）。
4. **不要**再试 rekey / keyfix，两者都已实机证伪。

---

## 2A. ★★ 为什么"别人同芯片打通了"——逐行对比的答案（11-14 16:0x）

用户追问"其他芯片不是闭源的吗？他们能做你做不得？"。固件确实都是
Qualcomm 闭源 blob，但没人逆向固件，所有项目都照抄 Linux 主线开源
`ath10k` 的 WMI/HTT 协议。差距不在闭源，在**两条实现走的路不同**：

### 2A.1 Trogdor 根本不连这台 AP
`work/trogdor/port/WlanRadio.c:3705-3713`：
```c
/* An RSN element we can answer: pairwise CCMP, key management PSK,
 * group CCMP, management protection not required. */
if (memcmp(p, oui, 3) || p[3] != 4) return -1;   /* group must be CCMP */
...
static const uint8_t rsn[22] = {48,20,1,0, 00 0f ac 04, ...};  /* rsn[7]=4 */
```
group cipher 不是 CCMP 就**直接拒绝加入**。它跑通的是纯 CCMP 网络。

### 2A.2 我们为这台 AP 主动放宽了那行拒绝
`Wcn3990Hif.cpp:2648-2685`：
```c
/* ChinaUnicom's 2.4 GHz BSS advertises a TKIP group with CCMP in the
 * pairwise list, and a CCMP-group request makes it answer assoc status 41
 * ("invalid group cipher") - chain-554's RSN dump settled this. */
if (group != 2 && group != 4) return -1;   /* 2=TKIP 放行 */
rsn[7] = group;                            /* group = AP's choice → TKIP */
```
本机 AP 原文：`ap rsn len=24 01 00 00 0f ac 02 | 02 00 | 00 0f ac 02 00 0f ac 04 | 01 00 00 0f ac 02`
= group **TKIP**、pairwise TKIP+CCMP、AKM PSK（WPA/WPA2 混合模式）。

⇒ 参考实现从未在"TKIP 组播"这条分支上验证过；我们为了能连上它，
走进了参考没覆盖、固件可能也没写好的分支。这解释了为什么
`rekey`（chain-613）与 `keyfix`/TKIP MIC（chain-614）都无效。

### 2A.3 ★ 我们还删掉了两条参考都做的一步（chain-615 已补回）
`ath10k mac.c:3164-3180`（`ath10k_bss_assoc`）：
```c
ath10k_wmi_vdev_up(...);
ath10k_mac_set_sar_power(ar);
/* Workaround: Some firmware revisions ... have buggy powersave state
 * machine and must be poked with peer param command. */
ath10k_wmi_peer_set_param(ar, vdev, bssid, peer_param->dummy_var, 1);
```
`wmi.h:6336`：`WMI_PEER_DUMMY_VAR = 0xff, /* dummy parameter for STA PS workaround */`

`WlanRadio.c:4469-4472`（assoc response 处理）：
```c
wj_send_peer_assoc();
wj_send_vdev_up();
wj_peer_param(0xff, 1);   /* its power save workaround */
```

我们的 assoc 处理（`Wcn3990Hif.cpp:4408-4420`）只做 peer_assoc → vdev_up，
**没有这一步**。原注释记录的理由是 chain-535 发 peer param 255 后固件回
0x401 early-crash；但该归因已失效——0x401 风暴后来归因到
`ast_skid_limit=0`，由 `initfix=3` 修好（chain-572+），chain-535 早于该修复。

**为什么它能解释"广播通、单播全 0"**：PS 状态机有 bug 时 AP 认为 STA 在
睡眠，会把**所有单播帧缓存**到收到 PS-Poll 才发（轮询式驱动永不发
PS-Poll），而**广播/组播仍在 DTIM 投递**。这正是 chain-612/613/614
测到的形状：`uni=0`、`grp=90..102`、`DP: reject=0`、DHCP/DNS（广播）正常。

修复 `0b00c94`：在 `hif_join_send_vdev_up()` 末尾、`pspoke=1` 时发
`WMI_PEER_SET_PARAM(0xff, 1)`。放在这里而不是 assoc 处理里，是为了让
deferred-VDEV_UP 路径也覆盖到，并保持 peer_assoc → vdev_up → poke 的顺序。
`pspoke` 默认 0，`pspoke=0/1` 严格单变量（995 B、仅 1 字节差）。
同时把已证伪的 `keyfix` 门控撤掉、TKIP MIC 8/8 改为无条件（chain-614
kf1 已证 TKIP MIC 单独无效），腾出 boot-arg 槽位。

### 2A.4 结论（对用户问题的直接回答）
不是"闭源导致做不了"，而是：
1. 参考实现只走纯 CCMP 网络，**面对本机 AP 它会直接不连**；
2. 我们为了连上它放宽了 RSN 检查，进入参考未覆盖的 TKIP 分支；
3. 而且我们还删掉了两条参考都执行的 PS poke —— 这一步与"广播通、
   单播全 0"的现象最吻合。

chain-615 A/B 正在验证第 3 点。若 `pspoke=1` 出现 `uni>0`，则根因确认。

### 2A.5 ★ chain-615 判决（16:40）：pspoke 也被实机证伪

| | ps0（pspoke=0） | ps1（pspoke=1） |
|---|---|---|
| VALID | `install`✓ `join=5`×35 `UFSFAIL=0` | `install`✓ `join=5`×55 `UFSFAIL=0` |
| `JOIN: peer ps poke` | 0 | **1（确实执行）** |
| **`uni/grp`（无上限）** | **0 / 63** | **0 / 116** |
| `RXU:` / `DP: reject` / `PANIC` / `disk I/O` | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| DHCP bound / dns set / SIOCSIFADDR | ✓ / ×3 / ×2 | ✓ / ×3 / ×2 |
| `REAL DATA PLANE` | 42 | 151 |
| `bad` | 0/0 | 0/1（1 个未完成） |

⇒ PS workaround 无效。**本轮三个假设全部实机证伪**：
`rekey`（chain-613）、`keyfix`/TKIP MIC（chain-614）、`pspoke`（chain-615）。

### 2A.6 三个假设排除后的收敛结论（可证伪）
1. **装 key 前单播能收**（EAPOL M1/M3，rk1.cap:4516/4518/4532）；
   **装 PTK/GTK 后所有单播消失**，广播/组播照常。
2. 主机侧 `bad=0/0`、`DP: reject=0`、`RXU:` 0 ⇒ 帧**没进** `hif_wr_msdu`。
3. 与 key 生命周期相关的三条路径（peer 重映射重装 PTK、TKIP MIC 长度、
   PS 状态机 poke）**全部排除**；`PEER_ASSOC flags` / `WMI_KEY_*` /
   `KEY_COMPLETE` 回执语义 / vdev 参数均核对无差异。
4. ⇒ 剩余最可能：**固件在"TKIP 组播 + CCMP 单播"混合模式下不向主机交付
   单播 MSDU**。这是本机 AP 的独有配置，参考实现（Trogdor）明确拒绝这种
   AP（`WlanRadio.c:3713` group 必须 CCMP），所以**没有可抄的实现**。
   「固件内部为什么这样」仍无直接证据，不下结论。

### 2A.7 上网的最短路径（明确、可执行）
| 路径 | 成本 | 预期 |
|---|---|---|
| **A. 把路由器改成「WPA2-PSK / 仅 AES」**（SSID/密码都不变） | 改一个下拉框 + 重启设备，**无需 CI、无需改代码、无需换 args** | 参考实现验证过的配置，最可能一次通 |
| **B. 开手机热点（WPA2/AES）连它** | 需换 `wifissid`/`wpass`（`mk-autojoin-args.py` 有 45 B 余量，最坏 +5 字符仍 995 B 内），一次上机 | 同上 |
| C. 硬啃 TKIP 混合模式 | 无参考实现，需继续挖固件行为 | **无时间表**，不建议 |

A 是首选：因为 group cipher 变成 CCMP 后，我们的 RSN 检查、GTK 安装、
PTK 安装全部与 Trogdor 的纯 CCMP 路径一致，等于**把环境变成参考实现
验证过的那个**。

## 2B. ★★★ 16:5x 新证据：PTK 正确、但协议栈不回 ARP（推翻"仅 RX 问题"）

用户否掉了"改路由器"这条建议（对：不能要求别人为一部手机改加密），
要求"先搞清我们和同芯片差在哪，能不能直接造一样的环境移植"。
按这个方向做完整核对后，得到三条硬结论。

### 2B.1 PTK 推导**完全正确**（离线独立复算，不是推断）
`console-exp-y0.cap` 是唯一带 `wpadiag=1` 的 cap，含 M1/M2 全文与密钥指纹。
从 M1 取 ANonce、从 M2（802.11 头 24B + LLC 8B，EAPOL 起于偏移 32）取
SNonce，用 Python `hashlib/hmac` 独立复算：

| | 独立计算 | 设备日志 | |
|---|---|---|---|
| PMK[0:4] | `a28efd65` | `a28efd65` | ✅ |
| KCK[0:4] | `4a0a0614` | `4a0a0614` | ✅ |
| 完整 PTK | `4a0a06147c9f2d68e0bc747181fc548452e9b2efaca5252963e971677deedd1a1bdf8ea4d428db5c62a66d62261bc29e` | — | — |
| TK | `1bdf8ea4d428db5c62a66d62261bc29e` | — | — |

⇒ PBKDF2、PRF-48、nonce 排序（`min/max`）、MAC 排序全部正确，
**TK 也是对的**。所以不是"密钥算错"。

### 2B.2 主机侧逐项与参考实现核对，**全部一致**
| 环节 | 结论 |
|---|---|
| `hif_wpa_send_key` 布局 / `WMI_KEY_*` / cipher ID(2=TKIP,4=CCMP) | 与 ath10k `wmi.h:5066-5083`、`mac.c:271-276` 一致 |
| `0x5003` 语义 | `VDEV_INSTALL_KEY_COMPLETE`（`wmi.h:1285`），末字是 key index 不是失败码 |
| WMI INIT | 与 Trogdor `cfg[46]` 一致（仅 `num_wds_entries` 0 vs 2，与单播无关） |
| join 序列 | 一致（含 chain-615 补回的 `peer_param(0xff,1)` PS poke） |
| TX 帧构造（802.11 头 / LLC / HTT 头 / `flags1` 的 TID） | 与 `WlanRadio.c:3999 wj_tx_put` 逐行一致 |
| RX 路径（`wr_msdu`/`wr_in_ord`/`wr_post`/ring 参数 2048/1023/2048） | 同源，一致 |

⇒ **驱动算法不是差异所在**。

### 2B.3 ★ 决定性新发现：协议栈根本不回 ARP
`console-exp-ps1.cap` 时序（同一轮、同一镜像）：
```
5365: DHCP: bound inet 192.168.101.18 mask 255.255.255.0 router 192.168.101.1 addr=0 rt=0
6408: RX #37  arp op=1 192.168.101.1 > 192.168.101.18   ← 网关问"谁是 .18"
7051: RX #59  arp op=1 192.168.101.1 > 192.168.101.18   ← 再问
```
地址已装（`addr=0` 是 `ifioctl(SIOCAIFADDR)` 返回 0，`rt=0` 路由成功，
见 `hakios.dhcp.c:400-429`），网关的请求也进了驱动并交给
`hakios_net_input()`（`hakios.net.c:183-217`，`ifnet_input`），
**但协议栈没有发出任何 ARP reply**（整轮 `[DATA PLANE] ARP frame 42 bytes`
只有 18 条，全是 **request**，没有 reply）。

**实时交叉验证**（PC 与手机同网段，PC=192.168.101.4）：
```
ping 192.168.101.18  →  6/6 超时，"无法访问目标主机"，PC 的 ARP 表始终无 .18
同一 30 s 窗口内手机串口：grp 计数在涨（广播收得到），ARP 发送 0 条
```
⇒ **广播 ARP 请求能收到，但我们不回。** 这意味着：
1. 不只是"收不到单播"——**别人也无法主动找到我们**；
2. 协议栈的**入向处理链**（`ifnet_input` → `arp_input`）有问题，
   这是纯主机侧、可修的；
3. 之前"单播不通"的结论需要修正为**双向**：我们发出的 CCMP 单播
   （TCP SYN 等）AP 侧也未见有效响应。

### 2B.4 与"同芯片"的差异只剩一个，且不是驱动
固件来源不同：Trogdor 从 CoachZ 磁盘加载 MPSS 固件（`WlanRadio.c:707`、
`:725` board-2.bin），我们走 picasso 的 WLFW/QMI，版本
`SERVICE_READY fw 840630314`。BDF/RF 校准随板。
**这是唯一剩下的环境差异，但"移植驱动"并不能改变它。**

### 2B.5 关于"整体移植 Trogdor"的评估（如实）
- Trogdor 的 HAL 是**自包含**的（`wc_alloc:2283`、`wh_send:2616` 都在同一
  文件内），所以 5627 行整文件移植在工程上可行。
- 但**平台胶水**（SMMU 映射、CE 基址、电源/时钟、WLFW 固件加载）是我们
  自己写的、且已被实机证明能跑（HIF READY / scan / join / TX / DHCP）。
  Trogdor 的那套是 SC7180 的。
- 整文件替换 = 把已验证的一半换成未验证的一半，且不会改变 2B.4 的固件差异。
  **不建议作为下一步**；若仍要试，应作为独立分支并行，不动现有基线。

### 2B.6 下一步（按证据强度，主机侧优先）
1. **修协议栈入向/ARP**：`hakios_net_input` → `ifnet_input` 之后为什么
   `arp_input` 不产生 reply。加一条有界探针：入向帧计数 + `ifnet_input`
   返回码 + `in_ifaddr` 是否存在。这能直接判定是 mbuf 构造、接口地址、
   还是 `IFF_NOARP`/标志问题。
2. ARP 通了以后，PC 能 ping 手机 = **单播 TX 的独立判据**（不再依赖 AP）。
3. 若 ARP 仍不通，再查 `SIOCAIFADDR` 的 `ifra_broadaddr` 是否让接口被当成
   点到点（`IFF_POINTOPOINT`），以及 `in_ifaddr` 是否挂在 en0 上。
4. 固件/BDF 差异只在 1–3 全部排除后才值得动（风险高、无参考）。

## 3. ★ 用户现场症状：转圈静止（不是重启）

用户澄清：**fb 是他手动退的，设备没有重启**，之前一直卡在转圈画面且
圈圈静止。撤回 14:2x 两处错判：
1. "PSHOLD watchdog 自行复位" — 错。CDC 报
   `panic=0 watchdog=1 armed=1 reset=2`，watchdog 武装但未咬，
   `reset=2` 是开机原因记录。**设备状态 ≠ 复位原因。**
2. "与 RX 是两条独立线" — 错。冻结现场心跳 `polls=293000..298000` 六次
   `rx=2229 beacons=1890 ord=171 msdu=171` **全不动**；健康 400 s 采集里
   每 1000 polls `rx`/`beacons` 各涨 15–16 ⇒ WiFi 接收数据面同时全停。

**症状定义**：系统早就完全无响应，framebuffer 停止提交新帧 ⇒ 转圈静止、
触摸无响应。硬证据：
- `SpringBoard pid=0 not running`、`backboardd pid=0 not running`。
- `disk5(OwnerData, 20 GiB)` / `disk3(System)` APFS 读大面积失败：
  15 s 实时串口 `disk5 tx_enter() failed`×343、`device is not ready`×279、
  `got err 16`×72（累计 1167）、`disk5: I/O error`×33、`tx_flush #817`、
  `dyld VNOP_GETXATTR: errno 93`。
- `CPUFREQ busy=3/2/2` ⇒ CPU 几乎全闲，**排除 11-11 那次忙等自旋饿死 UI**。
- `PANIC BEGIN`=0、`CDC panic=0` ⇒ 不是内核崩溃。

**不能用的假判据**：`TOUCH d_frames=0`、`GPU fence=2`、`PICASSO_FBUC`、
`htt=64`、`ord 冻结`——健康组同样恒定。

**触发点未取证**：`rx.cap` 400 s 内全部错误计数为 0，故障发生在采集结束后
到约 36 分钟之间，中间无日志。"卷 I/O 先挂 / 共享 SMMU-DMA 一起挂"
**没有证据，不下结论**。候选：seed-613 20 GiB 重建的 UFS 队列超时残留；
长时真实 TX 触发 DMA 故障。

**rk1 首轮旁证**：第一次 rk1 冷启动即 `NATIVE_UFS_FAILURE`×5、
`disk5: I/O error`×897、`smp2p=00000000`×2764、`join=0`（HIF 没起来），
与 v32 §3 的 Data 重建期 UFS 瞬时超时同形 ⇒ **该故障冷启动也会出现**。
按 v32 §3「再启动一次即可恢复」重跑即有效（0 UFS 故障）。

**恢复**：`reboot-fastboot` + 重新 boot 即可；**不要清 marker**（v25：
journal 已 complete 且 identity 匹配 seed-613，清了丢 Data）。这是恢复不是修根因。
**下一步**：一次 >20 min 连续采集 + 末尾 `status`/心跳，抓转变瞬间。

---

## 4. 参考项目：为什么"别人能做"

固件都是 Qualcomm 闭源 blob，但**没人在逆向固件**——所有项目照抄的是
Linux 主线开源 `ath10k/ath11k/ath12k` 的 WMI/HTT 协议。差距不在闭源。

- **非同芯片**（`pong` QCA6490/ath11k PCIe、`unixnu-wcn7860` WCN7860/PCIe-MHI）：
  总线/DMA 架构不同，能抄的是**系统层**（InterfaceNamer 终止标记、
  DNS 发布、ifnet/mbuf 交付、Apple80211 能力），数据面不可移植。
  这部分已用于打通 configd/DNS/设置。
- **同芯片同总线可逐行抄数据面的只有 Trogdor**（SC7180 / WCN3990 / SNOC）。
  但 `WlanRadio.c:3705` 明确只应答 **"pairwise CCMP, group CCMP"**，
  `wj_send_key():3965` 硬编码 `cipher=4` / 16 B key / MIC `0/0/0`，
  **从未实现 TKIP 分支**。本机 AP 恰是 `group=TKIP` 混合模式
  ⇒ 我们踩进了参考实现没覆盖的分支。
- 主线正确时序：`PEER_MAP → install_key → PEER_AUTHORIZE`
  （`mac.c:6616/6706`、`txrx.c:217`）；Trogdor 在 `WJ_M4`（msg4 被 ACK
  或 300 ms）才装 key（`:4682`），我们在 msg3 handler 内立刻装
  （`Wcn3990Hif.cpp:3309`）。

**零成本用户验证**：把路由器改成「WPA2-PSK / 仅 AES」或开手机热点
（现代热点通常纯 CCMP），让设备连它。若网页立刻能开 ⇒ 一刀锁死是混合模式
cipher 配置；若仍打不开 ⇒ 才继续查别的。不用改代码、不用 CI。

---

## 5. 代码与产物

| 项 | 值 |
|---|---|
| HEAD | `9e4dfe3`（chain-614 判据 + keyfix）、`60417b5`（rekey，已证伪/移除） |
| seed | **seed-613**，设备 Data 一致（`phase=2`），本轮未刷 userdata / 未清 marker |
| CI | `43571493379` SUCCESS（`9e4dfe3`）；`43567243311` SUCCESS（`60417b5`）；`43562360905` SUCCESS（RX 探针） |
| 上机链 | `work/chain-614.sh <run>`（kf0/kf1，含 VALID 重试） |
| 历史链 | `work/chain-613.sh` / `chain-613-rerun.sh` / `chain-612.sh` |
| 脚本 | `work/fix-late-peer-rekey.py`、`work/fix-rx-verdict-and-tkip-mic.py` |
| cap | `out/full-flash/logs/console-exp-{rx,rk0,rk1,kf0,kf1}.cap` |
| 磁盘 | C: 剩 ~0.9 G（**不要**下载 5.87 GB `picasso-userdata-613`，会爆盘） |

命令：
```sh
bash work/chain-614.sh 43571493379     # kf0/kf1 各 400 s，自动重试无效轮
# 判据看心跳 uni=/grp=（无上限），不是 RX: # 行数
```

---

## 6. 铁律增补（11-14）

1. **判据先自审**：任何"计数=N 就断言冻结"前，先找代码里的
   `if (x < N)` 打印上限。本轮已踩：`htt=64`、`RX: #` 64 行。
2. **设备状态 ≠ 复位原因**：设备在 fastboot 不等于它自己复位过，
   先问用户 / 查 `reset=` 语义。
3. **A/B 前置 checkpoint 不成立即无效轮**：`join=5` / `WPA: install` /
   `DATASTORE ready=1` 任一缺失，必须重跑，不得把 0 单播解读为修复无效
   （chain-613 rk1 首轮 UFS 故障就是例子）。
4. **归因被证伪立刻写文档**：rekey 已实机证伪，不得留在代码里当"可能有用"。
5. **闭源不是障碍**：能抄的是 Linux 开源驱动协议；抄之前先确认参考实现
   覆盖了当前配置组合（本机 = TKIP 混合模式，Trogdor = 纯 CCMP）。
6. 其余沿用 v24 §6：一次只派一个 CI、`-f fast=true`、采集 ≥400 s、
   kern_exec exec 路径禁加探针、995 B cmdline 等长替换、
   用户 6 处未提交改动不碰、`abi-check.py` 0 HARD、patch/源码 LF、
   commit message 不写未实机验证的结论。

---

## 7. 下一步（按判决分支）

- **kf1 若 `uni>0`** ⇒ TKIP MIC 是根因，固化 `keyfix` 默认 1，
  再让用户实测 Safari / 设置 > Wi-Fi 详情。
- **kf1 若 `uni=0`** ⇒ keyfix 证伪，排除 TKIP MIC；下一刀按
  §4 的**换纯 CCMP AP** 零成本验证（区分"cipher 组合"与"固件行为"），
  或加 `WMI_PEER_ASSOC` 的 crypto flag / `WMI_VDEV_PARAM` 隐私位探针。
- **并行必修**：§3 的运行期卷 I/O 挂死 → UI 冻结，需 >20 min 长时采集。
- 用户可操作判据（日志不可判）：设置 > Wi-Fi 详情不卡、Safari 域名解析。

**不要**在新证据前重复清 marker、整树 rebase、或再次尝试 rekey。
