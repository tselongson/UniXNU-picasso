# WiFi (WCN3990) 交接文档 — 2026-11-12 17:1x（v20）

> 【历史归档】本文已被 **`wifi-wcn3990-handoff-2026-11-12-v21.md`** 取代，冲突以 v21 为准。
> v21 新增：18:2x 设置详情仍卡死的取证、卡死真因（configd 打开接口服务被
> entitlement 拒绝，见 `hakios.wlan.cpp` 文件头注释）、今日完整进度总表、
> 慢在哪的方法论复盘、以及「最后一次构建 + 分层验收」的换思路。

> ~~**本文件是当前唯一权威状态源。**~~
> 判据口径：**checkpoint 明文 + 用户可操作性，不是「0 panic」**。

---

## 0. 一句话现状

**用户 17:1x 实测「网站还是加载不进去」，取证后发现更靠前的真因：
WiFi 关联已被 AP 踢掉且不重连，所以这次不能用来判定 DNS。**

```
live-safari-fail.cap: join=0, ord=89 msdu=90 冻结, beacons 仍涨
live-dnsfix-status.cap:
  JOIN: access point ended it, frame 0xc0 reason 3
  JOIN: stopped in state 5 (deauth/disassoc)
  apple80211: the station says: the link, 0 / DHCP: link down, reset
```
`dhcpok/n2early2/dnsfix` 三个 400 s 采集里 deauth 都是 0 次 ⇒ 采集结束后
长时间运行才发生（AP 无流量踢 STA）；`join=0` 永不恢复是**源码级 bug**：
`hif_join_begin()` 选中 BSS 后清 `g_join_req.pending`(:3942)，而 wifiauto
10 s 重扫 (:4814) 与 scan-completed 触发 (:4549) 都依赖它，
`g_autoscan_sent` 恒为 1 ⇒ 一次 deauth 后所有重连入口关闭。
**已修：`5eff826`**（`hif_join_reset()` 对 wifiauto 重新 arm pending +
autoscan，保留 ssid/口令与 10 s 退避；CI `43190244911`）。

**`console-exp-recon.cap`（5eff826，600 s，0 PANIC）**：
- `join=5` 从 `polls=1000` 一直保持到 `118000`（设备实时复查到 `134000`），
  **600 s 内没有 deauth** ⇒ 重连修复没被触发（无回归，但触发验证仍待更长运行）。
- 三条 resolver 路径的 errno（设备自己回答）：
  `/private/var/run/resolv.conf`=0；`/private/etc/resolv.conf`=**30 EROFS**；
  `/etc/resolv.conf`=**30 EROFS**。
  ⇒ pong 的 `/etc` 别名是**构建镜像时**做的（`rootful18.py` protected 列表），
  运行时内核补不了。
- 结合开源源码：`dns_configuration_copy()` 是纯用户态 XPC
  （`dnsinfo_copy.c`/`dnsinfo_server.c`）；`dnsinfo_flatfile.c` 虽解析
  `/etc/resolv.conf`，但 `dns-configuration.c:1720` 的 flatfile 分支只在
  **已有 default resolver** 的 else 分支执行，启动时
  `dns_configuration_set(NULL×7)`（`ip_plugin.c:9097`）走 if 分支不读文件。
  ⇒ **写 resolv.conf 路线到头，不可能让 Safari 解析**。

