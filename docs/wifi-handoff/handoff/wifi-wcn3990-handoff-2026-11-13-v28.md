# WiFi (WCN3990) 交接 — 2026-11-13 19:4x（v28：chain-607 判决 → 修掉 configd 崩溃循环）

> **【历史归档】本文件已不是权威状态源，请以
> `handoff/wifi-wcn3990-handoff-2026-11-13-v29.md` 为准。**
> v29 记录 verify-608 完整 400 s 实机判决：`857e48f` 的 DNS 发布三项全通、
> configd 稳定 1 次；采集末期的 panic 取证为 UFS 存储故障，与 WiFi 无关。
> 判据口径：checkpoint 明文 + 用户可操作性，不是「0 panic」。
> 本版核心：`6673613` 的 DNS 方向已由源码证实正确，但**实机暴露一个回归**——
> `wifiui_configure_service()` 每次崩掉 configd，而我上轮把 DNS 发布放在它
> 后面，导致 DNS 分支一次都没跑。`857e48f` 停用该调用并把 DNS 拆到独立线程。
> **修复未上机，不得宣称已修好。**

---

## 0. 一句话状态

```
驱动/固件/握手/数据面/en0/内核 DHCP ✅   *TIMEOUT* 发布 / wifid sets×17 ✅
chain-607 (6673613) 实机：0 PANIC / DATASTORE ready=1 / DHCP bound / join=5
  ❌ configd started with = 6（应 1），每轮 service begin 后 configd 自身
     EXC_CORPSE → launchd 重启（pid 34→238→305→327→335→353）
  ❌ dns wait= / dns servers= / dns set= 全 0 → DNS 分支从未执行
  ⇒ 因果：DNS 发布被挪到 wifiui_configure_service() 之后，
     而该调用会崩 configd ⇒ 永远走不到 DNS
── 857e48f：停用 configd 进程内的 SCPreferences service 调用
   （v26 §4.8 已有结构性结论）+ DNS 改由独立 detached pthread 发布
   门禁全过，CI 43418179396 在跑
── 设备已在 fastboot（64993333），后台任务 okoQkE 等 CI 后自动跑 chain-608
```

---

## 1. chain-607 实机判决（CI `43410190812`，c1 400 s）

`console-exp-c1.cap`（527,466 B / 7,100 行），产物与单变量均已在 v27 §8 核实。

| 判据 | c1 实测 | 期望 |
|---|---|---|
| `PICASSO_WIFIUI` | 43 | — |
| `configd started with` | **6** | **1** |
| `quiet KERN_DENIED` / `key=Plugin:InterfaceNamer` / `service begin` | 各 6 | 各 1 |
| `truthful=1 publish=1`（精确整行） | ✅ | ✅ |
| `service entry=65799 wifi_class=0` | 1（第 2 轮） | — |
| `dns wait=` / `dns servers=` / `dns set=` | **0 / 0 / 0** | ≥1 / ≥1 / 3 |
| DNS INPUT KEYS SET OK | **0** | 2 |
| `wifid sets` | 17 | ≥1 |
| `DATASTORE ready=1` / `DHCP: bound inet` / `join=5` | 1 / 1 / 8 | ✅ |
| `PANIC BEGIN` | 0 | 0 |
| `Early boot complete` | 2 | ✅ |

**因果链（逐行取证，不是推测）**：
```
3265 PICASSO_WIFIUI service begin already=0 published=1
3266 Failed to send exception EXC_CORPSE_NOTIFY. error code: 5 for pid 34
3267 PICASSO_WIFIUI configd started with DYLD_INSERT_LIBRARIES=...   (重启)
...
4410 service begin   4411 service entry=65799 wifi_class=0
4416 EXC_CORPSE pid 238   5101 configd started with ...
5312 service begin   5315 EXC_CORPSE pid 305   5546 configd started with ...
```
每一轮 `service begin` 后紧跟**configd 自己 pid** 的 EXC_CORPSE，然后 launchd 重启；
c0 采集因 `fastboot_ready=0 after 60 polls`（COM5 `cannot open COM5: 5`）失败，
但 v26 已多次验证 c0（wifiui=0）完全 inert。

