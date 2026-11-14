# WiFi (WCN3990) 交接 — 2026-11-12 19:2x（v22）

> **唯一权威状态源**。v21 及更早为历史归档，冲突以本文为准。
> 判据口径：checkpoint 明文 + 用户可操作性，不是「0 panic」。

---

## 0. 一句话状态

驱动侧全部实机打通；剩余两件事（设置 App 的 Wi-Fi 详情卡死、域名解析不通）
**同源**，根因已用 Apple 源码 + 参考项目真机文档 + 本仓探针三重定位：
**configd 从未发布 `Plugin:InterfaceNamer` 的终止标记**。
今天新跑的 chain-604 A/B **证伪了 v21 §3 的归因和 HEAD `830c374` 的修复方向**。

```
firmware/WLFW/CE/HTC/WMI/HTT ✅  scan ✅  auth ✅  assoc ✅  VDEV_UP ✅
WPA2 四次握手/PTK/GTK ✅  PEER_AUTHORIZE ✅  真实 rx/tx 进 Darwin 栈 ✅
en0 link / KEV ✅  内核 DHCP 192.168.101.12/24 + 默认路由 ✅  0 PANIC ✅
── 设置详情 ⛔  DNS ⛔  状态栏 ⛔（同一个上游根因）
```

---

## 1. chain-604：ifaceuc 单变量 A/B（本轮新跑，13m39s）

镜像 `boot-exp-ifaceuc-base.img` / `boot-exp-ifaceuc.img` 全 65 MB 仅差 9 字节
（`ifaceuc=1` 写进 padding，995 B 等长），各 400 s。两组均 0 PANIC、
`join=5 / keys installed / DHCP bound / DNS published / SIOCSIFADDR` 完全一致。

| 判据 | base | ifaceuc=1 |
|---|---|---|
| `HakiosWLAN: interface user client bound` | 0 | **1**（类绑定成功） |
| `HakiosWLAN: interface user client open type=` | 0 | **0** |
| `configd_open where=denied class=IOEthernetInterface` | 1 | 1 |
| `*QUIET*` / `*COMPLETE*` / `*TIMEOUT*` | 0 | 0 |
| `stack_open` / `wifid sets` / `SCAN_REQ` / `ASSOCIATE` | 0 | 0 |

⇒ **`830c374`（v21 §3 认定的修复）被证伪**：UserClient 类挂上了，
但没有任何进程打开它，configd 的 open 拒绝没变，设置卡死的开关没动。
时序也不是主因：`where=denied` 在日志第 4049 行，晚于绑定（3067 行）。
该探针挂在 `is_io_service_open_extended` 的 **MAC 拒绝分支**
（`kIOReturnNotPermitted`），在 `IOService::newUserClient()` **之前**——
所以改 `IOUserClientClass` 属性根本到不了，这是 A/B 失败的结构性原因。

日志：`out/full-flash/logs/console-exp-ifaceuc{,-base}.cap`，
链输出 `work/boot-604-ab.out`。CI `43201094920` success（HEAD `830c374`）。

---

## 2. 真正的根因（三重证据，不是推断）

**① Apple 源码**：`work/refs/apple-oss/configd/SystemConfiguration.fproj/SCNetworkInterface.c:4911
__wait_for_IOKit_to_quiesce()` —— `dispatch_once` + `SCDynamicStoreNotifyWait`，
等 `Plugin:InterfaceNamer` 字典里出现 `kInterfaceNamerKey_Quiet`、
**iOS 分支的 `kInterfaceNamerKey_Complete` 或 `kInterfaceNamerKey_Timeout`** 之一。
Preferences / wifid / SpringBoard 都同步阻塞在 `SCNetworkInterfaceCopyAll()` 里。

**② 终止标记由谁发布**：`Plugins/InterfaceNamer/ifnamer.c`
- `updateStore()` (:628) 是唯一写 `Plugin:InterfaceNamer` 的地方；
- `timerCallback()` (:4312) 在 `S_stack_timeout` 后**无条件**
  `addTimestamp(*TIMEOUT*)` + `updateStore()`；
- `quietCallback()` (:4142) 在 `S_connect == MACH_PORT_NULL` 时直接 return，
  不发布标记；`S_connect` 来自 `stackCallback()` 的 `IOServiceOpen(IONetworkStack)`;
- `exec_InterfaceNamer()` (:4524) 从 `CFBundleGetInfoDictionary(bundle)` 读
  `WaitStackTimeout` / `WaitQuietTimeout`（默认 **300 / 240 秒**）。

**③ 本仓实机探针**：5 份 400 s 日志中上述标记全 0；`root_busy` / `busy_scan`
探针（挂在 `is_io_service_get_busy_state` MIG，只对 configd 打印）**0 次**，
`stack_open` 0 次 ⇒ InterfaceNamer 的 `setup_IOKit()` 从未执行。
`PICASSO_NETWORK stack load=0 found=1 resource_busy=1`：IOResources busyState 恒 1，
IOKit 永不 quiesce。

