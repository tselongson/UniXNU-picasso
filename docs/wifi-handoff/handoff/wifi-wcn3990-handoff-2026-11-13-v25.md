# WiFi (WCN3990) 交接 — 2026-11-13 12:2x（v25：journal 死锁实机解出；wifiui A/B 判决——注入链全通，卡在 IOKitWaitQuiet=53）

> 【历史归档】本文已被 **`wifi-wcn3990-handoff-2026-11-13-v26.md`** 取代，冲突以 v26 为准。
> v26 相对 v25 的实质变化：
> 1. 查明 `53 = KERN_DENIED`（`kern_return.h:326`）= Mach 权限拒绝，
>    **不是「IOKit 仍忙」**；Apple `IOKitWaitQuiet` 公开于 iOS 18.4+，
>    本设备 20H392 上符号存在但被 gate，等价于既有「符号缺失」分支；
> 2. 发现并修正 `kIOReturnTimeout` 常量笔误（`0xe00002e6` → `0xe00002d6`）；
> 3. 发现注入库把「建 service」也挂在 `truthful` 门控上，而 pong 的
>    `configure_interface()` 是无条件执行的 —— 偏离参考实现；
> 4. 三处一并修复（commit `641a2d8`），门禁全过，CI `43374250847` 已派；
>    上机链 `work/chain-606.sh` 已就绪。

> **本文件是当前唯一权威状态源。** v24 及更早为历史归档，冲突以本文为准。
> 判据口径：checkpoint 明文 + 用户可操作性，不是「0 panic」。
> **本版最重要的内容：v24 的「刷配对 userdata 即可上机」前提被实机证伪，
> 真正的阻塞是 UFS 尾部一次性 Data-init journal 绑定了旧 seedHash。**

---

## 0. 一句话状态

```
驱动/固件/握手/数据面/en0/内核 DHCP(192.168.101.12/24) ✅（chain-599 实机）
v24 两刀代码就绪（346e3d5 注入库三步 + ea2c30d AMFI 放宽）、CI success、
最终镜像 bootimg-604 的 c0/c1 单变量（995 B / 仅 1 字节差）验证通过。
★ v24 的「刷配对 userdata 即可上机」被实机证伪（§2）：真正阻塞是
  UFS 尾部一次性 Data-init journal 绑定旧 seedHash，刷 userdata 碰不到它。
★ 本轮已实机解出该死锁：marker 精确清零 16 KiB → phase=1 → Fresh →
  DATASTORE ready=1、0 PANIC（§3）。
★ chain-605 A/B 实机判决已出（§5）：
  - 注入链全部打通（AMFI 放宽 + env + dyld 加载 + root）——ea2c30d 生效；
  - control 组 0 条 WIFIUI，单变量成立；
  - IOKitWaitQuiet 符号存在但返回 53 → 诚实性门控按设计拒绝发布 →
    service 按设计 skip → 设置详情/DNS 仍未解；
  - v24 断言"20H392 没有 IOKitWaitQuiet"被证伪（quiet_api=yes）。
→ 唯一剩余缺口精确到一行：IOKitWaitQuiet 返回 53 的语义待查（§5.2）。
→ 内核 DHCP/Wi-Fi 在新 Data 上正常（两组 DHCP: bound、join=5、0 PANIC）。
```

## 1. 本轮通读与独立核查范围

- `.workbuddy-ai/memory/` **14 份全文**（10-26…11-12）、`handoff/` 全部
  `wifi-wcn3990-handoff-*.md`（v4=11-09.md、v9=11-10.md、v10=11-11.md、v11~v24），
  `docs/research-playbook.md`、`README.md`。v24 之后无 v25+、无 11-13 日志（本轮新建）。
