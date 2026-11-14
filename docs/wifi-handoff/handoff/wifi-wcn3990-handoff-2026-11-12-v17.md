# WiFi (WCN3990) 交接文档 — 2026-11-12 13:15（v17）【历史归档】

> ⚠️ **已被 `handoff/wifi-wcn3990-handoff-2026-11-12-v18.md` 取代，仅作历史对照。**
> v18 纠正了 v17 的「`stack_open` 0 次 ⇒ configd 从未 open」误判，
> 给出 chain-594 墙钟时间线、Apple configd 逐行因果链，并记录了
> `5bebed6`（HEAD 恢复可上机，chain-597 0 PANIC）与 `2b941d3`（提前绑定 A/B）。
> 冲突时以 v18 为准。
> 判据口径：**checkpoint 明文 + 用户可操作性，不是「0 panic」**。
> 本文如实记录两次被证伪的归因（含我自己的），避免下一轮重查。

---

## 0. 一句话现状

```
固件/HIF/握手/密钥/数据面/en0/link 通知/rmtfs <4G/IONetworkStack 绑定  全部 ✅（chain-594 实机）
唯一缺口不变：configd 从未发 SIOCSIFADDR —— DHCP / inet / IP 三个全 0
netsvc=1 已把 1786 字节的 SCPreferences plist 写进
  /private/var/preferences/SystemConfiguration/preferences.plist（write error=0）
  ⇒ configd 仍不建 IPv4 service
chain-595/596 想加只读探针定位，两次都在同一位置 panic（见 §2）；
  595 的归因（VNOP_READDIR）已被 596 证伪。HEAD(d6c9b03) 上机会崩，
  **可用基线仍是 219b972 / boot-exp-netsvc.img（chain-594 镜像）**。
```

---

## 1. 今天中午三轮判决（同一启动配置 `netsvc=1 netfix=3 netstack=1 wifiauto`）

| 链 | commit | 结果 | 关键差异 |
|---|---|---|---|
| **594** | `219b972` | ✅ 跑满 400s，0 panic，全部内核 checkpoint 过；**`SIOCSIFADDR/DHCP/inet` 全 0** | 无探针（对照） |
| **595** | `8d30f39` | ❌ panic @ 行 937（`PICASSO_INJECT view stage=5` 之后），991 行 | +probe2048、+bundles(VNOP_READDIR)、+configd_open 探针、+busy_observe 放宽 |
| **596** | `d6c9b03` | ❌ **panic @ 同一行 937，925–960 行逐字一致**（991 行 / 61134B vs 61210B） | bundles 换成 vnode_lookup+snprintf（无 readdir/uio/4K 分配） |

panic 现场（两轮一致，仅代码地址不同）：
```
PICASSO_INJECT view stage=5 error=0 path=/usr/lib/libpicasso-sb.dylib
=== PICASSO CDC PANIC BEGIN ===
ESR_EL1 0x96000047 (kernel data abort)   FAR_EL1=内核虚拟地址
fmt: %s at pc 0x%016llx ...              （随后 watchdog pet rejected）
```

位置语义：`native_owner_data_prepare()` 依次调
`native_picasso_apps_mount`（APP_MOUNT）→ `picasso_ui_library_mount`（INJECT view）→
`picasso_ifnamer_mount`（594 在此打印 netsvc/config stage）。595/596 死在
**`picasso_ifnamer_mount()` 入口到 `picasso_netsvc_prepare()` 第一条 printf 之间**。

---

## 2. 被证伪的归因（今天新增两条，别再重查）

1. ~~chain-595 的 VNOP_READDIR 是 panic 元凶~~（commit `d6c9b03` 的说法）
   → **证伪**：chain-596 把 bundles helper 换成纯 `vnode_lookup`+`snprintf`
   （无 readdir、无 uio、无 4K 分配），panic 位置与 595 逐字一致。
   两个实现完全不同的版本都崩 ⇒ 元凶不是 readdir 本身。
