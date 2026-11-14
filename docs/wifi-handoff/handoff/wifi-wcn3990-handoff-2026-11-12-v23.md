# WiFi (WCN3990) 交接 — 2026-11-12 21:5x（v23）

> **本文件是当前唯一权威状态源。** v22 及更早为历史归档，冲突以本文为准。
> 判据口径：checkpoint 明文 + 用户可操作性，不是「0 panic」。

---

## 0. 一句话状态

驱动侧全部实机打通；剩余两件事（设置 App 的 Wi-Fi 详情卡死、域名解析不通）
**同源**，根因已用 Apple 源码 + 参考项目真机文档 + 本仓探针三重定位：
**configd 从未发布 `Plugin:InterfaceNamer` 的终止标记**。
今天新写的第一刀（从 configd 主动发布真实 `*TIMEOUT*`）**代码已提交、CI 编译
通过，但 CI 因 seed 身份不匹配而失败，尚未上机**。

```
firmware/WLFW/CE/HTC/WMI/HTT ✅  scan ✅  auth ✅  assoc ✅  VDEV_UP ✅
WPA2 四次握手/PTK/GTK ✅  PEER_AUTHORIZE ✅  真实 rx/tx 进 Darwin 栈 ✅
en0 link / KEV ✅  内核 DHCP 192.168.101.12/24 + 默认路由 ✅  0 PANIC ✅
── 设置详情 ⛔  DNS ⛔  状态栏 ⛔（同一个上游根因）
── 第一刀 ae3f56c：代码就绪，CI 因 seed recipe 失配失败，未上机
```

---

## 1. 今天的判决

### 1.1 chain-604 证伪了 v21 §3 的修复方向（实机 A/B）

`830c374`（v21 认定「设置卡死 = configd 打开接口服务被 entitlement 拒绝」，
修法是给接口服务挂 UserClient）此前 CI success 但**从未上机**。今天补跑
chain-604 单变量 A/B：镜像全 65 MB 仅差 9 字节（`ifaceuc=1` 写进 padding，
995 B 等长），各 400 s。

| 判据 | 对照 | `ifaceuc=1` |
|---|---|---|
| `HakiosWLAN: interface user client bound` | 0 | **1**（类确实挂上了） |
| `HakiosWLAN: interface user client open type=` | 0 | **0** |
| `configd_open where=denied class=IOEthernetInterface` | 1 | 1（拒绝没变） |
| `*QUIET*` / `*COMPLETE*` / `*TIMEOUT*` | 0 | 0 |
| `stack_open` / `wifid sets` / `ASSOCIATE` | 0 | 0 |

两组都 0 PANIC、`join=5` / `keys installed` / `DHCP bound` / `DNS published` 一致。
⇒ **`830c374` 无效**。时序也不是主因：`where=denied` 在日志第 4049 行，
晚于绑定（3067 行）。结构性原因：该探针挂在 `is_io_service_open_extended`
的 **MAC 拒绝分支**（`kIOReturnNotPermitted`），位于
`IOService::newUserClient()` **之前**，所以改 `IOUserClientClass` 属性
根本到不了。

### 1.2 真正根因（三重独立证据）

**① Apple 源码**：`work/refs/apple-oss/configd/SystemConfiguration.fproj/SCNetworkInterface.c:4911
__wait_for_IOKit_to_quiesce()` —— `dispatch_once` + `SCDynamicStoreNotifyWait`，
等 `Plugin:InterfaceNamer` 字典出现 `kInterfaceNamerKey_Quiet`、
**iOS 分支的 `kInterfaceNamerKey_Complete` 或 `kInterfaceNamerKey_Timeout`**。
Preferences / wifid / SpringBoard 全部同步阻塞在 `SCNetworkInterfaceCopyAll()`。
configd 不建 service ⇒ DNS 也不发布。

**② 谁发布**：`Plugins/InterfaceNamer/ifnamer.c`
- `updateStore()` (:628) 是唯一写 `Plugin:InterfaceNamer` 的地方；
- `timerCallback()` (:4312) 在 `S_stack_timeout` 后**无条件**
  `addTimestamp(*TIMEOUT*)` + `updateStore()`；
- `quietCallback()` (:4142) 在 `S_connect == MACH_PORT_NULL` 时直接 return；
  `S_connect` 来自 `stackCallback()` 的 `IOServiceOpen(IONetworkStack)`；