---

## 2. 精准定位

### 2.1 为什么 DNS 一次都没发布
`6673613` 为了避免 240 s 有界等待推迟已验证的 checkpoint，把
`wifiui_publish_dns()` 移到 `wifiui_bootstrap()` **最后**，即
`wifiui_configure_service()` **之后**。而后者会崩 configd ⇒
bootstrap 从未走完 ⇒ DNS 分支不可达。这是我上一轮的判断失误：
「保护已验证 checkpoint 时序」反而把新功能放到了一个会崩溃的调用后面。

### 2.2 为什么 `wifiui_configure_service()` 会崩
v26 §4.8 的硬结论：configd **自己**在主 RunLoop 上持有
`preferences.plist-lock` 文件锁，注入库运行在 configd 进程内，
无论 `wait=false`（`locked=0`）还是 `wait=true`（死锁 → 5cb8977 撤销）
都拿不到锁。**从 configd 进程内建 SCPreferences service 结构上不可行**。
chain-607 进一步实测：该调用现在直接触发 configd 崩溃/被 watchdog 杀。
⇒ 正确做法是**不调用它**，而不是继续调参数。

### 2.3 `6673613` 的 DNS 方向本身是对的（源码闭环，v27 §9）
`ip_plugin.c:4849` `merge_list` 含
`{ kSCPropNetDNSSupplementalMatchDomains, ALLOW_EMPTY_STRING, TRUE }`，
`active_protos == kProtocolFlagsNone` 时正是靠该键避免 `goto done`；
`:5021` `service_dict_set` → `:8673` `update_dnsinfo()` →
`dns_configuration_set()` → `_dns_configuration_store()`。
**无需改动 DNS 键/字典，只需让它真正被执行。**

---

## 3. 修复（commit `857e48f`，1 file +60/−15）

1. `WIFIUI_ALLOW_INTERNAL_SERVICE = 0`：`wifiui_bootstrap()` 不再调用
   `wifiui_configure_service()`；打印
   `PICASSO_WIFIUI service disabled (configd-internal prefs lock, chain-607)`。
   函数保留引用（编译期常量门控），便于未来在独立 root 进程方案里复用。
2. **新增 `wifiui_dns_thread()`**：注入时起第二个 detached pthread，
   自带 `dlopen(SystemConfiguration)` / `dlsym(SCDynamicStoreCreate,
   SCDynamicStoreSetValue)` / 独立 session，自己有界等待
   `/private/var/run/resolv.conf` 并发布 DNS。
   **InterfaceNamer / service 路径上的任何故障都无法阻止 DNS 发布。**
3. `picasso_springboard_probe()` 的 configd 分支起两个线程；
   `wifiui=1` 内核注入门控不变 ⇒ c0 不加载库，仍完全 inert。
4. `wifiui_publish_dns()` 的键与字典保持 `6673613` 不变。

门禁：WSL clang `-fsyntax-only -Wall -Wextra -Werror` **RC 0 / 0 errors**
（`work/tmp-wifiui-syntax.py` selftest 加 `wifiui_dns_thread` 引用）；
括号/圆括号/方括号 0/0/0；`abi-check.py` **0 HARD**；
`.m` sha `73e1e5fc…`（recipe 变）；用户 5 个 evidence 删除与 charter.md 原样保留。

CI `43418179396`：`fast=true create_data_seed=true`（不带 build_userdata）。

---

## 4. 下一轮上机（`work/chain-608.sh`，后台任务 `okoQkE` 自动执行）

**只跑 c1 判决组**：chain-607 的 c0 在设备往返时失败
（`fastboot_ready=0`），而 c0 inert 已在 v26 多次验证，避免再冒一次风险。

