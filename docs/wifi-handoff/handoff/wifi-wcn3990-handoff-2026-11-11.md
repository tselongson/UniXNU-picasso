# WiFi (WCN3990) 项目交接文档 — 2026-11-11 14:00（v10）

> **⚠️ 本文件已归档。当前权威状态源是
> [`wifi-wcn3990-handoff-2026-11-11-v11.md`](wifi-wcn3990-handoff-2026-11-11-v11.md)。**
> v11 完成并实机闭环了「加 WiFi 就开不了机」的修复（`9572b30` / CI `42973205561`），
> 并纠正了本文 §5.3「三处缺一不可」的遗漏（实际需要第四处 C 循环改动），
> 另发现 `PICASSO_JOBS error=22` 在全部 100 份历史日志中 100% 失败——
> 本文 §5.2 依赖的 "runtime 放行 wifid" 从未在真机生效。
> 以下保留为历史与推理记录。

> ~~**本文件是当前唯一权威状态源。**~~ `README.md`、`docs/*.md`、
> `wifi-wcn3990-handoff-2026-11-10.md`(v9) 及更早版本均为历史归档，冲突时以本文为准。
> 事实来源：`out/full-flash/logs/console-exp-*.cap`（串口原文）、`work/chain-5*.log`、
> `.workbuddy-ai/memory/2026-11-10.md`。
> 判据口径：**checkpoint 明文，不是「0 panic」**（见 `wifi-wcn3990/docs/diagnostic-charter.md`）。

---

## 0. 一句话现状

```
固件启动 ✅ scan ✅ auth ✅ assoc ✅ VDEV_UP ✅ EAPOL ✅
  ── WPA2 四次握手 + 装 PTK/GTK + PEER_AUTHORIZE ✅（chain-580，零参数开机）
  ── 真实数据帧进 Darwin 协议栈 ✅（msdu=21 bad=0，en0 attach + link up）
  ── 显示上屏 ✅（根因是打槽工具吃掉 offline-legacy-validate，已修）
  ── WiFi 开着能开机进系统 ✅（m3：wlan_delay_ms=150000）
  ── 设置里 Wi-Fi ❌（当前可用镜像不含 wifid 放行；含放行的镜像与旧 Data 不匹配）
  ── DHCP / 凭据持久化 ⬜（依赖 wifid 起来）
```

**结论：驱动侧已经完成；剩下的是「设置里能看到并操作 Wi-Fi」这一层用户态。**

---

## 1. 当前设备状态（2026-11-11 13:40 实测）

| 项 | 值 |
|---|---|
| 在跑镜像 | `out/full-flash/boot-exp-m3.img`（= `boot-583.img` / `15da938` + m3 args） |
| 系统 | 已进系统，屏幕正常、可操作（用户 03:09 确认） |
| 串口心跳 | `WCN3990-HIF: heartbeat polls=39555000 stage=3 state=5 htt=0 join=0` |
| WiFi | HIF 已初始化但**不自动连**（`wifiauto` 关闭），这是有意的 |
| PANIC | 0 |

回 fastboot：`python work/serial-cmd.py COM5 reboot-fastboot 3`

---

## 2. 当前可用配置（m3，逐字节可复现）

镜像：`out/full-flash/boot-exp-m3.img`（由 `boot-583-slot.img` 改 args 得到）

```
-v debug=0x14e wdt=0 cpus=8 sheng-smp=1 armcpu-self-ipi=1 armcpu-spi-ipi=1
armcpu-cpufreq=1 sheng-cpufreq=4 picasso_inject_sb=/usr/lib/libpicasso-sb.dylib
picasso_inject_apps=1 amfi_allow_any_signature=1 picasso-wled=0 native-ufs-root=1
sheng-pmgr=2 nointr_consio=1 nointr_iolog=1 picasso_usb=2 picasso_usb_smmu=6
picasso_usb_control_ms=1000 picasso_usb_reset=pshold offline-mobile-graphics-load=1
offline-legacy-register=1 offline-legacy-phase=4 offline-legacy-gop=1
offline-legacy-pm=1 offline-legacy-transfer=1 offline-legacy-cpu-present=1
sheng-fb-gpu=1 picasso-ls-rebuild=1 sheng-staged-apps=1 picasso-aks-state=1
sheng-data-persist=2 picasso-data-init-blank=1 picasso-data-replace-20g=1
offline-hid-input=1 sheng-touch=4 wifi=1 picasso-boot-logo-hold=1
offline-legacy-validate=1 sheng-vsync=60 sheng-banner=0 wlan_delay_ms=150000
```

