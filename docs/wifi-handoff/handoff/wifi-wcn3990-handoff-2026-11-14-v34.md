# WiFi (WCN3990) 阶段文档 — 2026-11-14 v34

**时间**：2026-11-14 17:0x
**HEAD**：`0b00c94`（fork `kd439-pool`）
**设备**：运行 `boot-exp-ps1.img`，已关联（`join=5`）、DHCP `192.168.101.18`、`uni=0 grp=116`
**唯一权威状态源**：本文件。v33 及更早为历史归档。

---

## 0. 一句话状态

```text
固件/HIF/扫描/关联/WPA2 握手/装密钥/PEER_AUTHORIZE/DHCP/DNS/键盘/发送 ✅
  ── 广播/组播接收 ✅（无上限计数 grp=90..116）
  ── ★ 单播双向不通 ❌（uni=0；PC ping 手机 6/6 超时）
  ── ★★ 协议栈不回 ARP ❌ ← 已实测排除 if_input（rc=0），问题在栈内 arp_input（见 §2.0b）
  ── 运行期卷 I/O 挂死 → UI 冻结（另一条线，见 §5）
```

---

## 1. 本轮（v33→v34）已确证的事实

### 1.1 PTK 推导**完全正确**（离线独立复算）
`console-exp-y0.cap`（唯一 `wpadiag=1`）含 M1/M2 全文与 `pmk=/kck=` 指纹。
M2 是整帧 dump（802.11 头 24B + LLC/SNAP 8B ⇒ EAPOL 起于偏移 32），
`SNonce = frame[49:81]`。用 Python `hashlib/hmac` 独立复算：

| | 独立计算 | 设备日志 | |
|---|---|---|---|
| PMK[0:4] | `a28efd65` | `a28efd65` | ✅ |
| KCK[0:4] | `4a0a0614` | `4a0a0614` | ✅ |
| 完整 PTK | `4a0a06147c9f2d68e0bc747181fc548452e9b2efaca5252963e971677deedd1a1bdf8ea4d428db5c62a66d62261bc29e` | — | — |
| TK | `1bdf8ea4d428db5c62a66d62261bc29e` | 同一 PRF | — |

⇒ PBKDF2 / PRF-48 / nonce 排序 / MAC 排序全对，**TK 也对**。密钥不是根因。

### 1.2 主机侧与两条参考逐项核对，全部一致
| 环节 | 结论 |
|---|---|
| key 命令布局 / `WMI_KEY_*` / cipher(2=TKIP,4=CCMP) | = ath10k `wmi.h:5066-5083`、`mac.c:271-276` |
| `0x5003` | `VDEV_INSTALL_KEY_COMPLETE`（`wmi.h:1285`），末字是 key index，不是失败码 |
| WMI INIT | = Trogdor `cfg[46]`（仅 `num_wds_entries` 0 vs 2，与单播无关） |
| join 序列 | 一致（含 chain-615 补回的 `peer_param(0xff,1)` PS poke） |
| TX 帧构造 | = `WlanRadio.c:3999 wj_tx_put` 逐行（含 `flags1` 的 TID） |
| RX 路径 | 同源，ring 2048/1023/2048 一致 |

### 1.3 三个假设已实机证伪（不要重试）
`rekey`（chain-613）、`keyfix`/TKIP MIC（chain-614）、`pspoke`（chain-615）。
判据均为**无上限** `uni=/grp=` 计数，且两组都是有效轮。

### 1.4 ★ 决定性新发现：协议栈不回 ARP
`console-exp-ps1.cap` 同时序：
```
5365  DHCP: bound inet 192.168.101.18 ... addr=0 rt=0   ← 地址已装（ifioctl 返回 0）
6408  RX #37  arp op=1 192.168.101.1 > 192.168.101.18   ← 网关问"谁是 .18"
7051  RX #59  arp op=1 192.168.101.1 > 192.168.101.18
```
整轮 `[DATA PLANE] ARP frame 42 bytes` 18 条**全是 request，无 reply**。

PC 侧独立验证（同网段，PC=192.168.101.4）：
`ping 192.168.101.18` → 6/6 超时、"无法访问目标主机"、ARP 表始终无 `.18`；
同一 30 s 窗口手机串口 `grp` 在涨、ARP 发送 0 条。

⇒ **广播能收、我们不回**。别人也无法主动找到我们。这是纯主机侧问题。