- `exec_InterfaceNamer()` (:4524) 从 `CFBundleGetInfoDictionary(bundle)` 读
  `WaitStackTimeout` / `WaitQuietTimeout`（默认 **300 / 240 秒**）。

**③ 本仓实机探针**：5 份 400 s 日志中上述标记全 0；`root_busy` / `busy_scan`
探针（挂在 `is_io_service_get_busy_state` MIG，只对 configd 打印）**0 次**，
`stack_open` 0 次 ⇒ InterfaceNamer 的 `setup_IOKit()` 从未执行。
`PICASSO_NETWORK stack load=0 found=1 resource_busy=1`：IOResources busyState 恒 1，
IOKit 永不 quiesce。

### 1.3 两个一直在误导判断的诊断陷阱（务必记住）

1. `PICASSO_NETWORK config stage=5 error=0 stack_s=5 quiet_s=5` 里的
   `stack_s=5 quiet_s=5` 是 `printf` 的**硬编码字符串**
   （`patches/picasso-network-timeout-xnu.patch:367`），**不是测量值**；
   `config stage=5` 只说明 union mount 成功，不能证明 configd 读到了
   `WaitStackTimeout/WaitQuietTimeout=5`。
2. `configd_open` 探针只打印 `proc_name == "configd"` 的 open，
   Preferences / wifid 的尝试一直不可见。

---

## 2. 参考项目：真机验证过的解法（照抄，别重造）

`UniXNU-pong/migration/ipados18/services107-interface-namer-handshake.md`
（device-capture）+ `services108-native-audio-port-query.md`：22H374 真机进程栈
`WiFiManagerScheduleWithQueue → WiFiDeviceManagerOpen → SCNetworkInterfaceCopyAll
→ dispatch_once wait`，另一线程 `__wait_for_IOKit_to_quiesce →
SCDynamicStoreNotifyWait`；SpringBoard 同步等 `wifi_manager_open`。
解法是**独立 root SC client** 发布真实终止标记；
services107 结果：`kIOReturnTimeout`、TIMEOUT 通知发布成功、wifid 收到系统
上电请求、SpringBoard 回到正常 RunLoop、首帧出现；
services108 记「用户确认系统 Wi-Fi 已修复」。

`UniXNU-pong/userland/wifi-bootstrap.c` 是完整可抄实现：

| 函数 | 行 | 作用 |
|---|---|---|
| `publish_namer_result()` | 136 | `IOKitWaitQuiet(5s)` → 超时后 `SCDynamicStoreSetValue("Plugin:InterfaceNamer", {"*TIMEOUT*":true})`，**不谎报 QUIET** |
| `publish_interface()` | 101 | `State:/Network/Interface` 的 `Interfaces` 加 en0 |
| `configure_interface()` | 27 | `_SCNetworkInterfaceCreateWithIONetworkInterfaceObject` + `SCNetworkServiceCreate` + `EstablishDefaultConfiguration` + `SetEnabled` + `SCNetworkSetAddService` + `SCPreferencesCommitChanges/ApplyChanges` ⇒ **建原生 service**（DNS 缺的那一环） |

**权限依据**（决定实现形态的关键）：`configd.tproj/session.c:1039
checkWriteAccess()` 对无 writeProtect 的 key 放行条件 =
`isConfigd(sessionPid == getpid())` / `hasRootAccess(callerEUID == 0)` /
entitlement。SpringBoard 是 mobile(501) **写不了** ⇒ 代码必须跑在
configd（root，且进程内自建 session 满足 `isConfigd`）。
其余参考：`work/refs/hakios/userland/src/net_test.m`（MIG 20011
`ipconfig_set_service`）、`Trogdor-ipadOS/docs/plans/wifi.md:21`（DHCP 与
DNS 同一项）、`work/refs/hakios-picasso` 是伪造 DHCP 的假栈**不可用**。

---

## 3. 第一刀（已提交、CI 编译通过、但 CI 因 seed 失配失败，未上机）

### 3.1 代码（commit `ae3f56c`，4 files +228/−2，已推送 fork）

