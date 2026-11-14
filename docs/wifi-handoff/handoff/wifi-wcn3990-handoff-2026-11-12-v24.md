# WiFi (WCN3990) 交接 — 2026-11-12 22:5x（v24）

> **本文件是当前唯一权威状态源。** v23 及更早为历史归档，冲突以本文为准。
> 判据口径：checkpoint 明文 + 用户可操作性，不是「0 panic」。
> 本轮两个 commit（`346e3d5` + `ea2c30d`）已推送。
> CI `43241917214` **success**（编译、新 Data seed、userdata 全部产出）；
> 最终镜像 CI `43249934650` **success**（`picasso-bootimg-604` / `seed-604`）。
> c0/c1 已在最终镜像上离线验证：995 B、单变量仅 1 字节不同。
> **尚未上机**；刷 userdata 属不可逆操作，执行前须用户确认。

---

## 0. 一句话状态

驱动侧全部实机打通（含内核 DHCP）。剩余两件事（设置 App Wi-Fi 详情卡死、
域名解析不通）同源：**configd 从未发布 `Plugin:InterfaceNamer` 终止标记、也从未为 en0 建原生 service**。
v23 的第一刀只做了 pong 三步里的两步；本轮按参考项目补齐第三步，修正两处会导致
判据失真/谎报的代码问题，并修好了一条**根本跑不起来的验证链**。

```
firmware/WLFW/CE/HTC/WMI/HTT ✅  scan ✅  auth ✅  assoc ✅  VDEV_UP ✅
WPA2 四次握手/PTK/GTK ✅  PEER_AUTHORIZE ✅  真实 rx/tx 进 Darwin 栈 ✅
en0 link / KEV ✅  内核 DHCP 192.168.101.12/24 + 默认路由 ✅  0 PANIC ✅
── 设置详情 ⛔  DNS ⛔  状态栏 ⛔（同一上游根因：无终止标记 + 无 service）
── 本轮 346e3d5（三步一次做全）+ ea2c30d（AMFI 放宽，否则注入必被 dyld 丢弃）
── CI 43241917214 success（注入库真机 SDK 编译通过、新 seed/userdata 已产出）
── CI 43249934650 success（复用 seed-603，bootimg-604 为最终镜像）；未上机
── c0/c1 单变量镜像已在 bootimg-604 上离线验证（995 B / 仅 1 字节差）
```

---

## 1. 本轮通读范围（用户要求「亲自读完全部项目日志」）

- `.workbuddy/memory/` **14 份全文逐份读完**（10-26、10-29、10-31、11-02…11-12）；
  `.workbuddy-ai/memory/` 同 14 份经 `cmp` 逐字节验证为镜像，内容一致。
- `handoff/wifi-wcn3990-handoff-2026-11-12-v23.md`（权威）+ `docs/research-playbook.md`
  + `README.md` + `handoff/2026-11-10-project-history-digest.md`。
- 一手源码：`work/refs/apple-oss/configd` 的 `SCNetworkInterface.c:4911`、
  `Plugins/InterfaceNamer/ifnamer.c`（`timerCallback`/`quietCallback`/`setup_IOKit`/
  `exec_InterfaceNamer`）、`configd.tproj/session.c:1039 checkWriteAccess`、
  `Plugins/common/plugin_shared.h:47-49`（key 字面量）。
- 参考项目：`UniXNU-pong/userland/wifi-bootstrap.c`（逐行）、
  `migration/ipados18/services107-interface-namer-handshake.md`（真机进程栈）、
  本仓 `scripts/picasso_linux_inputs.py` / `prepare-picasso-ram-data.py` /
  `picasso_ui_trust.py` / `.github/workflows/compose-bootimg.yml`。
- 外网核对（按 research-playbook §2 短查询）：Apple `IOKitWaitQuiet` 官方文档、
  configd InterfaceNamer 插件机制、`DYLD_INSERT_LIBRARIES` 在 platform binary /
  `__RESTRICT` / setuid 下被 prune 的公开资料。

---

## 2. 精准定位：对 v23 的四处修正（都有源码或实机证据，不是推测）