**四个必须保留的显示/节拍参数**（少一个就黑屏或停 logo）：
`offline-legacy-validate=1`、`sheng-vsync=60`、`sheng-banner=0`、`offline-legacy-gop=1`。

**WiFi 关键参数**：`wifi=1` + `wlan_delay_ms=150000`（`15da938` 的默认值仍是 70000，
`45d577a` 才改成 150000，所以 m3 必须显式带）。

m3 实测（400 s 采集）：

```
PICASSO_FBUC call=   109      verify_swap failed  0
SpringBoard          525      Early boot complete 2
PANIC                  0      HEARD BEACON        0   （wifiauto 关闭，符合预期）
```

---

## 3. 已验收的 checkpoint（全部有串口原文）

| checkpoint | 状态 | 证据 |
|---|---|---|
| 固件启动 / WMI ready | ✅ | chain-580 `WMI: service ready` |
| scan / auth / assoc | ✅ | chain-579 `auth status=0` `assoc status=0` |
| WPA2 四次握手 | ✅ | `WPA: msg3 seen` / `msg4 sent` |
| 装 PTK + GTK / PEER_AUTHORIZE | ✅ | `keys installed` `peer authorized` |
| 真实数据帧进协议栈 | ✅ | chain-580 `msdu=21 bad=0`，`en0` attach + link up |
| 显示上屏 | ✅ | m3 `verify_swap failed 0` / `FBUC 109` |
| WiFi 开着能进系统 | ✅ | m3（用户确认） |
| 设置里 Wi-Fi 开关 | ❌ | 见 §5 |
| DHCP 拿到地址 | ⬜ | 依赖 wifid；`configd` 已跑但没对 en0 发 DHCP |
| 凭据持久化 | ⬜ | 现在仍是 boot-arg 明文 `wpass=` |

---

## 4. 11-10 夜 ~ 11-11 凌晨定位的四个根因（按发现顺序）

### 4.1 显示「卡住」：打槽工具吃掉了 `offline-legacy-validate`

`work/make-545.py` 原来把 `offline-legacy-validate=1` 当**可替换槽位**，
换成 `joinskip=0`。而 `NativeLegacyDisplay::validateSwap()` 在
`enabled != 1` 时直接 `return false` → SpringBoard 每帧 `verify_swap failed`
→ 一帧都上不了屏。chain-546 起所有打槽过的镜像全部中招，
之前 WiFi 崩得早、没走到用户态，所以没人看到。

对照（同一台设备）：

| 组 | 配置 | verify_swap failed | 结果 |
|---|---|---|---|
| g7 | 旧基线 437 + `wifi=0` | 0 | 进桌面 ✅（用户确认） |
| g8' | 当前 580 + `wifi=0`，无 validate | 6155 | 黑屏 |
| g9 | 当前 580 + `wifi=0` + validate | 0 | 有 logo、进系统 ✅（用户确认） |
| g10 | 当前 580 + `wifi=1` + wifiauto + validate | 0 | 有 logo、握手完成，但 UI 卡 |

**已修**：`make-545.py` 槽位改用 `picasso-buttons=1`，并加注释禁止再用
`offline-legacy-*` 做槽位。

### 4.2 WiFi 开着会饿死 UI：HIF 初始化与合成器抢 CPU

| 组 | 配置 | FBUC | SpringBoard | 结果 |
|---|---|---|---|---|
| g13 | 当前内核 + 旧完整 args + `wifi=0` | 120 | 226 | 进系统 ✅ |
| g14 | 同 g13，只把 `wifi` 改 1 | 33 | 34 | 进不去（且 WiFi 没连上：`beacons=0 join=0`） |
| k11 | `wlan_delay_ms=120000` | 29 | 34 | 仍饿死 |
| **k12** | `wlan_delay_ms=150000` | **160** | **230** | **进系统 ✅** |

