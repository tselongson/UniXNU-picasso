# WiFi (WCN3990) 交接 — 2026-11-14 v35（协议栈入向：因果反转与 wifix 修复）

**时间**：2026-11-14 18:5x
**HEAD**：`e20ca34`（fork `kd439-pool`；`b074059` 为 wifix 修复本体、
`b1af80f` 默认化、`e20ca34` = pskeep + CE1 诊断，parent `58cd878`）
**CI**：`43587191778` SUCCESS（wifix A/B）、`43590692930`（默认化收尾）、
`43596531534`（pskeep，复用 seed-613）
**设备**：在线（`64993333 fastboot`），PC `192.168.101.4` 与网关 `192.168.101.1` 同网段
**唯一权威状态源**：本文件。v34 及更早为历史归档。

---

## 0. 一句话状态

```text
固件/HIF/扫描/关联/WPA2 握手/装密钥/PEER_AUTHORIZE/DHCP/DNS/键盘/TX ✅
  ── 驱动层广播接收 ✅（grp=44..116）
  ── 驱动层单播接收 ❌ uni=0        ← v33/v34 当成"固件不交付单播"查了三天
  ── PC ping 不通 / 单播不入栈 ❌
  ★ 本轮定位：根因是【en0 的 IF_LLADDR 与空口 STA MAC 不一致】（确定性、逐行可证）
  ★★ chain-617 w0 实机同时更正了 v34 的两条结论（见 §1.5）：
    · 协议栈【有】回 ARP reply：19 个 TX ARP 帧里 2 个是单播 dst=网关 MAC。
      v34「不回 ARP」是用 RX 侧 `arp op=2` 计数（且有 64 行上限）判的，属判据错误。
    · 协议注册 / DLIL input 线程 / in_ifaddr 全部正常：
      `noproto=0`、`stack_in==recv`、`addr=192.168.101.18`。
  ⇒ 真相是「ARP reply 发出去了，但里面填的 MAC 与 802.11 SA 不一致，对端不接受」。
  ★★ w1（wifix=1）实机闭环：PC `ping 192.168.101.18` **6/6 全回 0% 丢包**、
      `uni=0 → 41`、单播 ARP reply 被接受、三层 IPv4 流量、0 PANIC / 0 UFS ✅
  ── 已默认化（b1af80f，收尾 CI `43590692930`），不带新 boot-arg 即为修复后行为
  ── ⚠ 剩余：运行约 6 分钟后 WiFi RX 数据面停摆（§5.4，另一条线，未修）；
      Safari 端到端 HTTP/HTTPS 本轮**未做用户实测**
  ★ chain-619 干净 A/B 判决（20:2x，两组卷全健康、0 PANIC）：
      pspoke=0/1 采集期内 ping 均 6/6、采集后均 0/20 ⇒ pspoke 再次证伪。
      现成心跳计数把停摆层级定位为「CE2/WMI 信标照常、CE1 HTT 数据消息静默」：
      ps0 drri 持续推进而 ord/msdu/httidx/uni/grp 全冻结；ring=1/1 说明
      attention-drain 未参与，全部 MSDU 走 RX_IN_ORD + repost。
  ★ chain-621 判决（21:1x，CI 43596531534 SUCCESS）：pskeep 证伪。
      k0(pskeep=0) 采集后 ping 20/20，k1(pskeep=1) 13/20 且 5 次超时；
      实时现场 psk=14→19 一直在重发 PS-off，ce1=204 仍冻结 27 个心跳周期
      ⇒ PS 方向关闭，默认值保持 0。k0 的 20/20 只是窗口内尚未停摆，不是修复。
  ★★ chain-622 判决（23:2x，CI 43606329627 SUCCESS）：拿了决定性的 rri1。
      `CE1 STALL #1 polls=210000 ce1=341 drri1=00150015 sw1=21 post1=20 nb1=0`
      ⇒ 固件 CE1 写指针（21）与 host 读指针（21）**完全同步**、当前槽 nbytes=0
      ⇒ **是固件停止写 CE1，不是 host 读不到已完成描述符**。
      ★ 同时纠正 chain-621 的一个误判：停摆不是「CE1 单独静默」，而是
      **整个 target→host 方向停止** —— `rx=3069 / beacons=2899 / mgmtev=2899`
      与 `httidx=1262 / ord=239 / msdu=240` 一起在 `polls=189000`（约 810 s）
      冻结，CE2 的 `drri=4` 也冻结；而 TX 仍正常入队（`WPA: tx frame rc=0`）。
  ── 下一刀：停摆时发 WMI 命令看固件是否还响应 + 主动 VDEV_DOWN/UP 重关联，
      既验证「固件 RX 路径挂起 vs 固件崩溃」，也可能成为运行期保活
  ⚠ 网络层已闭环，但「Safari 能开网页」未实测，不得宣称全部完成
```

**因果必须反过来读**：v33/v34 把 `uni=0` 当根因，按"固件 TKIP 混合模式不交付单播"
查了 `rekey` / `keyfix` / `pspoke` 三个假设，全部实机证伪。但 `uni/grp` 是
**驱动层** `Wcn3990Hif.cpp:983-984` 在 `hakios_net_input()` **之前**统计的，
它只说明"固件交了什么帧"，**不说明协议栈处理了什么**。三个假设全灭正是因为
方向反了：不是固件不给单播，是**对端学到的 MAC 与空口 STA 不一致，单播根本
发不到手机**（`arp reply` 也一样，见 §1.5）。

---

## 1. ★ 精准定位链（全部 Apple 官方 / XNU 源码 / 本仓逐行，非推测）

### 1.1 先纠正 v34 的一处推理错误：`rc=0` 什么都没证明

`console-exp-rc.cap`（HEAD `58cd878`）8 条采样全部 `net: in #n rc=0 netin=0`，
v34 §2.0b 据此断言"帧确实被投递进协议栈，`ifp->if_input` 不是 NULL"。

- XNU `bsd/net/dlil.c:2051`：`ifnet_input()` → `ifnet_input_common()`，
  最终 `err = (*input_func)(ifp, m_head, ...)`，`input_func = ifp->if_input_dlil`。
  **这个版本根本没有 `ifp->if_input` 这条路径**（`if_input` 是 FreeBSD/Linux 概念）。
- Apple《Network Interfaces and Protocol Plumbers》(NKE Conceptual) 原文：
  > "The hardware driver or its support code calls ifnet_input ... **The packet is
  > queued. Processing resumes on a different thread.** ... **Any packets not
  > matching an attached protocol are dropped**, as are any promiscuous packets."

⇒ `rc=0` 只证明**入队成功**；帧可能随后被 `dlil_input_en0` 线程丢弃，
也可能该线程根本没被调度。**"if_input 为 NULL" 与 "帧已进栈" 两个结论都不成立。**

### 1.2 真正的入向链与两个可丢弃点

```text
ifnet_input()                          [dlil.c:2051]  入队, 返回 0
  → dlil_input_en0 线程                [dlil_input.c:852]
      error = (*ifp->if_demux)(ifp, m, frame_header, &protocol_family)
            = ether_demux()            [ether_if_module.c:448]
              · 从 m_pkthdr.pkt_hdr 读 ether_type
              · 遍历 desc_blk 找 DLIL_DESC_ETYPE2 == ETHERTYPE_ARP/IP
              · 找不到 → return ENOENT                       ← 丢弃点 A
      · protocol_family=0 / 无匹配协议 → 帧被 drop, no_protocol++ ← 丢弃点 A'
  → ether_inet_input() → in_arpinput() [in_arp.c:1650+]
      · 需 in_ifaddr 挂在【接收 ifp】且 IP == target_ip      ← 丢弃点 B
      · match: bcmp(sender_hw, IF_LLADDR(ifp), sdl_alen)==0 → 忽略自发帧
  → ARP reply / ip_input
```

`desc_blk` 由 `ether_add_proto()` 填充，而它只在 **`ether_attach_inet()`**
（`bsd/net/ether_inet_pr_module.c:558`，`ifnet_attach_protocol_v2` 注册
`ETHERTYPE_IP` + `ETHERTYPE_ARP` 两条 demux）被调用时才出现。

`ether_attach_inet()` 的调用链（逐条 git grep 确认）：

```text
proto_register_plumber(PF_INET, APPLE_IF_FAM_ETHERNET=2, ether_attach_inet, ...)
    [ether_if_module.c:742, 由 bsd_init.c:837 ether_family_init() 在 NETHER>0 时调用]
proto_plumb(PF_INET, ifp)              [kpi_protocol.c:399]
  ← in_domifattach(ifp)                [netinet/in.c:285]
      ← in_control(SIOCPROTOATTACH)    [in.c:1202]  ← 返回真实 errno
      ← in_control(SIOCAIFADDR)        [in.c:1468]  ← "(void) in_domifattach(ifp); error = 0;"
                                                        ★ 错误被丢弃，失败只 log(LOG_ERR)
```

