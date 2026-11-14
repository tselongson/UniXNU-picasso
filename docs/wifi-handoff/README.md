# WCN3990 WiFi 移植 — 接手 / 求助指南

> **本文是给第一次接触本项目的人看的入口。** 仓库是公开的，用于交接与对外求助。
> 状态截至 **2026-11-14 22:0x**（开发分支 `kd439-pool`，代码 HEAD `e20ca34`）。
> 文档随该分支持续更新；仓库默认分支 `main` 的 README 有指向本目录的入口。
> **唯一权威技术状态源**：[`handoff/wifi-wcn3990-handoff-2026-11-14-v35.md`](handoff/wifi-wcn3990-handoff-2026-11-14-v35.md)。
> 本文与其他所有文档冲突时，以 v35 为准；v34 及更早是历史归档。

---

## 1. 先读这三份（不要从 v1 开始读）

| 顺序 | 文件 | 作用 |
|---|---|---|
| 1 | **本文** | 现状、导航、已证伪清单、操作纪律 |
| 2 | [`handoff/wifi-wcn3990-handoff-2026-11-14-v35.md`](handoff/wifi-wcn3990-handoff-2026-11-14-v35.md) | 唯一权威：精准定位链、实机判决、剩余缺口、铁律 |
| 3 | [`logs/2026-11-14.md`](logs/2026-11-14.md) | 最新逐轮事实（按时间追加，含失败的实验与环境轮） |

需要历史脉络时再查 [`handoff/2026-11-10-project-history-digest.md`](handoff/2026-11-10-project-history-digest.md)
和 [`handoff/README.md`](handoff/README.md) 的版本索引；[`logs/README.md`](logs/README.md) 说明日志口径。
[`WORKSPACE-README.md`](WORKSPACE-README.md) 是本地工作区的说明快照（链接已重定位到本目录）。

---

## 2. 项目是什么

在 **Redmi K30 5G（picasso / SM7250）** 上跑定制 XNU（UniXNU，Darwin 23 / iOS 16.7.16 20H392），
把 **WCN3990（ath10k SNOC / WCN3990 CE）** WiFi 从零做到可用。WiFi 驱动在
`wifi-wcn3990/source/`，是移植 + 自研混合，不是 Linux 驱动整体移植。

- 主要源码：`Wcn3990Hif.cpp`（CE/HIF/HTC/WMI/HTT/数据面）、`PicassoWlan.cpp`（板级胶水/接口）、
  `hakios.net.c`（ifnet 接入）、`hakios.dhcp.c`（内核 DHCP 客户端）、`hakios.apple80211.c`（wifid ioctl）。
- 参考实现（都在本地工作区，非本仓库）：同芯片 `Trogdor-ipadOS`（CoachZ / WCN3990 SNOC，
  唯一跑通完整栈的同芯片参考）、不同芯片 `pong-wifi`（QCA6490）、`upstream/sheng`（WCN7850）、
  `unixnu-wcn7860`（PeachNet）。能抄的是**系统层**（InterfaceNamer / DNS / ifnet / KEV / Apple80211），
  PCIe 家族的 CE/SNOC 底层不可移植。
- 一手资料：`work/refs/apple-oss/{xnu,configd}`、Apple 官方《Network Interfaces and Protocol
  Plumbers》、Linux `ath10k`、LKML WCN3990 CE 补丁系列。

---

## 3. 当前状态（如实，不含糊）

### 已实机闭环 ✅
固件上电 / HIF / CE / HTC / WMI / HTT、扫描、auth/assoc/VDEV_UP、WPA2 四次握手 +
PTK/GTK + PEER_AUTHORIZE、真实 RX/TX 数据面、en0 link 与 KEV、内核 DHCP（IP + 默认路由）、
DNS 发布（三条 `dns set=1`）、开机进桌面可操作、键盘输入、**单播双向**。

最后一项是 2026-11-14 的决定性突破：根因是 **en0 的 `IF_LLADDR`(`00:0a:f5:39:90:01`)
与空口 STA MAC(`02:00:00:00:00:01`) 不一致**，对端学到错误 MAC，单播永远投递不到手机。
改完 A/B：`w0 ping 0/6 → w1 6/6 → w2 6/6`，`uni` 0→41，网关/PC 的单播 ARP reply
首次到达手机串口。已默认化（`b1af80f`），不带新 boot-arg 即生效。

