# WiFi (WCN3990) 交接 — 2026-11-13 14:0x（v26：IOKitWaitQuiet=53 根因查实并修复）

> **【历史归档】本文件已不是权威状态源，请以
> `handoff/wifi-wcn3990-handoff-2026-11-13-v27.md` 为准。**
> v27 查实 v26 §4.8 的 `c6725d1` DNS 方案把配置写到了 IPMonitor 的**输出键**
> `State:/Network/Global/DNS`（第三方写入不触发 `dns_configuration_set()`），
> 并补充 `SupplementalMatchDomains` 与有界等待 DHCP 两处修复
> （commit `6673613`）。v26 的其余结论与全部实机证据仍然有效。
> 判据口径：checkpoint 明文 + 用户可操作性，不是「0 panic」。
> 本版核心：v25 §5.2「唯一剩余缺口精确到一行」已查明 —— **53 = KERN_DENIED**，
> 且同时发现两处叠加缺陷（错误常量、偏离参考实现的门控），一并修复。

---

## 0. 一句话状态

```
驱动/固件/握手/数据面/en0/内核 DHCP ✅   journal 死锁已实机解出 ✅
注入链四道关全通（AMFI/env/dyld/root）✅   c0 对照成立 ✅
── v25 唯一缺口：IOKitWaitQuiet 返回 53 → truthful=0 → 不发布 *TIMEOUT*
                 → service skipped → 设置详情/DNS 未解
── v26：查明 53 = KERN_DENIED（Mach 权限拒绝，非 "IOKit 仍忙"）
         + 修正 kIOReturnTimeout 常量笔误
         + 按 pong 参考实现把 service 创建从 truthful 门控解耦
         → commit 641a2d8，门禁全过，CI 43374250847 在跑
── 未上机（设备当前离线）。交付边界：代码就绪，不得宣称剩余两项已修好。
```

---

## 1. 本轮通读范围（用户要求「读取最新交接文档和全部项目日志」）

- `handoff/`：v20/v21/v22/v23/v24（11-12）、**v25**（11-13 12:43，权威）；
  v19 及更早要点。
- `.workbuddy-ai/memory/` **16 份全文脉络**（10-26…11-13），其中
  **11-13 日志全文**、**11-12 日志 400–1069 行全文**（扩写部分）逐行读；
  `.workbuddy/memory/` 15 份 `cmp` 逐字节验证为镜像，11-13 仅 AI 侧有。
- `docs/research-playbook.md` 全文。
- 一手源码（本轮新增下载）：
  **`work/refs/iokituser/IOKitLib.c` / `IOKitLib.h`**（Apple 官方 IOKitUser），
  `work/refs/apple-oss/xnu/osfmk/mach/kern_return.h`、
  `iokit/IOKit/IOReturn.h`、`iokit/Kernel/IOUserClient.cpp`、
  `iokit/Kernel/IOService.cpp`、
  `work/refs/apple-oss/configd/sctest/SCTestInterfaceNamer.m`。
- 参考项目：`UniXNU-pong/userland/wifi-bootstrap.c`（`configure_interface()`
  与 `--configure-only` 全文）、
  `migration/ipados18/services107-interface-namer-handshake.md`。
- 外网：Apple `IOKitWaitQuiet` 官方文档（确认 iOS 18.4+ 才公开）、
  IOKitLib API 手册（busyState 语义）、KERN_DENIED 相关公开讨论。

---

## 2. ★ 精准定位：53 是什么、为什么会卡住（不猜）

### 2.1 实机原文（chain-605，`console-exp-c1.cap`）

```
PICASSO_WIFIUI configd started with DYLD_INSERT_LIBRARIES=/usr/lib/libpicasso-sb.dylib
PICASSO_WIFIUI loaded in configd
PICASSO_WIFIUI pid=34 euid=0 quiet_api=yes quiet_result=53 truthful=0 marker=*TIMEOUT*
PICASSO_WIFIUI key=Plugin:InterfaceNamer already=0 truthful=0 publish=-1
PICASSO_WIFIUI en0_known=1 arrival_publish=1
PICASSO_WIFIUI service skipped (no honest terminal marker)
```

### 2.2 53 的确定语义