### 2.1 v23 解法 B「独立新文件 + 独立 dylib 可保住旧 seed」**不成立**

`recipe()`（`picasso_linux_inputs.py:38`）确实是显式枚举，新增源码文件不在其中。
但 seed 对注入库的锁定不只在 recipe：

| 约束 | 位置 | 含义 |
|---|---|---|
| `manifest.files['libpicasso-sb.dylib']` 的 sha256/bytes | `validate():68-80` | seed bundle 固化 dylib 字节 |
| `audit['ui_trust']['owned_entries'] == 1` | `validate():107-111` | trust module 只认 1 个自有模块 |
| `settings['injected_library'] == {'path':..., 'sha256': library}` | `validate():125` | macOS Data seed 绑定该 dylib |
| `digest(board.payload.owned_trust_library) == digest(seed dylib)` | `stage_setup():267-268` | 复用 seed 时**构建产物被旧 dylib 覆盖** |
| `reuse():254` | `picasso_linux_inputs.py` | `shutil.copyfile(seed dylib → out/picasso/libpicasso-sb.dylib)` |

⇒ **只要复用旧 seed，boot.img 里的注入库就是旧字节**；改 probe.m、改 build 脚本、
新增第二个 dylib 都会落到同一个校验链上。而放宽 trust module /
`check-picasso-data-artifacts.py` 同样在 `recipe()` 列表内，改它们照样改 recipe。
**结论：任何改变注入库行为的方案都必须新建 Data seed。**

另核实 `prepare-picasso-ram-data.py:485` 的
`identity = canonical_uuid(info['VolumeUUID'])` 来自 `hdiutil create` 随机生成的
APFS 卷 UUID ⇒ **新 seed = 新 Data 身份**，v23 §3.4 的阶段 A 判断正确，
设备旧 Data 需要配对的 userdata 才能挂载（11-11 的 m1 panic 已实机证实）。

**权限依据也已逐行核实**（第一刀注入 configd 的前提）：
`session.c:1048 checkWriteAccess()` 只对 `kSCDAccessControls_writeProtect` 的 key
要求 entitlement，而 `_SCD.c:234` 只把该控制设在两个 `SCDTEST_WRITE_PROTECT*_KEY`
测试 key 上 ⇒ `Plugin:InterfaceNamer` / `State:/Network/Interface` 均无写保护；
放行条件是 `isConfigd(sessionPid == getpid())`（`session.c:763`）或
`hasRootAccess(callerEUID == 0)`（`session.c:771`）。configd 进程内注入同时满足
两者，本机 configd pid=34、`/usr/libexec/configd`、root 已在 cap 中出现。

### 2.2 第一刀只做了 pong 三步里的两步，缺 DNS 的那一环

`UniXNU-pong/userland/wifi-bootstrap.c` 的 `--configure-only` 顺序是
`publish_interface()` → `publish_namer_result()` → **`configure_interface()`**：
`_SCNetworkInterfaceCreateWithIONetworkInterfaceObject` + `SCNetworkServiceCreate`
+ `EstablishDefaultConfiguration` + `SetEnabled` + `SCNetworkSetAddService`
+ `SCPreferencesCommitChanges/ApplyChanges`。services108 记「用户确认系统 Wi-Fi 已修复」。
v23 §3.5 把它列为「若设置解卡但 DNS 仍无，下一刀」——本轮**一次做完**，
避免 11-12 18:2x 用户已明确批评的「单点补丁 → 10 分钟 CI → 7 分钟上机」串行循环。

顺序不可换：`SCPreferencesApplyChanges` 自己会等 InterfaceNamer
（pong 源码注释），所以必须在终止标记之后。

**板级差异（不是猜）**：本内核没有预链接 `IO80211Interface`
（`hakios.wlan.cpp:100`，接口服务是 `IOEthernetInterface`），pong 的
`IOObjectConformsTo(entry,"IO80211Interface")` 硬检查会直接失败。
改用 Apple 自己用的两个属性：BSD name == en0 +
`SCNetworkInterfaceGetInterfaceType()`，后者由 `SCNetworkInterface.c:1790`
根据 **controller** 的 `IO80211Controller` 判定（我们的 stub 在
`hakios.wlan.cpp:304` 已发布），故接口类回退不影响 Wi-Fi 类型判定。