结论：与数据面无关，是 `wcn3990_hif_poll()` 初始化阶段的 CPU 占用。
`netpoll=6`（k8）无效；`wifiauto=0`（k9/k10）仍饿死（HIF 没 READY 也重）。
**150 s 延迟是实测有效的分界**；`HakiosCompat.cpp` 默认已改 150000（`45d577a`）。

### 4.3 「完全没反应」：scan 在 ioctl 上下文里睡 3 秒

`PicassoWlan.cpp picasso_sta_scan()` 末尾 `for (i<30) IOSleep(100);`，
在**调用者 ioctl 上下文**里阻塞 3 秒；`hakios_sta_ask_scan()` 是同步的。
设置里一点 Wi-Fi/扫描就挂住 SpringBoard 和 configd；`wifiauto` 走同一路径，
所以 k13/k14 卡、k12（无 wifiauto）不卡。`PICASSO_IPC SpringBoard ... queued=9`
积压是**后果**，不是原因。

**已修**（`9ead7fc`）：等待改 5×100 ms（500 ms），扫描在 modem 线程继续。

### 4.4 打槽参数重复（工具坑，会污染对照）

g8 第一版生成出 `wifi=1 wifi=0` 同时存在，`PE_parse_boot_argn` 取第一个，
导致「wifi=0 组」实际是 wifi=1。**strip 集合必须包含 `wifi` / `wifiauto` / `wpass`**，
改完要 `print` 出最终 token 列表核对。

---

## 5. 设置里 Wi-Fi 的阻塞点 —— 以及一条不用刷 5.87 G 的便宜路径（关键）

### 5.1 现状与 m1 的 panic 证据

`45d577a` 为了让设置里有 Wi-Fi 后端，把 `com.apple.wifid` 从
`board.toml` 的 `offline_disabled_jobs` **删掉**了。这改了 **seed baseline**，
于是 boot 里内嵌的 Data seed 与设备上的旧 Data 不再匹配：

```
PICASSO_DATASTORE overlay first=25439066 windowBlocks=5243041 open=4 header=-1 present=0
PICASSO_DATASTORE ready=0 direct=0 volatile=0 failed=0 gpt_after=0 bytes=21474836480
PICASSO_DATASTORE refused publication state=4; old contents preserved
NATIVE_OWNER_DATA done stage=0 error=2 readonly=-1 device=ffffffff path=
=== PICASSO CDC PANIC BEGIN ===
fmt: offline scratch Data mount failed: %d @%s:%d
```

`error=2`(ENOENT)、`header=-1 present=0` = 新 seed 在 Data 窗口里找不到自己的头；
store 出于安全**拒绝覆盖旧内容**（`refused publication state=4`），
`native_owner_data_prepare()` 返回错误 → `kern_exec.c` panic。
`picasso-data-init-blank=1` / `picasso-data-replace-20g=1` 都已经在 args 里，**不足以**让它格式化。

→ 所以「改 `offline_disabled_jobs`」这条路必须配 **5.87 GB 的 userdata** 才能开机。

### 5.2 设计意图：`runtime_enabled_jobs` 就是为这件事准备的

`board.toml` 原文：

```
# Seed baseline: keep this list stable for existing persistent Data identity.
# runtime_enabled_jobs below is applied before launchd to both new and restored Data.
runtime_enabled_jobs = ["com.apple.bluetoothd"]
```

`patches/picasso-runtime-jobs-xnu.patch` 原文：

```
/* Update the one approved launchd job before launchd opens its persistent
 * overrides. The seed recipe/UUID stays stable. ... */
static const uint8_t picasso_jobs_before_sha[32] = {...};
static const uint8_t picasso_jobs_after_sha[32]  = {...};
```

机制：在 launchd 读 `disabled.plist` 之前，把内容从 **before SHA** 原子改写成
**after SHA**；before = 全部 `offline_disabled_jobs`，after = 去掉
`runtime_enabled_jobs`。**Data 的 seed recipe/UUID 不变** → 旧 Data 照常挂载。