| 证据 | 结论 |
|---|---|
| `osfmk/mach/kern_return.h:326` | **`#define KERN_DENIED 53`** —— Mach 权限拒绝 |
| `work/refs/iokituser/IOKitLib.c:1429` | `IOKitWaitQuiet()` 第一步是 `io_registry_entry_from_path(masterPort, "IOService:/", &root)`，第二步才 `io_service_wait_quiet(root, ...)` |
| `iokit/Kernel/IOUserClient.cpp:3415` | `if (main_port != main_device_port) return kIOReturnNotPrivileged;` —— MIG 层权限门 |
| `IOKitLib.h:554-579` + Apple 官方文档 | `IOKitWaitQuiet` 公开于 **iOS 18.4+**；本设备 **20H392 = iOS 16.7.16** |
| `IOService.cpp:5830+` `waitQuietWithOptions` | 真正走到等待时只会返回 `kIOReturnSuccess` / `kIOReturnTimeout`，**不会是 53** |

⇒ **53 来自「API 存在但被 gate 拒绝」，根本没走到「等 IOKit 归零」那一步**。
它不是「IOKit 还在忙」，而是「这个 API 在本机不可用」。

**这与 v24 代码里预设的「`IOKitWaitQuiet` 符号不存在」分支语义完全相同**——
两者都表示「无法用该 API 确认 IOKit 状态」，应走**真实有界等待 + 发布
`*TIMEOUT*`**。v25 把它当成「不能诚实发布任何标记」，是把「API 不可用」
误判成了「IOKit 状态不确定且不可放弃等待」。

`*TIMEOUT*` 不声称 IOKit 已 quiesce，只表示「不再等」：
- `SCNetworkInterface.c:4946-4950` 在 iOS 上接受 `*TIMEOUT*`（不分平台）；
- `ifnamer.c:4312 timerCallback()` 超时后也是**无条件** `addTimestamp(*TIMEOUT*)`；
- pong services107 真机正是发布 `*TIMEOUT*` 后解卡。

### 2.3 同时发现的第二个缺陷：错误常量

```
IOReturn.h:136  #define iokit_common_err(r) (sys_iokit|sub_iokit_common|r)
IOReturn.h:169  kIOReturnTimeout = iokit_common_err(0x2d6) = 0xe00002d6
注入库 v24 写的是 0xe00002e6   ← 差 0x10，笔误
```
即使将来本机真的返回原生超时，也会被误判为「不诚实」。属通用性缺陷。

### 2.4 第三个缺陷：偏离参考实现的门控

`UniXNU-pong/userland/wifi-bootstrap.c` 的 `--configure-only`：

```c
(void)publish_interface();
(void)publish_namer_result();     // 返回值被丢弃
return configure_interface();     // ★ 无条件执行
```

而我们的 `wifiui_bootstrap()` 是：

```c
if (already || published == 1)
    wifiui_configure_service(sc, iokit);
else
    wifiui_log("service skipped (no honest terminal marker)");
```

**诚实性只应约束「是否宣称 `*QUIET*`」，不该阻止「为真实 en0 建原生
service」**。`configure_interface()` 用的是真实 IORegistry en0 对象，不伪造
任何设备字典，且函数本身幂等。我们多加的这一层门控，正是 c1 里
`service skipped` 的直接原因，也是 DNS 链断掉的地方。

---

## 3. 修复（commit `641a2d8`，1 file +50/−24）

文件：`package/picasso-springboard/picasso-springboard-probe.m`
可复现脚本：`work/fix-wifiui-wait-quiet.py`（从干净 HEAD 逐字节可复现，已验证）

1. **命名常量替代魔数**
   `WIFIUI_KERN_SUCCESS=0`、`WIFIUI_KERN_DENIED=53`、
   `WIFIUI_IORETURN_TIMEOUT=0xe00002d6`（修正笔误）、`WIFIUI_WAIT_SECONDS=5`。
2. **诚实的等待分类**（控制流不针对 53 做特判，通用）
   - `KERN_SUCCESS` → `*QUIET*`（唯一可以宣称 quiesce 的情况）
   - `kIOReturnTimeout` → `*TIMEOUT*`，记 `waited=1`（原生调用已真实等 5 s）
   - **其它任何返回**（KERN_DENIED / KERN_NOT_SUPPORTED / MIG / entitlement
     失败）→ 自行 `sleep(5)` 补齐真实有界等待 → `*TIMEOUT*`
   - 仅额外打印一行 `quiet KERN_DENIED: API gated on this OS, bounded wait
     elapsed` 便于设备自证，不参与分支
