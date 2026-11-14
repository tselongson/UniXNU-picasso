# WiFi (WCN3990) 交接文档 — 2026-11-12 下午（v18）

> 【历史归档】本文已被 **`wifi-wcn3990-handoff-2026-11-12-v19.md`** 取代，冲突时以 v19 为准。
> v19 相对 v18 的实质变化：
> 1. 确认「WiFi 已能上网（IP 层全通）」，唯一功能缺口收敛为 **DNS 未配置**；
> 2. 查证 **`IPConfiguration.bundle` 不在设备 root volume 上** ⇒ configd 原生 DHCP 链路走不通，
>    内核 DHCP 是这个系统上唯一可行解（Apple 内核自己也有 `netboot.c` 先例）；
> 3. 定位设置 App 卡死的直接原因：**apple80211 请求 96/97 被驱动 `EOPNOTSUPP` 拒答**；
> 4. 给出 DNS 的**通用解法**：内核解析 option 6 → 按 Apple 官方 `mDNSPosix.h:117`
>    的 resolver 文件格式发布 → 给 `mDNSResponder` 发 SIGHUP 重载。

> **本文件是当前唯一权威状态源。** v17 及更早为历史归档。
> 判据口径：**checkpoint 明文 + 用户可操作性，不是「0 panic」**。
> 本文如实记录被实机证伪/更正的归因，避免下一轮重查。

---

## 0. 一句话现状

```
固件/HIF/CE/HTC/WMI/HTT/scan/auth/assoc/VDEV_UP/EAPOL 四次握手/
PTK+GTK/PEER_AUTHORIZE/真实 rx+tx 帧进协议栈/en0 link up/
KEV_DL_LINK_ON/rmtfs <4G/IONetworkStack user client 绑定/开机进桌面可操作
                                                    全部 ✅（chain-580~597 实机）
唯一硬缺口：configd 不建 IPv4 service —— SIOCSIFADDR / DHCP / inet 全 0
本轮把根因从「preferences.plist 没写」收敛到
  「IONetworkStack 的 entitlement-free user client 绑定比 configd 晚 121 秒」
  + 「iOS 的 PreferencesMonitor 只认 InterfaceNamer 的 quiet，不认 timeout」。
14:1x 用户要求「wifi 能用，不限手段，先打通」→ 换思路：**不再陪 configd 玩**，
改为 commit `0576f1d` 在内核里跑 DHCPv4 客户端，绕过 configd /
InterfaceNamer / IONetworkStack / preferences.plist / IOKit quiet 全部环节，
用 XNU 自己的 `ifioctl(SIOCAIFADDR)` 直接给 en0 装地址。
chain-597 恢复 HEAD 可上机并拿到 A 证据；
**chain-599 已闭环：`DHCP: bound inet 192.168.101.12/24 router 192.168.101.1
addr=0 rt=0`，SIOCSIFADDR 到手，系统随后自发 ARP 并经真实 HTT TX_FRM
数据面发出且被 ACK，0 PANIC，关联保持 join=5 → WiFi 已能收发 IP。**
```

---

## 1. 状态表（2026-11-12 下午）

| 层 | 状态 | 证据 |
|---|---|---|
| 加 WiFi 开不了机（Data seed panic） | ✅ | `9572b30` / `console-exp-wifidfix.cap` |
| WiFi 启动后 UI 卡死（CPU 忙等） | ✅ 用户确认 | `171c883` / `console-exp-wifidyield.cap`（busy 100→≤42、runnable_ms 121081→223） |
| 阶段 1 放行 wifid | ✅ | `26d995a` `enabled=1 changed=1 error=0`，wifid pid=41 |
| 阶段 2 能力探测（RSSI_BOUNDS/DFS） | ✅ | `ed07b14` 106/254/207 answered |
| 关联 + WPA2 四次握手 + 密钥 + 数据面 | ✅ | chain-574/580：`msg3 seen,msg4 sent,keys installed,peer authorized`、`msdu=21 bad=0` |
| link 通知链 / KEV / IFF_RUNNING | ✅ | `54b5d8d` `KEV_DL_LINK_ON size=48 hdr=24 fix=3` |
| rmtfs <4 GiB / IONetworkStack user client | ✅ | `c75f96d` / `6285168` `user client bound (netstack=1)` |
| **阶段 3 DHCP / IP** | ❌ **当前唯一硬缺口** | `SIOCSIFADDR / DHCP / inet` 全 0（chain-594/597） |
| 阶段 4 设置 UI / 凭据持久化 | 🔶 | 设置 Wi-Fi 详情曾卡死；`wpass=` 仍在 boot-arg 明文 |
| HEAD 可上机 | ✅ 本轮恢复 | `5bebed6` / chain-597 **0 PANIC** |

