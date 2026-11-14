# WiFi (WCN3990) 进度总表与换思路 — 2026-11-12 18:3x（v21）

> **本文件是当前唯一权威进度源。** v20 及更早为历史归档，冲突以本文为准。
> 判据口径：checkpoint 明文 + 用户可操作性，不是「0 panic」。

---

## 0. 一句话状态

**驱动/固件/关联/加密/数据面/内核 DHCP 全部实机打通；剩余两件都在用户态：
① 设置 App 的 Wi-Fi 详情仍卡死（已定位到 IOServiceOpen 被 entitlement 拒绝，修复已写好待上机）；
② 域名解析不通（configd 从未建网络 service）。**
今天 6 次 CI、7 次上机，方法上是串行打补丁，本轮起改为「最后一次构建 + 分层验收」。

---

## 1. 今日完整进度（16:2x 接手 → 18:3x）

| 时间 | 动作 | 实机判决 |
|---|---|---|
| 16:2x | 通读 v19/v18、11-12 全部日志 | 质疑「只差 DNS」：v19 三条假设证据不足 |
| 16:3x | 单字节 A/B `netstack=1→2` | `stack early client=2 registered=1` 生效，但 `stack_open`/`root_busy` 仍 0 ⇒ configd 原生链没恢复（**证伪**） |
| 16:4x | 全机型参考盘点（含 `UniXNU-pong`） | 所有真机联网机型的 DNS 都来自 configd；无「写 resolv.conf 让 Safari 解析」的先例 |
| 16:4x | `9fb7091` option 6 + resolver 文件 + SIGHUP | AP 给 option 6（DNS=网关）、`/private/var/run/resolv.conf` 可写、mDNSResponder pid=132 在跑（推翻 v19 三条假设） |
| 17:1x | 用户实测「网站打不开」→ 实时取证 | **关联已被 AP deauth（frame 0xc0 reason 3）、join=0、msdu 冻结** ⇒ 该次不能当 DNS 判决 |
| 17:1x | `5eff826` 掉线重连 + 三路径写入 | 600 s `join=5` 稳定、0 PANIC；`/etc` 与 `/private/etc/resolv.conf` 均 **EROFS(30)** |
| 17:3x | `c670eb5` 96/97/70（pong 权威定义） | `REFUSED=0` 清零，但 `wifid sets`=0、configd 仍无 service |
| 18:2x | 用 `c670eb5` 重新上机供用户实测 | **设置 Wi-Fi 详情仍进不去**；Preferences 同步等（`queued=6`、7 线程 `runnable_ms=0`） |

### 提交与证据
`9fb7091`（测量轮）→ `5eff826`（重连+三路径）→ `c670eb5`（96/97）→ 待提交 `ifaceuc`。
CI `43185638638` / `43190244911` / `43192969177` 全 success；
日志 `console-exp-{dnsfix,recon,role,settings}.cap` 与 `live-settings-*.cap`。

---

## 2. 已通 / 未通清单

**✅ 实机闭环**：固件/WLFW/CE/HTC/WMI/HTT、扫描、auth/assoc、VDEV_UP、
WPA2 四次握手、PTK/GTK、PEER_AUTHORIZE、真实 rx/tx 进 Darwin 栈、
en0 link、KEV_DL_LINK_ON、内核 DHCP（`192.168.101.12/24` + 默认路由）、
装地址后系统自发 ARP 走真实数据面被 AP ACK、开机进桌面可操作、
掉线自动重连（`5eff826` 代码级，600 s 未触发自然 deauth 故未验证）、
apple80211 96/97/70 如实应答、0 PANIC。

**❌ 未通**：
1. 设置 App 点 Wi-Fi 详情卡死 —— 根因见 §3，修复 `work/fix-iface-userclient.py` 待上机。
2. 域名解析 —— configd 从未建 service，原生 DNS 配置未发布。
3. 状态栏/SystemConfiguration 不显示已连接（与 #2 同源）。
4. `wpass=` 仍在 boot-arg 明文（凭据持久化）。
5. Safari HTTPS 从未在「关联稳定 + IP 对照」下完整实测。
6. 5G MGMT_TX `status=3`（非阻塞）。

---

## 3. 设置卡死的真因（源码 + 实机双证，别再查 96/97）

`wifi-wcn3990/source/hakios.wlan.cpp` **文件头注释**与
`HakiosNetworkStackUserClient` 注释原文：
> 「configd 的 InterfaceNamer 打不开 user client ⇒
> `SCNetworkInterfaceCopyAllWithPreferences` 在 **Preferences（设置 > Wi-Fi）
> 里永远等待**」
> 「接口服务**故意没有 user client**，让框架 open 失败后回退 socket ioctl」

实机 `PICASSO_NETWORK configd_open where=denied class=IOEthernetInterface`；
96/97 修好后 `REFUSED=0` 但设置仍卡 ⇒ 卡的是 **configd 打开接口服务被
entitlement 拒绝**，不是 Apple80211 GET。
探针只打印 `configd` 进程的 open（patch 里 `if (!strcmp(caller,"configd"))`），
所以 Preferences/wifid 的尝试一直不可见。