2. ~~「Data seed 没有任何网络服务配置」（v16 §3）~~
   → 精确化：seed manifest 里确实 0 处 `SystemConfiguration`，但**设备持久
   Data 卷上已存在一个 63 字节的空 `preferences.plist`**（`prefs_error=0
   prefs_size=63`），是之前某次开机留下的；netsvc=1 把它覆盖成了 1786 字节
   （`native_gpu_addon_write` 返回 0 = `vn_rdwr`+`VNOP_FSYNC` 全成功）。
3. （沿用 v14 §2.1）v11/v13「已排除 SHA 不匹配」、v13「firmlink 是真因」等，见 v14。

## 2A. 595/596 共同改动清单（真正嫌疑范围）

panic 点之前**实际执行**的新代码只有三处：
| 改动 | 说明 | 风险 |
|---|---|---|
| `probe[512] → probe[2048]` | exec 线程栈：`picasso_ifnamer_mount` 已有 `buffer[2048]`+`digest[32]`，再叠 `picasso_netsvc_prepare` 的 `probe[2048]`+`path[128]` | **栈越界**（最强假设，待证伪） |
| bundles helper 被调用 | 595/596 两个实现都崩 ⇒ 与实现无关，指向「在 exec 上下文多这一层」或与 probe2048 叠加 | 待证伪 |
| `int verify_error` 中段声明 | gnu11 合法 | 低 |

panic 点**之后**才会执行的改动（`configd_open` 两处、`busy_observe` 放宽、
`stack_open` 原样）不在本次嫌疑内。

> 教训：**kern_exec 的 exec 路径是本项目探针的禁区**——那条线程栈余量小，
> 任何 >1KB 的局部数组或新函数调用都可能踩 guard page。取证要么放驱动
> 线程（`net_thread`/`net_ioctl`，chain-580~594 已验证安全），要么缩小数组。

---

## 3. 不变的精准定位（用户问「到底哪里的问题」）

- **硬指标**：`SIOCSIFADDR` / `DHCP` / `inet` 全 0。configd 对 en0 发了 29 次
  ioctl，几乎全是 `SIOCGIFMEDIA(0xc02c6938)` + `SIOCSIFFLAGS(0x80206910)`，
  **从不发地址 ioctl** ⇒ DHCP DISCOVER 一帧都没出去过。
- 已排除（见 v16 §5 与本文 §2）：驱动/固件/数据面、plist 写入路径
  （Apple `PREFS_DEFAULT_DIR_PATH="/Library/Preferences/SystemConfiguration"`，
  seed.json 实锤 `/Library/Preferences → ../private/var/preferences`）、
  netsvc 写入失败、DHCP 协议本身（还没轮到跑）。
- **未证伪的三个候选**（v16 §5.3 的 A/B/C 原样保留）：
  - A. InterfaceNamer 插件没跑（`IOBusyInterest`/`root_busy` 0 次是间接证据，
    探针有 `getServiceRoot()` 盲区，且本地 configd 源码版本 ≠ 设备 20H392）
  - B. 磁盘上 `/System/Library/SystemConfiguration/*.bundle` 不齐
    （Apple `pluginsCopy()` 靠**扫描磁盘 bundle** 发现插件，
    `plugins_builtin[]` 只给已发现的 bundle 填函数指针；
    项目只挂载了 InterfaceNamer.bundle 一个）——未取证
  - C. 插件跑了但 service 没激活（prefs 被覆盖 / CurrentSet 不匹配 /
    `IO80211Interface absent` 导致接口不被 `SCNetworkInterfaceCopyAll` 认出）

---

## 4. 「DHCP 能不能直接抄」——能，API 已核实

用户方向正确：DHCP 协议是纯软件。两条路线都已在**本地**核实：

1. **内核 DHCP 客户端（绕开 configd）**——所需符号在 `kernel.macho` 全部存在：
   ```
   ifioctl 48   socreate 27   sock_socket 5   sock_sendmbuf 8
   sock_receivembuf 2   default_route_add 1   dhcpol_parse_packet 1
   dhcpol_find 1   in_control 6   current_proc 29
   ```
   可抄：XNU `bsd/kern/netboot.c` 的 `inet_aifaddr()`（=`ifioctl(so,SIOCAIFADDR,&ifra,
   current_proc())`）、`default_route_add()`、`get_ip_parameters()` 的
   `dhcpol_parse_packet/dhcpol_find` 解析。收发可走驱动已有的
   `net_output`/`hakios_net_input`（`hakios.net.c:155/183`）或 `sock_*` KPI。
   **限制**：这样拿到的 IP 不进 SystemConfiguration，设置/状态栏仍不认——
   是"能上网"，不是"日用"。