3. **service 创建与 truthful 解耦**：照 pong 无条件调用
   `wifiui_configure_service(sc, iokit)`，新增 `service begin already=… published=…`
   checkpoint 便于判读。
4. `wifiui=1` 门控原样不动 ⇒ **c0（wifiui=0）完全 inert，单变量仍成立**。

### 门禁（全过）

| 检查 | 结果 |
|---|---|
| 从 HEAD 精确字符串复现 | `IDENTICAL = True` |
| WSL clang `-fsyntax-only -Wall -Wextra -Werror`（CF/IOKit stub 头，`work/tmp-wifiui-syntax.py`） | **RC 0 / 0 errors** |
| `work/abi-check.py` | **0 HARD**（4 个既有 soft） |
| 括号/圆括号平衡 | 45/45、347/347 |
| 关键断言 10 条（错误常量消失、无 53 特判、QUIET 仅 success、service 无条件、门控未动…） | 全 PASS |
| 用户 6 处未提交改动 | 原样保留，提交只含 1 个文件 |

> 写入坑：`os.replace` 与 `Path.write_bytes` 被环境写入保护拒绝
> （ACL 显示 FullControl 仍 Access denied），改用 Edit 工具通道逐处替换成功。
> 下一棒若遇同类问题直接用 Edit，不要反复试文件复制。

---

## 4. CI 与上机（进行中）

### 4.1 为什么必须新建 Data seed

`scripts/picasso_linux_inputs.py:38 recipe()` 显式 hash
`package/picasso-springboard/picasso-springboard-probe.m` 的字节 ⇒ 改它必改
recipe；`validate():68` 要求 `manifest.recipe_sha256 == recipe(tree)`，
`reuse():254` 还会把 seed 里的 `libpicasso-sb.dylib` **覆盖回构建产物**。
已本地复算确认：`.m` sha256 从 `6930a0fe…` 变为 `e8ec2690…`。
⇒ 复用 seed-603/604 会让旧 dylib 覆盖修复，**必须显式新建 seed**。
`patches/*.patch` 不在 recipe 内，但 `truthful` 逻辑在用户态 `.m` 里，
无法用内核 patch 承载 —— 此路已排除。

### 4.2 已派

```
CI 43374250847  HEAD 641a2d8  **SUCCESS（29m12s）**
gh workflow run compose-bootimg.yml -R tselongson/UniXNU-picasso \
   --ref kd439-pool -f fast=true -f create_data_seed=true -f build_userdata=true
```
产物（已全部上传）：
`picasso-data-seed-605` (1,190,966 B) / `picasso-bootimg-605` (36,118,534 B) /
`picasso-userdata-605` (5,870,693,806 B)。
一次派单同时产出 seed + userdata + bootimg，`chain-606` 直接用这个 run id。
`Build the SpringBoard inject library` 在真实 iPhoneOS SDK 下编译/链接/签名通过
（CI success 已证明；本地 WSL clang 语法检查亦过）。

### 4.3 上机链 `work/chain-606.sh <run-id>`（已写好，`bash -n` 通过）

按 v25 §2/§3 的实机结论，顺序**不可换**：

1. 等 CI success → 下载 bootimg + userdata 到**唯一新目录**（不删旧目录，
   避开 safe-delete 批量拦截；605 首跑就是栽在 `rm -rf`）
2. **`fastboot getvar partition-size:userdata` 几何核对**（本轮新增）：
   `make-marker-clear-sparse.py` 的 marker LBA `30870533` 是在 seed-603 设备上
   实测的，`PicassoPersistence.h:100` 显示它只由 `gStoreGeometry.last` 与
   常量决定，**不依赖 seed 字节**；`board.toml` 的 `size_mib=60` 与
   `picasso-data-replace-20g=1` 本轮未改。仍加一道闸门：期望
   `0x1AC2BFB000`（115 GiB），不符则 `exit 16` 停下重新测量，避免写错位置。