### 1.5 ★★ 已定位到具体一行：`if_input` 从未赋值
`hakios.net.c:434` `ifnet_allocate_extended()` 用 `IFNET_INIT_LEGACY`，
init 参数里有 `.demux = ether_demux`、`.output = net_output`，
但**全文件没有任何一处设置 `ifp->if_input`**，也没调用 `ether_ifattach()`。

XNU 的 `ifnet_input()` 就是把帧交给 `ifp->if_input`；为 NULL 时帧被丢弃
（返回错误而我们没检查返回值）。这精确解释了：帧进了驱动（RX trace 能看到）、
交给了 `ifnet_input`、协议栈毫无反应（不回 ARP、不回 ICMP、不处理 TCP）。

mbuf 构造本身是对的：`mbuf_pkthdr_setheader(packet, header)` 把以太头放进
`m_pkthdr.header`，data 指向载荷——这正是 `ether_input` 期望的形态。

---

## 2. 本轮实际改动与结果（chain-616）

### 2.0 结论先说：`if_input` 无法从本树赋值，本轮只交付"判据"
三次构建把这条路的边界钉清楚了：

| CI | 结果 | 原因 |
|---|---|---|
| `43579193615` | FAIL | `net/if_ethersubr.h` 不存在（该 SDK 不导出） |
| `43580429286` | FAIL | `incomplete definition of type 'struct __ifnet'` |
| `43581180860` | 见 §6 | 只保留 `rc` 探针的版本 |

关键事实（来自 31 MB `kernel-bootstrap.log`）：
- `INSTALLHDR if_var.h` 有；`if_ethersubr.h` **完全没有**；
- 即使 include `if_var.h`，`struct ifnet` 在本树里**是不透明类型**
  （`struct __ifnet` 只有前向声明）。

⇒ **`ifp->if_input` 在驱动层不可读写**，`ether_input()` 也没有导出声明。
"在驱动里赋值"这条路在当前 KPI 面上走不通——这是试到底后的结论。

### 2.0b ★ 实测判决（`console-exp-rc.cap`，400 s，`58cd878`）：`if_input` 假设**证伪**
```
net: in #1 len=42  rc=0 netin=0     ← ARP
net: in #2 len=90  rc=0 netin=0
net: in #3 len=342 rc=0 netin=0     ← DHCP
net: in #4 len=42  rc=0 netin=0
... 共 8 条采样（前 8 帧），**全部 rc=0**
```
`ifnet_input()` 返回 0 ⇒ **帧确实被投递进协议栈**，`ifp->if_input` 不是 NULL。
本轮同时确认：`PANIC 0`、`join=5`×32、DHCP bound、`uni=0 grp=44`、
`ARP frame 42 bytes`×24（全是 request，仍无 reply）。

⇒ **问题不在驱动投递，而在协议栈内部**：`ether_input` → 解复用 → `arp_input`
→ `in_arpinput` 这一段。下一棒应从这里查，不要再怀疑 `if_input`。

三个最可能的具体原因（按可查性排序）：
1. `in_arpinput()` 找不到挂在 **en0** 上的 `in_ifaddr`
   （`SIOCAIFADDR` 返回 0 只说明 ioctl 成功，不保证地址挂在预期 ifp）；
2. ARP 的 demux 处理器未注册（我们只给了 `.add_proto = ether_add_proto`，
   没有走 `ether_ifattach()` / `ifnet_attach_proto` 的完整注册）；
3. mbuf 形态：`ether_input` 可能要求 data 仍包含以太头（我们按
   `m_pkthdr.header` + data 跳过头的形态给），需对照该版本 `ether_input` 原文。

### 2.1 交付内容（`58cd878`，可编译）
`hakios_net_input_unit()` 保留 `ifnet_input()` 的 `errno_t` 并打印：
```c
errno_t rc = ifnet_input(n->ifp, packet, &stats);
/* 前 8 帧 + 每 64 帧 */
printf("net: in #%llu len=%llu rc=%d netin=%d\n", ...);
```
**这一条就能定论**：若 `ifp->if_input` 为 NULL，`ifnet_input()` 会失败、
`rc != 0`；若 `rc == 0`，说明帧确实进了协议栈，ARP 不回的原因在别处。

`netin` 仍解析、默认 0（无行为变化），槽位留给下一棒复用。

### 2.2 下一棒若要真正赋值 `if_input`，只有三条路（都需先验证）
1. **改内核侧**：在 `unixnu-source-kernel.patch` 里让 `ifnet_allocate_extended`
   或 `ifnet_attach` 对 `IFNET_FAMILY_ETHERNET` 默认挂 `ether_input`，
   而不是在驱动里碰不透明结构；