顺序不可换：等 CI → 下载 bootimg → 设备进 fastboot（当前已在，
`64993333`）→ 几何核对 `0x1AC2BFB000` → 16 KiB marker 清零 →
**用新 boot.img** 重生成 c1（旧 `boot-exp-c1.img` 是 `6673613`，不可复用）
→ 400 s 采集 → 判读。采集后**不重启**，留给用户在桌面上实测。

| 判据 | 期望 |
|---|---|
| `configd started with` | **1**（6 = 回归未修） |
| `service disabled (configd-internal prefs lock, chain-607)` | 1 |
| `dns wait=` / `dns servers=` | 各 ≥1 |
| `dns set=1 key=Setup:/Network/Service/picasso-wifi/DNS` | 1 |
| `dns set=1 key=State:/Network/Service/picasso-wifi/DNS` | 1 |
| `dns set=1 key=State:/Network/Global/DNS` | 1 |
| `truthful=1 publish=1` / `wifid sets` / `DHCP: bound` / `join=5` | ✅ |
| `DATASTORE ready=1` / 0 `PANIC BEGIN` | ✅ |
| **用户实测** | `http://192.168.101.1` → 域名 → 设置 > Wi-Fi 详情 |

⚠ DNS 最终判据**只能是用户实测**：`getaddrinfo()` 结果不进串口日志，
`State:/Network/Global/DNS` 全仓库只在源码注释里（无打印代码）。

---

## 5. 未通清单（均未宣称已修）

1. **DNS / 域名解析** —— `857e48f` 待上机 + 用户实测；
2. **设置 App 点 Wi-Fi 详情卡死** —— `*TIMEOUT*` 已发布、`wifid sets`×17，
   但用户 11-12 实测仍卡；`ifaceuc=1` 已被 11-12 A/B 证伪；
   下一个可查方向是 Preferences 等的 XPC 通道（v21 §3）；
3. **状态栏 / SystemConfiguration 显示已连接** —— 与原生 service 同源；
4. **凭据持久化** —— `wpass=` 仍是 boot-arg 明文；
5. **Safari HTTPS 端到端实测** —— 从未在「关联稳定 + IP 对照」下做完；
6. 5G MGMT_TX `status=3`（非阻塞）。

v27 §9.1 的「独立 root 进程」预案（`UniXNU-pong/userland/wifi-bootstrap.c
--configure-only` + 本仓库 `xcrun -sdk iphoneos` 编译链）仍未执行，
**必须先有 chain-608 的 DNS 判决**。

---

## 6. 铁律（v27 §6 全部沿用，新增 1 条）

18. **新增功能不要放在已知会崩溃/重启的调用后面**。chain-607 把 DNS
    发布挂在 `wifiui_configure_service()` 之后，导致它一次都没执行。
    有界等待/异步工作应放独立线程，与不稳定的控制面路径解耦。

---

## 7. 关键路径

| 用途 | 路径 |
|---|---|
| 唯一权威 | 本文 v28 |
| 本轮修复 | `857e48f`（CI `43418179396`） |
| chain-607 判决 | `out/full-flash/logs/console-exp-c1.cap`、`work/chain-607-auto.out` |
| 上机链（新） | `work/chain-608.sh [boot.img]`（`bash -n` 通过） |
| 自动调度 | 后台任务 `okoQkE`（CI 完成 → 等 fastboot → 自动跑 608） |
| 语法检查 | `work/tmp-wifiui-syntax.py` |
| 源码证据 | `work/refs/apple-oss/configd/Plugins/IPMonitor/ip_plugin.c:4849/8673/8953-9004`、`dns-configuration.c:270-276/1758` |
| 参考项目 | `UniXNU-pong/userland/wifi-bootstrap.c`、`work/refs/hakios/userland/src/net_test.m` |