复用本仓已实机验证的内核 exec 阶段注入通道（4 份 cap 均有
`PICASSO_INJECT SpringBoard started with DYLD_INSERT_LIBRARIES`），
把注入目标扩到 configd，**门控 `wifiui=1`**（0/缺省完全不改变现有行为）。

| 文件 | 改动 |
|---|---|
| `patches/picasso-wifiui-xnu.patch`（新） | 扩 `picasso_springboard_inject_env()` 的注入目标到 configd |
| `unixnu` | `LATE_OVERLAYS` 末尾加 `'wifiui'` ⇒ 63 个 z 前缀，字典序排在 `zzzzzz-picasso-springboard-inject.patch` **之后**，能匹配它注入的函数体 |
| `package/picasso-springboard/picasso-springboard-probe.m` | constructor 按 `getprogname()` 分流：configd 只跑**纯 C**后台线程（不碰 UIKit/objc），SpringBoard 状态栏路径原样。等 en0（≤200 s，HIF t+168 s）→ 真实有界 `IOKitWaitQuiet(5 s)`（20H392 若无该符号则真实 `sleep(5)`）→ **成功才发 `*QUIET*`，否则只发 `*TIMEOUT*`**（不谎报），已有原生终止标记一律保留；再发布 `State:/Network/Interface` 的 en0。SC/IOKit 符号全走 `dlopen/dlsym` |
| `scripts/build-picasso-sb-inject.py` | 显式加 `-framework CoreFoundation` |

### 3.2 离线门禁（全过）

`abi-check.py` **0 HARD**；.m 括号/圆括号平衡 0；`unixnu` AST 可解析；
patch hunk 声明 `-6 +24` 与实际一致、LF-only；
**重建「springboard-inject patch 应用后」的 kern_exec.c 片段后
`git apply --check` rc=0**；overlay 字典序已验证。
CI 里 `Build the SpringBoard inject library` 步骤 **success**（新代码可编译可链接）。

### 3.3 ⚠ CI 失败根因（不是代码问题，但必须知道）

CI `43226773470` **failure**，失败步骤
`Reuse persistent Data seed (new seed requires explicit input)`，
日志末尾：

```
ValueError: No compatible iOS16 Compose input bundle is retained.
Restore the saved linux-inputs or explicitly create a new seed;
existing persistent Data requires its exact seed.
```

**因果链**（`scripts/picasso_linux_inputs.py`）：

1. `recipe(tree)` (:38) 的 sha256 覆盖
   `package/picasso-springboard/picasso-springboard-probe.m`、
   `scripts/build-picasso-sb-inject.py` 等文件的**字节**；
2. 我改了这两个文件 ⇒ **recipe 哈希变了**；
3. `reuse()` (:238) 要求 `reuse.json` 的 `recipe_sha256 == recipe(tree)`，
   `acquire()` (:184) 从历史成功 run 里找**同一 recipe** 的 seed artifact；
4. 旧 seed（`43201094920` 的 `picasso-data-seed-601`）是**旧 recipe** ⇒ 失配。

**这是项目既有约束，不是 WiFi 逻辑缺陷**（11-11 日志已记过同款坑）。
凡触及 `picasso-springboard-probe.m` / `build-picasso-sb-inject.py` /
`picasso-astc-guard.m` / `picasso-statusbar-metrics.m` /
`picasso_ui_trust.py` / `prepare-picasso-ram-data.py` / `board.toml` 的
提交，都会让持久 seed 复用失效。

### 3.4 解法：两阶段（下一棒从这里开始）

**阶段 A（一次 CI，约 15 分钟）**：派
```
gh workflow run compose-bootimg.yml -R tselongson/UniXNU-picasso \
   --ref kd439-pool -f fast=true -f create_data_seed=true
```
`create_data_seed=true` 让 workflow 带 `--allow-new-seed` 显式新建
（`reuse-optional` 只在「无兼容 seed」且显式授权时新建，见
`picasso_linux_inputs.py:289-297`；IO/provenance 失败仍 fatal）。
⚠ **注意**：新 seed = **新 Data 身份**，设备上已刷的旧 Data 卷将无法
挂载 ⇒ 需要 `build_userdata=true` 出 userdata 镜像并重新刷 Data
（磁盘需 ~12 GB 峰值；C 盘现 16 GB 可用）。
**这一步会改变设备持久 Data 身份，属不可逆操作，执行前需用户确认。**