### 诊断陷阱（别再引用）
`PICASSO_NETWORK config stage=5 error=0 stack_s=5 quiet_s=5` 中的
`stack_s=5 quiet_s=5` 是 `printf` 的**硬编码字符串**（patch 第 367 行），
不是测量值；`config stage=5` 只说明 union mount 成功，不能证明 configd
读到了 `WaitStackTimeout/WaitQuietTimeout=5`。
另：`configd_open` 探针只打印 `proc_name == "configd"` 的 open，
Preferences / wifid 的尝试一直不可见。

---

## 3. 参考项目：真机验证过的解法（照抄，别重造）

`UniXNU-pong/migration/ipados18/services107-interface-namer-handshake.md`
（device-capture）与 `services108-native-audio-port-query.md`：
22H374 真机进程栈显示
`WiFiManagerScheduleWithQueue → WiFiDeviceManagerOpen → SCNetworkInterfaceCopyAll
→ dispatch_once wait`，另一线程 `CWFSCNetworkInterface →
__wait_for_IOKit_to_quiesce → SCDynamicStoreNotifyWait`，SpringBoard 同步等
`wifi_manager_open`。

**解法**：不修 configd，由独立 root 用户态进程主动发布真实终止标记。
`UniXNU-pong/userland/wifi-bootstrap.c` 是完整可抄实现：

| 函数 | 行 | 作用 |
|---|---|---|
| `publish_namer_result()` | 136 | `IOKitWaitQuiet(5s)` → 超时后 `SCDynamicStoreSetValue("Plugin:InterfaceNamer", {"*TIMEOUT*":true})`，**不谎报 QUIET** |
| `publish_interface()` | 101 | `State:/Network/Interface` 的 `Interfaces` 加 en0 |
| `configure_interface()` | 27 | `_SCNetworkInterfaceCreateWithIONetworkInterfaceObject` + `SCNetworkServiceCreate` + `EstablishDefaultConfiguration` + `SetEnabled` + `SCNetworkSetAddService` + `SCPreferencesCommitChanges/ApplyChanges` ⇒ **建原生 service，正是 DNS 缺的那一环** |

services107 真机结果：`kIOReturnTimeout`，TIMEOUT 通知发布成功，
wifid 收到系统上电请求、发现 1 个设备，SpringBoard 回到正常 RunLoop、
首帧出现；services108「用户确认系统 Wi-Fi 已修复」。

其余参考：`work/refs/hakios/userland/src/net_test.m`（MIG 20011
`ipconfig_set_service`）、`Trogdor-ipadOS/docs/plans/wifi.md:21`（DHCP 与 DNS 同一项）、
`work/trogdor/port/CoachzWifiNet.cpp:499`（InterfaceNamer 等待 IONetworkStack）、
`work/refs/hakios-picasso` 是伪造 DHCP 的假栈，**不可用**。
外网：Apple 官方 `IOKitWaitQuiet` 文档确认「等所有 IOService busyState 归零」；
本地 `work/refs/apple-oss/configd` 与 `work/refs/apple-oss/xnu` 比外网更权威
（v21 §5 已记「开源 configd 与设备 20H392 行为多次不符」，别逐行推闭源行为）。

**权限**：`configd.tproj/session.c:1039 checkWriteAccess()` —— 无 writeProtect 时
需 `isConfigd` / `hasRootAccess(euid==0)` / entitlement。pong 用 root launchd job。

---

## 4. 第一刀已落地，实机验证进行中（20:4x 更新）

**commit `ae3f56c`**（4 files +228/−2，已推送 fork；CI `43226773470`
fast=true）。不是计划，是已提交并编译通过的代码。

**为什么注入 configd**：Apple `configd.tproj/session.c:1039
checkWriteAccess()` 对无 writeProtect 的 key 放行条件是
`isConfigd(sessionPid == getpid())` / `hasRootAccess(callerEUID == 0)` /
entitlement。SpringBoard 是 mobile(501) 写不了 `Plugin:InterfaceNamer`；
configd 是 root 且自建 session 满足 `isConfigd`。本仓 4 份 cap 均有
`PICASSO_INJECT SpringBoard started with DYLD_INSERT_LIBRARIES`，证明该
内核 exec 阶段注入通道实机可用，复用它成本最低。