- 一手源码：`work/refs/apple-oss/configd` 的
  `SCNetworkInterface.c:4909 __wait_for_IOKit_to_quiesce()`（while(TRUE) 等
  `*QUIET*/*COMPLETE*/*TIMEOUT*`）、`Plugins/common/plugin_shared.h:47-49`、
  `SCNetworkInterface.c:1789 processNetworkInterface()`（按 **controller**
  `IOObjectConformsTo("IO80211Controller")` 判 Wi-Fi 类型，不看接口对象）、
  `configd.tproj/session.c:1039/1072 checkWriteAccess`。
- 项目内核源码：`drivers/NativeUfs/PicassoPersistence.h:82-109`、
  `PicassoDataInit.h:13-39`、`PicassoRecord.h:27-45`、
  `patches/picasso-native-ram-block-mode1-guard.patch:139-156, 320-410`。
- 参考项目：`UniXNU-pong/userland/wifi-bootstrap.c:27-97`、
  `migration/ipados18/services107-interface-namer-handshake.md`（真机进程栈）、
  `services108-*.md`（用户确认系统 Wi-Fi 已修复）。
- 外网（按 research-playbook §2 短查询）：configd InterfaceNamer 机制（DeepWiki /
  Apple OSS）、`DYLD_INSERT_LIBRARIES` 在 platform binary / setuid / `__RESTRICT` /
  AMFI 下被 prune 的公开资料。

### 1.1 证据分级校正（两位调研代理一致，用户明确要求「不要乱猜」）
- **源码事实（条件式）**：`ifnamer.c` 的 `setup_IOKit()` 是否装载 timer，
  决定 300 s（`WAIT_STACK_TIMEOUT_DEFAULT`）后能否由 `timerCallback()`
  原生发布 `*TIMEOUT*` 自动解卡；`SCNetworkInterface.c:4946-4950` 对
  `*TIMEOUT*` 的接受不分平台。
- **本机推测（不是实机判据）**：「本机走了 timer 未装载分支」。v24 依据的
  「5 份 400 s 日志 `root_busy`/`busy_scan` 探针 0 次」是间接证据，
  **不得写成既定事实**。
- v24 §2.3 自己也把「`setup_IOKit()` 未走到装 timer」标为待取证项。

---

## 2. ★ 推翻 v24 核心前提：刷 userdata 不能让新 seed 挂载

### 2.1 v24 的说法
> 「新建 seed = 新 Data 身份（`hdiutil` 随机卷 UUID），设备旧 Data 与新 boot
> 不匹配 → `offline scratch Data mount failed` panic。
> 刷 `picasso-userdata-603`（与新 seed 配对）即可上机。」

### 2.2 本轮实机证据（4 组 cap，全部 2026-11-13）

| 实验 | 配置 | 结果 |
|---|---|---|
| `console-exp-seedchk.cap` | c1 镜像 + **旧** Data（刷 userdata 之前），90 s | PANIC，`header=-1 present=0` |
| `console-exp-c0.cap` | c0（wifiui=0）+ **新刷** Data，400 s 采集 | PANIC，同样 58 KB |
| `console-exp-c1.cap` | c1（wifiui=1）+ **新刷** Data，400 s 采集 | PANIC，同样 58 KB |
| `console-exp-oldseed.cap` | **旧 seed** 镜像 chain-599 `boot-exp-dhcpok.img` + 同一设备/同一新刷 Data，90 s | **`phase=2`、`open=0 header=0 present=759423`、`DATASTORE ready=1`、`NATIVE_OWNER_DATA done stage=4 error=0`、0 PANIC** |

**三份 panic 日志的 DATASTORE 八行逐字相同**（`seedchk`/`c0`/`c1`）：
```
PICASSO_STORE probe mode=2 blocks=5243041 partition=2883584..30944250
              partitionBlocks=28060667 imageBlocks=2045952 reserve=262144
              gpt=4607b886/7a8e105c
PICASSO_STORE open first=25439066 blocks=5243041 maxBlocks=5243041 lowGuard=22555482
PICASSO_DATA_INIT phase=0 first=25439066 blocks=5243041 marker=30870533
PICASSO_DATASTORE blank-scan status=0 zero_blocks=0 rel=0 lba=25439066 offset=0 crc32=00000000
PICASSO_DATASTORE overlay first=25439066 windowBlocks=5243041 open=4 header=-1 present=0
PICASSO_DATASTORE ready=0 direct=0 volatile=0 failed=0 gpt_after=0 bytes=21474836480
PICASSO_DATASTORE refused publication state=4; old contents preserved
NATIVE_OWNER_DATA done stage=0 error=2 readonly=-1 device=ffffffff path=
```

