# WiFi (WCN3990) 项目交接文档 — 2026-11-11 15:2x（v11）

> **⚠️ 本文件已归档，当前权威状态源是
> [`wifi-wcn3990-handoff-2026-11-11-v12.md`](wifi-wcn3990-handoff-2026-11-11-v12.md)。**
> 本文把「0 PANIC + Early boot complete」当成修好，但用户实际症状是
> **WiFi 一启动就锁屏卡死、完全没反应**。v12 给出了精准根因
> （两个无限循环的 `machine_delay_until` 忙等把两大核占满）与修复。
> 本文其余内容（Data seed 修复、`error=22` 取证）仍有效。

> ~~**本文件是当前唯一权威状态源。**~~ `wifi-wcn3990-handoff-2026-11-11.md`(v10) 及更早版本均为历史归档。
> 事实来源：`out/full-flash/logs/console-exp-*.cap`（串口原文）、
> `.workbuddy-ai/memory/` 全 13 份日志、CI `42973205561` 日志与产物、
> `upstream/silenzio-picasso` commit `9572b30`。
> 判据口径：**checkpoint 明文，不是「0 panic」**（`diagnostic-charter.md`）。

---

## 0. 一句话现状（2026-11-11 15:2x 实机）

```
「加 WiFi 就开不了机」  ✅ 已修復並實機閉環（9572b30 / CI 42973205561 / wifidfix）
  ── 0 PANIC、DATASTORE ready=1、Early boot complete、SpringBoard、FBUC 正常
  ── WiFi 驅動 en0 attach + link up、心跳活到 polls=187000（400s 採集全程）
「WiFi 與系統並存」      ✅ 已達成（系統能開機 + WiFi 驅動同時在跑）
驅動側全鏈路             ✅ 早已打通（chain-580：四次握手 + 真實幀進協議棧）
設置裡 Wi-Fi 開關        ❌ 仍未通；本輪找到真正阻塞點（§4：PICASSO_JOBS error=22）
DHCP / 憑據持久化         ⬜ 依賴 wifid
```

**本轮修的是「开不了机」，不是「设置里能联网」。两者别混。**

---

## 1. 用户三问的精确答案

### 1.1 「加 WiFi 为什么开不了机」= 4 个不同阶段的不同根因，前 3 个早已修

| 阶段 | 症状 | 根因 | 修复 |
|---|---|---|---|
| ① 11-05/06 | `launchd exec ENOEXEC`、UFS stall | modem 上电落在 root 单线程阶段 | `picasso_wlan_deferred_start()` + `wlan_delay_ms` |
| ② 11-06~08 | CE 门铃后 SError / 静默复位 | SMMU 流 `0xc0` 无路由、CE 寄存器 un-clocked 读 | 481 早期绑定 0xc0（UEFI SEC）+ `62572ba` |
| ③ 11-09/10 | VDEV_UP 后 SError | WMI INIT 资源池缺 `ast_skid_limit` 等 | `wmiinit=1` / `initfix=3` |
| ④ **当前** | **`offline scratch Data mount failed: 2` @kern_exec.c** | **`45d577a` 把 `com.apple.wifid` 从 `offline_disabled_jobs` 删掉；该列表是 `picasso_linux_inputs.py:recipe()` 的输入 → Data seed 身份/UUID 变了 → boot 内嵌 seed 与设备已刷的旧 Data 不匹配** | **本轮 `9572b30`** |

m1 串口原文（`out/full-flash/logs/console-exp-m1.cap:950-965`）：
```
PICASSO_DATASTORE overlay first=25439066 windowBlocks=5243041 open=4 header=-1 present=0
PICASSO_DATASTORE ready=0 direct=0 volatile=0 failed=0 gpt_after=0 bytes=21474836480
PICASSO_DATASTORE refused publication state=4; old contents preserved
NATIVE_OWNER_DATA done stage=0 error=2 readonly=-1 device=ffffffff path=
=== PICASSO CDC PANIC BEGIN ===
fmt: offline scratch Data mount failed: %d @%s:%d
```
**不是 WiFi 驱动崩溃，是 Data 身份不匹配。** `picasso-data-init-blank=1` 不足以格式化。

另有 3 个「开着 WiFi 进不了桌面」的独立根因，11-11 凌晨均已修：
- `make-545.py` 打槽吃掉 `offline-legacy-validate=1` → 黑屏（`verify_swap failed` 6155 次）；
- HIF 初始化抢 CPU 饿死合成器（FBUC 120→33）→ `wlan_delay_ms=150000`；
- `picasso_sta_scan()` 在 ioctl 上下文 `IOSleep` 3 秒 → `9ead7fc` 改 500 ms。