**修法（照搬本仓库已验证的做法）**：`IONetworkStack` 用
`stack->setProperty(kIOUserClientClassKey, "HakiosNetworkStackUserClient")`
让无 entitlement 的 open 成功；同样给 `wlan` 接口服务挂
`HakiosWLANInterfaceUserClient`，`ifaceuc=1` 门控保留单变量对照。
不实现 pong 的 selector-0 tunnel（那是 22H374 的 CoreWiFi binding；
本 iOS16 构建按设计回退 socket ioctl）。

---

## 4. 已证伪 / 已排除（黑名单，别重查）

| 假设 | 结论 |
|---|---|
| 写 `/etc/resolv.conf` + SIGHUP 能修原生 DNS | **证伪**：`/etc` 与 `/private/etc` 均 EROFS；pong 的别名是构建期做的；`dns_configuration_copy()` 是纯用户态 XPC；flatfile 只在已有 default resolver 的 else 分支读 |
| `netstack=2` 提前绑定能恢复 configd 链 | **证伪**（单字节 A/B） |
| SIGHUP 让 configd 重载 plist | **证伪**：`configd.m:141 catcher()` 只处理 SIGINT/SIGTERM |
| 内核回填 preferences.plist 让 configd 重读 | **证伪**：`prefsmon.c:213` 只监听 dynamic-store keys，不监听文件 |
| mDNSResponder 没启动 | **证伪**：pid=132/122 实机存在（串口丢行 + 按需启动） |
| AP 没给 option 6 | **证伪**：`DHCP: options ...6 dns=1`、DNS=192.168.101.1 |
| BPF 缺失 | **排除**：`hakios.net.c:452` 已有 `bpfattach` |
| 96/97 拒答是设置卡死根因 | **排除**：修好后仍卡 |
| `IPConfiguration.bundle` 不存在 ⇒ 原生 DHCP 不可行 | **可疑**：iOS 的 IPConfiguration 是 launchd/Mach service，不是 plugin bundle |

---

## 5. 慢在哪（诚实复盘）

**方法病根：串行「单点补丁 → 等 10-12 分钟 CI → 等 7 分钟上机 → 看一个
checkpoint → 再猜下一个」**，每轮只验证一个假设。真正的墙是
configd/Preferences 用户态黑箱：开源 configd 与设备 20H392 版本不同，
逐行推行为多次失效；探针可视范围还被 `configd` 过滤限制；而根因
**写在我们自己的文件头注释里**，直到 18:2x 才对上。

---

## 6. 换思路（本轮起执行）

1. **停止逐点 A/B**：执行 `work/fix-iface-userclient.py`（已写好），
   门禁 → 提交 → **最后一次**驱动侧构建。
2. **验收分层、先判据后代码**，请用户在同一台设备依次测：
   ① `http://192.168.101.1`（纯 IP 层）→ ② 域名 → ③ 设置详情。
   **IP 与域名的对照今天一直没做**：IP 通/域名不通 = 只差 DNS；
   IP 都不通 = 还有更基础的数据面问题，方向完全不同。
3. **若这轮仍不通，不再加第 N 个补丁**。给两条明确路线让用户选：
   - (a) 整体嫁接参考项目 `work/refs/hakios` 的网络栈（`net_test` job、
     `fw/network`、`ipconfig_set_service` MIG 20011），需重建 seed/注入
     arm64 二进制；
   - (b) 承认设置/状态栏属 iOS 系统集成限制，以「能上网」为交付目标。

---

## 7. 铁律（沿用）

1. 一次只派一个 CI；`fast=true` 复用 seed；采集 ≥400 s（HIF t+168 s）。
2. 判据是 checkpoint 明文 + 用户可操作性，不是 0 panic。
3. kern_exec exec 路径禁加探针/写文件（chain-595/596 panic）。
4. 改 .c/.cpp 先跑 WSL 完整 XNU 树 `-fsyntax-only`（加 `-I .../source`），
   再 `abi-check.py` 0 HARD、括号平衡 0。
5. 995 B cmdline 已满：新门控复用 padding，不新增 token 破坏单变量。
6. commit message 不写未实机验证的结论；归因被证伪立即写文档。
7. 用户 6 处未提交改动（5 个 `docs/evidence/*.log` 删除 +
   `diagnostic-charter.md`）一律不提交、不恢复。

---

## 8. 关键路径

| 用途 | 路径 |
|---|---|
| 接口 UserClient 修复脚本 | `work/fix-iface-userclient.py`（待执行） |
| 接口发布 / 真因注释 | `wifi-wcn3990/source/hakios.wlan.cpp` |
| Apple80211 GET | `wifi-wcn3990/source/hakios.apple80211.c` |
| 内核 DHCP / DNS 发布 | `wifi-wcn3990/source/hakios.dhcp.c` |
| 参考项目 | `UniXNU-pong/userland/package-network.c`、`pong-wifi/PongWifiNet.cpp`、`work/refs/hakios/userland/src/net_test.m` |
| 最新判决日志 | `console-exp-{dnsfix,recon,role,settings}.cap`、`live-settings-*.cap` |
| 上机 / 串口 | `SECS=400 bash work/boot-variants.sh <TAG>`；`python work/serial-cmd.py COM5 status 8` |