3. `flash-userdata-605.sh <新 userdata>` 刷配对 Data（只覆盖分区起始 root windows）
3. **`make-marker-clear-sparse.py` → `fastboot flash userdata marker-clear.sparse.img`**
   精确清零尾部 16 KiB journal marker ⇒ `phase=1` → Fresh。
   ★ 新 seedHash 会让旧 journal 拒绝新 boot，**刷 userdata 碰不到尾部 marker**，
   这一步不做必然 `offline scratch Data mount failed` panic（v25 §2 四组实机）
4. `mk-autojoin-args.py` → `mk-args.py wifiui=0/1` ⇒ c0/c1 各 995 B、
   **全镜像仅 1 字节不同**、显示参数与 `wifiauto` 全在（离线断言）
5. 各 400 s 采集 → checkpoint 判读

`--skip-flash` 可跳过 2/3（仅限同一 seed 已刷过时）。

### 4.4 判据（不是 0 PANIC）

| 判据 | 期望 |
|---|---|
| c0 `PICASSO_WIFIUI` 行数 | **0**（单变量对照成立） |
| c1 `quiet KERN_DENIED ... bounded wait elapsed` | 1（设备自证 API 被 gate） |
| c1 `truthful=1 publish=1` | `*TIMEOUT*` 已发布 |
| c1 `service begin` / `service result=0` | 原生 en0 service 已建 |
| c1 `wifid sets` / `SCAN_REQ` / `ASSOCIATE` | 原生 Wi-Fi 栈恢复 |
| 两组 `DATASTORE ready=1`、`DHCP: bound`、`join=5`、0 `PANIC BEGIN` | 无回归 |
| **用户实测** | 设置 > Wi-Fi 详情不再卡死；Safari 输域名可解析 |

⚠ **`State:/Network/Global/DNS` 恒 0**：该字符串全仓库只在
`picasso-springboard-probe.m:93` 的**注释**里，没有任何代码打印到串口
（v25 §4.2 已记）。**不能**拿它当「DNS 未发布」的证据。DNS 最终判据只能是
用户实测 Safari 输域名。

⚠ **本轮另修一个判据 bug（会直接导致误报）**：`'publish=1' in log` 作为
子串会匹配到 **`arrival_publish=1`** —— 那是完全不同的 checkpoint
（`State:/Network/Interface` 的 en0 到达）。v25 的 c1 实际是
`publish=-1` + `arrival_publish=1`，旧脚本却报「marker published」。
已在 `work/chain-606.sh` 与 `work/chain-605-verdict.py` 改为**精确匹配
`PICASSO_WIFIUI key=Plugin:InterfaceNamer` 整行**（要求 `truthful=1` 且
` publish=1`）。用 v25 的 c0/c1 cap 回归验证：现在正确判为
`publish=False / service=False / VERDICT: no honest terminal marker`，
与 v25 §5 实机判决一致。

### 4.5 磁盘约束与零下载决策（14:2x–15:0x）

- 完整 `chain-606.sh` 首发在**解压 `picasso-userdata-605` 时因磁盘不足失败**：
  `error extracting "userdata-root.sparse.img": There is not enough space on
  the disk` → `exit 7`。C 盘只剩 ~5.5 GB，而 5.87 GB artifact zip +
  7.99 GB sparse 峰值需 ~14 GB。已按用户授权清理工作区可再生产物
  （旧 userdata-603 副本 7.99 GB、21 个过期 CI 目录 4.03 GB、
  34 个旧实验镜像 2.21 GB、TEMP 过期 zip、半截 sparse 4.04 GB），
  回收站亦已清空，但 C 盘本身已满，无法腾出 14 GB。
- **改走零下载路径** `work/chain-606-lite.sh`（`bash -n` + 参数链离线预演通过，
  995 B / 单变量仅 1 字节差 offset `60233858`）。依据 v25 §2.2/§3 实机结论：
  刷 userdata 只写分区起始 root windows，**碰不到** 20 GiB Data 窗口与尾部
  journal；真正让新 seed 挂载的是 **16 KiB marker 清零**
  （`phase=1` → `discard=true` → 内核用 **boot 内嵌的新 seed** 重建 Data 窗口）。