### 1.2 「现在 WiFi 全链路打通了吗」= 分层，不能笼统说「通了」

- **驱动侧全链路 ✅ 已打通并有实机 checkpoint**：固件启动 → scan → auth → assoc →
  VDEV_UP → EAPOL → WPA2 四次握手 → 装 PTK/GTK → PEER_AUTHORIZE →
  真实 ARP/IPv4/IPv6 帧进 Darwin 协议栈 → `en0` attach + link up
  （chain-580 `msdu=21 bad=0`）。
- **系统侧 ✅ 本轮打通**：能开机进桌面、0 panic、WiFi 开着系统不死。
- **设置侧 ❌ 未通**：`wifid` 从未启动（日志零出现），设置里没有 Wi-Fi 开关，
  DHCP / 凭据持久化 ⬜。
- 所以严格答案：**驱动 + 系统已通，用户态「设置里联网」没通**。

### 1.3 goal「WiFi 和系统并存」= 消除互斥

此前互斥：能开机的 m3(15da938) 不含 wifid 放行；含放行的 45d577a/9ead7fc
一开机就 Data panic。本轮修复后两者不再互斥（§3 实机证据）。

---

## 2. 本轮修复：`9572b30`（kd439-pool）

设计意图（已用代码核实，非推测）：`runtime_enabled_jobs` 在 launchd 读
`disabled.plist` 之前由 XNU patch 原子改写；**它不在 `recipe()` 的 settings 里**
（`picasso_linux_inputs.py:51-54` 只含 `skip_setup_tool / skip_setup_args /
offline_disabled_jobs / owned_trust_library / size_mib / adt / kernel_args`），
所以改它不影响 seed UUID。

改动 3 个文件：

| 文件 | 改动 |
|---|---|
| `board.toml` | 把 `com.apple.wifid` 放回 `offline_disabled_jobs` 的 15da938 原位（wifianalyticsd 与 wifip2pd 之间）→ 恢复 seed baseline；`runtime_enabled_jobs = ["com.apple.bluetoothd","com.apple.wifid"]` |
| `patches/picasso-runtime-jobs-xnu.patch` | before SHA `24358e02…` → `c549e57f…`（55 项 plist）；after SHA **保持** `0e38d793…`；单次删除改 2 次循环（见下）；注释/printf 同步；hunk 头自动重算 |
| `scripts/check-picasso-startup.py` | 断言改为两个 job |

**关键恒等式（本地复算证明）**：从 `offline_disabled_jobs` 删 wifid ≡ 运行期删
wifid，两个 after plist 字节完全相同 → after SHA 不变，只有 before SHA 变。

### 2.1 v10 §5.3 的错误：说「三处，缺一不可」，实际需要第四处

patch 里 `picasso_jobs_enable()` 原本是**一条硬编码 bluetoothd entry + 一次 memmove**：
```c
static const char entry[] = "\t<key>com.apple.bluetoothd</key>\n\t<true/>\n";
... memmove(...); size -= sizeof(entry)-1;
if (!picasso_jobs_match(bytes, size, after_sha)) { error = EINVAL; goto done; }
```
若只改三处（board.toml + 两个 SHA + check 断言），after 变成「删两条」，
但运行期只删一条 → 字节流永不匹配 after SHA → EINVAL → wifid 仍被禁用，
且 `scripts/tests/picasso-runtime-jobs-host.c` 的 `assert(!run() && is_after())` 会挂。

字节级模拟（`work/fix-wifid-runtime-jobs.py`）：
```
delete-ONE-entry == after plist ? False
delete-TWO-entries == after plist ? True
```
→ 改为 `for (i=0;i<2;++i)` 循环，长度用编译期 `sizeof` 常量（不引入 strlen 依赖）。

### 2.2 patch 工程纪律

- 全部改动由 `work/fix-wifid-runtime-jobs.py` 生成，**hunk 头用脚本自动重算**
  （`-9535,6 +9535,124` / `-9635,6 +9753,7`），不手工维持行数——日志里踩过多次坑。
- git 存 LF、工作区 CRLF（`core.autocrlf=true`）→ 脚本按 LF 读写，最小 diff。
- 内置 6 项自检：SHA==plistlib、字节级 C 循环模拟==after、hunk 头/行数、
  C 括号平衡、checker 语法、TOML 解析。
- 另用真实 `git apply --check` 在构造的 kern_exec.c 骨架上验证通过
  （临时骨架 `work/.patchcheck` 已清理）。

---

## 3. 实机闭环证据（本轮新增）