2. **修 configd 链**：需要先拿到 chain-596 想取的 A/B 证据。

---

## 5. 当前可用基线与回退

| 项 | 值 |
|---|---|
| **可上机镜像** | `out/full-flash/boot-exp-netsvc.img`（chain-594，commit `219b972`，sha `39a22ba9…`） |
| **HEAD 状态** | `d6c9b03` **上机会崩**（595/596 同一问题），先回退再继续 |
| 回退命令 | `git -C upstream/silenzio-picasso revert d6c9b03 8d30f39` 或 checkout `219b972 -- patches/picasso-network-timeout-xnu.patch` 后重commit |
| 设备 | fastboot 在线（`64993333`） |
| 磁盘 | C: 27 GB 可用 |

---

## 6. 下一步（按序，不猜）

1. **二分 panic**：先只回退 `probe[512]`（保留其它探针）派 CI；若仍崩再回退
   bundles 调用。每步单变量。目标是让 HEAD 恢复"能上机"。
2. **取证改为安全位置**：A 探针（`configd_open`/`root_busy`）保持在
   `IOUserClient.cpp`（panic 点之后才执行，嫌疑排除）；B 探针（bundle 存在性）
   放到**驱动侧**（`hakios.net.c` 的 `net_ioctl` 首次 configd 调用时，或
   `net_thread` 延迟触发），彻底离开 exec 路径。
3. 拿到 A/B 证据后按 v16 §4 判据走：`netsvc` 诊断 → `SIOCSIFADDR` → DHCP → `inet`。
4. 若 configd 链证实无法修，按 §4.1 落地内核 DHCP（`dhcp=1` 门控，默认关，单变量），
   判据：`DHCP OFFER/ACK` 明文 + `ifioctl SIOCAIFADDR` 返回 0 + `inet` 地址打印。
5. 用户实测：设置里点 Wi-Fi 详情是否还卡（UI 判据只能用户给）。
6. 凭据持久化、真实 DNS/HTTP 验证（拿到 inet 之后）。

---

## 7. commit / CI 速查（11-12 中午）

| commit | 内容 | CI | 结果 |
|---|---|---|---|
| `3349072` | netsvc 预置 Wi-Fi/DHCP service | `43147820424` | **failure**（编译错：`picasso_ifnamer_read` 未声明，kernel-bootstrap.log 222091 行） |
| `219b972` | 修编译（加前向声明） | `43149474624` | success |
| `8d30f39` | chain-595 取证探针 | `43153756136` | success（构建），**上机 panic** |
| `d6c9b03` | 换掉 readdir（我的错误归因） | `43156480983` | success（构建），**上机同位置 panic** |

工具（可复现）：`work/fix-netsvc-plist.py`、`work/fix-netsvc-forward-decl.py`、
`work/fix-netsvc-probe.py`、`work/fix-probe-panic.py`、`work/chain-594.sh`、
`work/chain-595.sh`、`work/chain-596.sh`。
日志：`out/full-flash/logs/console-exp-{netsvc,probe,evid}.cap`。

## 8. 铁律增补（11-12）

- **kern_exec exec 路径禁加探针**：栈余量小，>1KB 局部数组/新函数调用都可能
  踩 guard page（595/596 实测）。
- 一次归因被证伪就立刻在文档记录，不要带着旧结论排查（本文 §2 第 1 条）。
- `picasso_ifnamer_read` 的 capacity 参数同时是"拒绝大文件"的阈值
  （`va_data_size > capacity → EFBIG` 且 `*size` 不赋值）——回读大文件必须给足缓冲。
- 其余沿用 v16 §5 / v14 §6：一次一个 CI、`fast=true` 复用 seed、采集 >168s、
  patch LF、hunk 头脚本重算、ABI 0 HARD、用户 6 处未提交改动不碰。