### 2.3 `IOKitWaitQuiet` 在本机型不可用，且原实现会「谎报」

Apple 官方文档：`IOKitWaitQuiet` 是 **iOS 18.4+ / iPadOS 18.4+**；
本设备是 **20H392（iOS 16.7.16）**，pong 的 22H374 才用得上。
第一刀已用 `sleep(5)` 兜底，但原代码对 `IOKitWaitQuiet` 的**任何**非 0 返回
都发布 `*TIMEOUT*`——若返回 `kIOReturnNotPrivileged` 之类，等于伪造一次
「已完成等待」。按 pong services107 的口径，只有 `KERN_SUCCESS(0)` 或
`kIOReturnTimeout(0xe00002e6)` 才是诚实的终止结果。

补充源码事实（写给下一棒，别再重复推）：`__wait_for_IOKit_to_quiesce()`
在 `SCNetworkInterface.c:4946-4950` 对 `*TIMEOUT*` 的接受**不分平台**
（`#if TARGET_OS_IPHONE` 只包了 `*COMPLETE*`）；`ifnamer.c:4312 timerCallback()`
在超时后无条件 `addTimestamp(*TIMEOUT*) + updateStore()`，即使 `S_connect==NULL`。
也就是说，原生链理论上 300 s（`WAIT_STACK_TIMEOUT_DEFAULT`）后会自动解卡。
但本仓 5 份 400 s 日志中 `root_busy`/`busy_scan` 探针 0 次 ⇒
`setup_IOKit()` 从未走到装 timer 那步（`IONotificationPortCreate` /
`IORegistryEntryFromPath` / `IOServiceAddInterestNotification` 之一失败），
与「Preferences 一直卡」一致。**这是待取证项，不是结论**；用户已在 11-12
明确要求不要再逆向闭源 configd 状态机，故本轮按 pong 的旁路做。

### 2.4 `work/chain-605.sh` 原本根本跑不起来（两个确定性 bug）

用现成 CI 产物 `out/run-43201094920/picasso-bootimg-601/boot/boot.img` 离线复算：

| 问题 | 证据 | 后果 |
|---|---|---|
| COMMON 16 个实验 token + `make-545.py` 打槽 | `mk-args.py` 实测 `does not fit: 1125 > 995` | 链在 exit 13 挂掉 |
| COMMON 与 python 断言要求 `wifiauto=1`，但两者都没装它 | 生成的 args 里 `wifiauto=1: False` | 即便不超长也不会关联，`join=0`，判据全废 |

修正为 **chain-599 已实机闭环的日用参数路径**：
CI 原始 boot.img → `work/mk-autojoin-args.py`（剥 4 个 `offline-hid-*`
装饰 token，装 `wifiauto=1 wifissid=… wpass=…`，保留全部显示开关）→
`mk-args.py` 只改 `wifiui=N`。离线验证：995 B 等长、c0/c1 **全镜像仅 1 字节不同**
（offset 60221570）、`wifiauto=1` / `offline-legacy-validate=1` / `wifi=1` 全在。
`scanfix/initfix/rxfix/dpfix/netfix/dhcp` 均已由 `a2b20a3`/`54b5d8d`/`0576f1d`
转正为编译默认，不需要再显式传。

### 2.5 ★ 最致命的一处：configd 的 `DYLD_INSERT_LIBRARIES` **必然**被 dyld 丢弃

这不是 v23 §3.5 里「platform binary 可能拒绝注入」的猜测，而是本仓内核代码
的确定性事实：

```
security/mac_base.c: picasso_dyld_path_policy()
    if (error || call != 90 || strcmp(policy, "AMFI") ||
        !picasso_dyld_inject_target(p)) return;      // 不放宽 AMFI_DYLD_OUTPUT_ALLOW_PATH_VARS
bsd/kern/kern_exec.c: picasso_dyld_inject_target()
    if (!strcmp(p->p_name, "SpringBoard")) return true;
    ... (picasso_inject_apps=1 时再接受 .app 路径)
```

