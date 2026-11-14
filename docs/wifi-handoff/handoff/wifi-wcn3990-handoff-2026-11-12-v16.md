# WiFi (WCN3990) 交接文档 — 2026-11-12 10:5x（v16）

> **⚠️ 本文件已归档。当前权威状态源是
> `handoff/wifi-wcn3990-handoff-2026-11-12-v17.md`（v17，2026-11-12 13:15）。**
> v17 记录：netsvc 写入成功但 DHCP 仍 0；chain-595/596 取证探针两次
> 在同一位置 panic，595 的 VNOP_READDIR 归因被 596 证伪；HEAD(d6c9b03)
> 上机会崩，可用基线是 219b972 / boot-exp-netsvc.img。



> **本文件是当前唯一权威状态源。** v15 及更早为历史归档。
> 判据口径：**checkpoint 明文 + 用户可操作性，不是「0 panic」**。

## 0. 一句话状态

驱动与内核侧已实机闭环：WPA2 四次握手、PTK/GTK、PEER_AUTHORIZE、
HTT TX 被 ACK、帧进协议栈、`link down → link up`、KEV_DL_LINK_ON
报文布局正确、`IONetworkStack` 免 entitlement user client 已绑定、
rmtfs 缓冲稳定在 4 GiB 以下（169 s 复位消失）。
**当前唯一缺口在 configd 侧**：`SIOCSIFADDR / DHCP / inet` 仍全 0
（`console-exp-final.cap`），`stack_open` 0 次。
**第五刀（`netsvc`，已写好未上机）**：给 configd 预置
`SystemConfiguration/preferences.plist`（Wi-Fi + DHCP service）。

## 1. 状态表（2026-11-12 10:56）

| 层 | 状态 | 证据 |
|---|---|---|
| 开机 / 与系统并存 | ✅ | `console-exp-final.cap`：Early boot ×2、DATASTORE ready=1、0 PANIC |
| 关联+加密+数据面 | ✅ | 同上：`keys installed, peer authorized, link up`、心跳到 polls=55000 |
| link 通知链 | ✅ | `KEV_DL_LINK_ON size=48 hdr=24 family=2 unit=0 name=en fix=3` |
| rmtfs 4 GiB 上限 | ✅ | `rmtfs: buffer ... pa cdde0000`，`iovec invalid` = 0，采集跑满 349 s |
| IONetworkStack user client | ✅ 绑定生效 | `IONetworkStack user client bound (netstack=1)` |
| **阶段 3 DHCP / IP** | ❌ 未通 | `SIOCSIFADDR / DHCP / inet` 全 0；`stack_open` 0 次 |
| 阶段 4 设置 UI / 凭据持久化 | ⬜ | Preferences 卡死与 DHCP 疑似同源（InterfaceNamer/无 service）；`wpass=` 明文在 boot-arg |

## 2. 昨夜已实机验证的四个提交（`kd439-pool`，CI 全绿）

| commit | 内容 | 实机判决 |
|---|---|---|
| `8703f5b` | radio `.up=true→false` | `linkup2.cap`：`attached ... link down` → `link is up`；SIOCSIFADDR 仍 0 |
| `54b5d8d` | KEV 报文 4 字节错位（`kern_event_msg` 尾部 `event_data[1]`）+ IFF_UP/RUNNING + configd ioctl 可见 | `fix3b.cap`：`KEV_DL_LINK_ON size=48 hdr=24 ... fix=3`，configd `SIOCGIFMEDIA` ×8；SIOCSIFADDR 仍 0 |
| `6285168` | 按同芯片 CoachZ 移植免 entitlement 的 `IONetworkStackUserClient`（`netstack=1`） | `ns1.cap`：`user client bound`、0 PANIC（该轮被 rmtfs bug 截断） |
| `c75f96d` | rmtfs 缓冲 `max_pnum=0x100000`（<4 GiB） | `final.cap`：`pa cdde0000`、`iovec invalid`=0、跑满 349 s |

**`stack_open` 0 次** = configd 从未 `IOServiceOpen(IONetworkStack)`；
`WaitStackTimeout=5` 超时不会创建 `S_connect`（Apple configd
`ifnamer.c:3965/4108/4159`），所以 `Plugin:InterfaceNamer` 完成通知
永不发布 → Preferences 的 `SCNetworkInterfaceCopyAllWithPreferences /
dispatch_once` 一直等（设置点 Wi-Fi 详情卡死），configd 也不建 IPv4 服务。

## 3. 第五刀（已实现、未提交/未 CI/未上机）：`netsvc` 预置网络服务

**硬证据（离线，零成本）**：
- CI `43049291288` 的 `linux-inputs/source-var-manifest.json`：
  `SystemConfiguration` / `preferences.plist` / `NetworkServices` / `IPv4` /
  `DHCP` 出现次数全部为 **0**；
- `data.dbrdisk`、`setup.dbr` 同样全 0；
- `seed.json` 的 `created_directories` 没有 `preferences/SystemConfiguration`
  （`/Library/Preferences` 是指向 `../private/var/preferences` 的符号链接）。
  ⇒ **Data seed 里没有任何网络服务配置**。