**`ifnet_attach()` 不做 plumb**（`dlil.c:4815` 全函数无 `proto_plumb` 调用；
`git grep proto_plumb(` 全域只有 `in.c:285`、`in6_ifattach.c:478/674` 三处调用者）。
所以 en0 上有没有 IP/ARP 协议，完全取决于 `in_domifattach()` 是否成功过一次，
而它**失败时串口看不到任何东西**（`log(LOG_ERR)` 进内核消息缓冲，不是串口）。

**已在本轮把所有 cap 全文检索过**：`can't attach ip`、`proto_plumb returned`、
`WARNING.*attach` 全部 0 条 —— 这**不能**证明 plumb 成功，因为
`proto_plumb` 找不到 plumber 时根本不会调用 `ether_attach_inet()`，也就没有
`WARNING`。日志本身还会丢行（项目铁律）。⇒ 必须靠 `wifix=2` 的
`SIOCPROTOATTACH rc=` 和 `noproto=` 计数定论。

### 1.3 ★ 确定性缺陷：两个 MAC 不一致（纯主机侧，与芯片/固件无关）

| 位置 | 值 |
|---|---|
| `Wcn3990Hif.cpp:4562` `WMI_PDEV_SET_BASE_MACADDR`（固件 base MAC） | `02:00:00:00:00:01` |
| `Wcn3990Hif.cpp:3101` 数据帧 802.11 **addr2 / SA** | `g_join_mac` = `02:00:00:00:00:01` |
| `Wcn3990Hif.cpp:3415` 管理帧 SA | `g_join_mac` |
| `Wcn3990Hif.cpp:3237` PTK 推导的 `own` | `02:00:00:00:00:01` |
| `Wcn3990Hif.cpp:4926` 内核 DHCP `chaddr` | `g_join_mac` |
| **`PicassoWlan.cpp:3960` `picasso_wlan_mac` → en0 `.address` → `IF_LLADDR()`** | **`00:0a:f5:39:90:01`** ❌ |

`wcn3990_hif_net_tx()`（`Wcn3990Hif.cpp:3092-3108`）把以太帧的**整个 14 字节头丢掉**：
```c
memcpy(frame + 4,  g_join_ap.b.bssid, 6);   /* addr1: AP */
memcpy(frame + 10, g_join_mac, 6);          /* addr2: 固定 02:00:... */
memcpy(frame + 16, eth, 6);                 /* addr3: 只取 DA */
memcpy(frame + 32, eth + 14, n);            /* 以太源 MAC 被丢弃 */
```
而 ARP/IP 载荷里的 sender hardware address 由 `ether_frameout_extended` 用
`IF_LLADDR(en0)` = `00:0a:f5:39:90:01` 填写。实机日志对得上：
```
picasso_wlan_tx: [DATA PLANE] ARP frame 42 bytes: dst ff:ff:... src 00:0a:f5:39:90:01
```
⇒ **同一个 STA 在 802.11 层是 `02:00:...`，在 ARP 载荷里自称 `00:0a:f5:...`。**

后果（Apple 源码逐条）：
1. 网关/PC 从 ARP 学到 `.18 → 00:0a:f5:39:90:01`，发单播回包时 dst 用这个 MAC；
   AP 的桥接表里注册的是 `02:00:00:00:00:01` ⇒ **单播帧根本不会投递给手机**
   ⇒ 驱动层 `uni=0`。广播不需要查桥接表 ⇒ `grp` 正常。
2. 即便协议栈生成 ARP reply，其 sender hw 也是 `00:0a:f5:...`，对端学到错 MAC。
3. 万一有单播帧真的到达，`ether_demux()`（`ether_if_module.c:472-478`）
   ```c
   if (_ether_cmp(eh->ether_dhost, IF_LLADDR(ifp))) m->m_flags |= M_PROMISC;
   ```
   会把 dst=`02:00:...` 的帧判为**不属于本接口**并置 `M_PROMISC`，
   DLIL 依官方文档"promiscuous packets are dropped"丢弃。
4. DHCP 能成功，恰恰因为内核 DHCP 客户端 `chaddr` 用 `g_join_mac`
   （`Wcn3990Hif.cpp:4926`），与空口 SA 一致；且 DISCOVER/OFFER 都是广播。
   **DHCP / DNS 全部是旁路，从不经过协议栈入向。**

⇒ 所有"已打通"的能力都是旁路；所有需要协议栈入向的能力全废。因果闭合。

### 1.4 与不同芯片参考项目的核对（用户指定方向）

| 项目 | 芯片/总线 | 与入向相关的事实 |
|---|---|---|
| `upstream/sheng/package/WifiNet.cpp` | WCN7860 / ath12k PCIe | `:485 check_input()` 每秒比对 `ifnet_stat().packets_in` 与驱动 `gRxFrames`，注释原文：*"The interface's input thread made runnable and then never run while frames kept coming (i468: from 231 s on, a priority 81 thread with no stack, so **the DHCP answers never reached the client** and the lease was lost)"*；停摆时 `boot_cpu_kick()` 重发 AST；`:533 trafficLog()` 每秒调用 |
| `pong-wifi/PongWifiNet.cpp` | QCA6490 / ath11k PCIe | 同款 `gStall` / `check_input()` / `log_traffic()`（`:111`、`:707-714`） |
| `Trogdor-ipadOS/.../CoachzWifiNet.cpp` | **WCN3990 SNOC** | `ifnet_attach(gIf, nullptr)` + `ifnet_input` + `mbuf_adj(m,14)`，dhcp 走 configd 原生链 |
| `unixnu-wcn7860/.../PeachNet.c` | WCN7860 | `ifnet_allocate_extended` 参数、`ifnet_attach(p,NULL)`、`mbuf_pkthdr_setheader` + `ifnet_input` 与本仓同源 |
| `work/refs/apple-oss/xnu` `bsd/net/if_fake.c:3758` | Apple 官方虚拟接口 | `mbuf_pkthdr_setheader` + `m_adj(14)` + `ifnet_input` 与本仓 `hakios_net_input_unit()` **逐行同构** ⇒ mbuf 形态可排除（v34 §2.0b 第 3 条候选作废） |
| `work/refs/xnu8792/kpi_interface.h`（本轮下载，iOS16 / xnu-8792.61.2） | 设备 20H392 | `struct ifnet_stats_param` 含 `no_protocol`（`:1184`）、`ifnet_stat`（`:2684`）、`ifnet_get_address_list_family`（`:2755`）、`ifaddr_address`（`:3055`）全部**公开 KPI**，编译风险已排除 |

**确定性差异清单**：本仓缺 `ifnet_stat` 入向计数、缺 en0 AF_INET 地址快照、
缺 DLIL input thread 停摆检测、且 en0 lladdr 与空口 STA 不一致。前两项已由
`b074059` 补上；第三项经 w0 实机判定**不需要**（`stack_in==recv`，input 线程正常）。

### 1.5 ★★ chain-617 w0 实机判决：三候选全排除，v34「不回 ARP」是判据错误

`out/full-flash/logs/console-exp-w0.cap`（750,969 B，400 s，`wifix=0` 对照组，
有效轮 `install=1 / join5=45 / DHCP bound`），checkpoint 明文：

```text
net: en0 in recv=7  stack_in=7  stack_out=7  noproto=0 err=0 addr=none
net: en0 in recv=26 stack_in=26 stack_out=48 noproto=0 err=0 addr=192.168.101.18 drop=0/2
net: en0 in recv=47 stack_in=47 stack_out=72 noproto=0 err=0 addr=192.168.101.18 drop=0/2
net: en0 in recv=61 stack_in=61 stack_out=95 noproto=0 err=0 addr=192.168.101.18 drop=0/2
```

⇒ **PF_INET/ARP 协议已注册**（`noproto=0`，`ether_demux` 匹配成功）、
**DLIL input 线程在跑**（`stack_in` 严格等于驱动 `recv`）、
**`in_ifaddr` 挂在 en0**（`addr=192.168.101.18`）。
§1.2 的两个"可丢弃点 A/B"及 input-thread 停摆假设**全部实机证伪**，
`wifix=2` 的 plumb 假设同属被证伪（保留为幂等诊断，其 `rc` 可交叉印证）。