`ae3f56c` 只把 configd 加进了 `picasso_springboard_inject_env()`（写环境变量），
**没有**把 configd 加进 `picasso_dyld_inject_target()`（AMFI 放宽）。
于是 20H392 的 dyld 在 configd 启动时会 prune 掉 `DYLD_INSERT_LIBRARIES`，
注入库的 constructor 根本不执行 ⇒ `PICASSO_WIFIUI loaded in configd` 恒 0，
前两步/第三步写得再对也不会跑。本仓 cap 里的
`PICASSO_INJECT dyld policy pid=26 old=69 new=6b` 只出现在 SpringBoard(pid 26)，
与此完全一致。外网核实的 setuid / `__RESTRICT` / hardened runtime 会丢弃
DYLD_* 是同一层机制；这里更直接——内核自己按进程名做了白名单。

**修复（`ea2c30d`）**：`patches/picasso-wifiui-xnu.patch` 增加第二个 hunk，
在 `picasso_dyld_inject_target()` 里接受 `p_name == "configd"` 且 `wifiui=1`，
让同一个 boot-arg 同时控制「注入环境变量」与「AMFI 放宽」；`wifiui=0` 时
所有既有路径逐字节不变。

hunk 行号不是猜的：用 `work/tmp-rebuild-kern-exec.py` 从
`unixnu-source-kernel.patch` 内嵌的 `zzzzzz-picasso-springboard-inject.patch`
重建成「应用后」的 `kern_exec.c`，量出两个函数之间固定偏移 28 行，
加上第一个 hunk 的 +18 行增长 ⇒ `@@ -5851,6 +5869,12 @@`。
验证：`git apply --check --verbose` 在重建文件上 **rc=0，两个 hunk 都成功**；
hunk 计数 6/24 与 6/12 精确；LF-only。`patches/*.patch` 不在 `recipe()`
列表里 ⇒ 这个修复**不会**让刚生成的新 seed 失配。

---

## 3. 本轮代码改动

**commit `346e3d5`**（分支 `kd439-pool`，已推 fork，1 file +231/−10）：
`package/picasso-springboard/picasso-springboard-probe.m`

1. 新增 `wifiui_configure_service()`：按 pong `wifi-bootstrap.c:27-97` 逐函数照抄，
   dlsym 解析 25 个 SC/IOKit 符号，从**真实** en0 IORegistry 对象建 service；
   幂等（已有 en0 service 就跳过）；每一步打到 `/dev/console`
   （`service entry= / en0_named= / type= / applying / defaults=…apply= / result=`）。
2. `IOKitWaitQuiet` 错误分支收敛：新增 `truthful` 门控，非 success/timeout 不发布
   任何终止标记，也不跑 configure（`service skipped (no honest terminal marker)`）。
3. configd 路径去掉 `@autoreleasepool`，纯 C，不碰 objc runtime。
4. 保持 `wifiui=1` 单变量门控不变（`wifiui=0` 完全 inert），
   `unixnu` LATE_OVERLAYS 未改。

**commit `ea2c30d`**（1 file +13）：`patches/picasso-wifiui-xnu.patch` 增加
第二个 hunk，把 configd 加入 `picasso_dyld_inject_target()`（详见 §2.5）。
这是「第一刀能不能加载」的决定性修复；没有它，`346e3d5` 的用户态逻辑
永远不会执行。

**离线门禁（全过）**：`work/abi-check.py` 0 HARD；括号/圆括号/方括号平衡 0、
负深度 0；`clang -fsyntax-only -x c -Wall -Wextra -Werror` 配 CoreFoundation/IOKit
stub 头 **EXIT 0 / 0 errors**（脚本 `work/tmp-wifiui-syntax.py`，WSL clang 18）；
overlay patch 两个 hunk 用重建的 `kern_exec.c` 做 `git apply --check` **rc=0**，
hunk 计数精确、LF-only（脚本 `work/tmp-rebuild-kern-exec.py`）；两个提交
仓库 blob 均 CRLF=0。用户 6 处未提交改动原样保留，只 add 自己的文件。