### 2.3 精确根因（一手源码 + 日志，非推测）

1. **seed 身份其实是相同的**：下载 `picasso-data-seed-603` / `-604` 比对，
   `seed_identity_sha256=3c934757…`、`volume_uuid=1C037EC5-4122-4F7E-AA84-7546EEADF0A9`、
   `raw_sha256=d5c93010…` **三者逐字一致**（604 就是复用 603）。
   设备日志 `uuid=1C037EC5-…` 也一致 ⇒ **不是"新 Data 身份不匹配"**。
2. **刷写位置 ≠ 内核读取位置**：
   `fastboot flash userdata userdata-root.sparse.img` 写分区**起始** 7.8 GiB
   （LBA `2883584` + `imageBlocks=2045952` = root windows）；
   而 20 GiB Data 窗口在 LBA **25439066**（`first=`）、
   一次性 journal marker 在 LBA **30870533**（`marker=`，分区尾部）。
   sparse 完全不覆盖这两处 ⇒ 刷写前后日志逐字相同。
3. **`phase=0` 的真实含义**：`PicassoPersistence.h:89-101` 把几何量 +
   **seedHash** 组成 88 字节 identity；`PicassoDataInit.h:24` 在
   `memcmp(saved, identity, 88) != 0` 时直接 `return 0`。
   注释明写：**"A complete journal NEVER grants reset again, including after
   a damaged header or a different seed"**（`PicassoDataInit.h:5-6`）。
   `create_data_seed` 换了 seedHash，设备尾部的旧 journal 因此永远拒绝新 seed。
4. `phase != 1` ⇒ `picasso-native-ram-block-mode1-guard.patch:139` 传给
   `overlay.open()` 的 `discard=(initPhase==1)` 为 false ⇒
   `Overlay::open()` 在 `!reset` 且 `!initializeBlank` 时 `return Open::Kept`，
   非空窗口 `header=-1` ⇒ `Open::Failed`(state 4) ⇒ 拒绝发布 ⇒ panic。
5. 另核实 `discardRequested()` 已硬编码 `return false`，
   注释 "Picasso never replaces a nonempty store, even if a stale reset
   bootarg is supplied" ⇒ **`sheng-data-reset` 之类 boot-arg 救不了**。

### 2.4 与 11-07 历史记录的对照
`2026-11-07.md:99` 已记「`fastboot erase userdata` + 重刷无效，erase 5.4 秒
只是 discard」，且 `erase-reflash-userdata.sh` 注释说需要「all-zero window」。
当时问题不是 journal，**本轮是第一次把 journal/seedHash 这条链钉死**。

---

## 3. ★ 实机修复：精确清零一次性 journal marker

`work/make-marker-clear.sparse.py` 生成 56 字节 sparse：
- `DONT_CARE` chunk 跳过分区相对块 `27986949`（= 30870533 − 2883584），
  不写 root windows、不写 20 GiB Data 窗口；
- `FILL 0` chunk 只写 **4 个块 / 16 KiB**（`PicassoRecord.h:27-45`
  两个 slot × `blocks()=2`），把 journal 变成 `record.open()` 的 state=2
  「unused」；
- `picasso-data-replace-20g=1`（日用 args 里已有）⇒ `authorized` ⇒
  `PicassoDataInit.h:18-22` 写 pending、返回 `phase=1` ⇒
  `overlay.open(discard=true)` 主动重写 header/bitmap（**不要求整窗全零**，
  见 `picasso-native-ram-block-mode1-guard.patch:373-380`）⇒ `Open::Fresh`。