**`c670eb5` 判决（`console-exp-role.cap`，400 s，0 PANIC）**：96/97 已经
`answered`、`REFUSED=0`（清零），`join=5`、DHCP bound、DNS 发布正常；
**但 `wifid sets`/`SCAN_REQ`/`ASSOCIATE` 仍是 0，configd 仍没建 service**。
原因不是 bug：参考项目 Trogdor AGENTS.md 明确「要有 **saved network**
才会开机自动加入」，我们的凭据只在 `wpass=` boot-arg，wifid 数据库里
没有已知网络，只做能力探测符合预期。
⇒ **96/97 解决的是「设置详情卡死」的直接原因，需用户实测验收**：
设备此刻在系统里（`join=5`、0 PANIC、`msdu=87` 持续涨）。
若能进原生 WiFi 界面并手动连接，wifid 会走原生 ASSOCIATE → configd
建 service → 发布 DNS → 原生解析自动通，这是零成本的最优解。
若设置能进但 Safari 仍不通 ⇒ 只剩用户态注入 `ipconfig_set_service`
（MIG 20011，需 arm64 二进制 + launchd job + 重建 seed；设备上
`ipconfig`/`scdutil` 均不存在，无法复用）。

驱动/固件/IP 层全通，DNS 三个前提已被实机证实；**最终端到端判决必须在
关联稳定（join=5）时进行**：

```
DHCP: options 53,54,51,58,59,1,28,3,6 dns=1      AP 给了 option 6
DHCP: dns 192.168.101.1 count=1                  DNS = 网关
DHCP: dns write /private/var/run/resolv.conf error=0 close=0 bytes=77
DHCP: dns resolver pid=132 / reload signalled     mDNSResponder 在跑、SIGHUP 已发
0 PANIC，msdu=51 bad=0/0
```

**需要用户实测（零成本、决定性）**：
- `https://www.apple.com/library/test/success.html`（参考项目 hakios 的验收页）
- 对照 `http://192.168.101.1`（网关，验证 IP 层）

---

## 1. v19 被实机推翻/更正的假设（别再重查）

| v19 的说法 | 实机判决 | 证据 |
|---|---|---|
| 「200 个 exec 里没有 mDNSResponder ⇒ 没跑」 | **错**：pid=132 存在 | `console-exp-dnsfix.cap:5040`。参考项目 notes.md 早警告「/dev/console 丢行，日志里没有 ≠ 没发生」；守护进程按需启动 |
| 「/etc 是只读根卷，resolv.conf 大概率写不进去」 | **错**：`/private/var/run/resolv.conf` 一次写成功 | `dnsfix.cap:5036-5039`，77 B、fsync+close 全 0。这正是 `UniXNU-pong/userland/package-network.c:44` 用的标准路径 |
| 「AP 提供的 DNS 被静默丢弃」 | **方向对、此前从未证实**：option 6 确实存在且值为网关 | `dnsfix.cap:4950/5035` |
| 「IPConfiguration.bundle 不存在 ⇒ 原生 DHCP 不可行」 | **可疑**：iOS 的 IPConfiguration 是 launchd/Mach service，不是 configd plugin bundle | boot.img 有 `com.apple.network.IPConfiguration` job 名；参考项目 `net_test.m:190-230` 的 MIG 20011 `ipconfig_set_service` |
| 「只差 DNS、写 resolv.conf + SIGHUP 即可」 | **作为修复存疑**：所有真机联网机型的 DNS 都来自 configd 原生链 | 见 §2 |

---

## 2. 全机型参考盘点（用户点名「怎么不参考 unixnu」）

| 机型/参考树 | DNS 怎么来的 | resolv.conf |
|---|---|---|
| `UniXNU-pong/userland/package-network.c` | `SCDynamicStoreCopyValue("State:/Network/Global/DNS")` 取 `ServerAddresses`；写 `/private/var/run/resolv.conf`、`/etc/resolv.conf` 设别名；用 `getaddrinfo()` 验证原生解析 | 有，但注释明写「给 Unix bootstrap tools 用」「**不保证修复 mDNSResponder/原生 resolver**」 |
| `work/refs/hakios`（真机 HTTPS 200 / bag.xml） | `rc.jobs.net` → `net_test dhcp en0` → `ipconfig_set_service` 让 configd 建 service 并「publishes what it learns」 | **没有** |
| `Trogdor-ipadOS`（同芯片 WCN3990） | `wifi.md:21` DHCP+DNS 同一项，Safari HTTPS 通；靠 configd | 没有 |
| `unixnu-wcn7860` | `PeachNet.c` 只有数据面元数据注释 | 没有 |
| `work/refs/hakios-picasso`（同芯片 picasso） | 硬编码 BSSID + 伪造 DHCP 应答的假栈 | 没有 |