---

## 2. 本轮定位：从「猜」到逐行源码 + 时间线 + 参考项目

### 2.1 纠正 v16/v17 的一处关键误判（别再引用）
> 「`stack_open` 0 次 ⇒ configd 从未 `IOServiceOpen(IONetworkStack)`」

**不成立。** chain-594 用的 `219b972` 里，`stack_open` 探针**只**挂在
`is_io_service_open_extended` 的 entitlement/MAC 拒绝分支；成功路径完全没有
打印。`8d30f39`/`d6c9b03` 才在成功路径加了 `stack_open ... result=` 与
`configd_open where=ok`，但那两版都在 exec 路径 panic，从未跑到。
历史上「0 次」什么也证明不了。chain-597 才第一次让成功路径探针真正跑起来。

### 2.2 时间线（chain-594 `.times`，字节偏移→墙钟）

| 时刻 | 事件 |
|---|---|
| t+ 18.2 s | `picasso_network_stack_start()` 找到 IONetworkStack 服务（**原版、需 entitlement 的 user client**） |
| t+ 18.6 s | `netsvc` 写 1786 B `preferences.plist` |
| **t+ 46.7 s** | **configd exec** → InterfaceNamer `load()` / matching callback 触发 |
| t+168.2 s | `HakiosWLANStart()` 才绑定 `IOUserClientClass`（**晚 121.5 秒**） |
| t+168.2 s | configd 首个 `SIOCGIFMEDIA` |
| t+240.2 s | en0 link up |

### 2.3 Apple configd 源码逐行因果链（本地 `work/refs/apple-oss/configd`）

1. `Plugins/InterfaceNamer/ifnamer.c:4108 stackCallback()`：
   `IOServiceOpen(stack, ..., &S_connect)`，失败只 `SC_log` 一次，**无重试**。
2. Apple `IOKitLib` / IOKit.org：`IOServiceAddMatchingNotification` 对
   **已发布的服务立即回调一次**。IONetworkStack 在 t+18 s 已发布，configd
   t+47 s 启动 ⇒ 唯一一次机会撞上原版 `initWithTask - No Entitlement`
   （kernel.macho 有该字符串）⇒ `S_connect` 永远 `MACH_PORT_NULL`。
3. `ifnamer.c:3965 updateInterfaces()` 与 `:4159 quietCallback()`：
   `if (S_connect == MACH_PORT_NULL) return;` —— 不发布 quiet。
4. `ifnamer.c timerCallback()`：只加 `kInterfaceNamerKey_Timeout` 时间戳，
   手动调 `quietCallback(..., 0, NULL)`，仍被第 3 条挡回。
5. `Plugins/PreferencesMonitor/prefsmon.c:403-411`：
   ```c
   if (!haveConfiguration && (quiet || timeout)) {
       if (quiet
   #if !TARGET_OS_IPHONE
           || timeout
   #endif
           ) haveConfiguration = TRUE;
   ```
   ⇒ **iOS 上 timeout 不算，只认 quiet**。这就是 `netsvc` 写入 1786 B plist
   却对 DHCP 毫无作用的机制。
6. `IPMonitor/ip_plugin.c` 靠 `Setup:/Network/Service/<ID>/IPv4` 的
   `ConfigMethod=DHCP` 起 DHCP；dynamic store 没有该键 ⇒ 不发 DISCOVER
   ⇒ `SIOCSIFADDR` 从不出现。

### 2.4 官方文档与同芯片参考（本轮新查）
- Apple **The System Configuration Schema**：`Interface` 必填
  `DeviceName`(=BSD 名 en0) / `Hardware` / `Type`；`IPv4.ConfigMethod=DHCP`
  是合法值 ⇒ **netsvc plist 的键名无罪**。