**更关键：v34 §1.4「协议栈不回 ARP」是判据错误。**
- TX 侧 `PicassoWlan.cpp:3277` 的 `picasso_wlan_tx: [DATA PLANE] ARP frame 42 bytes: dst ... src ...`
  **完全不打印 ARP opcode**；带 `arp op=` 的是 `Wcn3990Hif.cpp:1038` 的 **RX** 侧
  `hif_rx_trace()`，且只有 64 行上限（v33 §2.6 已记这个上限）。
  用「RX 里没有 `arp op=2`」推断「我们没有回 ARP reply」是无效推理。
- 直接按**目的 MAC**统计 w0 全部 19 个发出的 ARP 帧：
  ```text
  17 × dst ff:ff:ff:ff:ff:ff          (request / announcement)
   2 × dst 94:32:c1:ad:33:e6          (★ 单播 = ARP REPLY，发给网关)
  ```
  两条单播 reply 与网关两次 `arp op=1 192.168.101.1>192.168.101.18`
  （cap 行 7426 / 9010，**均在 DHCP bound 行 6279 之后**、地址已装）**逐行一一对应**：

  ```text
  7426  RX #35  arp op=1 192.168.101.1>192.168.101.18          (网关问)
  7427  TX ARP frame dst 94:32:c1:ad:33:e6 src 00:0a:f5:39:90:01  ← reply
  7428  [REAL DATA PLANE] ARP 42 bytes over HTT TX_FRM          ← 真发上空口
  7430  tx compl status=0 count=1 ack=66 noack=0 discard=0      ← AP 已 ACK
  9010  RX #61  同上
  9011  TX ARP frame dst 94:32:c1:ad:33:e6 src 00:0a:f5:39:90:01
  9013  tx compl status=0 count=1 ack=103 noack=0 discard=0
  ```

  即：**协议栈每次收到网关的 ARP request，下一行就回了单播 reply，AP 也 ACK 了。**
  v34「协议栈不回 ARP」100% 是判据错误，不是故障。
- PC 侧 `ping -n 4 192.168.101.18` 仍是「请求超时 ×2 + 来自 192.168.101.4 的
  回复: 无法访问目标主机 ×2」，`PING_RC=0` 但**真实回包 0**（Windows 对本机
  生成的 unreachable 也返回 0，不可当判据）。

⇒ **协议栈回了 ARP reply，但对端没有接受/学到。** 唯一确定性差异就是
§1.3 的 MAC 不一致：以太帧 src 与 ARP 载荷 `arp_sha` 都是
`00:0a:f5:39:90:01`，而 802.11 SA 被 `wcn3990_hif_net_tx()` 强制改成
`02:00:00:00:00:01`。AP 的桥接/ARP snooping 看到两者不符，学到的
`.18 → 00:0a:f5:...` 也无法对应到已关联的 STA ⇒ 单播（含 PC 的 ping）
始终不能投递。**这正是 `wifix=1` 把 en0 lladdr 对齐到 `g_join_mac` 要修的。**

---

## 2. 三个已实机证伪的假设（不要再试）

`rekey`（chain-613，60417b5）、`keyfix`/TKIP MIC 8/8（chain-614，9e4dfe3）、
`pspoke`/STA PS workaround（chain-615，0b00c94）。判据均为无上限 `uni=/grp=` 计数，
且两组都是有效轮。另已排除：PTK 离线独立复算完全正确（PMK/KCK/TK 全匹配）、
主机侧 key 命令/INIT/join/TX 帧/RX 路径与 ath10k+Trogdor 逐项一致、
`if_input=NULL`（v34 结论本身不成立，见 §1.1）、mbuf 形态（if_fake 同构）。

---

## 3. 本轮改动（commit `b074059`，3 files +99/−1）

**全部在 `wifi-wcn3990/source/`，不在 `recipe()` 覆盖范围内** ⇒
seed-613 身份不变，**禁止清 marker / 禁止刷 userdata**。
可复现脚本 `work/fix-wifi-input-617.py`（从 `58cd878` 逐字节复现，`ALL_IDENTICAL=True`）。

单一等长 boot-arg `wifix=0|1|2`（995 B，全镜像仅 1 字节差，offset `60233886`）：

| 值 | 行为 |
|---|---|
| `0` | 完全旧行为（对照组） |
| `1` | `PicassoWlan.cpp` `picasso_wlan_net_attach()` 把 `picasso_wlan_mac` 对齐为 `02:00:00:00:00:01`，en0 `IF_LLADDR` 与空口 SA/固件 base MAC/DHCP chaddr 统一 |
| `2` | `1` + `hakios.dhcp.c` 在 `SIOCAIFADDR` 之后显式发 Apple 官方 `SIOCPROTOATTACH`（`bsd/sys/sockio_private.h:120 = _IOWR('i',80,struct ifreq)`，Apple 自家 `tests/net_test_lib.c:412 ifnet_attach_ip()` 同款），打印真实 errno。幂等，失败无害 |

无条件、有界（每 ~8 s 一行）诊断：`hakios.net.c` `net_thread` 打印
```
net: en0 in recv=N stack_in=N stack_out=N noproto=N err=N addr=a.b.c.d drop=N/N
```
- `recv` = 驱动交给 `ifnet_input` 的帧数
- `stack_in` / `no_protocol` / `errors_in` = `ifnet_stat()` 栈层计数
- `addr` = `ifnet_get_address_list_family(AF_INET)` 里 en0 的第一个 IPv4 地址

**判据解释（w0 已分流，结论见 §1.5）**：
| 现象 | 结论 / 下一刀 |
|---|---|
| `noproto` 随 `recv` 上涨 | PF_INET/ARP 未 plumb；看 `DHCP: plumb SIOCPROTOATTACH rc`（0/17 已注册、6=无 plumber、1=EPERM）。**w0 实测 0 ⇒ 已排除** |
| `stack_in` 恒 0 而 `recv` 涨 | `dlil_input_en0` 线程停摆 ⇒ 照 sheng/pong `check_input()` + CPU AST kick。**w0 实测相等 ⇒ 已排除** |
| `addr=none` | `in_ifaddr` 没挂到 en0。**w0 实测 `.18` ⇒ 已排除** |
| TX ARP 帧出现**单播 dst** | 那就是 ARP reply；**w0 有 2 条** ⇒ 协议栈会回 ARP，v34 结论作废 |
| PC `ping` 出现源为手机 IP 的真实回包 | **修好**（唯一外部硬判据） |

⚠ **两个判据陷阱（本轮踩过，写进铁律）**：
1. TX 侧 `[DATA PLANE] ARP frame` 打印**没有 opcode**；`arp op=2` 只存在于 RX 侧
   且受 64 行上限截断。判断"是否回了 ARP"必须用 **TX 帧的 dst MAC**（单播=reply）。
2. Windows `ping` 对**自己生成的**「无法访问目标主机」也返回 rc=0，统计里
   还计入"已接收"。必须解析回包来源 IP，不能看 `PING_RC`。

### 门禁（全过）
- `work/check-src.py`：两个 driver 文件 ok；括号/圆括号平衡
  `PicassoWlan.cpp 1022/1022、3194/3194`，`hakios.dhcp.c 85/85、332/332`，
  `hakios.net.c 56/56、239/239`
- `work/abi-check.py`：**0 HARD**（4 个既有 soft）
- 离线单变量预演：`wifix=0/1/2` 各 **995 B**、53 tokens、必需参数全在、
  两两 `byte diff 1`（offset `60233886`）
- iOS16 官方头 `work/refs/xnu8792/kpi_interface.h` 确认全部 KPI 与字段存在
- 用户 6 处未提交改动（5 个 `docs/evidence/*.log` 删除 + `diagnostic-charter.md`）
  **原样保留**，提交只含 3 个文件

---

## 4. 上机链与判据

```sh
cd ~/miphone
ARMS="w0 w2 w1" bash work/chain-617.sh 43587191778
```
链自动：等 CI success → 下载 `picasso-bootimg-*` → **校验 4 条探针字符串真的在内核镜像里**
→ `mk-autojoin-args.py` → 生成 `wifix=0/1/2` 三组 995 B 镜像（离线断言单字节差）
→ 每组 `fastboot boot` + 400 s 采集 + VALID 重试（`install` / `join=5`）
→ **每组采集结束后自动从 cap 提取 `DHCP: bound inet` 的 IP 并执行 PC 侧 `ping -n 4`**
（外部硬判据，不依赖我们的日志）→ 汇总 `noproto` / `addr` / `plumb rc` / `arp op=2` / `uni,grp`。

⚠ 同 seed-613 ⇒ **链里没有 marker-clear，没有 userdata 刷写**。
⚠ PC `192.168.101.4` 已实测能 ping 通网关 `192.168.101.1`（1 ms），同网段成立。