### 仍未通 ❌
1. **运行期 WiFi 数据面停摆**（当前技术主线，见 §4）。
2. **卷 I/O 挂死**：用户看到「全屏黑屏 + 静止加载圈」、`disk5 tx_enter() failed: 5`、
   `disk3: device is not ready`、`SpringBoard pid=0`，且 `NATIVE_UFS_FAILURE`/SMMU/PANIC
   全 0 ⇒ 块设备层停止服务，不是 WiFi，也不是已修好的 SMMU→UFS 链。长时采集证据仍缺。
3. Safari 端到端 HTTP/HTTPS **未做用户实测**；设置 > Wi-Fi 详情未复测。
4. `wpass=` 仍是 boot-arg 明文（凭据持久化未做）。

> **不要说「WiFi 修好了」**：网络层（L2/L3 单播）已闭环，但「能开网页」没有证据。

---

## 4. 当前阻塞：固件的 target→host 方向整体停止（不只是 CE1）

`chain-622`（CI `43606329627`，纯诊断，两组各 900 s）拿到了决定性的 `drri1`：

```text
CE1 STALL #1 polls=210000 ce1=341 drri1=00150015 sw1=21 post1=20 nb1=0 rri2=00040004 httidx=1262 ord=239 msdu=240
```

`drri1` 高 16 位（**固件** CE1 写指针）= `0x15` = 21，host 的 `sw1` 也 = 21，`nb1`（该槽
nbytes）= 0 ⇒ 两者完全同步、ring 里没有未读完成 ⇒ **是固件停止写 CE1，不是 host 读不到**。
「host 侧 ring 状态 / 描述符可见性」整支可以排除。

**并且纠正一个此前写错的判读**：停摆不是「CE1 单独静默、CE2/WMI 照常」。
精确定位到 `polls=189000`（约 810 s，`rx` 3066→3069 只 +3 后冻结）：

```text
rx=3069  beacons=2899  mgmtev=2899  httidx=1262  ord=239  msdu=240  drri(CE2)=4   ← 一起冻结
[REAL DATA PLANE] IPv4 85 bytes over HTT TX_FRM
WCN3990-HIF: WPA: tx frame 103 bytes slot=1/2/3 prefetch=52 rc=0                  ← TX 仍在入队
卷错误 / PANIC / NATIVE_UFS_FAILURE 全 0
```

⇒ **整个 target→host 方向（CE1 + CE2）都停了，只有 host→target 的 TX 还在工作。**
之前之所以觉得「CE2 正常」，是因为采样点落在停摆之前。

### 已被这一轮实机排除
| 候选 | 依据 |
|---|---|
| HTT RX buffer 耗尽 / repost 漏槽 | `msdu=240` 与 `httidx` 增量 239 一一配对，`bad=0/0` |
| poll 循环提前 return | `stage=3` 全程不变；`g_ce_quiet=0` 且全 cap 无 `quiet heartbeat` |
| `PICASSO_WAIT` 诊断导致 | 那是每 2 分钟一次的 UI 快照（`PICASSO_SNAPSHOT_PERIOD_S=120`），纯读 |
| deauth / beacon miss / 被 AP 踢 | 全 cap 无相关日志，`join=5` 全程不变 |
| 卷 I/O 拖垮 | 两组卷错误 / PANIC / UFS failure 全 0 |
| CE 寄存器配置 | `CE_DST_WM=0x50` 写 `[15:0]=dst_n`、CTRL1 清 bit17/18、`CE_STATUS` 写 `0x1f`，与 `snoc.c:256-263` + Trogdor `wc_power_up/wc_service` 一致 |
| RX_IN_ORD 解析 | ath10k `htt.h:1104-1116`（`info`@p[1]、`msdu_count`@p[6..7]、12 B 描述符@8）逐字节一致 |
| `pspoke` / `pskeep` | chain-619 / chain-621 两次独立 A/B 证伪，`pspoke` 还有害 |

### 下一刀（一次上机分流两件事）
1. 停摆时**发一条 WMI 命令并等响应**（如 `WMI_GET_STATS`）：
   有响应 ⇒ 固件活着、只是 RX 路径挂起；无响应 ⇒ 固件已崩。
