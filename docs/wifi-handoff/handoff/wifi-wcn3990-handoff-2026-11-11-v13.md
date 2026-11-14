# WiFi (WCN3990) 项目交接文档 — 2026-11-11 16:2x（v13）【历史归档】

> **⚠️ 已被 `wifi-wcn3990-handoff-2026-11-11-v14.md` 取代。**
> v14 里阶段 1（放行 wifid）已实机打通：`enabled=1 changed=1 error=0`、
> `wifid` pid=41 启动。本文 §4 的两条排除结论已被实机推翻：
> 真因是设备持久 Data 的 `disabled.plist` 被 launchd 重写（2833 B ≠ seed 2748 B）。
> 本文其余内容（CPU 饿死修复、Data seed 修复、组件清单）仍有效。
>
> ~~**本文件是当前唯一权威状态源。**~~ v12 及更早版本均为历史归档。
> 判据口径：**checkpoint 明文 + 用户可操作性，不是「0 panic」**。

---

## 0. 一句话状态

**「加 WiFi 卡死系统」已经彻底解决并经用户实测确认**：能开机、能进桌面、触摸有反应，
Wi-Fi 驱动同时在线（`HIF_READY` / `en0` link up）。
**「设置里开 Wi-Fi」仍未通**，剩余 **4 个阶段**，其中只有阶段 1 是硬阻塞。

---

## 1. 状态表（2026-11-11 16:2x）

| 层 | 状态 | 证据 |
|---|---|---|
| 加 WiFi 开不了机（Data seed 不匹配 panic） | ✅ 已修 + 实机闭环 | `9572b30` / CI `42973205561` / `console-exp-wifidfix.cap`：0 PANIC、`DATASTORE ready=1`、`NATIVE_OWNER_DATA stage=4 error=0` |
| WiFi 启动后 CPU 被吃满 / UI 卡死 | ✅ **已修 + 实机闭环 + 用户确认** | `171c883` / CI `42981481378` / `console-exp-wifidyield.cap`：见 §2 |
| 系统与 WiFi 并存（不崩、可操作、驱动在线） | ✅ **达成** | 同上：`d1/d2 busy ≤42`、`runnable_ms ≤223`、触摸 131 帧、`polls=55000 state=5` |
| 设置里 Wi-Fi 开关 | ❌ 阶段 1 阻塞 | `PICASSO_JOBS jobs enabled=0 changed=0 error=22`，`wifid` 进程零出现 |
| 接口类型 / IO80211 可见性 | ❌ 阶段 2 未验证 | `IO80211Interface is absent alloc NULL` → 回退 `IOEthernetInterface` |
| DHCP / IP | ⬜ 阶段 3 | 全部日志无任何 IP/DHCP 证据 |
| 设置界面 / 凭据持久化 | ⬜ 阶段 4 | 依赖前 3 阶段 |

---

## 2. 本轮实机验证（决定性，用户已确认「能进桌面，有反应」）

镜像 `out/full-flash/boot-exp-wifidyield.img` = CI `42981481378` 产物**逐字节复制**
（commit `171c883`，零参数修改，`sha 894997344e8aca5c`）。
采集 `out/full-flash/logs/console-exp-wifidyield.cap`（468498 B / 5877 行 / 400 s，HIF 启动于 t+168 s）。

| 判据 | 修复前 `console-exp-wifidfix.cap` | 修复后 `console-exp-wifidyield.cap` |
|---|---|---|
| `PICASSO_CPUFREQ` d1 `busy`（Wi-Fi 启动后） | **恒 100** | 最大 **42**（多数 17~25） |
| 同上 d2 `busy` | **恒 100** | 最大 **42** |
| `PICASSO_THR` 最大 `runnable_ms`（Wi-Fi 启动后取样） | **121081 ms** | **223 ms**（↓540×） |
| 非零 `runnable_ms` 线程数 | 88 | 33 |
| `TOUCH d_hid`（Wi-Fi 启动后，10 s 窗口增量） | **0 / 0 / 0 …** | **26, 26, 29, 50（合计 131）** |
| `TOUCH d_frames` | 0 | 29、50（两窗口） |
| `PANIC BEGIN` | 0 | 0 |
| `DATASTORE ready` / `NATIVE_OWNER_DATA` | 1 / stage=4 error=0 | 1 / stage=4 error=0 |
| `en0` | attached, link up | attached, link up |
| `WCN3990-HIF` | polls=187000 | polls=55000, `stage=3 state=5`（`HIF_READY`） |