**`work/chain-605.sh`**（工作区脚本，不在 git 仓库内）已按 §2.4 重写，
`RUN` 改为命令行参数 `bash work/chain-605.sh <成功 CI run id>`，
判读增加 `truthful=1` / `publish=1` / `service result=0` /
`State:/Network/Global/DNS`。

---

## 4. CI 与下一步（必须由用户确认的那一步）

### 4.1 已派（不动设备，可逆）

```
CI 43241917214  HEAD 346e3d5  SUCCESS (约 45 min)
gh workflow run compose-bootimg.yml -R tselongson/UniXNU-picasso --ref kd439-pool \
   -f fast=true -f create_data_seed=true -f build_userdata=true
```

实机产物（全部已上传，旧 seed 保留 90 天）：
`picasso-data-seed-603` (1.21 MB) / `picasso-bootimg-603` (36 MB) /
`picasso-userdata-603` (5.87 GB)。
**`Build the SpringBoard inject library` 步骤 success** ⇒ 新增 231 行的
`wifiui_configure_service()` 在真实 iPhoneOS SDK 下 `-Wall -Wextra -Werror`
编译、链接、ad-hoc 签名全部通过（这是本轮最重要的构建验证）。
`Build kernel + payload`、`Export audited iOS16 inputs`、
`Retain the exact Data seed` 也全部 success。

- `create_data_seed=true`：新 recipe 无兼容旧 seed，显式新建（reuse-optional 只在
  「无兼容 seed」且显式授权时新建；IO/provenance 失败仍 fatal）。
  产物 `picasso-data-seed-<n>` 是**新增 artifact**，旧 seed 保留 90 天，可逆。
- `build_userdata=true`：同时产出与新 seed 配对的 `userdata-root.sparse.img`
  （~200 MB 压缩 artifact），供后续刷入。
- 派单后**没有刷任何设备**，设备状态未变（当前仍在 fastboot）。
- 该 run 的 HEAD 是 `346e3d5`，**不含** `ea2c30d` 的 AMFI 修复，所以它的
  boot.img 只能用于验证编译/导出 seed，**不能拿去上机判 wifiui**。

### 4.1b 第二次 CI（最终镜像）

`ea2c30d` 只改 `patches/*.patch`，**不在 `recipe()` 覆盖范围**，因此 recipe
不变、刚导出的新 seed 可直接复用。43241917214 成功后已派：

```
gh workflow run compose-bootimg.yml -R tselongson/UniXNU-picasso --ref kd439-pool \
   -f fast=true
```

`acquire()` 会从 43241917214 的 `picasso-data-seed-603` 找到同 recipe 的
artifact；不带 `create_data_seed`、不再需要 `build_userdata`（userdata 已有）。

**已派发并 success：`43249934650`，HEAD `ea2c30d1ed33`（含 AMFI 修复）。**
`Reuse persistent Data seed` **success**（证明 `patches/*.patch` 不在 recipe、
新 seed 可复用）、`Build kernel + payload` success（证明 overlay 第二个 hunk
在真实 CI tree 上 apply 成功、内核编译通过）、0 failed steps。
产物 `picasso-data-seed-604` (1.21 MB) / `picasso-bootimg-604` (36 MB)。

**最终镜像上的离线验证已通过**：`bootimg-604` → `mk-autojoin-args.py` →
`mk-args.py wifiui=0/1` ⇒ 两组 995 B、`wifiauto=1` / `wifissid` / `wpass` /
`offline-legacy-validate=1` / `wifi=1` 全在，**全镜像仅 1 字节不同**
（offset 60348546）。`chain-605` 必须用 `43249934650`。

### 4.2 ⚠ 需要用户明确确认才能做（不可逆）

新 seed 的 `volume_uuid` 来自 `hdiutil create` 的随机 APFS UUID ⇒ **Data 身份变化**。
设备已刷的旧 Data 卷与新 boot 不匹配，11-11 m1 实机为
`offline scratch Data mount failed` panic。