`scripts/check-picasso-startup.py` 里也写着
`# Keep seed identity unchanged while applying only the approved runtime job release.`
并硬断言 `board['runtime_enabled_jobs'] == ['com.apple.bluetoothd']`。

### 5.3 便宜路径的确切改法（三处，缺一不可）

1. `board.toml`
   - 把 `com.apple.wifid` **放回** `offline_disabled_jobs`（恢复 seed baseline 原样）。
     原文位置已核对：`git show 45d577a^:board.toml | grep -n wifid` → 第 214 行，
     在 `com.apple.wifianalyticsd` 与 `com.apple.wifip2pd` 之间；`45d577a` 就是删了这一行。
   - `runtime_enabled_jobs = ["com.apple.bluetoothd", "com.apple.wifid"]`。
2. `patches/picasso-runtime-jobs-xnu.patch`
   - 重算两个 SHA-256 常量：
     `before = sha256(plistlib.dumps({job: True for job in offline_disabled_jobs}))`
     `after  = sha256(plistlib.dumps({job: True for job in offline_disabled_jobs
                                      if job not in runtime_enabled_jobs}))`
     （`plistlib` 默认 FMT_XML，键序为插入序，与脚本一致）。
3. `scripts/check-picasso-startup.py`
   - 更新 `assert board['runtime_enabled_jobs'] == [...]` 这一行。

**预期收益**：只需 `-f fast=true`（复用 seed）构建 + 刷 boot，
**不需要** 5.87 GB userdata，也不需要重刷 Data。
**必须先验证**：本地跑 `python3 scripts/check-picasso-startup.py`，
它会重算 before/after SHA 并和 patch 里的常量比对，能提前抓错。

> 风险提示：`45d577a` 还同时改了 `hakios.wlan.cpp`（用具体
> `IONetworkInterface` 子类，之前抽象类名永远返回 NULL），这一项与 Data 无关，
> 保留即可。若 §5.3 走通但设置里仍看不到 Wi-Fi，下一个怀疑对象是
> `com.apple.wifid` 依赖的其它 job 是否也在 disabled 列表里。

---

## 6. 备选路径：刷 userdata（磁盘够时）

产物已在 GitHub 上（CI `42875987953` success）：

| artifact | 大小 |
|---|---|
| `picasso-bootimg-579` | 34 MB |
| `picasso-userdata-579` | **5598 MB（十进制 5.87 GB）** |
| `picasso-data-seed-579` | 1 MB |

步骤：

```bash
tools/bin/gh.exe run download 42875987953 -R tselongson/UniXNU-picasso -n picasso-userdata-579 -D out/run-42875987953/userdata
# 产物是 sparse image，放到约定路径：
#   out/userdata/userdata-root.sparse.img
bash work/flash-userdata.sh          # 会覆盖 userdata 分区（用户 11-07 已授权）
# 再刷同一次构建的 boot（打槽后）
```

磁盘需求：zip + 解压 ≈ **12 GB 峰值**。

**当前磁盘（2026-11-11 13:40）**：C: 只剩 **0.91 GB**（全盘唯一分区）。

| 项 | 大小 |
|---|---|
| 回收站 | 3.37 GB |
| 项目目录 `miphone/` | 2.98 GB（`out/full-flash` 475 MB、两个旧 run 各 ~195 MB） |

→ 即使清空回收站 + 删旧 run，也只有约 4.3 GB，**仍然不够**。要走这条路必须先换盘/扩容。

---

## 7. 仓库 / 分支 / CI 清单

### 7.1 开发线 `kd439-pool`（fork: `tselongson/UniXNU-picasso`）

```
9ead7fc  wlan: never block an ioctl for 3 s in the scan path        ← HEAD
45d577a  wlan: enable wifid, defer bring-up past the compositor, publish the interface
15da938  wlan: make the link-service interval tunable (netpoll=<ms>)
5187ba4  wlan: accept decrypted TKIP-group frames (bit28 is not fatal here)
8048649  wlan: a connect request must not be gated on the BSS-table size
bf0e5cf  wlan: fix the wifiauto passphrase self-assignment (passlen=0)
97a55ce  wlan: wifiauto must also default the CE3 rotating TX buffers
50c0e8b  wlan: keep re-scanning until the requested network appears (wifiauto)
b7e350e  wlan: hif_wr_msdu() copies out of the DMA buffer before folding
```