**共同结论**：没有一例靠写 resolv.conf 让 Safari 解析。Apple 开源 mDNSResponder
仓库（GitHub API 列根目录）**只有 mDNSCore/mDNSPosix/mDNSShared/mDNSWindows，
没有 macOS/iOS 平台层** ⇒ v19 引的 `mDNSPosix.c /etc/resolv.conf` 是 POSIX 层，
无 iOS 源码背书。`kernel.macho`（55 MB）里 `resolv.conf`/`dns_configuration`/
`ServerAddresses` 各 0 处 ⇒ XNU 无内核 DNS 配置接口。

---

## 3. 本轮两个提交 / 两次实机判决

| commit | 内容 | 实机 |
|---|---|---|
| `9fb7091` | 整份重写 `work/fix-wlan-dns.py`：option 6 解析 + 打印完整 option tag 列表、按 Apple 格式写候选 resolver 路径（每条打印 errno）、`proc_name` 找 mDNSResponder + SIGHUP；全部在驱动线程 `dhcp_install()`，不碰 launchd exec 路径 | CI `43185638638` / `console-exp-dnsfix.cap`，§0 判决 |
| （单变量 A/B，零重建） | `netstack=1→2` 原地只改 1 字节（全 65 MB 仅 1 字节不同） | `console-exp-n2early2.cap`：`stack early client=2 registered=1` 跑到了，但 `stack_open`/`configd root_busy` 仍 0、`config stage=5 quiet_s=5` 与对照组一致 ⇒ **configd 原生链没恢复** |

门禁：ABI 0 HARD、括号/圆括号 0、**WSL 完整 XNU 树 `.cpo.json` 真实参数
`clang -xc -fsyntax-only` EXIT 0 / 0 errors**。

---

## 4. 已排除（避免下一轮重踩）

- **SIGHUP 不能让 configd 重载**：`configd.m:141 catcher()` 只处理 SIGINT/SIGTERM，
  SIGHUP 无 case；`plugin_support.c` 无 reload；`prefsmon.c:403` 只有 quiet 才
  `establishNewPreferences()`（iOS 上 timeout 不算）。
- **configd 不监听 plist 文件变化**：`prefsmon.c:213 watchSCDynamicStore()` 只
  `SCDynamicStoreSetNotificationKeys()` 监听 dynamic-store keys；无 kqueue/
  dispatch_source 文件监听 ⇒ 「内核回填 plist 让 configd 自动重读」不成立。
- **`netstack=2` 提前绑定**：见 §3，干净证伪。
- **BPF 缺失**：`hakios.net.c:452` 已有 `bpfattach`（v18 已排除，别再加）。
- 96/97 语义仍无权威定义；pong `services88-native-peripherals.md` 显示同类
  问题靠补齐 IO80211 UserClient 的 ioctl tunnel 解决，不是补零应答。

---

## 5. 剩余缺口清单

| # | 缺口 | 状态 |
|---|---|---|
| 0 | **AP deauth 后不自动重连**（日用可用性） | 🔶 `5eff826` 已修待上机；判据 `re-arming connect` → 再次 `assoc=0`/`keys installed`/`DHCP: bound`/`join=5` |
| 1 | DNS / 域名解析（Safari 输域名） | 🔶 三个前提已实机证实；**用户实测无效**（当时已掉线），待 0 修好后重测 |
| 2 | 设置 App 点 Wi-Fi 详情卡死 | ❌ apple80211 96/97 `EOPNOTSUPP`；Preferences 同步等 XPC |
| 3 | 状态栏 / SystemConfiguration 显示已连接 | ❌ 内核 DHCP 的 IP 不进 dynamic store（与 #1 同源） |
| 4 | 凭据持久化 | ❌ `wpass=` 仍在 boot-arg 明文 |
| 5 | Safari HTTPS 端到端实测 | ⬜ **从未在关联稳定时做过**，需用户配合 |
| 6 | 5G MGMT_TX `status=3` | ⬜ 非阻塞 |