2. 停摆时**主动 `VDEV_DOWN` → `VDEV_UP` → 重新关联**：
   能恢复就说明是可恢复的连接/固件状态，同时也给出**运行期保活**的可行手段
   （用户能上网，代价是周期性短暂重连）。

**在 (1)(2) 有判决前，不改 CE1/固件，也不得宣称已修好。**

---

## 5. ★ 已证伪 / 已排除清单（接手前必读，避免重复造轮子）

完整逐条证据在 v35 §2/§6 与 `logs/`。这里只列结论：

**SMMU / CE 底层**：WLAN CE 流是 `0xc0`（DT 一直正确），内核侧追了很久的 `0x20` 是错的；
`wcn3990-hif-assign=3`（MSS_MSA）后 HLOS 失权，**彻底放弃**；`SMR[6]/[7]`/`sCR0` 由 TZ 锁定
只有 `S2CR` 可写，改 `SMR` 会挂死总线；`CE_HOST_IE(0x2c)`/`CE_MISC_IE(0x34)` **只写**（读会挂死 AHB）；
运行期读 CE 主体寄存器会触发 un-clocked SError（`62572ba` 已修）；USB SMR 编码不要动。

**扫描 / WMI**：WMI TLV 数组 tag 是 `16/17/18/19`（ath10k 编号，我们原来写成 ath11k 的
`17/18/19/20`，`ec5673e` 已修）；`VDEV_UP` 在扫描前发送是致命的（Linux 从不发）；
`START_FAILED(64)+INTERNAL_FAILURE(4)` 表示固件**根本没开始扫描**，不是「扫描能活」；
`WMI_SCAN_EVENT_BSS_CHANNEL(type=4)` ≠ 发现 AP；HTT RX fill 初值必须是 1023 不是 0；
`ast_skid_limit=16` 是 INIT 资源池的元凶；5G MGMT_TX 不可用，先打 2.4G；
`assoc status=41` 真因是 AP group cipher 为 **TKIP**，要按 AP 的 group cipher 构造。

**密钥 / 单播**：`rekey`（晚到 PEER_MAP 重装 PTK）、`keyfix`（TKIP GTK MIC 8/8）、
`pspoke`（STA PS workaround）**三个假设全部实机证伪**；PTK 用 Python 离线独立复算完全正确
（PMK/KCK/TK 全匹配）；WMI KEY 命令布局 / cipher ID / 0x5003 语义 / join 序列与 ath10k 全一致；
`0x5003` payload 末 4 字节是 key index 回执不是失败码。

**协议栈**：`ifnet_input()` 返回 `rc=0` **只证明入队**（Apple 文档：处理在另一线程），
「`if_input` 为 NULL」已证伪；协议**早就注册好了**（`noproto=0`、`SIOCPROTOATTACH rc=17` EEXIST），
不要再查 plumber / `in_ifaddr` / DLIL input thread stall；mbuf 形态与 `if_fake.c` 同构、
BPF 已有 `bpfattach`（别照 CoachZ 再加一次）；`IFNET_INIT_NX_NOAUTO` 已设；
`ifnet_attach(ifp, NULL)` 正确（所有参考都传 NULL）。

**判据陷阱（最容易害人）**：`PICASSO_FBUC` 有 160 次打印上限；`GPU fence=2` 全组恒定；
`htt=64` / `mgmtev=64` / `RX: #` 64 行都是上限；**TX 侧 ARP 日志不打印 opcode**，
判断「有没有回 ARP」必须看 TX 帧的目的 MAC（v34 用 RX 侧 `arp op=2` 得出「不回 ARP」是假判据，
害得方向错了三天）；Windows `ping` 退出码不可当判据（「无法访问目标主机」也算 rc=0）；
「0 PANIC」不等于成功（可能是 assoc 被拒）；`PICASSO_DATASTORE status=0` 是 NotRun 不是有旧数据。