**两组 census 都发生在 Wi-Fi 启动之后**（`wifidyield` census 在行 4026/5565，启动在行 3050），
所以是同位置可比数据，不是窗口错位。`TOUCH` 是窗口增量不是累计（`CpuFreqProbe.cpp:356-368`），
修复前一帧都没有，修复后用户手动滑动即产生 29/50 帧。

**注意：`beacons=0 / join=0 / msdu=0` 在本轮属正常** —— 用户态没有 wifid 去发扫描/关联请求，
驱动处于「固件就绪但无人驾驶」的空转状态。这不代表数据面坏了（见 §5 的完整链路证据）。

---

## 3. 修复内容（`171c883`）

`PicassoWlan.cpp` 把 `IOSleep` 重定义成 `picasso_sleep_ms()` → `machine_delay_until()`
（XNU **纯自旋**分支）。两个**无限**循环用它，各钉死一个大核：

| 循环 | 条件 | 每轮 |
|---|---|---|
| `modem_thread` GLINK watch | `glink_ms` 默认 **0** ⇒ `for(ms=0; glink_ms==0 \|\| ms<glink_ms; ms++)` 永不退出 | `glink_poll` + `picasso_wlan_ce_poll` + `picasso_usb_console_drive_poll` + `IOSleep(1)` |
| `modem_monitor` | `for(;;)` | `IOSleep(500)` |

修法：
- 在 `#undef IOSleep` **之前**用 `picasso_yield_ms()` 捕获原生 `IOSleep`
  （宏之后定义会被展开成自旋版本 → 修复变 no-op，脚本有顺序自检）。
- 两处忙等改原生 `IOSleep`（`delay_for_interval → clock_delay_until →
  assert_wait_deadline + thread_block`，阻塞让核）。
- GLINK tick 1 ms → **4 ms**（超过 `ml_delay_should_spin` 阈值，轮询量降 4 倍，
  CE ACK 仍在 CoachZ 的几 ms 服务窗口内）。
- 微秒级硬件握手 `IODelay(10/100)` 保持自旋不变。
- `wlan_spin=1` 可恢复旧忙等，供单变量 A/B（本轮未跑，默认路径即修复路径）。

脚本 `work/fix-wlan-cpu-starve.py` 可复现；`work/abi-check.py` 0 HARD。

**为什么「把 WiFi 推迟到进锁屏之后」注定无效**：推迟只改变忙等循环**开始**的时刻，
不改变它是无限忙等。这是本轮定位的关键，也是用户第 ② 条现场观察的答案。

---

## 4. 剩余 4 个阶段（按阻塞顺序）

### 阶段 1（唯一硬阻塞）：放行 `wifid`

- **现状**：`PICASSO_JOBS jobs enabled=0 changed=0 error=22`（EINVAL）。
  **全部 100 份历史 cap 100% 是 error=22、0 份 enabled=1**；本轮 `wifid` 字符串 0 次出现。
  设置里的 Wi-Fi 开关由 wifid/airportd 提供，它们没起 → 开关点不动。
- **已排除**（零成本静态取证）：
  seed 里 `disabled.plist` 的 sha256 = `c549e57f…` = patch 的 `before_sha`（`data-audit.json` + 从 `data.dbrdisk` 提取字节复算一致）；
  路径 `OFFLINE_POLICY_DIR='db/com.apple.xpc.launchd'` 与 helper 读取路径一致；
  最早一份 cap（11-08 23:52 `console-exp-d2.cap`）在 Data 全新、before/after/plist 三者自洽时就已经是 error=22
  ⇒ 排除「内容漂移 / before-after 不匹配 / entry 找不到」。