**阶段 B**：阶段 A 成功后，新 recipe 有了匹配的 seed artifact，
之后所有 `-f fast=true`（不带 `create_data_seed`）都能复用 ⇒ 回到
「一次只派一个 CI」的常态。

**若不接受换 Data 身份**：把第一刀的注入库代码从
`picasso-springboard-probe.m` **移出**，改为独立的新文件
（例如 `package/picasso-springboard/picasso-wifiui.m`）+ 独立 dylib，
但 `recipe()` 覆盖 `picasso-springboard-probe.m` 与
`build-picasso-sb-inject.py` 的字节，**新增文件也在 recipe 覆盖范围内
吗** ⇒ 不覆盖（recipe 列表是显式枚举，见 `picasso_linux_inputs.py:44-50`），
**独立新文件 + 独立 dylib 就不改 recipe**，可保住旧 seed。
代价：trust module 要支持第二个 CDHash（`picasso_ui_trust.py` 的
`make_bundle`/`verify_bundle` 目前硬编码 1 个 owned module），
且需扩展 `picasso_ui_library_mount` 与 `picasso_dyld_inject_target`。
**这是更安全但工作量更大的一条路。**

### 3.5 已识别风险与明确分支（不再逐点猜）

1. **configd 是 platform binary**，dyld 可能丢弃 `DYLD_INSERT_LIBRARIES`；
   注入库又链了 UIKit/Metal，非 UI 守护进程加载可能失败。
   判据 = 日志有无 `PICASSO_WIFIUI loaded in configd`。
   → 若无：改**独立纯 C dylib**（不链 UI 框架）或 root launchd job。
2. **`IOKitWaitQuiet` 在 20H392 可能不导出**（Apple 文档标 iOS 18.4+）→
   日志 `quiet_api=no` 会显示，此时走真实 5 s 等待，不谎报。
3. **只发终止标记可能仍不足以建 service** → 若设置解卡但 DNS 仍无，
   下一刀照抄 pong `wifi-bootstrap.c:27-97` 的 `configure_interface()`
   （`SCPreferencesCommitChanges` / `ApplyChanges`）。

---

## 4. chain-605 验证链（已写好，等阶段 A 成功后直接用）

`work/chain-605.sh`：CI `43226773470` → 下载 → `make-545.py` 打槽 →
生成 `wifiui=0`(c0 对照) / `wifiui=1`(c1 实验) 两镜像（995 B 等长、
**全镜像仅 1 字节不同**；其余固定 `scanfix=127 initfix=3 joinsel=4 wpa=1
dpfix=5 authz=0 wifiauto=1`）→ 各 400 s → 判读。
⚠ 链内 `RUN=43226773470` 需改成本轮成功的 CI 号。
⚠ 脚本会先删旧 c0/c1 产物，避免拿陈旧 cap 误判。

判据（**不是 0 PANIC**）：
`PICASSO_WIFIUI loaded in configd` → `publish=1` → `wifid sets`/`SCAN_REQ` →
`State:/Network/Global/DNS` → 用户实测「设置 > Wi-Fi 详情」不卡、
Safari 输域名可解析。

---

## 5. 已通 / 未通清单

**✅ 实机闭环**：固件/WLFW/CE/HTC/WMI/HTT、扫描、auth/assoc、VDEV_UP、
WPA2 四次握手、PTK/GTK、PEER_AUTHORIZE、真实 rx/tx 进 Darwin 栈、
en0 link、KEV_DL_LINK_ON、内核 DHCP（`192.168.101.12/24` + 默认路由）、
装地址后系统自发 ARP 走真实数据面被 ACK、开机进桌面可操作、0 PANIC、
wifid 已放行（`26d995a`，launchd 形式按 key 幂等改写）、`netsvc=1`
预置 SCPreferences plist（1786 B 写入成功）。

**❌ 未通**：
1. 设置 App 点 Wi-Fi 详情卡死 —— 与 #2 同源（InterfaceNamer 终止标记缺失）；
2. 域名解析（configd 未建 service ⇒ 未发布 DNS）；
3. 状态栏 / SystemConfiguration 不显示已连接（同源）；
4. 凭据持久化（`wpass=` 仍是 boot-arg 明文）；
5. Safari HTTPS 端到端实测（从未在「关联稳定 + IP 对照」下做过）；
6. 5G MGMT_TX `status=3`（非阻塞）。