### 7.2 合并线 `wifi-wcn3990-stage12`（本地 worktree `upstream/merge-wifi`）

```
2cb67cd  wifi: export rpmhVoteCryptoPath for the CE doorbell path (patch 05)
22832f9  wifi: add the WCN3990 package on top of stage12/picasso-ios16 (00ea4a0)
00ea4a0  ← 上游基线（silenzio111/UniXNU-picasso: stage12/picasso-ios16）
```

- 5 个 integration 补丁：`01-board-wifi-arg` / `02-build-native-wlan` /
  `03-bsd-init-hook` / `04-native-ufs-raw-read` / `05-rpmh-crypto-export`
- `.gitattributes` 已锁 `wifi-wcn3990/integration/*.patch text eol=lf`
- CI `42842132006` **success**；PR 未发（公开动作，等用户决定）：
  `https://github.com/silenzio111/UniXNU-picasso/compare/stage12/picasso-ios16...tselongson:wifi-wcn3990-stage12`

### 7.3 近期 CI

| run | commit | 参数 | 结果 |
|---|---|---|---|
| `42875987953` | `9ead7fc` | `fast + create_data_seed + build_userdata` | success（14m）→ boot-579 / userdata-579 |
| `42874469710` | `9ead7fc` | `fast` | cancelled（会 panic，主动取消） |
| `42869583781` | `45d577a` | `fast + create_data_seed` | success（25m）→ boot-577 |
| `42859391707` | `15da938` | `fast` | success（13m）→ boot-583（= 当前 m3 的底包） |

---

## 8. 启动参数速查（接手第一件事）

| 参数 | 作用 | 备注 |
|---|---|---|
| `wifi=1` | 启用 WCN3990 | `0` 时整包 inert |
| `wlan_delay_ms=150000` | 推迟 HIF 初始化 | <150 s 会饿死合成器（§4.2）；`45d577a` 起默认已是 150000 |
| `offline-legacy-validate=1` | **swap 校验总开关** | 缺了必黑屏（§4.1） |
| `sheng-vsync=60` / `sheng-banner=0` | 合成器节拍 | 缺了不提交帧 |
| `picasso-boot-logo-hold=1` | 保留静态苹果标 | `0` 会清屏；与「能否进桌面」无关（已证伪两次） |
| `wifiauto=1` + `wpass=<pw>` | 开机自动连 | 会触发 scan 路径；`9ead7fc` 前必卡 |
| `netpath=1` | 把收到的帧交给协议栈 | 默认开；`0` 只做对照 |
| `dpdiag=1` / `wpadiag=1` | 打印被丢帧/握手细节 | 诊断用，日常不要带 |
| `netpoll=<ms>` | 链路服务间隔 | 默认 2；实测 6 无改善（§4.2） |

**打槽工具会吃掉的 token**：`work/make-545.py` 的槽位表（现在用 `picasso-buttons=1`）。
任何改 args 的脚本，strip 集合必须含 `wifi` / `wifiauto` / `wpass` / `offline-legacy-validate`。

---

## 9. 铁律与坑（每条都有日志代价）

1. **一次只派一个 CI**。同一 workflow 连续派单会触发 GitHub 并发取消（chain-577 就这么丢的）。
2. **派 CI 默认只带 `-f fast=true`**，复用 seed。只有改了 `offline_disabled_jobs`
   才需要 `create_data_seed=true`，而且**必须同时** `build_userdata=true` 并刷 Data（§5.1）。
3. **patch 必须 LF**。本机 `core.autocrlf=true`，不锁 `.gitattributes` 会让 `git apply` 静默失败。
4. **单变量**。一次只改一个 token/一个开关，否则像 g8 那样得出反向结论。
5. **不要用 `joinsel=<n>` 做握手实验**：会选到别的 AP，导致 MIC 错、M3 不来（chain-573 的假 bug）。
6. **看到 logo 先确认 swap 有没有过**，再谈 hold。
7. **`verify_swap failed` 计数是上屏的唯一硬指标**，`0` 才算通。
8. 改 C/C++ 后跑括号平衡 + `python3 work/abi-check.py`（0 HARD 才提交）。