- 本地实证 `bootimg-605` 内嵌修复后的 dylib：
  `linux-inputs/libpicasso-sb.dylib` 94208 B / sha `2210d06c…`，
  含 `quiet KERN_DENIED`×1、`service begin`×1；
  而 seed-603/604 的 dylib 是 94176 B / `ca920097…`。
  seed-605 三项 identity（`seed_identity_sha256 83b02d8d…`、
  `volume_uuid 35FC67FA…`、`raw_sha256 b5a86951…`）与 603 全不同，
  证明确实是新建 seed 且已进入 boot。
- lite 链判据**可证伪**：只有新 dylib 会打印 `quiet KERN_DENIED` /
  `service begin`；若出现 ⇒ boot 内嵌 seed 自足、无需 userdata；
  若 `DATASTORE ready=0` / PANIC ⇒ 路径不成立，须腾出 ≥14 GB 重跑完整链。
  `chain-606.sh`（完整版）保留供磁盘充足时使用。

### 4.6 ★ chain-606-lite 实机判决（2026-11-13，c0/c1 各 400 s）

零下载路径**成立**：只清 16 KiB journal marker，boot 内嵌的 seed-605 就足以
完成 Data 初始化，两组都 `DATASTORE ready=1` / 0 PANIC /
`Early boot complete` / `DHCP: bound` / `join=5`。**5.87 GB userdata 确认不必刷。**

| 判据 | c0（wifiui=0） | c1（wifiui=1） |
|---|---|---|
| `PICASSO_WIFIUI` 行数 | **0** ✓ 单变量成立 | 10 |
| `configd started with DYLD_…` | 0 | 1 |
| `loaded in configd` | 0 | 1 |
| `quiet KERN_DENIED`（仅新 dylib 打印） | 0 | **1** ✓ 修复后的库在跑 |
| `quiet_result=53 truthful=1 marker=*TIMEOUT*` | — | **1** ✓ 53 已被正确处理 |
| `key=Plugin:InterfaceNamer … truthful=1 publish=1` | — | **1** ✓ 终止标记已发布 |
| `service begin` | — | **1** ✓ 门控解耦生效 |
| `wifid sets` | 0 | **17** ✓ 原生 Wi-Fi 栈恢复 |
| `service en0_named=1 type=IEEE80211` | — | 1（真实 en0、Wi-Fi 类型正确） |
| `service result=` | — | **1（失败）** |
| `DATASTORE ready=1` / 0 PANIC / `DHCP: bound` / `join=5` | ✓ | ✓ |

**唯一剩余缺口收窄到一个函数**：`wifiui_configure_service()` 里
`result=1` 且**没有** `service applying SCPreferences changes` 日志 ⇒
只可能是三个分支之一：`prefs_create==NULL`、`lock(prefs,0)==FALSE`、
或 `service_create(...)==NULL`（`current==0` 也会让 service 为 NULL）。
Apple 文档：`SCPreferencesLock(wait=false)` 在锁被占用时返回 FALSE
（`kSCStatusPrefsBusy`）；v24 注释也提到 `SCPreferencesApplyChanges` 会等
InterfaceNamer，注入在 configd **进程内**时锁可能被自己占用 —— **但这是假设，
不是判据**。

**commit `3ae7639`**（1 file +9/−1）只加细粒度 checkpoint，**不改控制流**：
```
service prefs=%d locked=%u
service found=%u set=%d current=%u service=%d
```
CI `43385490304`（`fast=true create_data_seed=true`，**不带 build_userdata**
—— lite 路径已证明不需要）在跑。下一轮上机用 `work/run-606b.sh`：
等 CI → 只下 36 MB bootimg → `work/chain-606-lite2.sh`（同一 marker 清零 +
c0/c1 各 400 s，新增 `service prefs=` / `service found=` 判据）。
定位到确切分支后再针对性修（很可能是 `lock(prefs,1)` 或改用独立 session，
**但必须等 checkpoint 明文，不猜**）。

日志：`work/chain-606-lite.out`、
`out/full-flash/logs/console-exp-{c0,c1}.cap`（第一组判决）、
`work/run-606b.out` / `work/chain-606-lite2.out`（诊断轮，待产出）。

### 4.7 ★ 诊断轮实机判决与最后一刀（commit `3ae7639` → `5cb8977`）

CI `43385490304` **success**（36m58s，只带 `create_data_seed=true`，
不带 `build_userdata`）。`chain-606-lite2` 实机（c0/c1 各 400 s）给出
**决定性的 checkpoint 明文**：