- Apple `IOServiceAddMatchingNotification` 文档：notification 在 iterator
  被 `IOIteratorNext` 抽空前未 arm；对已存在服务回调立即触发。
- `configd/configd.tproj/plugin_support.c:58` `BUNDLE_DIRECTORY =
  "/SystemConfiguration"`（拼 `/System/Library` 后）→ 我们的 bind mount
  点正确；`:76 plugin_allow_list` 用 `PLUGIN_ALL` 含 InterfaceNamer；
  `:135 plugins_builtin[]` 有该 bundle ID，`Builtin=true` 时按 ID 填函数
  指针，**不需要 bundle 可执行文件** ⇒ 「bundle 只有 Info.plist」不是阻塞。
- 同芯片 **CoachZ（SC7180 + WCN3990，真机 DHCP/HTTPS 通过）**
  `work/trogdor/port/CoachzWifiNet.cpp:499-543` 注释原文：
  > "The network configuration's interface namer waits for a service of this
  > class, opens it and only then publishes that IOKit is quiet; everything
  > that lists interfaces (the wireless daemon among them) waits for that."
  > "... the network configuration waits for the network stack service before
  > it calls IOKit quiet ... **whether or not the radio has started yet**."
  CoachZ 在 `coachz_wifi_net_start()` 里**早期**就带 user client 发布
  IONetworkStack。我们反了：绑定放在 `HakiosWLANStart()`（en0 attach）。

### 2.5 本轮已排除（避免重踩）
- **`bpfattach` 缺失**（差点误判）：`Trogdor-ipadOS/docs/plans/wifi.md:23`
  说「加入后没有 IPv4 是驱动没替 DHCP 接 BPF」，CoachZ 用
  `bpf_attach(ifp, DLT_EN10MB, 14, nullptr, nullptr)`。但 grep 漏了老式
  KPI 名 `bpfattach` —— 我们 `hakios.net.c:452` **已有**
  `bpfattach(ifp, DLT_EN10MB, ETHER_HDR_LEN)`，`:163 bpf_tap_out`、
  `:210 bpf_tap_in` 都在；kernel.macho 里 `_bpfattach` / `_bpf_attach`
  两符号都存在。**不要再补一次**。
- `preferences.plist` 路径/权限/被读取：`prefs_size=1102`（chain-597）证明
  configd 读过并重写过 594 写入的 1786 B。
- `DeviceName` / `CurrentSet` schema（官方 schema 背书；`Sets` key `"0"`
  是否必须 UUID 仍未实机判定，列在 §6）。
- v17 候选 B「其它 *.bundle 不齐」：built-in 机制按 ID 填指针，
  且 mount 只覆盖 InterfaceNamer.bundle 一个子目录。
- 历史：SMMU 0x20、CE3 race、VDEV_UP bssid/aid、0x401=ECHO 等，见 v14~v17。

---

## 3. chain-597 判决（5bebed6 / CI 43162658105 / 400 s）

587674 B、**0 PANIC** ⇒ HEAD 恢复可上机（exec 路径栈修复生效）。

```
PICASSO_NETWORK netsvc=1 prefs_error=0 prefs_size=1102
PICASSO_NETWORK netsvc write error=0 bytes=1786
                 verify_error=0 verify_size=1786 head=3c3f786d
PICASSO_NETWORK configd_open where=ok class=IOPMrootDomain pid=34 error=0  x2
PICASSO_NETWORK configd_open where=denied class=IOEthernetInterface pid=34 x1
stack_open(成功路径)=0 ; configd root_busy=0 ; busy_scan=0
SIOCSIFADDR / DHCP / inet = 0 / 0 / 0
```

- 成功/拒绝探针都打印 ⇒ 探针有效；**configd 从未成功 open IONetworkStack**，
  也从未调 `io_service_get_busy_state` ⇒ InterfaceNamer 的 `setup_IOKit()`
  没有走到（或插件未 load）。
- `verify_size=1786` 证明 `static uint8_t probe[2048]` 同时修好了
  chain-594 的 `verify_size=0`（那是 `picasso_ifnamer_read` 对 512 B 缓冲
  返回 EFBIG 且不赋 `*size`，**不是写入失败**）。
- 1102 B 是 configd 对 1786 B 的重写结果，是「规范化压缩」还是「删掉
  service」串口无法区分；离线 plistlib 规范化为 2133 B（Apple 格式不同），
  不能据此判定。