**唯一用户可操作判据**：设备留在桌面时，`ping -n 6 192.168.101.18`
（IP 以当轮 `DHCP: bound` 为准）+ `arp -a | findstr 192.168.101.18`；
有回包 + 有 ARP 条目 = 协议栈入向打通。然后 Safari 输域名、设置 > Wi-Fi 详情。

---

## 5. 实机判决

### 5.1 CI 与 w0（已完成）
- CI `43587191778` **SUCCESS**，`picasso-bootimg-*` 65,105,920 B，四条探针字符串
  （`net: en%u in recv=` / `DHCP: plumb SIOCPROTOATTACH rc=%d` / `wifix=%u lladdr=` /
  `SIOCPROTOATTACH`）全部在镜像内核里，`CHAIN617_PROBES_OK`。
- `wifix=0/1/2` 三组各 995 B、两两仅 1 字节差（offset `60242078`），`SINGLE_VARIABLE_OK`。
- **w0 有效轮**（400 s，`install=1 / join5=45 / DHCP bound 192.168.101.18`），
  明细见 §1.5。PC ping 真实回包 **0**，`arp -a` 无 `.18` 条目。
- w2 首轮**无效环境轮**（`beacons=0 / join=0 / install=0 / eapol=4`，HIF 起来但
  AP 无响应；v32 §3 / v34 §5 记录过同形冷启动故障），按铁律作废重跑。

### 5.2 链的两处修正（首跑暴露）
1. `chain-616` 沿袭下来的 `python3 ... || exit 16` 会让**无效轮直接终止整条链**，
   重试循环形同虚设 —— w1 因此完全没跑。已去掉 `|| exit 16`，并把 VALID 判据
   补上 `DHCP: bound`（光看 `install` + `join=5` 不够）。
2. 首跑的 PC ping 在 400 s 采集**结束后**才发，串口已停，看不到 PC 的 ARP
   request 和手机的应答。已改为在采集进行到 **300 s 时**发 `ping -n 6`
   （`work/ping-during-617-*.out`），并解析"来源为手机 IP"的真实回包。

### 5.3 ★ w1（`wifix=1`）：WiFi 单播双向打通，闭环成立

`ARMS="w1 w2" bash work/chain-617.sh 43587191778`（`work/chain-617-rerun.out`），
400 s 有效轮（`install / join=5×N / DHCP: bound 192.168.101.18`）。

| 判据 | w0 对照 | **w1** | **w2** |
|---|---|---|---|
| en0 lladdr | `00:0a:f5:39:90:01` | **`02:00:00:00:00:01`** ✅ | 同 w1 |
| TX ARP 帧 src | `00:0a:f5:39:90:01` | **`02:00:00:00:00:01`**（全部） | 同 w1 |
| 发给 PC 的单播 ARP reply | 0 | **有**（`dst 90:78:41:a1:dc:f9`） | 有 |
| **RX 侧 `arp op=2`（对端给我们的单播 reply）** | **0** | **2**（网关） | **3**（网关×2 + PC×1） |
| PC 的 ARP request 是否到达手机 | 未采集到 | — | **明确出现**（`#38 ... 192.168.101.4>192.168.101.18`） |
| **PC `ping` 真实回包（采集第 300 s）** | 0 | **6/6，0% 丢失，TTL=64，3–64 ms** ✅ | **6/6，0% 丢失，3–66 ms** ✅ |
| 驱动层 `uni` / `grp` | 0 / 44 | **41 / 72** ✅ | **38 / 45** ✅ |
| `noproto` / `stack_in` | 0 / ==recv | 0 / ==recv（113/113） | 0 / ==recv（83/83） |
| 三层流量 / DNS 发布 | 无 | IPv4 多帧、`MobileSafari` 启动 | `dns set=1`×3 |
| `PANIC` / `NATIVE_UFS_FAILURE` / UFS 错误 | 0 | **0 / 0 / 0** | **0 / 0 / 0** |
| `DATASTORE ready=1` / TOUCH | 1 | 1 / `d_frames=36 d_hid=36` | 1 |

**最关键的一条**：w1/w2 的 RX 帧里出现了
`dst=02:00:00:00:00:01 src=94:32:c1:ad:33:e6 arp op=2 ...` ——
**网关/PC 发来的单播 ARP reply，目的 MAC 正是我们统一后的 STA 地址**。
w0 时所有 RX 帧的 dst 都是广播/组播，从无单播。这一条直接证明对端现在
能正确学到并投递到手机，是 MAC 修复生效的最硬证据。

`work/ping-during-617-w1-a1.out` 原文：`来自 192.168.101.18 的回复: 字节=32
时间=4ms TTL=64` ×6，`数据包: 已发送 = 6，已接收 = 6，丢失 = 0 (0% 丢失)`。

**w2（`wifix=2`）判决同样通过，并给出决定性旁证**：
```
PicassoModem: ... wifix=2 lladdr=02:00:00:00:00:01
DHCP: plumb SIOCPROTOATTACH rc=17        (EEXIST = 协议早已注册)
work/ping-during-617-w2-a1.out: 已发送=6 已接收=6 丢失=0 (0%)，3–66 ms，TTL=64
net: en0 in recv=13 stack_in=13 stack_out=14 noproto=0 addr=192.168.101.18
```
`rc=17` 直接坐实 §1.2 的 plumb 假设**证伪**：`ether_attach_inet()` 早就把
IP/ARP demux 注册好了，`SIOCPROTOATTACH` 只是幂等返回 EEXIST。w1 与 w2 等价，
**唯一有效变量就是 en0 的 lladdr**。

⇒ **根因唯一确定并闭环**：把 en0 `IF_LLADDR` 对齐到固件/空口使用的 STA MAC
之后，对端能正确学到 `.18` 的 MAC，ARP reply 被接受，单播双向立即打通。
完整 A/B：`w0 ping 0/6` → `w1 6/6` → `w2 6/6`。

**默认化**：commit `b1af80f` 把 `PicassoWlan.cpp` 的 `wifix` 默认值改为 **1**，
`wifix=0` 仅保留给 A/B 对照。收尾 CI `43590692930`（`-f fast=true`，复用
seed-613）。日用参数链 `mk-autojoin-args.py` 生成的 995 B args 不含 `wifix`，
因此**新镜像不带任何新 boot-arg 即为修复后行为**。

### 5.4 ★ 剩余缺口：运行期 WiFi 接收数据面停摆（另一条线，未修）

w1 采集**结束后**的 PC ping 回落到 0（`REAL_PING_REPLIES=0`）。心跳序列给出
明确停摆点（v34 §5 同一现象，这次落在采集窗口内，取证更完整）：

```text
polls=47000 rx=906 beacons=730 uni=37 grp=72 ring=1/1 srri4=30 wr4=30
polls=48000 rx=921 beacons=745 uni=37 grp=72 ring=1/1 srri4=30 wr4=30
polls=49000 rx=936 beacons=761 uni=41 grp=72 ring=1/1 srri4=6  wr4=6   ← 最后一次增长
polls=50000..55000  rx=936 beacons=761 uni=41 grp=72 全部冻结
```
- `rx` / `beacons` / `ord=115` / `msdu=116` / `httidx=1138` 全停；
  **`drri` 仍从 `29` 变到 `2a`**（CE 读索引还在动）→ 不是整个 HIF poll 死掉。
- 同期 **TX 仍在发**（`[REAL DATA PLANE] IPv4 78/84 bytes over HTT TX_FRM`），
  `srri4==wr4`（CE4 TX ring 正常），`PICASSO_DISPLAY frame` 持续提交、
  `TOUCH d_frames=36`、GPU hangcheck 在跑 ⇒ **不是整机冻结/UI 卡死**。
- `PANIC=0`、`NATIVE_UFS_FAILURE=0`、UFS/APFS 错误 0（与 v34 §5 那次带
  `disk5 tx_enter failed` 的形态不同，是更干净的纯 RX 停摆）。
- 停摆点附近无异常日志，只有 `picasso_sta_scan: HEARD BEACON` 仍在打印
  （那是驱动主动查 BSS 表，不是 RX indication）。

**已核对、暂未定为根因**：HTT RX ring 是 2048 项，`g_htt_alloc` 每次
`(x+1) & 2047` 正确回绕（`:918`），`hif_wr_in_ord()` / `hif_wr_rx_ind()`
消费后都调 `hif_wr_post(pa)` repost（`:1166`、`:1204`），`httidx=1138 < 2048`
没有越界。`hif_wr_rx_ind()` 里 `if (!buf) return;` 会提前退出且不 repost，
但 `bad=0/0` 表明没有地址非法帧。**无实机证据证明哪一条是根因，不下结论。**

