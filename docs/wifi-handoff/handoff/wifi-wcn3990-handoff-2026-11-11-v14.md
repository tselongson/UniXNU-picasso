# WiFi (WCN3990) 项目交接文档 — 2026-11-11 21:4x（v14）

> **本文件是当前唯一权威状态源。** v13 及更早版本均为历史归档。
> 判据口径：**checkpoint 明文 + 用户可操作性，不是「0 panic」**。

---

## 0. 一句话状态

**驱动侧 WiFi 已全线打通（实机验证）**：wifiauto 单变量判决显示
auth→assoc→VDEV_UP→EAPOL 四次握手→PTK/GTK 安装→PEER_AUTHORIZE 全通过，
心跳 `msdu=53 ord=52 eapol=3` 持续推进、HTT TX 被 AP ACK、
peer map 首次到达。阶段 1（wifid 启动）、阶段 2（RSSI_BOUNDS/DFS 能力）
实机闭环；**down 方向 link 通知链已在实机验证**（用户关 WiFi 触发
`net: en0's link is down` + `the station says: the link, 0`）。

**DHCP 不发起的根因已修复两版、第二版待上机**：
1. `a70d4a5`：`hakios_net_link_state()` 无调用者 → 装 key 发 up /
   reset 发 down。实机验证：通知链路工作，但被下一层挡住。
2. `8703f5b`（CI `43031770138`）：`PicassoWlan.cpp:3978` 预置
   `.up = true` → `hakios.net.c:395` 拷进 n->up → 幂等检查
   （up==n->up）把 up 通知挡掉。改为 `.up = false`，出生如实
   link down，握手成功才真正拉起。

**设置 App Wi-Fi 详情卡死 = Preferences 进程内卡死**（不是驱动）：
点详情窗口里驱动心跳正常推进、**wifid 零新 ioctl** ——
Preferences 等系统 Wi-Fi 服务（XPC/IO80211 user client）无响应。
属阶段 4，DHCP 之后再处理。

---

## 1. 状态表（2026-11-11 21:4x）

| 层 | 状态 | 证据 |
|---|---|---|
| 加 WiFi 开不了机 / UI 卡死 | ✅ 已修 + 用户确认 | `9572b30` / `171c883` / `console-exp-wifidyield.cap` |
| **阶段 1 放行 wifid** | ✅ 打通 | `46cbb07`+`26d995a` / `console-exp-launchd.cap`：`enabled=1 changed=1 error=0`，wifid pid=41 |
| **阶段 2 能力探测** | ✅ 打通 | `ed07b14` / `console-exp-capfix.cap`：106/254/207 全 answered |
| **关联+加密+数据面** | ✅ **打通** | `console-exp-autojoin.cap` + `linkup.cap`：EAPOL 四步、keys installed、`msdu=53 eapol=3`、`peer mapped` |
| **link 通知链** | ✅ down 方向实机验证 | `live-linkup-userops2.cap`：`net: en0's link is down` + `the station says: the link, 0` |
| **阶段 3 DHCP / IP** | 🔶 `8703f5b` 待上机 | 判据：`link is up` → `SIOCSIFADDR` → DHCP → `inet` |
| 阶段 4 设置 UI / 凭据持久化 | ⬜ 未做（卡死已定性） | Preferences 进程内等系统 Wi-Fi 服务无响应 |

### 1.1 数据面判决原文（`console-exp-autojoin.cap`，wifiauto=1，400 s）

```
WCN3990-HIF: wifiauto: unattended connect for 'ChinaUnicom-33E6TD'
JOIN auth resp status=0 / assoc resp status=0 aid=24 / vdev up sent (bssid=94:32:c1:ad:33:e8)
JOIN: EAPOL frame 113 bytes ... / WPA: msg1 seen, msg2 sent (ANonce=...)
JOIN: EAPOL frame 193 bytes ... / WPA: install pairwise key idx=0 len=16 cipher=4
WPA: install group key idx=1 len=32 cipher=2
WPA: msg3 seen, msg4 sent, keys installed, peer authorized
JOIN: peer mapped id=104            <- 历史 100% timeout 的它首次到达
WPA: tx compl status=0 count=1 ack=3 noack=0   <- HTT TX 被 AP ACK
heartbeat polls=34000 ... rx=711 beacons=372 htt=31 httidx=1044
  mgmt=12 mgmtev=382 join=5 ind=0 ord=21 msdu=22 eapol=3
```