CI `42973205561`（`fast=true`，单次派发）**success 11m48s**，head `9572b30`。
产物 `picasso-bootimg-580`(36 MB) / `picasso-data-seed-580`(**1.18 MB = 复用**)。
CI 日志 `SEED_ARGS=()` 未带 `--allow-new-seed` 且 seed 复用成功 →
**seed 身份确实未变，不需要刷 5.87 GB userdata**（v10 §6 那条路可放弃）。

`out/full-flash/boot-exp-wifidfix.img` = CI 产物逐字节复制（sha `803a0238d6796d12`），
零参数修改；`SECS=400 bash work/boot-variants.sh wifidfix`。
日志 `out/full-flash/logs/console-exp-wifidfix.cap`（457852 B / 5556 行）。

| checkpoint | m1（坏） | **wifidfix（修后）** |
|---|---|---|
| `PICASSO CDC PANIC BEGIN` | **1** | **0** |
| `offline scratch Data mount failed` | 有 | **无** |
| `PICASSO_DATASTORE overlay` | `open=4 header=-1 present=0` | **`open=0 header=0 present=619151`** |
| `DATASTORE ready` | `0` | **`1 direct=1 failed=0`** |
| `NATIVE_OWNER_DATA done` | `stage=0 error=2` | **`stage=4 error=0 path=/dev/disk5s1`** |
| `Early boot complete` | — | **有（行 1405）** |
| SpringBoard / FBUC | — | **pid=26 起来、FBUC 正常** |
| `en0` | — | **attached, link up（行 3131）** |
| HIF 心跳 | — | **活到 `polls=187000`（采集全程 400s）** |

默认参数已含 `wifi=1` + 四个显示必需参数；`wlan_delay_ms` 默认 150000
（`HakiosCompat.cpp:386`），无需打槽。

---

## 4. 本轮新发现（重要）：`PICASSO_JOBS error=22` 是独立的遗留阻塞点

**这不是本轮修复的回归**：m3 基线 `console-exp-m3.cap:1015` 也是
`PICASSO_JOBS bluetoothd enabled=0 changed=0 error=22`。
统计 `out/full-flash/logs/*.cap` 全部 **100 份**：
```
100  PICASSO_JOBS ... enabled=0 changed=0 error=22
  0  PICASSO_JOBS ... enabled=1
```
即 **v10 §5.2/§5.3 假设的 "runtime_enabled_jobs 会放行 wifid" 在真机上
从未成功过**。本轮 wifidfix 也是 `enabled=0 changed=0 error=22`，
且日志中 `wifid` 零出现（`configd` pid=34、SpringBoard 都在）。
→ **「设置里 Wi-Fi 开关」仍未打通，这正是它的阻塞点。**

### 已排除（有证据，不是猜）
- seed 侧 plist 内容正确：CI `linux-inputs/data-audit.json` 明确记录
  `db/com.apple.xpc.launchd/disabled.plist` 的 `sha256 = c549e57f…`
  （= 本轮 before SHA），从 `data.dbrdisk` 提取的实际字节 + 末尾换行
  SHA 也完全一致。
- 路径一致：`OFFLINE_POLICY_DIR = 'db/com.apple.xpc.launchd'` vs helper 读
  `/private/var/db/com.apple.xpc.launchd/disabled.plist`。
- 11-07 刷入 userdata 时（`26fdb4f`）的 job 列表是 55 项含 wifid，
  plist SHA 同样是 `c549e57f…`；历史 patch 的 before 也是它。
- helper 循环/entry/SHA 转换逻辑经 host-test 等价状态机验证可通。

### 时间线取证：`error=22` 从「Data 全新、SHA 完全匹配」时就有

按 mtime 排序，含 `PICASSO_JOBS` 的最早日志是 **11-08 23:52 `console-exp-d2.cap`**，
结果已是 `enabled=0 changed=0 error=22`；此后 100/100 全部相同。
设备 userdata 是 11-07 刷入的（`26fdb4f`：55 项含 wifid，plist=`c549e57f…`），
而 11-08 那版 patch 的 before=`c549e57f…`、after=`7853da13…`——
**当时 Data 全新、plist 内容、before、after 三者完全自洽**，
理论上 helper 应删掉 bluetoothd 并成功。但它仍返回 22。

由此可排除（有证据，不是猜）：
- `!match(before_sha)`：sha 相同；
- `!match(after_sha)` 与 entry 找不到：11-08 版只删一条固定 entry，
  plist 里确实有该 entry，删完正是 `7853da13…`；
- 「设备持久 Data 的历史 plist 内容与 seed 漂移」：**首次开机就失败，来不及漂移**。