```
PICASSO_WIFIUI service en0_named=1 type=IEEE80211
PICASSO_WIFIUI service prefs=1 locked=0        <-- 就是这里
PICASSO_WIFIUI service result=1 type=IEEE80211
```

⇒ `SCPreferencesCreate` 成功，`SCPreferencesLock(prefs, FALSE)` 失败。

**Apple 一手源码（不是推测）**：
`SystemConfiguration.fproj/SCPLock.c:523`
```c
prefsPrivate->lockFD = open(prefsPrivate->lockPath,
    wait ? O_WRONLY|O_CREAT|O_EXLOCK
         : O_WRONLY|O_CREAT|O_EXLOCK|O_NONBLOCK, 0644);
```
`EWOULDBLOCK` 分支（:548）设 `kSCStatusPrefsBusy` 并立即返回 FALSE。
锁是 `preferences.plist-lock` **文件锁**，configd 主 RunLoop 正持有它。
pong `wifi-bootstrap.c:75` 可以传 `false` 是因为它在**独立进程**；
我们的注入库运行在 **configd 进程内**（`ea2c30d` 的 AMFI/DYLD 通道），
必然撞上同一把锁。`wait=TRUE` 时 Apple 去掉 `O_NONBLOCK`，阻塞等锁释放；
我们的 configure 路径在 **detached pthread**（`pthread_create` +
`pthread_detach`，见 `picasso_springboard_probe()`），**不在 configd 主 RunLoop**，
所以不会死锁；Apple 自己的 `avoid_SCNetworkConfiguration_deadlock()`
（SCPLock.c:432）已用 dispatch_once 等 PreferencesMonitor 完成升级。

**commit `5cb8977`**：`lock(prefs, 0)` → `lock(prefs, 1)`，一行控制流改动，
保留全部诊断 checkpoint，`wifiui=1` 门控不变。
从 `ea2c30d` 五处精确字符串编辑逐字可复现（`IDENTICAL=True`）、
WSL clang RC 0、abi-check 0 HARD。
CI `43392560737`（`fast=true create_data_seed=true`，不带 userdata）在跑；
`work/run-606c.sh` → `work/chain-606-lite3.sh` 等 CI 成功后自动上机
（marker 清零 + c0/c1 各 400 s，判据含 `service prefs=1 locked=1`、
`service defaults=` / `service applying` / `service result=0`）。

### 4.8 ★ `wait=true` 被实机证伪，改用原生 dynamic-store DNS（commit `c6725d1`）

**`5cb8977` 的 `lock(prefs,1)` 判断错了，已撤销。** chain-606-lite3 实机：

| 判据 | c0（wifiui=0） | c1（wifiui=1，5cb8977） |
|---|---|---|
| `configd started with` | **0** | **8**（pid 34→285→295→301→315→330→342→358） |
| `EXC_CORPSE` | 57（系统既有） | **50+**（每次紧跟 `service begin`） |
| `service prefs=` | 0 | **0**（一次都没打印） |
| 0 PANIC / DATASTORE ready=1 / join=5 | ✓ | ✓ |

`service begin` 之后、`service prefs=` 之前正是 `lock(prefs,1)` 所在 —— 
**阻塞的 `open(O_EXLOCK)` 永不返回**，configd 主 RunLoop 被拖死、watchdog 杀进程、
launchd 重启、再注入、再卡死。c0 零重启证明完全由 wifiui 引起。

根因（Apple `SCPLock.c:523` 实证）：锁是 `preferences.plist-lock` **文件锁**，
`wait=FALSE` 加 `O_NONBLOCK` 立即返回 `EWOULDBLOCK`/`kSCStatusPrefsBusy`；
`wait=TRUE` 去掉 `O_NONBLOCK` 会阻塞。configd **自己**在主 RunLoop 上持有这把锁，
注入库运行在 configd 进程内 ⇒ **无论 wait 取值都不可能拿到锁**。
pong 能用 `false` 只因它在独立进程。这是架构限制，不是参数问题。

**commit `c6725d1`（1 file +112/−10）**：
1. 恢复 `lock(prefs, 0)`（非阻塞，configd 健康），并在注释中明确禁止
   再引入 `wait=true`（含实机反证）。