**但 `SIOCSIFADDR` / DHCP / KEV 事件为 0** → 见 §5 根因。

---

## 2. 本次三个提交（kd439-pool，均已推 fork）

| commit | 内容 | 实机判决 |
|---|---|---|
| `46cbb07` | 目录 metadata 允许跟随 `/private/var` firmlink + 一次性分步诊断 | CI `42990096910`：`meta=0/0/0/0 vt=2/2/2/1`（4 个 vnode 检查全过） |
| `26d995a` | 接受 launchd 重写后的 plist，按 key 幂等删除两个 job | CI `42996741025`：**`enabled=1 changed=1 error=0`，wifid pid=41** |
| `ed07b14` | 补 `RSSI_BOUNDS(106)` + DFS/12/13 信道 + configd 的 SIOCSIFADDR 可见 | CI `43001859584` / `capfix.cap`：106 answered |
| `a70d4a5` | 装 key/open AP 发 KEV_DL_LINK_ON + HAKIOS_STA_LINK；断开撤回 | CI `43027833370` / `linkup.cap`：通知链路工作，**但被 `.up=true` 预置挡住**（SIOCSIFADDR 仍 0） |
| `8703f5b` | **radio `.up = true → false`**：出生如实 link down，握手才拉起 | CI `43031770138`，**待上机** |

### 2.1 被实机推翻的两条交接假设（重要，别再重查）

1. **v13 §4 说「最强候选 = `vnode_vtype(vp) != VDIR`（/private/var 是 firmlink）」**
   → 方向对，但只是第一层。`46cbb07` 修完后
   `PICASSO_JOBS diag meta=0/0/0/0 vt=2/2/2/1 rd=0 size=2833
   head=3c3f786d6c207665 before=0 after=0 e=-1/-1`：
   4 个 vnode 全过、能读到文件，但 `error=22` 仍在。
2. **v11/v13 说「已排除 before/after SHA 不匹配」**
   → **错的**。那条结论来自时间线推理，从未真正读过设备上那份文件。
   设备持久 Data 的 `disabled.plist` 是 **2833 B**，seed before 是 **2748 B**，
   `before=0 after=0`。真因是 **内容漂移**（launchd 改写）。

### 2.2 `26d995a` 的正解来源（参考其他机型，非猜测）

`Trogdor-ipadOS/platform/coachz-prelink/patches/xnu/coachz-prelink-watchdog-replace.patch`
明确注释：「**launchd rewrites disabled.plist in its own XML form while it runs**」，
并给出「按 key 幂等删除 / 其余字节原样保留」的做法（CoachZ 是要保持一个 job
禁用，我们要放行两个 job，方向相反但方法同源）。

我们的实现：
- 保留 seed 的 before/after SHA **精确快速路径**；
- 否则要求 root-owned 的合法 XML plist，只删 bluetoothd/wifid 两个
  `<key>…<true/>` 条目，**其余字节逐字保留** —— 比整文件 SHA 白名单
  **更强地**保证 "unknown contents are preserved"，不是放宽；
- `<false/>` 视为已启用跳过；两个 key 都不在则 `changed=0` 幂等成功；
- 回读校验按路径分别用 SHA / 结构+key 缺席；
- 诊断加 `form=`/`removed=`/96 字节可打印 head。

---

## 3. 实机闭环证据（`console-exp-launchd.cap`，400 s）