```
1. ✅ 43249934650 success、bootimg-604 已下载、c0/c1 离线验证通过
2. ✅ bootimg-604 在 out/run-43249934650/boot/boot.img；
   还需下载 43241917214 的 picasso-userdata-603（5.87 GB）
3. fastboot flash userdata <新 sparse>      ← 覆盖设备 Data，不可逆，需用户确认
4. bash work/chain-605.sh 43249934650        ← c0/c1 各 400 s 单变量 A/B
5. 请用户实测：设置 > Wi-Fi 详情是否不卡、Safari 输域名能否解析
```

若不授权刷 Data，则本轮只能停在「代码就绪 + CI 编译验证」，
**不得宣称剩余两项已修好**。

### 4.3 判据（不是 0 PANIC）

`PICASSO_WIFIUI loaded in configd`（dyld 是否接受注入）
→ `truthful=1 publish=1`（真实的 `*TIMEOUT*`/`*QUIET*` 已发布）
→ `arrival_publish=1` → `service result=0`（原生 en0 service 已建）
→ `wifid sets` / `SCAN_REQ` / `ASSOCIATE`
→ `State:/Network/Global/DNS` → 用户实测设置详情不卡 + Safari 域名解析。
对照组 c0 必须全 0（`wifiui=0`），否则单变量不成立。

### 4.4 已识别风险与明确分支（不再逐点猜）

1. **dyld 注入**：内核白名单漏掉 configd 的问题已由 `ea2c30d` 从代码上修掉
   （§2.5），但「真的加载成功」仍需实机判据
   `PICASSO_WIFIUI loaded in configd` + `configd started with DYLD_INSERT_LIBRARIES`。
   若仍为 0：说明还有 AMFI/trust 层的第二道限制（注入库的 CDHash 只在
   SpringBoard 路径被验证过），下一刀改独立 root launchd job
   （需把 arm64 二进制放进 Data，仍要新建 seed）。
2. **`SCPreferencesApplyChanges` 在 configd 进程内可能阻塞**：本实现在 detached
   pthread 里跑，不占 configd 主 RunLoop；`service applying` 日志会在卡住前打印。
3. **configd 自己的 `updateStore()` 可能覆盖我们写的字典**：本轮未加重写保护，
   若 `publish=1` 但设置仍卡，下一刀先查 `Plugin:InterfaceNamer` 是否被覆盖，
   再考虑延迟重发（有界、最多 N 次），不引入轮询炸弹。
4. **`DNS` 仍可能不通**：`resolv.conf` 路径已被 11-12 实机证明只服务 Unix/APT，
   原生 DNS 必须靠 configd `IPMonitor` 发布 `State:/Network/Global/DNS`
   （`ip_plugin.c:6245/6262`、`dns-configuration.c`）。本轮的 service 正是这条链。
   若 service 建成但 DNS 仍无，下一刀按 `work/refs/hakios/userland/src/net_test.m`
   的 `ipconfig_set_service`（MIG 20011）或 `SCDynamicStoreSetValue` 补，
   **不要再试更多 resolv.conf 路径**（11-12 已证伪）。

---

## 5. 已通 / 未通清单

**✅ 实机闭环**：固件/WLFW/CE/HTC/WMI/HTT、扫描、auth/assoc、VDEV_UP、
WPA2 四次握手、PTK/GTK、PEER_AUTHORIZE、真实 rx/tx 进 Darwin 栈、en0 link、
KEV_DL_LINK_ON、内核 DHCP（`192.168.101.12/24` + 默认路由）、装地址后系统自发
ARP 走真实数据面被 ACK、开机进桌面可操作、0 PANIC、wifid 已放行
（`26d995a`）、掉线重连（`5eff826`）、apple80211 96/97/70（`c670eb5`）、
DNS option 6 解析 + resolv.conf 写入 + mDNSResponder SIGHUP（`9fb7091`，测量轮）。