2. **新增 `wifiui_publish_dns()`**：读回内核 DHCP 从 option 6 写出的
   `/private/var/run/resolv.conf`（实机 `DHCP: dns 192.168.101.1 count=1`、
   `write ... error=0 bytes=77`），按 **Apple 标准字典**
   `State:/Network/Global/DNS` → `{ServerAddresses: [...]}` 发布。
   `kSCPropNetDNSServerAddresses` 字面量就是 `"ServerAddresses"`
   （`SCSchemaDefinitions.c:126`），`ip_plugin.c:6262` 发布的正是同一个键，
   `dns_configuration_set()` 消费它。**不硬编码任何 IP、不伪造协议**。
   这条写不经过 preferences 锁，且同一 session 的 `publish=1` /
   `arrival_publish=1` 已实证在 configd 进程内可用。
3. checkpoint：`dns servers=N` / `dns publish=1 key=State:/Network/Global/DNS`。
4. `wifiui=1` 门控不变；原生 service 尝试保留（拿不到锁就如实 `locked=0`
   并返回，不再拖死 configd）。
5. 门禁：从 `ea2c30d` **7 处精确字符串编辑逐字可复现**（`IDENTICAL=True`）、
   WSL clang `-Wall -Wextra -Werror` RC 0（stub 头补上 Apple 官方
   `CFStringCreateWithCString` 签名）、ABI 0 HARD。

CI `43400951270`（`fast=true create_data_seed=true`）在跑；
`work/run-606d.sh` → `work/chain-606-lite4.sh` 自动上机，判据新增
`dns servers=` / `dns publish=1` / `configd started with`（应为 1 次，非 8 次）。

### 4.9 交付边界（如实）

**已实机闭环**：驱动/固件/握手/数据面/内核 DHCP、进桌面可操作、
注入链四道关、`*TIMEOUT*` 终止标记发布、原生 Wi-Fi 栈恢复（`wifid sets`×17）、
journal 死锁解法、零下载 boot 路径、三处 `IOKitWaitQuiet` 修复。
**`c6725d1` 的 DNS 发布与 configd 稳定性待最后一轮实机验证**，因此
DNS / 状态栏 / 设置详情页**不得宣称已修好**；DNS 最终判据是用户实测
Safari 输域名（日志判不了）。
`chain-605-verdict.py` 已修正 `publish=1` 子串误匹配 `arrival_publish=1` 的
判据 bug，用 v25 c0/c1 回归后能正确判为 `publish=False`。

> **给下一棒的硬结论**：在这台设备上，**不要再尝试从 configd 进程内
> 创建 SCPreferences service**（自持锁，结构上不可行）。要建原生 service，
> 只能按 pong 的做法用独立 root 进程（需注入 arm64 二进制 + launchd job +
> 新建 seed）。「能上网」的最短路径是 dynamic-store DNS（`c6725d1`）+ 用户实测。

- `chain-606.sh` 首发在**解压 `picasso-userdata-605` 时因磁盘不足失败**：
  `error extracting "userdata-root.sparse.img": There is not enough space on
  the disk` → `exit 7`。C 盘只剩 ~5.5 GB，而 5.87 GB 的 artifact zip +
  7.99 GB sparse 峰值需 ~14 GB。已按用户授权清理工作区可再生产物
  （旧 userdata-603 本地副本 7.99 GB、21 个过期 CI 目录 4.03 GB、
  34 个旧实验镜像 2.21 GB、TEMP 过期 zip、半截 sparse 4.04 GB），
  但 C 盘本身已满，无法腾出 14 GB；回收站亦已清空。
- **改走零下载路径** `work/chain-606-lite.sh`（`bash -n` + 参数链离线预演通过）：
  依据是 v25 §2.2/§3 的实机结论 —— 刷 userdata 只写分区起始 root windows，
  **碰不到** 20 GiB Data 窗口与尾部 journal；真正让新 seed 挂载的是
  **16 KiB marker 清零**（`phase=1` → `discard=true` → 内核用
  **boot 内嵌的新 seed** 重建 Data 窗口）。
  已本地实证 `bootimg-605` 内嵌修复后的 dylib：
  `linux-inputs/libpicasso-sb.dylib` 94208 B / sha `2210d06c…`，
  含 `quiet KERN_DENIED` ×1、`service begin` ×1；
  而 seed-603/604 的 dylib 是 94176 B / `ca920097…`。
  seed-605 三项 identity（`seed_identity_sha256` `83b02d8d…`、
  `volume_uuid` `35FC67FA…`、`raw_sha256` `b5a86951…`）与 603 全不同，
  证明确实是新建 seed 且已进入 boot。