```
PICASSO_JOBS diag meta=0/0/0/0 vt=2/2/2/1 rd=0 size=2796
    head=3c3f786d6c207665 before=0 after=0 e=-1/-1
PICASSO_JOBS form=2 removed=1 head=<?xml version="1.0" encoding="UTF-8"?>...
PICASSO_JOBS jobs enabled=1 changed=1 error=0
PICASSO_EXEC ok pid=41 path=/usr/sbin/wifid
HakiosWLAN: class IO80211Interface is absent alloc NULL
HakiosWLAN: interface allocated as IOEthernetInterface
net: en0 is asked 0xc02869c9 by wifid          (SIOCGA80211)
apple80211: wifid gets 12 (CARD_CAPABILITIES) ... answered
apple80211: wifid gets 19 (POWER) ... answered
apple80211: wifid gets 254 (HW_SUPPORTED_CHANNELS) ... answered
apple80211: wifid gets 207 (CHANNELS_INFO) ... answered
apple80211: wifid gets 96 (?) / 97 (?) / 106 (RSSI_BOUNDS) ... REFUSED
WCN3990-HIF heartbeat: rx=13 beacons=0 join=0 msdu=0 eapol=0
```

`wifid` 没有退出，是周期性能力查询；**但一条 `sets` 都没有**，
即从未下发 `SCAN_REQ(10)` / `ASSOCIATE(20)`。

---

## 4. `ed07b14` 的依据（参考机型实机证据）

`Trogdor-ipadOS/AGENTS.md:46`：

> without APPLE80211_IOC_RSSI_BOUNDS (106, {version, min, max, threshold})
> that threshold is 0, every network is "Below minimum RSSI threshold" …
> **With DFS channels in the table and the bounds answered, a saved network
> joins at boot with no Settings open and the icon fills.**

`CoachzWifiNet.cpp:419` 确切值 `{1, -95, -40, -85}`（16 B）；
`:427-434` 确切信道表（2.4G 1-11 active、12/13 passive、
5G 36/40/44/48 与 149-165 active、52-64/100-144 DFS 标 `0x100`）。

**96/97 仍是未知请求，保持 REFUSED**：本驱动头文件注释明确禁止
「用零应答冒充真值」（零值框架无法区分真伪）。它们已带 type/length
写入 boot log，下一轮可从日志识别，不要凭空补零。

---

## 5. DHCP 根因链（两版修复，`a70d4a5` → `8703f5b`）

**第一层（`a70d4a5` 修）**：`hakios_net_link_state()`
（`hakios.net.c:299`，仓库里唯一的 KEV_DL_LINK_ON 开关）**全仓库无调用者**；
`HAKIOS_STA_LINK` 事件只由 `hakios.sta.c` 的 ioctl 路径发出，wifiauto
绕过它。修复：装 key 成功后发 `link_state(true)` + `apple80211_event(3,1)`；
open AP 分支补 `g_link_up=1` + 同样通知（该路径原本连 g_link_up 都不置位）；
`hif_join_reset()` 发 down。hakios.net.c 内部幂等。

**实机判决（CI `43027833370` / `linkup.cap`）**：通知链路本身工作
—— `keys installed ... link up` 出现、msdu=34 进栈、0 PANIC；
**但 `SIOCSIFADDR` 仍 0**。第二层根因实锤：
`PicassoWlan.cpp:3978` 预置 `.up = true` → `hakios.net.c:395
n->up = link->up` → 接口出生谎报 link 在上 → up 通知被幂等
（`up == n->up`）挡掉 → KEV_DL_LINK_ON 从未到达 configd。
`net: en0 so far 0 frames sent, 21 received` —— 帧进栈、无 DHCP 发出。

**down 方向已在实机验证**（用户在设置里的操作触发了）：
```
net: en0's link (Picasso WCN3990 Wi-Fi) is down
apple80211: the station says: the link, 0
```

**第二层修复（`8703f5b`，CI `43031770138`）**：radio 的
`.up = true → false`（一行）。attach 时如实打印 link down，
四次握手成功时 `hakios_net_link_state(true)` 才真正发出
KEV_DL_LINK_ON → configd 建 IPv4 服务 → DHCP。
符合头文件契约「else: wait for hakios_net_link_state()」。