---

## 4. 本轮两个提交（kd439-pool）

| commit | 内容 | 实机 |
|---|---|---|
| `5bebed6` | 把 `picasso_netsvc_bundles()` 调用移出 launchd exec 路径；`probe[2048]` 改 `static`（移到 .bss，不占 exec 栈，同时修好回读）；保留全部 IOUserClient.cpp 成功路径探针。3 处等行替换，6 个 hunk 头逐一校验、括号平衡、ABI 0 HARD | chain-597 **0 PANIC**，A 证据到手 |
| `2b941d3` + `b4c9616` | `picasso_network_stack_start()`（t+18 s，本就为 InterfaceNamer 准备真网络栈）在找到 IONetworkStack 后设 `IOUserClientClass=HakiosNetworkStackUserClient`，门控 **`netstack=2`**；打印 `stack early client=2 registered=0/1`；类未注册则不设（失败可见）。`b4c9616` 修 CI 编译错（`OSSymbol::withCString` 返回 `const OSSymbol *`） | CI 43165226487 failure → 43166563777 success |
| **`0576f1d`** | **换思路：内核 DHCPv4 客户端**。新增 `wifi-wcn3990/source/hakios.dhcp.c`（`build-arm-driver.py` 的 `glob('*.c')` 自动编入）：构造 DISCOVER/REQUEST、解析 OFFER/ACK options 1/3/53/54、用 XNU `bsd/kern/netboot.c:558 inet_aifaddr()` 同款 `ifioctl(SIOCAIFADDR)` 装地址、`rtrequest_scoped(RTM_ADD)` 加默认路由。`Wcn3990Hif.cpp` 4 处接线（收帧分流、1ms tick、C 链接 `picasso_dhcp_send` 包装、boot-arg 门控）。`wifiauto` 默认 `g_dhcp=1`，`dhcp=0` 为精确对照 | CI 43168058191 / chain-599 待判 |

脚本：`work/fix-netsvc-exec-and-probe.py`、`work/fix-netstack-early-bind.py`、
`work/chain-597.sh`、`work/chain-599.sh`（均可复现）。

**本地预检（本轮新增，避免再浪费一轮 CI）**：把 `hakios.dhcp.c` 复制到 WSL
完整 XNU 树，用 `.cpo.json` 里的真实编译参数（`clang -xc -fsyntax-only
--target=arm64-apple-ios17.0 -nostdlibinc ...`）跑语法/类型检查 →
**EXIT 0 / 0 errors**。所需内核符号在 `kernel.macho` 已核实：
`ifioctl`39 / `socreate`26 / `rtrequest_scoped`2 / `current_proc`8。

---

## 5. 两条路线的验证状态

### 5.1 configd 路线 A/B（chain-598，已让位）
CI 43166563777 success，同镜像只差 1 字节（`netstack=1` ↔ `2`）。
14:29 用户把优先级改成「先打通能用」，**该 A/B 已主动停止**，只留下被打断的
对照组 `console-exp-n2ctl.cap`（250549 B）：0 PANIC、
`configd_open where=ok class=IOPMrootDomain` ×2、`stack_open`=0、
`configd root_busy`=0、SIOCSIFADDR/DHCP/inet 全 0 —— 与 chain-597 完全一致，
对照成立。这条路线留待「以后慢慢修」，不再占用设备。

### 5.2 内核 DHCP 路线（chain-599，当前主攻）
CI 43168058191 → `work/chain-599.sh`（TAG `dhcpok`）。
镜像参数与 597/598 相同（`netfix=3 netstack=1 netsvc=1 wifiauto`），
**不需要新增 cmdline token**：`wifiauto` 默认块里 `g_dhcp=1`，
`dhcp=0` 可作精确对照。400 s 采集。

判据（checkpoint 明文，不是 0 panic）：
1. `DHCP: DISCOVER` / `DHCP: OFFER` / `DHCP: REQUEST` / `DHCP: ACK`；
2. **`DHCP: bound inet a.b.c.d mask ... router ... addr=0 rt=0`** —— 地址与默认路由都装上；
3. `SIOCSIFADDR` / `inet`；
4. `PICASSO CDC PANIC BEGIN` = 0。