- lite 链判据是**可证伪**的：只有新 dylib 才会打印
  `quiet KERN_DENIED` / `service begin`。若出现 ⇒ boot 内嵌 seed 自足、
  无需刷 userdata；若 `DATASTORE ready=0` / PANIC ⇒ 该路径不成立，
  必须腾出 ≥14 GB 后重跑完整 `chain-606.sh`。
- 等价的单变量仍成立：c0/c1 各 995 B、全镜像仅 1 字节不同
  （离线预演 offset `60233858`），显示参数与 `wifiauto` 全在。
- 输出 `work/chain-606-lite.out`；结果出来后补写判决与 §5 清单。
  `chain-606.sh`（完整版）保留，供磁盘充足时使用。

---

## 5. 未通清单（沿用 v25 §6，未宣称已修）

1. 设置 App 点 Wi-Fi 详情卡死 —— 待 chain-606 A/B + 用户实测；
2. 域名解析 / 状态栏 —— 同源，只能用户实测，日志不可判；
3. 凭据持久化（`wpass=` 仍是 boot-arg 明文）；
4. Safari HTTPS 端到端实测（从未在「关联稳定 + IP 对照」下做过）；
5. 5G MGMT_TX `status=3`（非阻塞）。

---

## 6. 铁律（沿用 v25 §8，本轮新增 2 条）

1–11 沿用 v25 §8（一次一个 CI、995 B 单变量、采集 ≥400 s、
kern_exec exec 禁加大数组、用户 6 处改动不碰等）。
12. 新建 Data seed ≠ 只刷 userdata：新 seedHash 会让 UFS 尾部 journal
    拒绝新 boot，必须执行 §4.3 步骤 3 的 marker 精确清零（16 KiB）。
13. **不要把「API 返回权限/不支持错误」当成「状态不确定所以什么都不能做」**：
    先查返回码的权威定义（`kern_return.h` / `IOReturn.h`），再决定它等价于
    哪个已有分支。`*TIMEOUT*` 不声称 IOKit quiesce，是诚实可发布的。
14. 改 recipe 覆盖文件（尤其 `picasso-springboard-probe.m`）⇒ 必须
    `create_data_seed=true`，且 seed 会把注入 dylib 覆盖回构建产物。
15. 环境写入保护：源码文件 ACL 正常却写入被拒时，用 Edit 工具通道，
    不要反复试 `os.replace` / `cp` / `WriteAllBytes`。

---

## 7. 关键路径

| 用途 | 路径 |
|---|---|
| 唯一权威 | 本文 v26 |
| 本轮修复 | `641a2d8`（`picasso-springboard-probe.m`） |
| 可复现脚本 | `work/fix-wifiui-wait-quiet.py` |
| 语法检查 | `work/tmp-wifiui-syntax.py`（WSL clang） |
| 上机链 | **`work/chain-606.sh <run-id>`** |
| 判读 | `work/chain-605-verdict.py` |
| marker 清零 | `work/make-marker-clear-sparse.py` → `out/full-flash/marker-clear.sparse.img`（56 B） |
| userdata 刷写 | `work/flash-userdata-605.sh <img>` |
| 日用参数 | `work/mk-autojoin-args.py` + `work/mk-args.py` |
| CI | `43374250847`（HEAD `641a2d8`，`fast=true create_data_seed=true build_userdata=true`） |
| 53 语义证据 | `work/refs/iokituser/IOKitLib.{c,h}`、`work/refs/apple-oss/xnu/osfmk/mach/kern_return.h:326`、`iokit/IOKit/IOReturn.h:169`、`iokit/Kernel/IOUserClient.cpp:3415` |
| 参考实现 | `UniXNU-pong/userland/wifi-bootstrap.c`、`migration/ipados18/services107-*.md` |
| 本轮日志 | `.workbuddy-ai/memory/2026-11-13.md` 末节 |