**上机判据（checkpoint 明文）**：
```
net: en0 attached on Picasso WCN3990 Wi-Fi (a radio), link down   <- 出生如实
WPA: msg3 seen, msg4 sent, keys installed, peer authorized, link up
net: en0's link (Picasso WCN3990 Wi-Fi) is up                     <- 真正拉起
net: en0 SIOCSIFADDR (configd/DHCP path)                          <- configd 响应
（DHCP discover/offer 或 tx 计数推进）→ inet 地址
```

---

## 5.1 设置 App Wi-Fi 详情卡死（已定性，阶段 4）

用户实机报告「设置主页点 Wi-Fi 详情卡死、进不去」。两次实时串口
抓取（`live-linkup-userops*.cap`）证明：
- 卡死窗口内**驱动心跳正常推进**（msdu 涨、0 hangcheck 异常）；
- **wifid 零新 ioctl** —— 卡死没走到驱动/wifid；
- Preferences 的大量 `PICASSO_THR`/`PICASSO_STK` 线程/栈采样是
  周期诊断，非看门狗。

结论：Preferences 在自己进程内等系统 Wi-Fi 服务
（wifid 的 XPC 接口 / IO80211 user client）无响应而冻结。
属阶段 4（UI 集成），在 DHCP 拿到 IP 之后处理——部分这类卡死
在系统拿到网络服务后会自行缓解；若仍卡，需实现 wifid 的
通知注册/ XPC 面（参考 CoachZ）。

---

## 5.2 剩余路线（按顺序）

1. **`8703f5b` 上机 400 s**（`python work/mk-autojoin-args.py
   <新镜像> out/full-flash/boot-exp-linkup2.img` 后
   `SECS=400 bash work/boot-variants.sh linkup2`），按 §5 判据。
2. 若 LINK_ON 已发但 configd 仍不响应：查 configd 对 KEV_DL 的监听
   （接口 family/unit 匹配、`IO80211Interface absent` 被当以太网的影响），
   从 configd unified log 取证（参考 CoachZ `log show` 读
   Data 卷 `db/diagnostics` + `db/uuidtext`），不要猜。
3. DHCP 通后：设置 UI（§5.1）与凭据持久化（`wpass=` 挪出 boot-arg）。

---

## 6. 关键文件 / 命令 / 铁律

| 用途 | 路径 |
|---|---|
| 本轮可复现脚本 | `work/fix-wifid-jobs-firmlink.py`、`work/fix-wifid-jobs-launchd-form.py`、`work/fix-wifid-capabilities.py`、`work/fix-link-notify.py`、`work/mk-autojoin-args.py` |
| 数据面闭环证据 | `out/full-flash/logs/console-exp-autojoin.cap` |
| 阶段 1/2 闭环证据 | `console-exp-launchd.cap` / `console-exp-capfix.cap` |
| 上机链 | `work/chain-585.sh` / `work/chain-586.sh`；`SECS=400 bash work/boot-variants.sh <TAG>` |
| 派 CI | `gh workflow run compose-bootimg.yml -R tselongson/UniXNU-picasso --ref kd439-pool -f fast=true` |
| 串口实时 | `python work/serial-cmd.py COM5 status 8` |

铁律：
1. 一次只派一个 CI；`fast=true` 复用 seed（不带 `create_data_seed`）。
2. **采集窗口必须长于 168 s**（HIF 启动点），默认 400 s。
3. 判据是 checkpoint 明文 + 用户可操作性；`beacons=0/join=0` 在
   「wifid 已起但无人请求扫描」时属正常，不能当驱动回归。
4. patch 必须 LF、hunk 头用脚本重算；改 C 后跑括号平衡 + `abi-check.py`。
5. commit message 不写未实机验证的结果。
6. 用户自己的 3 处未提交改动 + 5 个 `docs/evidence/*.log` 删除 +
   `diagnostic-charter.md` 未提交改动，**一律不要提交**。
7. 磁盘：C 盘 8.0 GB 可用；用户明确说「空间够就先不动」，
   **未删任何工作区文件、未清空回收站**。