**已通（实机）**：固件/HIF/CE/HTC/WMI/HTT、扫描、auth/assoc/VDEV_UP、
WPA2 四次握手、PTK/GTK、PEER_AUTHORIZE、真实 rx/tx 进栈、en0 link、
KEV、内核 DHCP（`192.168.101.12/24` + 默认路由）、系统自发 ARP 走真实
数据面被 ACK、开机进桌面可操作、0 PANIC。

---

## 6. 下一步（按用户实测结果分叉）

**若 Safari 能打开** → WiFi 上网闭环；剩余仅 #2/#3/#4 日用层，按 pong
`services88-native-peripherals.md` 的「补齐标准 ioctl tunnel、交回系统界面」
思路做设置 UI。

**若 Safari 打不开** → resolv.conf 路线证伪（pong 注释已预示），按参考项目走：
1. `net_test.m:190-230` 的 `ipconfig_set_service`（MIG msgh_id 20011，
   `bootstrap_look_up("com.apple.network.IPConfiguration")`）让 configd 直接建
   service 并发布 DNS；或
2. 用户态 root 程序直写 `SCDynamicStore` 的 DNS 键 / 调
   `dns_configuration_set()`（`dns-configuration.c:2106` 的用法）。
   两者都需要 arm64 iOS 用户态二进制注入 + launchd job，成本高于内核方案，
   但这是所有真机联网机型的共同路径。
3. 不再试更多 resolv.conf 路径、不再给 configd 发 SIGHUP、不再回填 plist。

---

## 7. 铁律（沿用 v14~v19）

1. 一次只派一个 CI；`fast=true` 复用 seed；采集窗口 ≥ 400 s（HIF 在 t+168 s）。
2. 判据是 checkpoint 明文 + 用户可操作性，不是 0 panic、不是「进程还在」。
3. **kern_exec exec 路径禁加探针/写文件**（chain-595/596 panic）。
4. patch 一律 LF、hunk 头重算、`git apply --check`；改 .c 后先跑 WSL
   `-fsyntax-only` 再派 CI；`abi-check.py` 0 HARD；括号平衡 0。
5. 995 B cmdline 已满，复用现有 token 取值，不新增 token。
6. commit message 不写未实机验证的结论。
7. 用户 6 处未提交改动（5 个 `docs/evidence/*.log` 删除 +
   `diagnostic-charter.md`）一律不提交、不恢复。
8. **归因被证伪立即写进文档**（v19 三条假设就是这么被推翻的）。
9. 先查自己树里有没有等价实现再照抄参考（`bpfattach` 教训）。

---

## 8. 关键路径

| 用途 | 路径 |
|---|---|
| 内核 DHCP + DNS 发布 | `wifi-wcn3990/source/hakios.dhcp.c`（`dhcp_option` / `dhcp_publish_dns` / `dhcp_install`） |
| 可复现脚本 | `work/fix-wlan-dns.py`（整份重写版）、`work/mk-autojoin-args.py`、`work/mk-args-variant.py` |
| 判决日志 | `console-exp-{dhcpok,n2early2,dnsfix}.cap` |
| 参考项目 DNS/网络 | `UniXNU-pong/userland/package-network.c`、`work/refs/hakios/userland/src/net_test.m`、`work/refs/apple-oss/configd/Plugins/IPMonitor/dns-configuration.c` |
| 上机 / 串口 | `SECS=400 bash work/boot-variants.sh <TAG>`；`python work/serial-cmd.py COM5 status 8` |
| CI | `gh workflow run compose-bootimg.yml -R tselongson/UniXNU-picasso --ref kd439-pool -f fast=true` |