---

## 6. 铁律（沿用 v14~v22）

1. 一次只派一个 CI；`-f fast=true` 复用 seed；采集 ≥400 s（HIF t+168 s）。
2. 判据是 checkpoint 明文 + 用户可操作性，不是 0 panic、不是「进程还在」。
3. **kern_exec exec 路径禁加大数组/写文件**（chain-595/596 栈越界 panic）。
4. 改 .c/.cpp/.m 先做语法与括号校验，再 `abi-check.py` 0 HARD；
   patch hunk 头重算 + `git apply --check`；patch 一律 LF。
5. 995 B cmdline 已满：新门控复用 padding 或等长替换，不新增 token。
6. commit message 不写未实机验证的结论；归因被证伪立即写文档。
7. 用户 6 处未提交改动（5 个 `docs/evidence/*.log` 删除 +
   `diagnostic-charter.md`）一律不提交、不恢复。
8. 参考项目优先：`work/refs/`、`UniXNU-pong/`、`Trogdor-ipadOS/` 都有
   同芯片/同 XNU 的现成实现，先 grep 再动手；查资料方法见
   `docs/research-playbook.md`。
9. **改 recipe 覆盖的文件 ⇒ 持久 seed 失效**（见 §3.3），要么两阶段重建
   seed（换 Data 身份），要么把改动移出 recipe 覆盖范围。

---

## 7. 关键路径

| 用途 | 路径 |
|---|---|
| 唯一权威状态 | 本文 v23 |
| 第一刀代码 | `patches/picasso-wifiui-xnu.patch`、`unixnu`（LATE_OVERLAYS）、`package/picasso-springboard/picasso-springboard-probe.m`、`scripts/build-picasso-sb-inject.py` |
| 验证链 | `work/chain-605.sh`（需改 `RUN=`） |
| 卡死机制源码 | `work/refs/apple-oss/configd/SystemConfiguration.fproj/SCNetworkInterface.c:4911`、`Plugins/InterfaceNamer/ifnamer.c`、`configd.tproj/session.c:1039` |
| 可抄实现 | `UniXNU-pong/userland/wifi-bootstrap.c`、`migration/ipados18/services107-*.md` |
| seed 复用机制 | `scripts/picasso_linux_inputs.py`（`recipe()` :38 / `acquire()` :184 / `reuse()` :238） |
| 判决日志 | `console-exp-ifaceuc{,-base}.cap`（chain-604）、`console-exp-c0/c1.cap`（chain-605，待产出） |
| 上机 / 串口 | `SECS=400 bash work/boot-variants.sh <TAG>`；`python work/serial-cmd.py COM5 status 8` |
| CI | `gh workflow run compose-bootimg.yml -R tselongson/UniXNU-picasso --ref kd439-pool -f fast=true` |
| 项目日志 | `.workbuddy-ai/memory/2026-11-12.md` 末两节 |
| 查资料方法 | `docs/research-playbook.md` |

---

## 8. 其他变更（今日已完成）

- **基线**（用户指定）：`wifi-wcn3990/manifest.json` 的 `baseline` →
  `YinCQL/UniXNU-picasso @ feat/fix-v1 @ d627aa6`，含完整 URL
  （`git ls-remote yincql` 验证 tip）。commit `817b715`，**未推送**。
  纯参考性变更：该分支 `wifi-wcn3990` 路径数为 0，不动构建树、CI 输入与
  `kd439-pool`；`integration.patch_base` 保留 `e790d5d`（四个 integration/
  补丁是针对那棵历史树写的，rebase 是另一件事）。
  ⚠ 上游 `feat/fix-v1` 的 `scripts/picasso_linux_inputs.py` 与本地**不同**
  （sha `f764963b…` vs 本地 `af42c128…`），故**它的 seed 不能直接复用**。
- `docs/research-playbook.md`（新）：如何查资料与参考项目，取自 11-09～11-12
  真实定位过程（本地资料地图、外网短查询、参考项目分级、证据分级、
  诊断自身 bug 的陷阱、何时停止查开始做）。
- `README.md` 权威状态指针 v17 → v22（v17 状态段保留为历史记录）。