```
fastboot flash userdata out/full-flash/marker-clear.sparse.img
  Sending 'userdata' (0 KB) OKAY [0.004s] / Writing OKAY / total 0.026s
```

**`console-exp-mkfix.cap`（c1 镜像，90 s）判决**：
```
PICASSO_DATA_INIT phase=1 first=25439066 blocks=5243041 marker=30870533
PICASSO_DATA_INIT complete=1
PICASSO_DATASTORE overlay first=25439066 windowBlocks=5243041 open=1 header=3 present=0
PICASSO_DATASTORE ready=1 direct=1 volatile=0 failed=0 gpt_after=0 bytes=21474836480
NATIVE_OWNER_DATA done stage=4 error=0 readonly=0 device=1000009 path=/dev/disk5s1
0 PANIC
```
⇒ **新 seed 现在能完成 20 GiB Data 初始化并挂载**。90 s 窗口尚未到
configd 注入阶段（HIF t+168 s），wifiui 的实际效果要等 400 s A/B。

> 注意：这是一次性 journal 重新授权，会把 20 GiB Data 窗口重置为
> boot 内嵌的新 seed（旧用户 Data 丢弃）。旧 seed 镜像曾在同一设备上
> `ready=1`，回退需要重新执行对应 seed 的 marker 清零流程。

---

## 4. 本轮修掉的工具/判据坑（都会导致误判）

1. **`work/chain-605.sh` 第一步就挂**：原脚本 `rm -rf out/run-$RUN`，
   被 shell 的 safe-delete 批量拦截（2214 文件 > 阈值 50），目录没删干净，
   `gh run download` 报 `boot/boot.img: The file exists` → `exit 7`，
   **没有任何上机采集**，`console-exp-c1.cap` 还是 11-10 的旧文件。
   修复：下载到唯一新目录 `out/run-$RUN-v605-$(date +%s)`（不删任何东西）；
   stale c0/c1 cap 改用 Python `os.remove` 精确删除并断言。
2. **`State:/Network/Global/DNS` 判据恒 0**：全仓库只在
   `picasso-springboard-probe.m:93` 的**注释**里出现，没有任何代码打印到串口。
   不能拿它当「DNS 未发布」的证据。DNS 最终判据只能是**用户实测 Safari
   输域名**。新脚本 `work/chain-605-verdict.py` 按实际可观测的 checkpoint 判读：
   `PICASSO_WIFIUI loaded in configd` → `truthful=1/publish=1` →
   `service result=0` → `wifid sets/SCAN_REQ/ASSOCIATE`（`hakios.apple80211.c:771`
   打印）→ 用户实测。
3. `wifid sets` / `SCAN_REQ` / `ASSOCIATE` 由 `hakios.apple80211.c:771` 的
   `printf("apple80211: %s %s %d (%s) ...")` 实际打印，是可观测的。
4. **仍未验证的风险（不猜，交给 A/B）**：`346e3d5` dlsym 的 25 个私有
   SC/IOKit 符号来自 pong 的 22H374（iPadOS 18），本机 20H392 是否存在
   未验证。若缺失，`PICASSO_WIFIUI service SC/IOKit symbols unresolved`
   会明确打印。`boot.img` 里能搜到
   `_SCNetworkInterfaceCreateWithIONetworkInterfaceObject` 字符串，但那可能
   只是注入库自带的 dlsym 名字，**不能**当系统 framework 导出证据。

---

## 5. ★ chain-605 A/B 实机判决（2026-11-13 12:3x–12:4x，各 400 s）