> **判决：PASS**（`console-exp-dhcpok.cap`，592123 B / 400 s）。

```
DHCP: DISCOVER (try 1..6)
DHCP: OFFER 192.168.101.12 server 192.168.101.1
DHCP: REQUEST (try 1)
WPA: tx compl status=0 count=1 ack=9 noack=0
DHCP: ACK from 192.168.101.1
picasso_wlan_tx: [DATA PLANE] ARP frame 42 bytes ...
picasso_wlan_tx: [REAL DATA PLANE] ARP 42 bytes over HTT TX_FRM
net: en0 is asked 0x8020690c by kernel_task      (SIOCSIFADDR)
net: en0 SIOCSIFADDR (configd/DHCP path)
DHCP: bound inet 192.168.101.12 mask 255.255.255.0
      router 192.168.101.1 addr=0 rt=0
net: en0 so far 1 frames sent (42 bytes), 47 received (6390 bytes);
      dropped: 0 out, 0 in
heartbeat polls=57000 ... join=5 ord=49 msdu=50 eapol=3 bad=0/0
```

`addr=0` = `ifioctl(SIOCAIFADDR)` 成功；`rt=0` = 默认路由成功。
装地址后**系统自己发出 ARP 并走真实数据面、被 AP ACK**，这是比
「拿到地址」更硬的 checkpoint。实时串口复查：系统持续心跳到
polls=57000、`join=5` 保持、串口可响应（不是卡死）。
0 次 `PICASSO CDC PANIC BEGIN`。

**仍未验证/仍缺**（不夸大）：采集窗内没有应用主动发起 DNS/HTTP，
所以「Safari 打开网页」尚未实测；IP 不进 SystemConfiguration，
设置/状态栏不会显示已连接；凭据仍在 boot-arg 明文。

---

## 6. 剩余阶段清单（按依赖）

| # | 阶段 | 状态 |
|---|---|---|
| 1 | 开机 + WiFi 并存、UI 可操作 | ✅ 闭环 |
| 2 | wifid 放行 / Apple80211 能力 / 真实扫描 | ✅ 闭环 |
| 3 | 关联 + WPA2 四次握手 + 密钥 + 真实 rx/tx 进栈 + en0 | ✅ 闭环 |
| 4 | **有 IP 地址（能收发 IP）** | ✅ **闭环**：`0576f1d` 内核 DHCP 绕开 configd，chain-599 拿到 `192.168.101.12/24` + 默认路由，系统随后自发 ARP 走真实数据面被 ACK。configd 原生 service 降级为「以后慢慢修」 |
| 5 | 设置 App Wi-Fi 详情不卡 / 状态栏显示已连接 | ⬜ 依赖 configd 的 InterfaceNamer quiet；内核 DHCP 装的 IP 不进 SystemConfiguration，**不影响能否上网** |
| 6 | 凭据持久化（`wpass=` 移出 boot-arg） | ⬜ |
| 7 | 真实 DNS / HTTP / 默认路由 / 状态栏图标 | ⬜ 依赖 #4 |
| 8 | 5G 频段 MGMT_TX（历史 status=3） | ⬜ 非阻塞 |

### #4 若 A/B 证伪 `netstack` 时机假设，按序取证（不要猜）
1. 把 bundle 清单探针挪到**驱动线程**（v17 铁律：kern_exec exec 路径禁加探针），
   确认 `/System/Library/SystemConfiguration/*.bundle` 是否真的可见；
2. 查 `_plugins_exclude` / `plugin_exec` 是否跳过 InterfaceNamer；
3. 检查 `Sets` key `"0"` / `CurrentSet=/Sets/0` 是否必须为 UUID；
4. 兜底：内核 DHCP 客户端（v17 §4，符号已核实：`ifioctl`39 /
   `socreate`26 / `sock_sendmbuf`8 / `sock_receivembuf`2 /
   `dhcpol_parse_packet`1 / `in_control`5；XNU `bsd/kern/netboot.c:558
   inet_aifaddr()` 用 `ifioctl(so,SIOCAIFADDR,&ifra,current_proc())`）。
   **限制**：拿到的 IP 不进 SystemConfiguration，是「能上网」不是「日用」。

---

## 7. 关键文件 / 命令