| 文件 | 改动 |
|---|---|
| `patches/picasso-wifiui-xnu.patch`（新） | 把 `picasso_springboard_inject_env()` 注入目标扩到 configd，**门控 `wifiui=1`**；0/缺省完全不改变现有行为 |
| `unixnu` | `LATE_OVERLAYS` 末尾加 `'wifiui'` ⇒ 63 个 z 前缀，字典序排在 `zzzzzz-picasso-springboard-inject.patch` 之后 |
| `package/picasso-springboard/picasso-springboard-probe.m` | constructor 按 `getprogname()` 分流：configd 只跑纯 C 后台线程（不碰 UIKit/objc），SpringBoard 原样。等 en0（≤200 s）→ 真实有界 `IOKitWaitQuiet(5 s)`（20H392 若无此符号则真实 `sleep(5)`）→ 成功才发 `*QUIET*`，否则只发 `*TIMEOUT*`，已有原生标记保留；再发布 `State:/Network/Interface` 的 en0 |
| `scripts/build-picasso-sb-inject.py` | 显式加 `-framework CoreFoundation`；SC/IOKit 符号全走 `dlopen/dlsym` |

**离线门禁全过**：`abi-check.py` 0 HARD；.m 括号/圆括号平衡 0；
`unixnu` AST 可解析；patch hunk `-6 +24` 自洽且 LF-only；
重建「springboard-inject 应用后」的 kern_exec.c 片段后
`git apply --check` rc=0；overlay 顺序已验证。CI 里
`Build the SpringBoard inject library` 步骤 **success**（新代码可编译可链接）。

**chain-605 判据（不是 0 PANIC）**：同镜像 995 B 等长、**全镜像仅 1 字节
不同**（`wifiui=0` c0 对照 / `wifiui=1` c1 实验），各 400 s。
1. `PICASSO_WIFIUI loaded in configd` —— dyld 是否接受注入；
2. `PICASSO_WIFIUI ... publish=1` —— `SCDynamicStoreSetValue` 是否放行；
3. `wifid sets` / `SCAN_REQ`、`State:/Network/Global/DNS`；
4. 用户实测设置 Wi-Fi 详情不再卡、Safari 输域名可解析。

**已识别风险与明确分支（不再逐点猜）**：
- configd 是 platform binary，dyld 可能丢弃 DYLD_；注入库链了
  UIKit/Metal，非 UI 守护进程加载可能失败 → 判据 ① 直接回答。
  若 ① 缺失：改**独立纯 C dylib**（需扩 trust module 支持第二个 CDHash）
  或 root launchd job（需重建 seed + `create_data_seed=true`，磁盘 16 GB 够）。
- `IOKitWaitQuiet` 在 20H392 可能不导出（Apple 标 iOS 18.4+）→
  日志 `quiet_api=no` 会显示，此时走真实 5 s 等待，不谎报。
- 只发标记可能仍不足以建 service → 若设置解卡但 DNS 仍无，
  下一刀照抄 pong `wifi-bootstrap.c:27-97` 的 `configure_interface()`
  （`SCPreferencesCommitChanges` / `ApplyChanges`）。

---

## 5. 铁律（沿用 v14~v21）

1. 一次只派一个 CI；`-f fast=true` 复用 seed；采集 ≥400 s（HIF t+168 s）。
2. 判据是 checkpoint 明文 + 用户可操作性，不是 0 panic、不是「进程还在」。
3. **kern_exec exec 路径禁加大数组/写文件**（chain-595/596 栈越界 panic）。
4. 改 .c/.cpp × 先 WSL 完整 XNU 树 `-fsyntax-only`，再 `abi-check.py` 0 HARD、
   括号/圆括号平衡、patch hunk 头重算 + `git apply --check`。
5. 995 B cmdline 已满：新门控复用 padding 或等长替换，不新增 token。
6. commit message 不写未实机验证的结论；归因被证伪立即写文档。
7. 用户 6 处未提交改动（5 个 `docs/evidence/*.log` 删除 +
   `diagnostic-charter.md`）一律不提交、不恢复。
8. 参考项目优先：`work/refs/`、`UniXNU-pong/`、`Trogdor-ipadOS/`
   都有同芯片/同 XNU 的现成实现，先 grep 再动手。

---

## 6. 关键路径

| 用途 | 路径 |
|---|---|
| 唯一权威状态 | 本文 v22 |
| 今日判决日志 | `console-exp-ifaceuc{,-base}.cap`、`work/boot-604-ab.out` |
| 卡死机制源码 | `work/refs/apple-oss/configd/SystemConfiguration.fproj/SCNetworkInterface.c:4911`、`Plugins/InterfaceNamer/ifnamer.c` |
| 可抄实现 | `UniXNU-pong/userland/wifi-bootstrap.c`、`migration/ipados18/services107-*.md` |
| 注入通道 | `patches/unixnu-source-kernel.patch`（`picasso_springboard_inject_env`）、`package/picasso-springboard/picasso-springboard-probe.m`、`scripts/build-picasso-sb-inject.py` |
| 上机 / 串口 | `SECS=400 bash work/boot-variants.sh <TAG>`；`python work/serial-cmd.py COM5 status 8` |
| CI | `gh workflow run compose-bootimg.yml -R tselongson/UniXNU-picasso --ref kd439-pool -f fast=true` |
| 项目日志 | `.workbuddy-ai/memory/2026-11-12.md` 末节 |