**设备证据**：configd 反复查 en0 的 media（×8）却不发地址 ioctl ——
"有接口、无 service" 的形状。Apple configd 只对
`preferences.plist` 里存在 service 的接口跑 IPConfiguration/DHCP。

**实现**（`work/fix-netsvc-plist.py`，改
`patches/picasso-network-timeout-xnu.patch`，+106/-1）：
- 新增 `picasso_netsvc_plist[]`：标准 SCPreferences schema
  （NetworkServices + Sets/CurrentSet，en0 / AirPort / IPv4 ConfigMethod=DHCP）；
- 新增 `picasso_netsvc_prepare()`：复用 `picasso_ifnamer_read` /
  `native_gpu_addon_write` / `mkdir`（与上游 `picasso_ifnamer_mount` 同源，
  在 launchd/configd 之前执行），先打印
  `PICASSO_NETWORK netsvc=%u prefs_error=%d prefs_size=%llu`（诊断），
  `netsvc=1` 时建目录并写 plist，再回读校验；
- 门禁全过：6 个 hunk 头重算后逐一匹配（`+9318,328`）、抽出 `+` 行
  括号/圆括号平衡 0、`abi-check.py` 0 HARD、文件 LF；
- **注意**：`PE_parse_boot_argn` 在 `kern_exec.c` 已有声明并使用
  （`launchdsuffix`、`exe_boothash_salt`），**不要**再加 `extern int`
  前向声明（`boolean_t` 返回类型会冲突——已踩过一次，脚本里已删）；
- 当前语义：`netsvc=0`（默认）只诊断；`netsvc=1` **无条件覆盖**目标文件
  （hook 有 `static int done` 只执行一次，时机在 configd 之前）。

## 4. 下一步（按顺序）

1. **提交 netsvc 补丁**（只 add `patches/picasso-network-timeout-xnu.patch`）
   → 派一次 CI（`fast=true`）→ 生成 `netsvc=1` 镜像上机 400 s。
   判据链：`netsvc=1 prefs_error=... prefs_size=...` → `write error=0` →
   configd 建 IPv4 服务 → **`SIOCSIFADDR`** → `DHCP` → `inet`。
   若 `netsvc=1` 仍无 `SIOCSIFADDR`：看 `prefs_error/prefs_size`（文件是否
   写成）与 `stack_open`，再从 Data 卷 `db/diagnostics`+`db/uuidtext` 读
   configd unified log（v14 §5.2 的 CoachZ 路径），不要猜。
2. **用户实测**：设置主页点 Wi-Fi 详情是否还卡（UI 判据只能用户给；
   `netstack=1` 已在内核里，若 service 建好很可能一并缓解）。
3. **凭据持久化**：`wpass=` 明文在 boot-arg（不进仓库、日志不打印），
   正式使用应移出；参考 CoachZ 的已保存网络。
4. **真实上网**：DNS / HTTP / 默认路由（拿到 inet 之后才有意义）。
5. `IO80211Interface absent` 回退 `IOEthernetInterface` 不是阻塞点：
   `SCNetworkInterface.c:1790` 判 Wi-Fi 看的是 controller 是否
   `IOObjectConformsTo("IO80211Controller")`（我们是）。

## 5. 关键文件 / 命令 / 铁律

| 用途 | 路径 |
|---|---|
| 本轮脚本 | `work/fix-net-service.py`、`work/fix-netstack-userclient.py`、`work/fix-rmtfs-lomem.py`、`work/fix-netsvc-plist.py`、`work/mk-netfix-variants.py`、`work/chain-590.sh`、`work/chain-593.sh` |
| Apple OSS 本地源码（已 clone，查源码别再 WebFetch） | `work/refs/apple-oss/configd`（9.8 M，含 InterfaceNamer/IPConfiguration）、`work/refs/apple-oss/xnu`（sparse：`bsd/{sys,kern}`、`iokit/IOKit`、`iokit/Kernel`、`osfmk`） |
| 参考实现 | `work/trogdor/port/CoachzWifiNet.cpp`（同 WCN3990）、`unixnu-wcn7860/source/bsd/dev/qcom/PeachNet.c`、`.../iokit/Kernel/arm/PeachWLAN.cpp` |
| 上机 | `SECS=400 bash work/boot-variants.sh <TAG>`；`python work/serial-cmd.py COM5 status 8` |
| CI | `gh workflow run compose-bootimg.yml -R tselongson/UniXNU-picasso --ref kd439-pool -f fast=true` |
| 镜像打参 | `work/mk-autojoin-args.py`（每日用参数）、`work/mk-args-variant.py`（追加/替换 token，995 B 预算） |

铁律（沿用 v14/v15）：一次只派一个 CI、`fast=true` 复用 seed；
采集窗口必须 >168 s（默认 400 s）；判据是 checkpoint 明文；
patch 必须 LF、hunk 头用脚本重算并逐一校验、改 C 后跑括号平衡 +
`abi-check.py`；commit message 不写未实机验证的结果；
**用户的 6 处未提交改动（5 个 `docs/evidence/*.log` 删除 +
`wifi-wcn3990/docs/diagnostic-charter.md`）一律不要提交**；
磁盘 27 GB 可用（用户已清理），未删工作区文件、未清回收站。