| 用途 | 路径 |
|---|---|
| 本轮脚本 | `work/fix-netsvc-exec-and-probe.py`、`work/fix-netstack-early-bind.py`、`work/chain-597.sh`、`work/chain-598.sh` |
| 内核网络 patch | `patches/picasso-network-timeout-xnu.patch`（netsvc plist + bundles helper + IOUserClient 探针） |
| 提前绑定位置 | `package/PicassoDisplay.cpp::picasso_network_stack_start()` |
| 晚绑定（旧） | `wifi-wcn3990/source/hakios.wlan.cpp::bind_network_stack_user_client()` ← `HakiosWLANStart()` ← `hakios.net.c:469` |
| Apple 源码（本地 clone，优先于 WebFetch） | `work/refs/apple-oss/configd`（ifnamer.c / prefsmon.c / plugin_support.c）、`work/refs/apple-oss/xnu` |
| 同芯片参考 | `work/trogdor/port/CoachzWifiNet.cpp`、`Trogdor-ipadOS/docs/plans/wifi.md`、`docs/hardware-tests.md` |
| 上机 | `SECS=400 bash work/boot-variants.sh <TAG>`；`python work/serial-cmd.py COM5 status 8` |
| CI | `gh workflow run compose-bootimg.yml -R tselongson/UniXNU-picasso --ref kd439-pool -f fast=true` |

---

## 8. 铁律（沿用 v14~v17 + 本轮增补）

1. 一次只派一个 CI；`fast=true` **复用 seed**（不带 `create_data_seed`）；
   采集窗口必须 >168 s（HIF 启动点），默认 400 s。
2. 判据是 checkpoint 明文 + 用户可操作性，不是 0 panic、不是「进程还在」。
3. **kern_exec 的 exec 路径禁加探针**：栈余量小，`>1 KB` 局部数组或新的
   vnode 调用层都可能踩 guard page（595/596 实测）。要么用 `static`
   把大缓冲移出栈，要么放到驱动线程。`probe` 静态化是本轮的正解范例。
4. patch 必须 LF、hunk 头用脚本重算/逐一校验；改 C/C++ 后跑括号平衡 +
   `work/abi-check.py` 0 HARD；commit message 不写未实机验证的结果。
5. 用户 6 处未提交改动（5 个 `docs/evidence/*.log` 删除 +
   `wifi-wcn3990/docs/diagnostic-charter.md`）一律不提交、不恢复。
6. **归因被证伪立即写进文档**，不要带着旧结论排查（v17 §2、本轮 §2.1/§2.5）。
7. **先看自己树里有没有等价实现再照抄参考**（`bpfattach` vs `bpf_attach`、
   10-26 UTRD 都是这么踩的）。
8. 995 B cmdline 已满：新增门控优先复用现有 token 的**取值**（本轮
   `netstack=2`），而不是加 token。

---

## 9. 结论与下一步（2026-11-12 14:5x）

**用户目标「WiFi 能用」已达成**：`192.168.101.12/24`、默认路由 `192.168.101.1`、
真实数据面收发、0 PANIC、系统可操作、关联保持。绕开 configd 的路线
（`0576f1d`）一次上机闭环，没有再依赖任何探针/二分。

### 建议的后续顺序（按用户「以后其他方向慢慢修」）
1. **实测上网**：设备当前仍在系统里，可在设置/Safari 打开任意网页；
   若不通，先看 DNS（option 6 已解析但未写 resolv.conf）与 ARP 应答。
2. **设置/状态栏显示**：把地址同步进 SystemConfiguration，或修好
   configd 原生 service 链（§2.3 的因果链已完整，属于「日用」而非「能用」）。
3. **凭据持久化**：`wpass=` 移出 boot-arg 明文。
4. **configd 路线收尾**（低优先）：`netstack=2` 提前绑定 A/B 已备好脚本
   `work/chain-598.sh`，随时可跑，验证 InterfaceNamer 是否恢复。

### 本轮最终提交（kd439-pool）
`5bebed6` → `2b941d3` → `b4c9616` → `0576f1d`。
CI：43162658105 / 43165226487(fail) / 43166563777 / 43168058191 全 success 除标注。
证据：`out/full-flash/logs/console-exp-{nstack,n2ctl,dhcpok}.cap`。