**下一棒（按可查性）**：① 加有界探针打印 `g_htt_alloc` / `g_htt_read` /
CE2 `nbytes` / `hif_wr_post` 次数，看停摆时 ring 是否已空；② 对照
`hif_service_htt_ring()`（attention-driven drain，注释明写 "Do not repost
here"）与 in_ord 路径是否重复消费或漏 repost；③ 长采集（>20 min）确认
是否间歇性自恢复（v32 §3「再启动一次即恢复」）。**不要靠猜改 CE/固件。**

#### ★ 20:2x 新发现：STA_POWERSAVE_MODE 的**重发时机**与参考实现不一致

这不是猜测，是 `work/trogdor/port/WlanRadio.c` 与我们驱动的逐行差异：

| | Trogdor `wj_begin():4589-4603` | 我们 `hif_join_once():3912-3941` |
|---|---|---|
| 8 条 pdev/vdev/sta_ps 命令 | `if (!once) { ... }` 只发一次 | `if (g_join_once) return;` 只发一次 ✅ 一致 |
| **`STA_POWERSAVE_MODE off (0x9001, TLV 0x6c)`** | **在 `if (!once)` 块之外，每次 `wj_begin()` 都重发** ❗ | **在 once 块内部，全生命周期只发一次** ❗ |

Trogdor 原文注释：`/* STA_POWERSAVE_MODE: off, with a thread that polls
every millisecond */` —— 轮询式驱动必须让固件**持续**保持 PS 关闭。
ath10k `mac.c:1974-2005` 同样是在 **vdev 已 started、关联完成后**由
`ath10k_mac_vif_ps()` 发 `WMI_STA_PS_MODE_DISABLED`，注释明写
「非运行态 vdev 要强制 enable PS；固件对过早关闭 PS 反应不好」。
我们的实机日志确认只发过一次：`JOIN: STA_POWERSAVE_MODE 0`
（`console-exp-final.cap`），调用点是 `g_join>=2` 分支里的
`hif_join_once()`（`:4196`，扫描/关联之前）。

⇒ **可证伪假设**：固件在关联后或运行期某阶段自行重新进入 PS，我们没有
按参考实现重发关闭命令，AP 开始缓存数据帧 ⇒ `httidx/ord/msdu/uni/grp`
冻结而 beacon 继续。这与 §5.4 的停摆形状完全吻合，且与
`pspoke`（`WMI_PEER_SET_PARAM(0xff,1)`，ath10k 在 `bss_assoc` vdev_up 之后
发的 PS workaround）是**同一条线上的不同手段**。

#### ★ chain-619 实机判决（20:36）：`pspoke` 再次证伪，且 ps1 有害

`work/chain-619-ps.sh`，零 CI、等长 `pspoke=0/1`（995 B、全镜像仅 1 字节差
offset 60242079），两组各 400 s **均为有效轮**（`install=1 join5=55 dhcp=1`）：

| 判据 | ps0 (`pspoke=0`) | ps1 (`pspoke=1`) |
|---|---|---|
| 采集第 300 s PC ping | **6/6 真实回包** | **6/6 真实回包** |
| **采集结束后 20 次 PC ping（主判据）** | **0/20** | **0/20** |
| `disk5 tx_enter` / `disk3 not ready` / `got err 16` | 0 / 0 / 0 | 0 / 0 / 0 |
| `NATIVE_UFS_FAILURE` / `PANIC BEGIN` / `dns set=1` | 0 / 0 / 3 | 0 / 0 / 3 |
| 采集期 `arp op=2` | 5 | 3 |
| 停摆点（最后一次 `ord` 增长） | `polls=34000` | `polls=44000` |

⇒ **两组都失败且卷全健康**，排除「存储故障冒充 PS 无效」，`pspoke`
（`WMI_PEER_SET_PARAM(0xff,1)`）在 lladdr 已修好的正确基线上**再次证伪**。

**采集后 ping 的原文纠正了一处判读**：20 条里 2 条 `请求超时`，另外 18 条是
`来自 192.168.101.4 的回复: 无法访问目标主机` —— 即 **PC 侧 ARP 缓存过期后
要不到应答**，不是手机整机掉线。因果是「手机不再回 ARP」。

**停摆层级（现成心跳计数，零成本取证）**：

```text
ps0: polls=34000 httidx=1122 ord=97 msdu=100 uni=45 grp=52 ring=1/1  ← 最后增长
     polls=55000 rx=1010 beacons=834 drri=32 仍在涨（CE2/WMI 信标正常）
                httidx/ord/msdu/uni/grp 五项全冻结，ind=0、ring=1/1
ps1: polls=47000 起 drri=4、rx=899、beacons=720 也全冻结（CE2 一并停）
```

- `ring=1/1` = `g_htt_ring_done_cnt/g_htt_ring_seen` 恒 1 ⇒ **attention-driven
  drain 只命中过一帧**，不是它的重复消费/漏 repost；所有 MSDU 都由
  `hif_wr_in_ord()` + `hif_wr_post()` 处理（`ind=0` 说明 RX_IND 从未出现）。
- ps0 的形状是 **CE2/WMI 管理管道继续跑、CE1 HTT 数据消息管道静默**；
  ps1 连 CE2 都停 ⇒ `pspoke` 不仅无效还有副作用，后续实验**不再带它**。
- 源码核对已排除：`wcn3990_hif_net_tx():3098` 的 802.11 FC 是
  `frame[0]=0x08 / frame[1]=0x41`（ToDS+Protected，**PM bit=0**），
  不是我们自己的 TX 帧误告诉 AP「我要睡」；`hif_service_ce1()` 的
  `g_post1` 初值 `dst_n-1` 是 `snoc.c ath10k_snoc_rx_post_pipe()` 的
  「留一槽」设计（`:5752` 注释），repost 指针领先读指针 n-1 属正确。

#### ★ chain-621 实机判决（21:1x）：`pskeep` 证伪，阻塞收敛到 CE1 管道

`e20ca34`（`work/fix-pskeep-621.py --apply`，+24/−3 单文件）：
- `pskeep=0|1` 门控，默认 0 行为不变；`=1` 在心跳里每 5000 polls
  重发 `hif_join_sta_powersave_mode(0)`（Trogdor `wj_begin()` 的块外重发）。
- 两个**无上限**诊断计数（旧 `g_htt_rx` 有 64 打印上限，无法判断 CE1 是否静默）：
  `psk` = PS-off 重发次数，`ce1` = 完成的 CE1 描述符数；两组都打印。
- 判据分流：`ce1` 仍涨而 `ord` 冻结 ⇒ 固件停止交付 MSDU（射频/PS 侧）；
  `ce1` 也冻结 ⇒ CE1 管道本身卡住（host repost / shadow doorbell 侧）。
- `work/chain-621-pskeep.sh`（CI `43596531534`），主判据仍是采集后 20 次
  ping + 卷健康快照；门禁全过（check-src / 括号 / abi-check 0 HARD /
  格式串 26 参数匹配 / 995 B 单字节差 / 明确断言 `pspoke` 不在 cmdline）。

**重发命令的正确性已逐字节核对**（避免白跑一轮 CI）：
`Wcn3990Hif.cpp:1397/1401` 的 `WMI_STA_POWERSAVE_MODE_CMDID=0x9001`、
`WMI_TLV_STA_POWERSAVE_MODE=108(0x6c)` 与 Trogdor
`wj_begin():4602-4605` 的 `wj_cmd_new(0x9001)` + `wj_tlv(m,4,0x6c,8)`
完全一致；payload `{vdev_id=0, mode=0}` 的 `mode=0` 正是 Linux
`wmi-master.h:5825 WMI_STA_PS_MODE_DISABLED`（`ENABLED=1`），
不是把语义写反。所以 `pskeep=1` 重发的确实是「PS off」。

**判决（`work/chain-621-pskeep.out`，CI `43596531534` SUCCESS）**：k0/k1 各 400 s
有效轮（`install=1 join5=45/55 dhcp=1`），两组卷错误 / PANIC / UFS failure 全 0。

| 判据 | k0 (`pskeep=0`) | k1 (`pskeep=1`) |
|---|---|---|
| 采集第 300 s PC ping | 6/6 | 6/6 |
| **采集后 20 次 PC ping（主判据）** | **20/20** | **13/20，5 次超时** |
| `psk` / `ce1`（采集末心跳 55000） | 0 / 156 | 11 / 173 |
| `dns set=1` / `arp op=2` / `PANIC` | 3 / 3 / 0 | 3 / 4 / 0 |

⇒ **`pskeep` 无效且略有害**（k1 更差），PS 假设关闭，默认值保持 0、不启用。
**k0 的 20/20 不是修复**：链结束后对设备的实时串口复查
（`work/621-pskeep-evidence/status-now.txt`）显示它照样会停摆。