---

## 10. 实验链索引（570 → 584）

| 链 | 变量 | 判决 |
|---|---|---|
| 573 | `rxfix=0/1` | M2 被 ACK 但 AP 不回 M3 → 凭据/SSID 错配 |
| **574** | `joinsel=4/1` | x0 四次握手 + 装密钥 + PEER_AUTHORIZE 闭环 |
| 575 | `netpath=0/1` | 两组都选到时钟 AP，无效；`dpdiag` 抓到 `r=3` 重复投递 |
| 576 | `wifiauto`（剥全部实验参数） | `passlen=0`（密码自赋值）+ `tx3=0`（START_SCAN 被覆盖） |
| 579 | `wifiauto` + 修复 | 握手全通；`bad=31`，被丢帧其实是已解密 ARP/IPv6（bit28） |
| **580** | `wifiauto` + bit28 修复 | **`msdu=21 bad=0`，`en0` up，真实帧进协议栈** |
| 581 | `logo-hold=1/0` | 两组几乎相同 → hold 证伪 |
| 582 | g7~g14（wifi/hold/validate 对照） | 定位 `offline-legacy-validate` 被吃 + HIF 饿死 UI |
| 583 | k9~k14（netpoll/delay/wifiauto/netpath） | 150 s 有效；scan 3 秒阻塞是「没反应」真因 |
| 584 | m1/m2/m3 | m1 panic（Data 不匹配）；m3 = 当前可用配置 |

日志：`out/full-flash/logs/console-exp-<tag>.cap`；链脚本 `work/chain-5*.sh`；
单组重跑 `SECS=<秒> bash work/boot-variants.sh <tag>`。

---

## 11. 下一步（按推荐顺序）

1. **走 §5.3 的便宜路径**（约 20 min CI + 1 次刷 boot）：
   board.toml 恢复 `offline_disabled_jobs` 并把 wifid 放进 `runtime_enabled_jobs`
   → 重算 patch 两个 SHA → 改 check 脚本断言 → 本地跑 `check-picasso-startup.py`
   → CI `fast` → 刷 boot → 设置里看 Wi-Fi 开关。
2. 若 wifid 起来但扫描仍卡：确认 `9ead7fc` 的 500 ms 是否生效（看
   `picasso_sta_scan` 前后 `PICASSO_IPC SpringBoard` 是否还积压）。
3. DHCP：`configd` 对 en0 发 DHCP 后，看 `en0` 是否拿到 `inet`；`REAL DATA PLANE` 只在
   `netpath=1` + 用户态触发时出现。
4. 凭据持久化：把 `wpass=` 从 boot-arg 挪到 wifid/钥匙串。
5. 已知限制：数据面 TX 槽无锁（极端并发可能选到同一槽）。暂不加锁以免引入死锁，
   等有帧损坏证据再处理。
6. 若要发 PR：用 §7.2 的 compare 链接。

---

## 12. 关键文件与命令

| 路径 | 用途 |
|---|---|
| `work/make-545.py` | 打槽（改 args 到 995 字节槽），**不要再用 display token 做槽位** |
| `work/boot-variants.sh` | 刷指定 `boot-exp-<tag>.img` 并采集 `SECS` 秒 |
| `work/serial-cmd.py COM5 <cmd>` | 串口命令（`version` / `reboot-fastboot`） |
| `work/chain-58*.sh` | 多组实验链脚本 |
| `work/flash-userdata.sh` | 刷 userdata（需 `out/userdata/userdata-root.sparse.img`） |
| `out/full-flash/logs/console-exp-<tag>.cap` | 串口原文（判读唯一依据） |
| `upstream/silenzio-picasso/` | 开发线 `kd439-pool` |
| `upstream/merge-wifi/` | 合并线 `wifi-wcn3990-stage12` |