**configd / 原生链**（历史上耗了最久，别再走）：设备 root volume **没有** `IPConfiguration.bundle`；
`IOKitWaitQuiet` 返回 `53=KERN_DENIED`（main_port ≠ main_device_port）；`*TIMEOUT*` 可解卡但
**不能在 configd 进程内创建 SCPreferences service**（`preferences.plist-lock` 文件锁由 configd
主 RunLoop 自持，`wait=1` 实测导致 8 次崩溃循环，`wait=0` 也拿不到锁）；`SIGHUP` 不能让
configd 重载；`prefsmon.c` 不监听 plist 文件变化；写 `resolv.conf` 不能进原生 DNS
（iOS 走 XPC `dnsinfo_server`），`State:/Network/Global/DNS` 是 IPMonitor 的**输出**键不是输入键，
`SupplementalMatchDomains` 必须带；`netstack=2` / `ifaceuc=1` 均被单变量 A/B 证伪。
⇒ 在这个系统上，**内核 DHCP 客户端是唯一可行的 DHCP 实现**（Apple 内核自己也有
`bsd/kern/netboot.c` 同类实现），不是「手搓简化旁路」。

**开机 / 显示**：Data seed 身份由 `picasso_linux_inputs.recipe()` 决定，改
`offline_disabled_jobs` / 注入库 / probe.m 都会**改变 seed UUID** 导致旧 Data panic，
正解是 `runtime_enabled_jobs`（不改 recipe）；HIF 把 `IOSleep` 重定义成
`machine_delay_until` 忙等会饿死 UI（`picasso_yield_ms()` 已修）；黑屏真因曾是
`make-545.py` 把 `offline-legacy-validate=1` 当装饰 token 覆盖掉，**禁止再用
`offline-legacy-*` 做等长替换槽位**。

---

## 6. 操作纪律（铁律，违反必翻车）

1. **单变量 A/B**：同一镜像、只差 1 字节 / 一个 boot-arg；cmdline 恰好 **995 B**，
   其余 token 逐字相同。离线先预演 `len(args)==995` 且全镜像 `byte diff == 1`。
2. **CI**：`gh workflow run compose-bootimg.yml -R tselongson/UniXNU-picasso --ref kd439-pool -f fast=true`；
   一次只派一个 CI（连续派会触发并发取消）；`push` 不自动跑；`gh` 在 `tools/bin/gh.exe`。
   bootstrap pass 的 `ld: symbol(s) not found` 是预期且被容忍的。
3. **默认复用 Data seed**：**不要**带 `-f create_data_seed=true`；同 seed ⇒
   **禁止清 journal marker、禁止刷 userdata**（会丢用户数据）。只有明确要重建 Data 身份时才新建
   seed，此时必须精确清 UFS 尾部 16 KiB journal marker（LBA 30870533）。
4. **采集 ≥400 s**（WLAN 初始化延迟约 150 s，120 s 窗口什么也采不到）；
   **判据是 checkpoint 明文 + 外部 ping，不是 0 PANIC**；采集不完整不得当判决；
   panic 必须定位到子系统，不能一票否定 WiFi。
5. **运行期停摆类故障的主判据必须放在采集窗口之后**（chain-617/618/619 采集第 300 s
   都是 6/6，采集后一律 0/20），并且同轮抓卷健康快照排除 disk5/disk3 冒充。
6. **给有打印上限的计数配无上限孪生计数**；优先用现成心跳计数分层再改代码。
7. **exec 路径禁加探针**（栈余量小，>1 KB 局部数组会踩 guard page）；大数组改 `static`。
8. **已证伪的 boot-arg 从后续 A/B 的 cmdline 彻底删除并断言**；A/B 链里 VALID 检查
   不得用 `|| exit N`（无效环境轮要重试，不能终止整条链）。
9. **用户 6 处未提交改动原样保留**（5 个 `docs/evidence/*.log` 删除 + `wifi-wcn3990/docs/diagnostic-charter.md`
   修改），只 `git add` 自己的文件，不要提交也不要恢复这些改动。
10. **提交纪律**：`work/abi-check.py` 0 HARD、`work/check-src.py`、括号平衡、
    `git diff --check`、patch hunk 头行数与 `git apply --check`、新增 `.c` 先跑 WSL 完整 XNU 树
    `-fsyntax-only`；patch blob 必须 LF（`core.autocrlf=true` 下 CRLF 会让 `git apply` 静默失败）；
    **commit message 不写未实机验证的结果**（`AGENTS.md`: never claim a device result that a log does not show）。
11. **凭据**：SSID / 密码只落在本地 `work/wifi-credentials.txt`，**绝不提交**；
    文档副本统一脱敏（见 §8）。