2. 查该 XNU 版本是否有 `ifnet_set_input()` 一类 KPI（本轮未在导出头里找到，
   需在 `bsd/net/kpi_interface.h` 原文确认）；
3. 改用 `ether_ifattach()` 走完整 attach 流程（会与现有
   `ifnet_attach`/`bpfattach` 重复，需要重排整段）。

---

### 2.1 chain-616 构建：第一次失败，已修（SDK 头文件差异）
CI `43579193615` FAIL 在 "Build kernel + payload"：
```
device/wifi-wcn3990/source/hakios.net.c:25:10: fatal error:
    'net/if_ethersubr.h' file not found
```
从该 run 的 31 MB `kernel-bootstrap.log` 确认：日志里有多次
`INSTALLHDR if_var.h`，**完全没有 `if_ethersubr.h`** ⇒ 这个 XNU 构建
导出 `if_var.h`、不导出 `if_ethersubr.h`。

修法（`7de2b40`）：保留 `<net/if_var.h>`（`struct ifnet` / `if_input`），
自己声明那一个符号：
```c
extern void ether_input(struct ifnet *ifp, struct mbuf *m);
```
`ether_input()` 是 `bsd/net/if_ethersubr.c` 里的非 static 函数，本驱动编进
内核，符号直接链接，不需要导出表。

**教训**：本项目此前从未 include 过 `if_var.h` / `if_ethersubr.h`，
这类"内核私有头是否导出"只能靠 CI 试；下次动内核私有结构前，先 grep
`kernel-bootstrap.log` 里的 `INSTALLHDR` 确认。

## 3. 判据（这轮必须一次说清）

| 判据 | 含义 |
|---|---|
| `net: en0 if_input=0x...` 且非 0 | 赋值生效 |
| `net: in #n rc=0` | 帧真的进了协议栈 |
| PC `ping 192.168.101.18` 有回 | **ARP 修好**（不依赖 AP、不依赖路由器） |
| 手机串口出现 `[DATA PLANE] ARP frame 42 bytes` 且 PC ARP 表出现 `.18` | 单播 TX 通 |
| 之后 `uni>0` | 单播 RX 也通（若仍 0，则 RX 是独立问题，见 §4） |

`ping` 通不通由 PC 侧判定，是本轮唯一不依赖日志的硬判据。

---

## 4. 如果 ARP 修好后仍 `uni=0`

那就把问题干净地分成两半：
- **TX 单播**：PC 能收到手机 ARP reply ⇒ TX 通；否则 TX 坏。
- **RX 单播**：ARP reply 发出去了（PC 收到）但 `uni` 仍 0 ⇒ 纯 RX 问题。

此时才有必要回到"固件/BDF 差异"：
Trogdor 从 CoachZ 磁盘加载 MPSS（`WlanRadio.c:707/:725` board-2.bin），
我们走 picasso WLFW/QMI（`SERVICE_READY fw 840630314`）。
**这是唯一剩下的环境差异，且"移植驱动"改不了它。**

### 关于"整体移植 Trogdor"（结论保留）
HAL 自包含（`wc_alloc:2283`、`wh_send:2616` 同文件），整文件移植工程可行；
但平台胶水（SMMU/CE/电源/WLFW）是我们写的且已被实机证明能跑，
Trogdor 那套是 SC7180 的。整替 = 用未验证换已验证，且不改固件差异。
**不作为下一步**；若要做，开独立分支并行，不动现有基线。

---

## 5. 并行的第二条线：运行期卷 I/O 挂死 → UI 冻结

用户澄清：**没有重启**，是他手动退的 fb；之前一直卡在转圈且圈圈静止。
- 现场：`SpringBoard/backboardd pid=0 not running`；
  `disk5(OwnerData)/disk3(System)` APFS 读大面积失败
  （`tx_enter failed`×343、`device is not ready`×279、`got err 16`×72、
  `tx_flush #817`、`dyld VNOP_GETXATTR errno 93`）；
  `CPUFREQ busy=3/2/2`（CPU 空闲，排除 11-11 忙等饿死）；`PANIC=0`。
- 400 s 采集期内这些错误全为 0，故障发生在采集结束后。
- 触发点未取证，**不下结论**。需 >20 min 连续采集抓转变瞬间。
- rk1 首轮冷启动也出现过同形 UFS 故障（`UFSFAIL×5`、`disk5 I/O×897`、
  `smp2p=0`），再启动一次即恢复 ⇒ 间歇性。

---

## 6. 代码与产物