**实时现场把层级定死**（`polls=71000→98000`，27 个心跳周期）：

```text
ce1=204  httidx=1146  ord=122  msdu=123  uni=58  grp=62   ← 全部冻结
rx=1267→1272   drri=34→39      ← CE2 / WMI 信标仍在推进
psk=14→19                       ← PS-off 一直在重发，仍停摆
[REAL DATA PLANE] IPv4 78/84 bytes over HTT TX_FRM  ← TX 还在发
卷错误 / PANIC / NATIVE_UFS_FAILURE 全 0，UI 健康
```

`ce1` 是我为这一轮新加的**无上限**计数（旧 `g_htt_rx` 有 64 打印上限，判不了后期静默）。
按预设判据，`ce1` 与数据面**一起冻结** ⇒ 不是「固件只不发数据消息但 CE1 还有
TX_COMPL 等流量」，而是 **CE1 这条管道整体静默**。于是：

- 已排除：attention-drain 重复消费（`ring=1/1` 只命中一帧）、RX_IND 路径（`ind=0`）、
  TX 帧误置 PM bit（`frame[1]=0x41`）、CE1 `g_post1=dst_n-1` 语义（snoc 留一槽设计）、
  **DMA 缓存一致性** —— `Wcn3990Hif.cpp:484` 明确 `g_dma` 是
  "physical base of the **uncached alias**"，`dst1[32]` 就在这块映射里。
- 仍未区分：固件不再往 CE1 写（`DST_WR` 状态机 / HTT RX buffer 计数）
  vs host 没读到已写的描述符（`g_sw1` 与 `nbytes`）。

#### ★★ chain-622 实机判决（23:2x，CI `43606329627` SUCCESS，commit `f524669`）

只加**纯诊断**（心跳 `drri1/sw1/po1/nb1` + 有界 `CE1 STALL` 快照 + `ce1kick` 门控，
默认零行为变化），两组各 **900 s**（停摆实测落在 4–14 分钟，400 s 窗口会漏）：

```text
CE1 STALL #1 polls=210000 ce1=341 drri1=00150015 sw1=21 post1=20 nb1=0 rri2=00040004 httidx=1262 ord=239 msdu=240
```

**`drri1` 高 16 位（固件 CE1 写指针）= 0x15 = 21，host `sw1` 也 = 21，`nb1=0`。**
两者完全同步、当前槽没有数据 ⇒ **固件停止往 CE1 写，不是 host 读不到已完成描述符。**
这一条把「host 侧 ring/可见性」整支排除掉了，`ce1kick` 也因此在 c0 组无事可做
（`CE1 kick:` 0 次；c1 组 900 s 内未触发阈值，`ce1stall=0`）。

**★ 同时纠正 chain-621 的一个判读错误**：停摆**不是**「CE1 单独静默、CE2/WMI 照常」。
精确定位到 `polls=189000`（约 810 s，`rx` 3066→3069 只 +3 后冻结）后：

```text
rx=3069  beacons=2899  mgmtev=2899  httidx=1262  ord=239  msdu=240  drri(CE2)=4   ← 一起冻结
[REAL DATA PLANE] IPv4 85 bytes over HTT TX_FRM
WCN3990-HIF: WPA: tx frame 103 bytes slot=1/2/3 prefetch=52 rc=0                  ← TX 仍在入队
```

即 **整个 target→host 方向（CE1 + CE2）都停了，只有 host→target 的 TX 还在工作**。
之前之所以觉得「CE2 正常」，是因为看的采样点还在停摆之前。

**这一轮排除的候选**（都有实机或源码依据）：

| 候选 | 依据 |
|---|---|
| HTT RX buffer 耗尽 / repost 漏槽 | `msdu=240` 与 `httidx` 增量 239 一一配对，`bad=0/0`，无泄漏 |
| poll 循环提前 return | `stage=3` 全程不变；`g_ce_quiet=0` 且全 cap 无 `quiet heartbeat` |
| `PICASSO_WAIT` 诊断导致 | 那是**每 2 分钟一次**的 UI 快照（`PICASSO_SNAPSHOT_PERIOD_S=120`，`snapshot 6 of 15`），纯读 |
| deauth / beacon miss / 被 AP 踢 | 全 cap 无相关日志，`join=5` 全程不变 |
| 卷 I/O 拖垮 | 两组卷错误 / PANIC / UFS failure 全 0 |
| CE 寄存器配置 | `CE_DST_WM=0x50` 写 `[15:0]=dst_n`、CTRL1 清 bit17/18、`CE_STATUS` 写 `0x1f`，与 `snoc.c:256-263` + Trogdor `wc_power_up/wc_service` 一致 |
| RX_IN_ORD 解析 | ath10k `htt.h:1104-1116` 精确布局（`info`@p[1]、`msdu_count`@p[6..7]、12 B 描述符@8）逐字节一致 |

#### ★ 23:4x chain-622 数据的第二次判读（修正与定锚）

更仔细地分离两组的冻结时刻后，还有三个此前没写清的事实：

1. **c0 里 CE2 先停、CE1 后停**：CE2 的 `drri` 冻结在 `polls=189000`（812 s），
   CE1 的 `ce1/drri1` 却一直动到 `polls=211000`（907 s，341→344、drri1 21→24，
   期间只有 3 条消息——TX_COMPL）。所以 c0 的图景是「**WMI 事件先停，数据路径
   苟延残喘 22 s 后也停**」，而不是我此前写的「189000 全停」。
2. **c1 里 CE1 单独停、CE2 全程活**：`drri(CE2)` 一直循环推进（5→14→24→33→2→12）
   到采集结束（`polls=214000`），只有 `ce1/httidx/ord/msdu` 在 line 14192 冻结
   （`ce1=419 drri1=20002`）。`ce1stall=0` 是因为冻结点落在采集末尾，
   剩余时间不足 15000 polls 的检查间隔。
3. **★ CE1 的停摆时刻两轮 boot 几乎相同**：c0 在 `polls=211000`、c1 在
   `~210000`（line 14192 对应约 210000）—— 以 ~4.3 ms/poll 计，两次都是
   **~15 分钟**。两次独立 boot 落在同一个 poll 计数上，说明这是一个
   **以时间/计数为锚点的可复现事件**（固件内部定时器、空闲超时或周期性
   状态机），不是流量相关的随机故障。这直接决定了恢复窗口的设计：
   检测阈值必须远小于 15 分钟，recover 才能在采集窗内真正触发。

**因此 chain-623 的 15000 polls（~105 s）检测阈值在 900 s 采集里大概率赶不上
末段停摆——已把阈值改为 800 polls（~3.4 s，commit `b8f9ff7`/`2d9697e`，镜像
CI `43612781309` 取的是 `b8f9ff7`，与 `2d9697e` 仅差一行注释文字）。**
3.4 s 安全：每个入队帧都会在 CE1 上产生 TX_COMPL，且系统持续发包。

**chain-623/624 的恢复设计（`recover=0|1`，commit `f15776d`）**：STALL 时调
`hif_join_reset("rx stalled: target->host went silent")`（**限 2 次**），
由 wifiauto 自动重 scan → 重 join → keys → VDEV_UP → DHCP，全程复用已实机
验证的状态机。重 join 同时就是**探活**：VDEV_START 必须在 CE2 上得到 0x5002
响应，join 超时即证明 WMI 任务也挂了。上游 ath10k 对这种「rx stuck 而 tx 仍
入队」的状态也是同一处理——commit `f78210dd4bea0`（"trigger hardware recovery
upon rx failures"，**Tested-on WCN3990 hw1.0 WLAN.HL.3.2.2**，与我们同固件族）
触发硬件重启而不是让设备瘫痪；其触发条件（`rx_confused`：MSDU_DONE 失败后
丢弃全部 RX）与我们的现象同族。若重 join 也无响应，下一层是 WLAN PD restart
（QMI WLFW 层，复杂度高，暂列兜底）。

#### ★ chain-624 判决（00:4x，CI `43612781309`，commit `b8f9ff7`）：判据教训 + 一次 PANIC 的完整取证

r0/r1 各 900 s。**r0（recover=0 对照）暴露了判据错误**：`ce1stall=43` ——
800 polls 单一 `ce1` 阈值大量误报，**正常空闲的 CE1 也能静默超过 3.4 s**
（数据 TX 走 CE4，每帧有 TX_COMPL，但系统安静时段本来就没有帧要发）。
r0 这轮 CE1 全程没永久停摆（`ce1/drri1` 213/214 对都在动），反而 **CE2 在
~5000 polls 就停了**（`rx=511` 冻结）——**CE2 的停止时刻是漂移的，CE1 的
~210000-polls 锚点仍是唯一稳定信号**。