| | c0（wifiui=0 对照） | c1（wifiui=1） |
|---|---|---|
| `PICASSO_WIFIUI` 行数 | **0**（对照成立） | **6** |
| `configd started with DYLD_INSERT_LIBRARIES` | 0 | **1** |
| `loaded in configd`（dyld 接受注入） | 0 | **1** |
| `pid=34 euid=0` | — | ✓ root |
| `IOKitWaitQuiet` 符号 (`quiet_api=yes`) | — | **存在**（v24 断言 20H392 没有 → **被证伪**） |
| `quiet_result` | — | **53**（非 0，非 0xe00002e6） |
| `truthful=0 publish=-1` | — | 诚实性门控按设计**拒绝发布** |
| `en0_known=1 arrival_publish=1` | — | State:/Network/Interface 的 en0 到达**已发布** |
| `service skipped (no honest terminal marker)` | — | 第三步按设计跳过 |
| 内核 DHCP（两组都应工作） | `SIOCSIFADDR x1` `DHCP: bound x1` | 同 |
| Wi-Fi | `join=5` 心跳正常 | 同（`msdu=42` 0 PANIC） |
| wifid 活动 | — | `gets 12/19/96/97/106/254/207` 全 answered |

cap：`out/full-flash/logs/console-exp-{c0,c1}.cap`（429522/593401 B）。
镜像：c0/c1 各 995 B、全镜像仅 1 字节差（offset 60348546）。

### 5.1 判决
1. **注入链全部打通**：AMFI 放宽（ea2c30d）+ env 注入 + dyld 加载 + root 权限，
   四道关在 20H392 实机全部通过。v24 最担心的第二道限制（trust/CDHash）
   **不存在**。
2. **诚实性门控按设计工作**：`IOKitWaitQuiet` 返回 53（非 success/非 timeout）
   ⇒ `truthful=0` ⇒ 不发布任何终止标记 ⇒ 第三步按设计 skip。
   没有伪造握手，符合 346e3d5 的设计意图与 services107 的口径。
3. **v24 断言修正**："IOKitWaitQuiet 是 iOS 18.4+，20H392 没有" → **错**；
   `quiet_api=yes` 证明符号存在，只是返回值不同。
4. **新 Data 上内核 DHCP 正常**：两组都 `DHCP: bound`，`join=5`，
   心跳 msdu 推进、0 PANIC ⇒ journal 清零没有破坏 Wi-Fi 数据面。

### 5.2 唯一剩余缺口（精确到一行）
`IOKitWaitQuiet` 在 20H392 返回 **53**，不在 346e3d5 允许发布的两个值
（`KERN_SUCCESS=0`、`kIOReturnTimeout=0xe00002e6`）内。
53 的语义未查（不在 IOReturn 常见值 0xE00002xx 内，也不是 mach 标准错误）。
**修法二选一（下一棒，均需新一轮 CI，patch 不在 recipe 可复用 seed-604）**：
- 查明 53 语义：若为"不支持/无权限"，则本机等价于"无 IOKitWaitQuiet"，
  应走 `sleep(5)` + 发布 `*TIMEOUT*` 路径（346e3d5 已有该分支，改条件即可）；
- 或按 services107 原则把"真实等待后未等到"也视为可发布的超时——但需
  在文档明确这不会伪造 IOKit 状态，且 5 s 是真实流逝的等待。

修好后重跑 A/B（journal 已 complete 且 identity 匹配 604 seedHash，
**同一 seed 可直接 resume，无需再清 marker**；换 seed 才需要 §3 流程）。

### 5.3 用户实测判据（日志不可判，见 §4.2）
设置 > Wi-Fi 详情是否不再卡死；Safari 输域名能否解析。



`bash work/chain-605.sh 43249934650`（c0 wifiui=0 对照 / c1 wifiui=1 实验，
各 400 s）**正在跑**。判据（不是 0 PANIC）：
```
c0 必须全 0 条 PICASSO_WIFIUI（单变量对照成立）
c1 PICASSO_WIFIUI loaded in configd   -> dyld 接受注入（ea2c30d 生效）
   truthful=1 publish=1                -> 真实的 *TIMEOUT* 已发布
   service result=0                    -> 原生 en0 SC service 已建
   wifid sets / SCAN_REQ / ASSOCIATE  -> 原生 Wi-Fi 栈恢复
   DHCP: bound / 192.168.101.x        -> 内核 DHCP 未被新 Data 破坏
   用户实测：设置 > Wi-Fi 详情不卡 + Safari 输域名能解析
```
若 `loaded in configd` 为 0 ⇒ 还有 AMFI/trust 第二道限制（注入库 CDHash
只在 SpringBoard 路径验证过），下一刀按 v24 §4.4 改独立 root launchd job。
若 `publish=1` 但设置仍卡 ⇒ 查 `Plugin:InterfaceNamer` 是否被 configd 的
`updateStore()` 覆盖。若 service 建成但 DNS 仍无 ⇒ 按
`work/refs/hakios/userland/src/net_test.m` 的 `ipconfig_set_service`（MIG 20011）
或 `SCDynamicStoreSetValue` 补，**不要再试 resolv.conf 路径**（11-12 已证伪）。