`EINVAL(22)` 在 helper 里的**唯一**来源是
`picasso_jobs_metadata()` 的 `if (vnode_vtype(vp) != type) error = EINVAL;`
（uid/gid/mode/nlink 失败返回的是 `EPERM=1`，与观测的 22 不符）。
helper 用 `VNODE_LOOKUP_NOFOLLOW` 依次检查
`/private/var`、`/private/var/db`、`/private/var/db/com.apple.xpc.launchd`
必须是 `VDIR`——iOS Data 卷上 `/private/var` 是 firmlink/符号链接，
不满足 `VDIR` 极可能就是根因。`picasso-runtime-jobs-host.c` 里
`bad==0 nodes[n].type=9` 这条注入测试恰好覆盖了 `vnode_vtype` 不匹配
失败路径，与观测一致。

**但这是「唯一与全部现有证据自洽的分支」，不是实机确认**——
现有日志只有一行汇总打印，没有分步诊断。下一步必须先取证再改。

### 下一步（先取证再改，别猜）
在 `picasso_jobs_enable()` 的 `done:` 前加**一次性诊断打印**（不改逻辑、不改 seed）：
```
size、metadata 阶段 error、before/after match 结果、读到的前 16 字节 hex
```
用同一 `-f fast=true` 派一次 CI、刷一次 boot，即可一锤定音落在哪个分支；
然后针对性修（若属 2/3 → 让 helper 接受设备历史 plist 并幂等按 key 删除，
需同步改 `check-picasso-startup.py` 与 host test 的白名单断言）。
**不要直接放宽 SHA 白名单**——它承担 "unknown contents are preserved" 的安全性质。

---

## 5. 磁盘清理（用户授权，已完成）

- C 盘 2.31 GB → **6.0 GB 空闲**。
- 回收站 3.37 GB / 25645 文件：先解析 `$I` 元数据确认用户个人文件
  （`robotdog/*.step`、`笨笨/*.py`、Sonify dex）的 `$R` 数据体**早已不存在**
  （纯孤儿元数据），现存数据全是本项目可再生产物（boot*.img 1.8 GB、
  kernel.macho 794 MB、CI json/log 804 MB）。`Clear-RecycleBin` 对按目录树
  移入的 `$R` 目录无效 → 分批（≤10/批）永久删除，**最终 0 文件 0 字节**。
- 工作区再删 668.6 MB 可再生产物（旧 CI run、verify-control、work/tmp、
  临时骨架）。清单：`work/cleanup-manifest-2026-11-11.json`。
- **完整保留**：`handoff/`、两套 memory 全 13 份日志、
  `out/full-flash/logs/*.cap` 全部串口证据、`boot-exp-m3.img`、
  `src/`、`upstream/`、`work/refs/`、`work/trogdor/`、全部脚本与参考项目。

---

## 6. 关键命令 / 文件速查

| 用途 | 路径 / 命令 |
|---|---|
| 修复脚本（可复现、自带 6 项自检） | `work/fix-wifid-runtime-jobs.py` |
| 本轮上机日志 | `out/full-flash/logs/console-exp-wifidfix.cap` |
| 本轮镜像 | `out/full-flash/boot-exp-wifidfix.img` |
| CI 产物 | `out/run-42973205561/boot/boot.img` |
| 刷机+采集 | `SECS=400 bash work/boot-variants.sh <TAG>` |
| 回 fastboot | `python work/serial-cmd.py COM5 reboot-fastboot 3` |
| 派 CI | `gh workflow run compose-bootimg.yml -R tselongson/UniXNU-picasso --ref kd439-pool -f fast=true` |

**铁律**（沿用）：一次只派一个 CI；`fast=true` 复用 seed；patch 必须 LF 且
脚本改 hunk 头；单变量 A/B + 控制组；判据是 checkpoint 明文不是 0 panic；
commit message 不写未实机验证的结果。

---

## 7. 下一步优先级

1. **§4 诊断**：加一次性打印 → CI → 上机，钉死 `error=22` 的分支。
2. 按诊断结果修 `picasso_jobs_enable()`，让 wifid 真正放行；
   验收 = 串口出现 `wifid` 的 `SCAN_REQ`/`SCAN_RESULT` 且返回真实 BSSID。
3. `configd` 对 `en0` 发 DHCP，看是否拿到 `inet`。
4. 凭据持久化（`wpass=` 从 boot-arg 挪到 wifid/钥匙串）。
5. 已知限制：数据面 TX 槽无锁（极端并发可能撞槽），有帧损坏证据再处理。