| 项 | 值 |
|---|---|
| HEAD | `58cd878`（ifnet_input rc 探针；36ade2a/7de2b40 的 if_input 尝试已撤） |
| seed | **seed-613**，设备 Data 一致，本轮未刷 userdata / 未清 marker |
| CI | `43574756332` SUCCESS（0b00c94）；`43581180860` 见 §2.0 |
| 上机链 | `work/chain-615.sh`（模板）、`work/chain-616.sh`（netin，判据含 rc） |
| 脚本 | `work/fix-peer-ps-poke.py`、`work/fix-rx-verdict-and-tkip-mic.py`、`work/fix-if-input.py`、`work/test-ccmp-ap.sh` |
| 关键 cap | `console-exp-ps1.cap`（ARP 证据）、`console-exp-y0.cap`（wpadiag/PTK） |
| 磁盘 | C: ~22 G |

## 7. 铁律增补（本轮）

1. **驱动日志"帧进了"≠"协议栈处理了"**：必须看 `ifnet_input` 返回值 +
   外部可观测行为（PC ping）。
2. **AP ACK 不代表解密成功**：802.11 ACK 在 CRC 后、CCMP 解密前。
3. **密钥正确性可以离线证伪**：wpadiag 的 M1/M2 足够复算 PMK/PTK，
   不要靠"握手过了所以密钥对"来推断。
4. 其余沿用 v33 §6。


---

## 8. 交接摘要（本轮收尾）

### 8.1 这轮做成了什么
1. **PTK 完全验证正确**（离线独立复算 PMK/KCK/TK 全部匹配）——
   彻底排除"密钥算错"，方法可复用（`y0.cap` + Python 复算）。
2. **主机侧与两条参考逐项核对全部一致**（key 命令、cipher、INIT、join、
   TX 帧、RX 路径）——排除"驱动算法不如参考"。
3. **发现并定位一个此前没注意的故障**：协议栈不回 ARP。
   内部时序（网关两次问 `.18`）+ 外部验证（PC ping 6/6 超时、ARP 表为空）。
4. **把 `if_input` 这条路探到底**：本树 `struct ifnet` 不透明、
   `if_ethersubr.h` 不导出 ⇒ 驱动层不可赋值，省掉下一棒重复尝试。
5. 交付 `net: in #n len=.. rc=.. netin=..` 探针：一次启动即可定论。

### 8.2 三个已实机证伪的假设（不要再试）
`rekey`（chain-613）、`keyfix`/TKIP MIC（chain-614）、`pspoke`（chain-615）。

### 8.3 下一棒第一步（已实测，方向明确）
**`rc=0` 已确认帧进栈**（§2.0b），所以不要再查 `if_input`。下一步按 §2.0b 的三条查：
1. `in_ifaddr` 是否真的挂在 en0（`SIOCAIFADDR` 返回 0 不等于挂对了接口）；
2. ARP 的 demux 处理器是否注册（对比 `ether_ifattach()`/`ifnet_attach_proto` 的完整流程）；
3. 该版本 `ether_input` 对 mbuf 的期望形态（data 是否应含以太头）。
2. ARP 通了以后，**PC ping 手机**就是单播 TX 的独立判据（不依赖 AP）。
3. 之后才回到 `uni`：若 ARP 通而 `uni` 仍 0，问题干净地分成 TX/RX 两半。

### 8.4 用户可操作判据（唯一不依赖日志的）
在 PC 上（同一网段，PC=192.168.101.4）：
```
ping -n 6 192.168.101.18
arp -a | findstr 192.168.101.18
```
有回包 + 有 ARP 条目 = 协议栈入向通了。

### 8.5 不要做的事
- 不要要求用户改路由器加密（已否决）；
- 不要再试 rekey / keyfix / pspoke；
- 不要在驱动里访问 `struct ifnet` 内部（本树不透明）；
- 不要清 marker、不要刷 userdata（seed-613 与设备一致）；
- 不要整体 rebase（会丢已验证的数据面）。

### 8.6 本轮新增的实测证据
- `console-exp-rc.cap`（400 s）：`net: in #1..#8` 全部 `rc=0`、`PANIC 0`、
  `join=5`×32、DHCP bound、`uni=0 grp=44`、`ARP frame 42 bytes`×24（无 reply）。

### 8.7 设备与仓库状态
- 设备：运行 `boot-exp-ps1.img`，已关联、DHCP `192.168.101.18`、`uni=0`；
  未刷分区、未清 marker、用户 6 处未提交改动原样保留。
- 仓库：`fork/kd439-pool` @ `58cd878`，已推送。
- 磁盘：C: ~20 G。