---

## 7. 环境速查

| 项 | 值 |
|---|---|
| 工作区 | `~/miphone`（本文档内路径已脱敏为通用占位） |
| GitHub 工作副本 | `~/miphone/upstream/silenzio-picasso` |
| remote / 分支 | `fork` = `tselongson/UniXNU-picasso` / `kd439-pool`（`origin` 无推送权限） |
| 串口 | COM5 = `USB\VID_1209&PID_0001\PICASSO01`（COM3/4 是蓝牙，别用） |
| 串口工具 | `work/serial-cmd.py COM5 status N`；`work/ensure-fastboot.sh` 封装 `reboot-fastboot` |
| 上机链 | `work/chain-*.sh`、`work/boot-variants.sh`、`work/mk-args.py`、`work/mk-autojoin-args.py` |
| 实机日志 | `out/full-flash/logs/console-exp-<tag>.cap`（+ `.times` 墙钟） |
| 实验网络 | `192.168.101.1` 网关 / `.4` PC / 手机 DHCP 拿 `.12`–`.18`；AP 为 2.4G、group TKIP + pairwise CCMP |

---

## 8. 关于本目录（脱敏与取舍）

- 源文档在本地工作区，不在 git 仓库里；本目录由
  `work/publish-wifi-handoff.py` 生成，**本地原文件从不修改**。
- **WPA 密码已替换为 `REDACTED`**（共 10 处）；本地用户名绝对路径已替换为
  `~/miphone` / `~`。请用本地凭据文件补回，不要把它贴到 issue / PR。
- **保留** SSID、BSSID、内网 IP、CI run id、提交哈希 —— 这些是复现实验和定位问题必需的，
  不是认证凭据。如果你打算进一步公开，请自行评估。
- **只上传 `.workbuddy-ai/memory/` 的 16 份日志**（权威完整版）。另有一套
  `.workbuddy/memory/` 是镜像，其中 11-13 / 11-14 是当天较早时刻的**截断快照**，
  内容严格是 `-ai` 版前缀，没有独有信息，故不重复上传。
- **未上传** `handoff/` 里约 414 MB 的二进制产物（`boot.img` / `kernel.macho` /
  `.a` / `.o` / `.json` 等）、`out/` 里的 `.cap` 串口日志和镜像，以及 `work/` 下所有脚本与凭据。
  需要时在求助时单独提供对应文件路径或片段。

---

## 9. 怎么求助最有效

发 issue / 找人时请附上：

1. 当前读的是 **v35 第几节**，以及 [`logs/`](logs/) 里对应日期的原文片段；
2. `console-exp-<tag>.cap` 的**关键行**（心跳序列、checkpoint、PANIC 首因），不要贴整个几百 KB 文件；
3. CI run id、boot-arg 全文（**记得删掉 `wpass=`**）、A/B 两组的单变量差异；
4. 判据的实际数值，而不是「好像不通」；说明采集窗口多久、ping 是在采集期内还是之后发的；
5. 你已经试过并排除的假设（对照 §5，能省掉大量重复提问）。

**当前最需要帮助的具体问题**：WCN3990 固件在运行 4–14 分钟后**停止整个 target→host
方向**（CE1 与 CE2 的 DRRI 都冻结，`drri1` 与 host 的 `sw1` 同步在 21、`nb1=0`），
而 host→target 的 TX 仍在入队（`WPA: tx frame rc=0`）；卷/PANIC 全 0、无 deauth、
`join=5` 不变、HTT RX buffer 无泄漏（`msdu` 与 `httidx` 增量一一配对）。
已排除 `pspoke`/`pskeep`（PS 方向两次 A/B 证伪）、CE 寄存器配置、RX_IN_ORD 解析、
poll 提前 return、`PICASSO_WAIT` 诊断干扰。**想请教：WCN3990 固件在什么条件下会
整体停掉 RX 而保留 TX？是 IMPS/idle 状态机、还是 PD 内部 RX 路径 assert？**
下一步我们会在停摆时发 WMI 命令探活 + 主动 `VDEV_DOWN/UP` 重关联。
有 WCN3990 / ath10k SNOC 运行期经验的同行
请直接看 v35 §5.4 与本文 §4。