**❌ 未通（同一上游根因）**：
1. 设置 App 点 Wi-Fi 详情卡死 —— InterfaceNamer 终止标记缺失；
2. 域名解析 / 状态栏 —— configd 未建 service ⇒ 未发布 `State:/Network/Global/DNS`；
3. 凭据持久化（`wpass=` 仍是 boot-arg 明文）；
4. Safari HTTPS 端到端实测（从未在「关联稳定 + IP 对照」下做过）；
5. 5G MGMT_TX `status=3`（非阻塞）。

---

## 6. 铁律（沿用 v14~v23，新增 2 条）

1. 一次只派一个 CI；`-f fast=true`；采集 ≥400 s（HIF t+168 s）。
2. 判据是 checkpoint 明文 + 用户可操作性，不是 0 panic、不是「进程还在」。
3. **kern_exec exec 路径禁加大数组/写文件**（chain-595/596 栈越界 panic）。
4. 改 .c/.cpp/.m 先做语法与括号校验，再 `abi-check.py` 0 HARD；
   patch hunk 头重算 + `git apply --check`；patch 一律 LF。
5. 995 B cmdline 已满：新门控复用 padding 或等长替换，不新增 token。
6. commit message 不写未实机验证的结论；归因被证伪立即写文档。
7. 用户 6 处未提交改动一律不提交、不恢复。
8. 参考项目优先：先本地 grep 再动手；查资料方法见 `docs/research-playbook.md`。
9. **改 recipe 覆盖的文件 ⇒ 旧 seed 失配；且 seed 逐字节锁死注入 dylib**
   （§2.1），所以改注入库 = 必须新建 seed + 配对 userdata。
10. **新建 seed = 新 Data 身份**（`hdiutil` 随机卷 UUID），刷 userdata 不可逆，
    执行前须用户确认；只派 CI 生成产物不触碰设备。
11. **上机前先离线核对镜像实际 args 与单变量字节差**，别等 400 s 采集
    才发现漏配（chain-532b、chain-605 两次教训）。

---

## 7. 关键路径

| 用途 | 路径 |
|---|---|
| 唯一权威状态 | 本文 v24 |
| 本轮代码 | `346e3d5`（注入库三步）+ `ea2c30d`（AMFI 放宽，决定性） |
| 第一刀基座 | `patches/picasso-wifiui-xnu.patch`、`unixnu` LATE_OVERLAYS（`ae3f56c`） |
| patch 重建/校验工具 | `work/tmp-rebuild-kern-exec.py`、`work/tmp-wifiui-syntax.py` |
| 验证链 | `work/chain-605.sh <成功 CI run id>`（已重写，离线验证 995 B / 1 字节差） |
| 日用参数 | `work/mk-autojoin-args.py`（chain-599 已实机闭环的配置） |
| Apple 机制 | `work/refs/apple-oss/configd/SystemConfiguration.fproj/SCNetworkInterface.c:4911`、`Plugins/InterfaceNamer/ifnamer.c:4312/4334/4524`、`configd.tproj/session.c:1039` |
| 可抄实现 | `UniXNU-pong/userland/wifi-bootstrap.c`、`migration/ipados18/services107-*.md` |
| seed 锁定证据 | `scripts/picasso_linux_inputs.py`（`recipe():38` / `validate():68-125` / `stage_setup():261` / `reuse():238`）、`prepare-picasso-ram-data.py:485` |
| CI | `43241917214` **success**（新建 seed-603 + userdata-603，HEAD 346e3d5）；`43249934650` **success**（HEAD ea2c30d，仅 `-f fast=true` 复用 seed-603，产出 `picasso-bootimg-604` = **最终镜像**）。常态派单只带 `-f fast=true`，**不要**带 `create_data_seed` |
| 最终镜像 | `out/run-43249934650/boot/boot.img`（c0/c1 已离线验证，995 B / 1 字节差） |
| 上机链 | `bash work/chain-605.sh 43249934650`（需先刷 userdata，用户确认后） |
| 上机 / 串口 | `SECS=400 bash work/boot-variants.sh <TAG>`；`python work/serial-cmd.py COM5 status 8` |
| 项目日志 | `.workbuddy/memory/2026-11-12.md`（本轮追加在末尾） |