**r1（recover=1）触发了一次真 PANIC，取证完整、根因链三环**：

1. 误报 STALL（polls=4000，`ce1=82`，其实只是空闲）→ recover 拆了**正常连接**；
2. 二次 join 重发 `VDEV_START`，但 **vdev 还在 UP**（没有先 `VDEV_DOWN`）→
   固件不响应 → `JOIN: peer map timeout`；
3. 诊断性回退 `sending auth anyway` 反复重传 → **CE3 mgmt 路径的 SMMU 竞态**：
   `ESR be000411`（与 chain-610 修掉的 CE4 那条完全同族）、`smr7=0x800100c0`
   （WLAN CE 流）、`cb61/cb4 fsr=0x400`，伴随 `disk5 I/O error` 刷屏 ——
   **txguard 只保护了 CE4，CE3 的 auth 重传堆叠是同一族竞态**。整机 panic，
   `join5=4`、采集在 `polls≈17000` 截断。

**三处修复（`5ef2bc3`，一次到位）**：
1. **判据改为 `ce1` 与 `msdu` 双冻结**（`g_msdu_mark`）：真停摆时两者同时死，
   普通空闲时 `msdu` 会走（不对，空闲时 msdu 也不动——但 43 次误报发生在
   msdu 仍增长的时段，即系统仍在收帧只是 CE1 tx_compl 稀疏；以 r0 数据为准）；
2. **recover 先发 `WMI_VDEV_DOWN`**（`0x5007`，TLV tag **94**——由
   CREATE@250=86 / UP@256=92 两个实机锚点插值），再 join_reset；重 START
   已 UP 的 vdev 正是 chain-624 死掉的那条链；
3. **`g_recover_active` 门控**：recover 的重 join 里 peer map timeout
   **不再 `sending auth anyway`**，改为 rearm scan；VDEV_UP 发出即清零
   （能走到 VDEV_UP 说明固件已响应 VDEV_START，回退分支重新安全）。

**顺带的判读修正**：`srri4=wr4=0`（c1）不代表「TX 路径停」——只是该轮没有
数据帧要发；「固件仍在消费 TX」的可靠判据是 `srri4==wr4 且非零`（c0 的 26/26）。

#### ★ chain-626 判决（02:4x，CI `43624210845`，commit `eff4a46`）：两阶段死亡模型确立

r0/r1 各 900 s，0 PANIC / 0 卷错误 / 0 UFS failure（625 的 998 次 `disk5 tx_enter`
是环境性的，本轮两组都是 0）。采集期内 ping 6/6，采集结束后 0/2（两组都停在
窗口末尾）。两个检测器**都精确工作**：

```
r0 STALL-GLOBAL #1 polls=96000 mgmtev=1467 ce1=182 drri1=00160016 sw1=22 rri2=002c002c
r1 STALL-GLOBAL #1 polls=127000 mgmtev=1927 ce1=228 drri1=00040004 sw1=4  rri2=00370037
```

**每 1000 polls 的完整序列（r0）给出了最终形态——RX 死亡是两阶段的**：

| 阶段 | 时刻 | 现象 |
|---|---|---|
| 正常 | polls 2000→95000 | `beacons/mgmtev/rx` 同步增长（~104 ms/信标），ord/msdu 偶发 |
| **阶段 1：信标流死** | **r0 polls=96000（~6.9 min）、r1 polls=127000（~9.1 min）** | `beacons=1468 / mgmtev=1467 / rx=1642` **三计数同时冻结且永不复原**；**时刻漂移** |
| （半死期） | 96000→211000 | **数据路径还活着**：ord/msdu 107→112（又收 5 个数据帧），ce1 还在动 |
| **阶段 2：数据路径死** | **两组都在 polls=211000（~15 min 锚点，第 4 次复现）** | ord/msdu/ce1/drri1 全冻结；**时刻固定** |

**此前几个判读在此框架下统一**：chain-622 c1 的「CE1 单独停、CE2 全程活」
就是只观察到了阶段 1；c0 的「全停」是两个阶段都走完；「15 分钟锚点」属于
**阶段 2**（固定），阶段 1 的时刻是漂移的。AP 仍在广播（阶段 2 前 beacon
stream 是我们收不到，不是 AP 停发）。

**recover 没触发的原因与接线修正（`ded03e4`）**：recover 挂在
`hakios_dhcp_datapath_stuck()`（probe 3 miss）上，而阶段 1 之后数据路径还活
着（uni 仍在涨 ⇒ probe miss 被清零），永远攒不满 3。但**信标流死（阶段 1）
时连接已经不可用**——AP 还在广播而我们收不到，且 VDEV_START 的响应走的数据
交付路径（阶段 2 前还活着）。已改为 **STALL-GLOBAL（mgmtev 冻结 ≥3 检查周期
≈3.4 s）直接触发 recover**（VDEV_DOWN + 重 join，限 2 次），probe 降级为数据
面诊断。打印上限 3→6。

**chain-627（CI `43629381141`，SECS=1200）判据**：`STALL-GLOBAL #1 → recover #1
→ connect requested → keys installed → DHCP bound → mgmtev 重新增长 →
ping-after 有回包` = 重关联自愈闭环；若 recover 后阶段 1 不再出现且 15-min
锚点被推迟/消除 = 用户体验变成「偶尔自动重连」而不是「15 分钟后彻底断网」。

**下一刀（两条一起做，一次上机即可分流）**：
1. 停摆时**发一条 WMI 命令并等响应**（如 `WMI_GET_STATS`）：
   有响应 ⇒ 固件活着、只是 RX 路径挂起；无响应 ⇒ 固件已崩。
2. 停摆时**主动 `VDEV_DOWN` → `VDEV_UP` → 重新关联**：能恢复就说明是可恢复的连接/固件状态，
   同时也给出**运行期保活**的可行手段（用户能上网，代价是周期性短暂重连）。

**在 (1)(2) 有判决前，不改 CE1/固件，也不得宣称已修好。**

### 5.5 交付边界（如实）
- **已修并闭环**：WiFi 单播双向、ARP 被对端接受、PC ping 通、三层 IPv4 流量、
  `uni>0`、0 PANIC。根因是 en0 lladdr 与空口 STA MAC 不一致，不是固件/密钥/
  TKIP 混合模式，也不是协议注册或 input 线程。
- **未通/未验证**：Safari 打开网页（DNS 三条 `dns set=1` 在 v29 verify-608
  已实机打通，但本轮**未做端到端 HTTP/HTTPS 用户实测**）；设置 > Wi-Fi 详情；
  §5.4 的运行期 RX 停摆；凭据持久化（`wpass=` 仍是 boot-arg 明文）。
- 用户实测时请注意：本轮 w1 在开机约 6 分钟内网络正常，之后 RX 停摆。
  **建议在开机后前 5 分钟试 `http://192.168.101.1`、一个域名、设置 > Wi-Fi 详情。**

### 5.6 ★ 19:58 用户实测反馈：网站仍进不去 + 全屏黑屏静止加载圈

用户报告「网站还是进不去，而且会突然弹出来加载图标，就是那种全屏黑屏
就剩一个加载圈圈的，但是加载圈圈也不转」。**这不是 WiFi 回归**，是 §5.4
并列的卷 I/O 挂死线；网络层修复后，整机被存储故障拖垮，Safari 连自己的
资源都读不出来。

**现场实时取证**（`work/620-frozen-evidence/status-before-reboot.txt`，4858 行，
`serial-cmd.py COM5 status 6`）：

```text
fs_tx_enter_internal:27568: disk5 tx_enter() failed: 5     (EIO，刷屏)
disk5: I/O error.
tx_flush_thread:1684: disk5 tx_flush(2171) failed, 5, # 166→168
disk3: device is not ready.
nx_buf_bread:628: disk3 buf_biowait() failed, error = 16   (EBUSY)
_vnode_dev_read:907: disk3 *** got err 16 reading blknum ... (num read errs: 3326→3333)
apfs_is_dir_empty:115: disk5s1 Failed to iterate directory 9856, error 5
/usr/lib/dyld: VNOP_GETXATTR: errno 93
SpringBoard / backboardd pid=0 not running
```

**逐项计数（决定性）**：`NATIVE_UFS_FAILURE` 0、`ocs=` 0、`poisoned` 0、
`SMMU` 0、`FAULT` 0、`PANIC BEGIN` 0；而 `disk5` 2584 行、`disk3` 1462 行、
`num read errs` 197 行、`VNOP_GETXATTR` 100 行。