- **最强候选**：`picasso_jobs_metadata()` 里 `if (vnode_vtype(vp) != type) error = EINVAL;`
  ——helper 用 `VNODE_LOOKUP_NOFOLLOW` 要求 `/private/var`、`/private/var/db`、
  `/private/var/db/com.apple.xpc.launchd` 都是 VDIR，而 iOS Data 卷上 `/private/var`
  是 firmlink/符号链接。（uid/gid/mode/nlink 失败是 EPERM=1，不是 22，可区分。）
- **要做的（下一步，先取证再改）**：在 helper 的 `done:` 前加**一次性分步诊断打印**
  （每个 vnode 的 vtype/uid/gid/mode/nlink、metadata error、before/after match 结果、
  前 16 字节 hex），一次 `fast=true` CI + 一次上机即可定位到唯一分支，再针对性修。
  **不要直接放宽 SHA 白名单**（它承担 "unknown contents are preserved" 的安全性）。

### 阶段 2：接口类型与 IO80211 可见性

- **现状**：`HakiosWLAN: class IO80211Interface is absent alloc NULL`
  → 回退 `IOEthernetInterface`（`hakios.wlan.cpp:123` 的 candidates 顺序）。
  `en0` 已 attach / link up，控制面 shim 已实现（`hakios.apple80211.c` 739 行，
  power/scan/join/SSID/BSSID/RSSI/channel/security/link 全部有，走 PF_SYSTEM socket）。
- **要做的**：wifid 起来后确认它能否凭 `IO80211InterfaceRole` 属性认到这块网卡
  （驱动在 `hakios.wlan.cpp:218` 设了该属性）；认不到就要让 `IO80211Interface` 类真正注册。
- 风险：中。可能只需属性/注册顺序，也可能要改 family 绑定。

### 阶段 3：IP 层（DHCP）

- **现状**：所有日志无任何 DHCP/IP 地址证据。
  内核侧收发都已接进 Darwin 栈：收 `hif_wr_msdu → hakios_net_input → ifnet_input`，
  发 `net_output → link->transmit → HTT TX_FRM`。
- **要做的**：阶段 1、2 通后看 configd / IPConfiguration 是否发 DHCP；
  必要时补 `ifnet` 的 `SIOCSIFADDR` / 事件上报路径。

### 阶段 4：设置界面 + 凭据持久化

- 扫描列表、输密码、自动加入、重启后记住（keychain / preferences）。
- 前 3 阶段通了这一步主要是调通，不是重写。

### 非阻塞但需留意

- `PICASSO_IPC Preferences pid=0 not running`：设置 App 未起，需从桌面手动点开。
- 本轮 `EXC_CORPSE_NOTIFY` 58 次 vs 基线 23 次（`m3`=23 / `k12`=23）：进程异常上报失败，
  未见对应服务崩溃主因，需确认是否新引入（暂不影响可操作性）。

---

## 5. 组件清单：驱动/固件侧已全通（有实机 checkpoint）

`console-exp-w1.cap` 是完整链路的存档证据（`join=5, eapol=4, msdu=4, bad=4/0`），逐项：