---

## 6. 未通清单（沿用 v24 §5，未宣称已修）
1. 设置 App 点 Wi-Fi 详情卡死 —— 待 A/B 判决；
2. 域名解析 / 状态栏 —— 只能用户实测，日志不可判；
3. 凭据持久化（`wpass=` 仍是 boot-arg 明文）；
4. Safari HTTPS 端到端实测（从未在「关联稳定 + IP 对照」下做过）；
5. 5G MGMT_TX `status=3`（非阻塞）。

---

## 7. 关键路径与本轮新增文件

| 用途 | 路径 |
|---|---|
| 唯一权威 | 本文 v25 |
| 最终镜像 | `out/run-43249934650/boot/boot.img`（CI `43249934650`，HEAD `ea2c30d`），另见 `out/run-43249934650-v605-*/boot/boot.img` |
| 已刷 Data | `out/run-43241917214/userdata/userdata-root.sparse.img`（7,994,355,624 B，sparse magic ed26ff3a，展开 7.80 GiB）；`work/flash-userdata-605.sh` |
| ★ 修复脚本 | `work/make-marker-clear-sparse.py` → `out/full-flash/marker-clear.sparse.img`（56 B） |
| ★ 修复证据 | `out/full-flash/logs/console-exp-mkfix.cap`（phase=1 / ready=1 / 0 PANIC） |
| 根因证据 | `console-exp-{seedchk,c0,c1,oldseed}.cap`；`out/seed-{603,604}/seed.json` |
| 验证链 | `work/chain-605.sh <CI run id>`（已修工具坑） |
| 判读 | `work/chain-605-verdict.py`（修 DNS 恒 0 判据） |
| 日用参数 | `work/mk-autojoin-args.py` + `work/mk-args.py`（995 B / 1 字节差） |
| 内核源码 | `drivers/NativeUfs/{PicassoPersistence.h,PicassoDataInit.h,PicassoRecord.h}`、`patches/picasso-native-ram-block-mode1-guard.patch` |
| 用户态代码 | `346e3d5` `package/picasso-springboard/picasso-springboard-probe.m`；`ea2c30d` `patches/picasso-wifiui-xnu.patch` |

## 8. 铁律增补（11-13）
12. **新建 Data seed ≠ 改设备 Data 身份**：`seed.json` 的 `volume_uuid` 可
    复用不变；真正阻止新 boot 挂载的是 **UFS 尾部一次性 journal 的
    seedHash 绑定**（§2.3）。刷 `userdata-root.sparse.img` 只换分区起始
    root windows，**永远碰不到** LBA 25439066 的 20 GiB 窗口和
    LBA 30870533 的 journal marker。
13. 换 seedHash 后必须执行 §3 的 marker 精确清零（16 KiB），
    否则 `phase=0` → panic；`sheng-data-reset` / `fastboot erase` 都救不了
    （`discardRequested()` 硬编码 false、erase 只是 discard）。
14. **判据字符串必须先确认有代码会打印**（§4.2），否则「计数为 0」
    会被误读成「功能未通」。
15. 其余沿用 v24 §6 全部 11 条（一次一个 CI、995 B、采集 ≥400 s、
    kern_exec exec 禁加大数组、用户 6 处未提交改动不碰等）。