⇒ **排除 v30 那条「CE4 gather 覆写 live DMA → SMMU CB fault →
NATIVE_UFS_FAILURE → UFS transport 挂死」链**（本现场一条首因日志都没有，
且 `txguard=1` 已实机验证采集期 0 UFS failure）。现在的形态是
**块设备层停止服务**：disk3 直接 `device is not ready / err 16`，
disk5 tx 无法开启 / EIO 5，APFS 疯狂重试，SpringBoard/backboardd 消失，
framebuffer 停止提交 ⇒ 全屏黑屏 + 静止加载圈、触摸无响应。

**与历史 14:2x 那次对照**：那次是连续运行约 36 分钟后出现，本次在
400 s 采集结束后出现；两次都 0 PANIC、0 NATIVE_UFS_FAILURE，UI 进程消失，
恢复方式都是重启。历史两个候选（seed-613 20 GiB 重建残留 / 真实 TX 触发
SMMU）中，**后者本轮已被现场计数排除**，前者仍无直接证据。**不下结论。**

**已采取的动作**（`work/chain-620-long.sh`；**20:0x 用户取消，未重试**）：
1. `reboot-fastboot` + 重新 boot 修复后的默认镜像 ⇒ **恢复设备**
   （v32 §3 / 14:2x 两次证实；**不清 marker、不刷 userdata**，seed-613 不变）。
2. **20 分钟连续串口采集**（`console-exp-long.cap`），让「转变瞬间」落在日志内
   —— 历史一直缺这一步（400 s 采集期内所有错误恒 0）。
3. **PC 每 30 s 带时间戳 ping**（`work/620-frozen-evidence/ping-timeline.txt`），
   把「网络何时断」与「首个存储错误行号」精确对齐，判定两者是否同源。
4. 采集是被动读取，**用户可在采集期间操作手机**；结束后设备留在桌面、不重启。

**实际结果（如实更正）**：设备在 `20:04` 被重启恢复正常，长采集于
`20:05:13` 被用户取消后台任务时一并终止 —— `console-exp-long.cap` 只有
62 KB / 约 90 s 的启动早期片段（`DATASTORE ready=1`，`DHCP bound`/心跳/
`wifix` 全 0），`ping-timeline.txt` 停在 `t=90s`，**没有抓到转变瞬间、
没有判决价值**；按用户要求未重启该任务、未重试。取消后的实时串口复查
（2301 行）显示 `disk5 tx_enter` / `disk3 not ready` / `got err 16` /
`SpringBoard pid=0` / `PANIC` **全 0**，即 **UI 是健康的**，但 WiFi 数据面
依旧停在 §5.4 的老位置 ⇒ 这次「卷 I/O 挂死」与「数据面停摆」**没有同时出现**。
此后 chain-619 两组也各抓了卷健康快照，均为 0 错误（见 §5.4）。

**下一棒（长采集证据仍缺，不猜）**：
- 若首个 `disk3: device is not ready` 与 ping 首次 FAIL 同一时刻 ⇒ 同源，
  查 UFS 控制器低功耗/clock gating 唤醒与 APFS `tx_flush` 的重试策略。
- 若存储错误先于网络断、且伴随 `httidx/ord/msdu` 冻结 ⇒ 查 WiFi CE 与 UFS
  共享资源（电源域 / SMMU stream）在运行期被置闲。
- 若两者无关 ⇒ 分成两条独立线分别修。
- 参考实现：`src/silenzio111_UniXNU-picasso/drivers/NativeUfs/RootCommands.h`
  的 `gHost.poisoned` 是「一次 stall 永久隔离、无 cancel/reset/retry」，
  但本现场 `poisoned`/`ocs=` 全 0，暂不支持走这条路。

---

## 6. 铁律增补（本轮）

1. **`ifnet_input()` 返回 0 只代表入队**；Apple 官方文档明确"处理在另一线程"、
   "不匹配已注册协议的包会被丢弃"。禁止再用 `rc=0` 推断"协议栈已处理"。
2. **驱动层计数 ≠ 协议栈计数**：`uni/grp` 是 `hif_wr_msdu` 在交付前统计的，
   只能说明固件交了什么；协议栈行为必须看 `ifnet_stat()` 或外部 ping。
3. **一个接口只能有一个 MAC**：802.11 SA / 固件 base MAC / DHCP chaddr /
   `IF_LLADDR` / ARP sender-hw 必须逐字节一致，否则 `ether_demux` 的
   `IF_LLADDR` 比较与 AP 桥接表会各自把帧判成"不属于本机"。
4. **旁路成功不代表协议栈通**：内核 DHCP、resolv.conf、dynamic-store DNS
   都不经过 `dlil_input`；它们全绿时入向仍可能完全死。
5. `ifnet_attach()` **不** plumb 协议；`SIOCAIFADDR` 里的
   `(void) in_domifattach(ifp)` **丢弃错误**。要让协议注册可观测，用官方
   `SIOCPROTOATTACH` 并打印 errno，或看 `ifnet_stat().no_protocol`。
   **本设备 w0 实测 `noproto=0` ⇒ plumb 正常，不要再查这条。**
6. **TX 日志不打印 ARP opcode，RX 侧 `arp op=` 有 64 行上限**：判断"有没有回
   ARP"必须用 TX 帧的**目的 MAC**（单播即 reply）。v34 用 RX 计数得出
   「协议栈不回 ARP」是假判据，害得方向错了三天。
7. **Windows `ping` 的退出码不可当判据**：本机生成的「无法访问目标主机」
   也算"已接收"且 rc=0。必须解析回包是否来自目标 IP，并配合 `arp -a`。
8. **外部 ping 必须在串口采集窗口内发**，否则抓不到 PC 的 ARP request 与
   手机应答的对应关系；采集结束后的 ping 只有 PC 侧结果，没有设备侧证据。
9. **A/B 链里 VALID 检查不得用 `|| exit N`**：无效环境轮必须重试，不能终止
   整条链（`chain-616` 的旧 bug 让 w1 一次都没跑）。
10. **「采集期通」不是「持续可用」**：chain-617/618/619 采集第 300 s 的
   ping 都是 6/6，但采集结束后一律 0/20。运行期停摆类故障的**主判据必须
   放在采集窗口之后**，并且同轮抓卷健康快照排除 disk5/disk3 冒充。
11. **优先用现成心跳计数分层，别急着改代码**：`ring=1/1`、`ind=0`、
    `drri` 是否推进，一次就把「attention-drain 重复消费」「RX_IND 路径」
    「CE2 是否还活」全排除了。给任何有打印上限的计数（如 `g_htt_rx<64`、
    `mgmtev<64`）配一个**无上限**孪生计数，否则后期静默无法判读。
12. **已证伪的 boot-arg 必须从后续 A/B 的 cmdline 里彻底删掉并断言**：
    `pspoke` 在 chain-615 与 chain-619 两次证伪，且 chain-619 的 ps1 让
    CE2 也停（有害）。`chain-621` 显式 `assert 'pspoke' not in s`。

其余沿用 v34 §7 / v33 §6：一次一个 CI、`-f fast=true`、采集 ≥400 s、
995 B 等长单变量、判据是 checkpoint 明文 + 外部 ping、用户 6 处改动不碰、
`abi-check.py` 0 HARD、commit message 不写未实机验证的结论。

---

## 7. 关键路径

| 用途 | 路径 |
|---|---|
| 唯一权威 | 本文 v35 |
| 本轮修复 | `b074059`（3 files，wifix A/B）+ `b1af80f`（默认化） |
| 可复现脚本 | `work/fix-wifi-input-617.py`（含默认化，`ALL_IDENTICAL=True`） |
| 上机链 | `work/chain-617.sh <run-id>`（默认 `ARMS="w0 w2"`，可用 `ARMS="w0 w1 w2"`） |
| 链日志 | `work/chain-617.out`、`work/boot-617-*.out`、`work/ping-617-*.out` |
| cap | `out/full-flash/logs/console-exp-w{0,1,2}.cap` |
| iOS16 官方头 | `work/refs/xnu8792/kpi_interface.h`（xnu-8792.61.2） |
| Apple OSS XNU | `work/refs/apple-oss/xnu`（xnu-12377.1.9，git show 读源码） |
| 源码证据 | `dlil.c:2051/4815`、`dlil_input.c:852`、`ether_if_module.c:448/742`、`ether_inet_pr_module.c:558`、`kpi_protocol.c:399`、`netinet/in.c:285/1202/1468`、`kpi_mbuf.c:124/829`、`in_arp.c:1690+` |
| 参考实现 | `upstream/sheng/package/WifiNet.cpp:485/533`、`pong-wifi/PongWifiNet.cpp:111/707`、`Trogdor-ipadOS/.../CoachzWifiNet.cpp`、`unixnu-wcn7860/.../PeachNet.c` |
| CI | `43587191778`（HEAD `b074059`，`-f fast=true`，复用 seed-613） |