| # | 组件 | 状态 | 证据 |
|---|---|---|---|
| 1 | WLFW 固件下载 / `HIF_READY` | ✅ | `state=5`（`enum hif_state` 末态） |
| 2 | CE 环 / HTC transport | ✅ | `stage=3`，`drri` 轮转 |
| 3 | WMI INIT / READY / 资源池 | ✅ | `WMI: INIT full Linux cfg vdevs=4 peers=33 tids=66 msdu=1056 … ast=16` |
| 4 | HTT AGGR_CFG | ✅ | `HTT: send AGGR_CFG ampdu=64 amsdu=6` |
| 5 | 扫描（beacon/probe） | ✅ | `beacons=62`、`mgmtev=62` |
| 6 | Auth / Assoc（WPA2-PSK + RSN group cipher 修复） | ✅ | `assoc status=0 aid=16` |
| 7 | VDEV_START / VDEV_UP | ✅ | `join=5` |
| 8 | EAPOL 四次握手 | ✅ | `eapol=4`，`EAPOL frame 113 bytes ver=1 type=3 … info=008a` |
| 9 | 装 PTK/GTK（TKIP GTK + CCMP PTK） | ✅ | `hif_wpa_handle_eapol()` 路径 + 解密后帧可见 LLC/SNAP |
| 10 | PEER_AUTHORIZE | ✅ | `hif_join_peer_param(3,1)`（`Wcn3990Hif.cpp:3155/4220`） |
| 11 | RX 数据面 → Darwin 栈 | ✅ | `hif_wr_msdu → hakios_net_input → ifnet_input`；`msdu=4` |
| 12 | TX 数据面（HTT TX_FRM + CE4 gather） | ✅ | `net_output → link->transmit`（`hakios.net.c:147`） |
| 13 | `en0` attach / link up | ✅ | `net: en0 attached on Picasso WCN3990 Wi-Fi (a radio), link up` |
| 14 | Apple80211 控制面 shim | ✅ 已实现 | `hakios.apple80211.c`（739 行） |
| 15 | 用户态 wifid / airportd | ❌ 阶段 1 | 见 §4 |

---

## 6. 关键文件 / 命令 / 铁律

| 用途 | 路径 |
|---|---|
| 本轮修复脚本 | `work/fix-wlan-cpu-starve.py` |
| v11 修复脚本（Data seed / runtime jobs） | `work/fix-wifid-runtime-jobs.py` |
| 卡死证据（修复前） | `out/full-flash/logs/console-exp-wifidfix.cap` (+`.times`) |
| 修复后验证证据 | `out/full-flash/logs/console-exp-wifidyield.cap` |
| 完整链路存档 | `out/full-flash/logs/console-exp-w1.cap` |
| 实时取证 | `python work/serial-cmd.py COM5 status 8` |
| 刷机采集 | `SECS=400 bash work/boot-variants.sh <TAG>` |
| 派 CI | `gh workflow run compose-bootimg.yml -R tselongson/UniXNU-picasso --ref kd439-pool -f fast=true` |

铁律：
1. 一次只派一个 CI，`fast=true` 复用 seed（`SEED_ARGS=()`，不带 `--allow-new-seed`）。
2. **采集窗口必须长于 168 s**（HIF 启动点），否则会得到「能进系统」的假阳性
   —— 这正是历史上 k12/g7 被误判的原因。
3. 判据是 checkpoint 明文 + **用户可操作性**，不是 0 panic、不是「进程还在」。
4. 单变量 A/B + 控制组；`wlan_spin=1` / `netpath=0` 这类门控保留。
5. 假判据黑名单：`PICASSO_FBUC` 有 160 次打印上限；`GPU fence=2` 全组恒定；
   `PICASSO_IPC/THR/STK` 是 `status` 命令触发的诊断输出（内容有效，有无不算判据）。

---

## 7. 下一步（建议顺序）

1. **阶段 1 取证**：helper 加分步诊断打印 → 一次 CI → 一次上机 → 定位 EINVAL 分支 → 针对性修。
   （已排除的三项不要再查；候选已压到 `picasso_jobs_metadata()` 的 vnode 类型判断。）
2. 阶段 1 通了立刻看 `wifid` 是否起、`IO80211Interface` 是否还 absent（阶段 2）。
3. 再跑一次真实 AP 的扫描/关联（用户态驱动），确认 `beacons/join/msdu` 从用户路径产生。
4. 阶段 3 DHCP → 阶段 4 设置界面/凭据。
