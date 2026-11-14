# WiFi (WCN3990) 交接文档 — 2026-11-12 15:50（v19）

> 【历史归档】本文已被 **`wifi-wcn3990-handoff-2026-11-12-v20.md`** 取代，冲突时以 v20 为准。
> v20 相对 v19 的实质变化（均有实机证据）：
> 1. `9fb7091` 测量轮证明 **AP 确实给了 option 6（DNS=192.168.101.1）**、
>    `/private/var/run/resolv.conf` 可写、**mDNSResponder pid=132 一直在跑**、SIGHUP 已发；
> 2. 全机型参考盘点（含用户点名的 `UniXNU-pong`）证明**所有真机联网机型的 DNS 都来自
>    configd 原生链**，pong 的 resolv.conf 只服务 Unix/APT 工具 ⇒ v19 的 DNS 方案作为「修复」存疑；
> 3. `netstack=2` 单变量 A/B 干净证伪；SIGHUP 重载 configd、回填 plist 两条路也被源码排除。
> 4. 唯一待判决的是用户实测 Safari 输域名。

> **（以下为 v19 原文，结论已被上面更正）**
> 判据口径：**checkpoint 明文 + 用户可操作性，不是「0 panic」**。

---

## 0. 一句话现状

**WiFi 已经能上网（IP 层全通），但域名解析没配 —— 所以「输 IP 能通、输网址打不开」。**
设置 App 点「WiFi 详情」卡死，是**原生详情页依赖的控制面命令没实现**，与网络是否通**无关**。

用户 2026-11-12 15:0x 的原话：
「设置里的 wifi 详情还是进不去啊，一点 WiFi 详情，设置 app 就卡死，但是可以退出到桌面，**我怎么联网**」

---

## 1. 用户「我怎么联网」的直接答案（不用进设置）

**这台设备的 WiFi 是开机自动连的，不需要在设置里点。**
启动参数（`console-exp-dhcpok.cap:1059` 的 boot-args）里写死了：

```
wifiauto=1 wifissid=ChinaUnicom-33E6TD wpass=<口令> netfix=3 netstack=1 netsvc=1
```

- 内核 DHCP 已拿到 `192.168.101.12`，装好默认路由：
  `DHCP: bound inet 192.168.101.12 mask 255.255.255.0 router 192.168.101.1 addr=0 rt=0`
  （`addr=0` = `ifioctl(SIOCAIFADDR)` 成功，`rt=0` = 默认路由成功）
- 装完地址后**系统自己发 ARP**，走真实 HTT TX_FRM 数据面，被 AP ACK：
  `picasso_wlan_tx: [REAL DATA PLANE] ARP 42 bytes` / `WPA: tx compl status=0 ack=10`
- 实时串口（设备当前就在系统里）显示数据面持续收发，不是假活：
  `rx=2388 beacons=2214 ord=104 msdu=105 eapol=3 bad=0/0`，
  且 `msdu` 从 chain-599 结束时的 50 涨到 105。

**所以：现在直接开 Safari 输 `http://<IP>` 就能通；输域名会失败，因为 DNS 没配（见 §2）。**

---

## 2. 唯一剩余的功能缺口：DNS 没配（本轮新发现）

### 2.1 证据（实机）

| 检查项 | 结果 |
|---|---|
| `DHCP: bound inet ...` | 1 次，有地址有路由 |
| 日志中 `DNS` / `dns` / `resolver` | **各 0 次** |
| `mDNSResponder` 是否 exec | **200 个 `PICASSO_EXEC ok` 里没有它** |
| 内核 DHCP 是否解析 option 6 | **否** —— `dhcp_option()` 里只有 `case 1/3/53/54`，无 `case 6` |
| 但 DISCOVER 是否索要了 DNS | **是** —— `hakios.dhcp.c:183-184` 的参数列表里写着 `*opt++ = 55; *opt++ = 5;` 然后 `1, 3, 6, 51, 54` |

⇒ **AP 提供的 DNS 服务器地址被静默丢弃了**。这是「有 IP、没域名」的直接原因。

### 2.2 参考项目对照（真机验证过）

`Trogdor-ipadOS/docs/plans/wifi.md:21`（同芯片 SC7180 + WCN3990 参考实现）：

> | 联网 | 已接入：peer、vdev 启动、关联、WPA2 四次握手和密钥、HTT 收发、**DHCP 和 DNS**；
> 驱动自己 `bpf_attach` 并 tap 收发 | 真机：认证、关联和四次握手完成，DHCP 和路由正常，
> **Safari 能打开 HTTPS**，已保存网络开机自动加入 |

⇒ 参考实现把 DHCP 和 DNS 列为同一项，两者都做完才「Safari 能打开 HTTPS」。
我们只做了一半（DHCP），所以停在「能通 IP」。

### 2.3 通用解法（用 Apple 官方通道，不手搓协议）

**关键发现（Apple 开源源码实证）：**

```
apple-oss-distributions/mDNSResponder
  mDNSPosix.h:117   #define uDNS_SERVERS_FILE "/etc/resolv.conf"
  mDNSPosix.c       ParseDNSServers(): fgets() + sscanf("%10s %15s") +
                    strncasecmp(keyword, "nameserver", 10) + inet_aton()
  mDNSPosix.c:1965  mDNSPlatformInit(): ParseDNSServers(m, uDNS_SERVERS_FILE);
  mDNSPosix.c:2017  注释："can call it in response to a SIGHUP"
```

即 **Apple 自己的 mDNSResponder 读一个纯文本 resolver 文件，每行一条 `nameserver <IPv4>`**，
不经过 configd、不经过 SystemConfiguration、不依赖任何 service。

**通用方案 = 让已有的 mDNSResponder 用它自己的格式拿到我们要给的 DNS 服务器**：
1. 内核 DHCP 解析 **option 6**（RFC 2132 标准选项，补全已有客户端的缺失项）；
2. 按 **Apple 标准格式**写 resolver 文件（`nameserver <IP>` 每行一条）；
3. 给 `mDNSResponder` 发 **SIGHUP** 让它重读（等价于桌面上的 `killall -HUP mDNSResponder`）。

**不手搓 DNS 协议、不造私有格式、不绕过 Apple 的守护进程。**

### 2.4 为什么「让 configd 原生链路跑通」这条路走不通（重要，别再走）

本轮查证（`boot.img` 路径扫描 + configd 开源树）：

- 设备 root volume 的 `/System/Library/SystemConfiguration/` 下**只有 2 个 bundle**：
  `EAPOLController.bundle`、`InterfaceNamer.bundle`。
  **`IPConfiguration.bundle` 不存在** —— iOS 的原生 DHCP 客户端在这个系统上**根本没有**。
- `work/refs/apple-oss/configd/Plugins/` 下也只有
  `IPMonitor / InterfaceNamer / KernelEventMonitor / LinkConfiguration / PreferencesMonitor /
  QoSMarking / SCNetworkReachability / SimulatorSupport / common`，
  **没有 IPConfiguration**（它是 iOS 闭源发行版才有的）。
- `boot.img` 里 `IPConfiguration` 出现 2 次，都是 **launchd job 名**
  （`com.apple.network.IPConfiguration`，与 `hostapd`/`velocityd`/`EAPOLController` 并列）。

⇒ **内核 DHCP 不是「手搓的简化旁路」，而是这个系统上唯一可行的 DHCP 实现** ——
Apple 自己在内核里就有同一个东西（`bsd/kern/netboot.c` 的 `inet_aifaddr()` /
`default_route_add()`，我们正是照它写的）。
同理，`mDNSResponder` 在设备上**存在**（`/sbin/mDNSResponder`），所以 DNS 可以交给它。

### 2.5 落地时的两个待验证点（下一轮直接做，别重新推导）

1. **写到哪个路径**：`/etc` 在 iOS 上是 firmlink 指向**已封存的根卷**（只读），
   所以 `/etc/resolv.conf` 大概率写不进去。需按候选列表逐个试写并打印每个的 vnode 错误：
   `/private/var/run/resolv.conf` → `/private/etc/resolv.conf` → `/etc/resolv.conf`。
   **让设备自己告诉我们哪个能写**，不要猜。
   （launchd 目录表里存在 `/private/var/run/mDNSResponder`，说明 Data 卷可写；
   而 `/private/etc/*` 是根卷条目。）
2. **mDNSResponder 会不会重读**：它只在 `mDNSPlatformInit()` 时解析一次，
   那时 DHCP 还没拿到租约（DHCP 在 t+168 s）。所以写完必须发 **SIGHUP**。
   内核可用 `proc_name()`（`kernel.macho` 里 17 处引用）遍历 pid 找
   `mDNSResponder`，再 `proc_signal(pid, SIGHUP)`（6 处引用）。
   项目内先例：`patches/picasso-ipc-census-xnu.patch:381` 就是这么找进程的。

### 2.6 已写好但**未完成**的实现（不要用，等重做）

`work/fix-wlan-dns.py` 写到一半，因增量编辑把 Python 三引号字符串截断而损坏；
**已把 `hakios.dhcp.c` 还原到干净基线，代码里没有半成品。**
该脚本的思路（option 6 解析 + vnode 写文件 + SIGHUP）是对的，
但**请整份重写**，不要在损坏的脚本上打补丁。

已经确认可用的部分（重写时直接用）：

- `vfs_context_current(void)` —— **无参数**，返回 `vfs_context_t`
  （`bsd/sys/vnode.h:1597`）。⚠️ 不是传指针的版本。
- `vnode_open(const char *path, int fmode, int cmode, int flags, vnode_t *vpp, vfs_context_t ctx)`
  （`bsd/sys/vnode.h:2164`）
- `vn_rdwr(enum uio_rw rw, struct vnode *vp, caddr_t base, int len, off_t offset,
  enum uio_seg segflg, int ioflg, kauth_cred_t cred, int *aresid, proc_t p)`
  （`bsd/sys/vnode.h:2354`）
- 完整可抄的写文件范例：`patches/picasso-ca-prefs-xnu.patch:39-67`
  （`vnode_open(O_CREAT|O_TRUNC|FWRITE|O_NOFOLLOW)` → `vn_rdwr(UIO_WRITE,…)` →
  `vnode_setattr` → `VNOP_FSYNC` → `vnode_close`）
- `proc_name(int pid, char *buf, int size)` / `proc_signal(int pid, int signum)`
  （`bsd/sys/proc.h:273` 有 `extern void proc_signal(int pid, int signum);`）
- 需要 `#include <sys/signal.h>` 才有 `SIGHUP`；另需
  `<sys/attr.h> <sys/mount.h> <sys/uio.h> <sys/vnode.h>`
- **写文件必须放在驱动线程**（`hakios_dhcp_tick`），**绝不能放 launchd exec 路径**
  —— chain-595/596 在 exec 路径 panic 过，v17 铁律明令禁止。

---

## 3. 设置 App 点「WiFi 详情」卡死 —— 已定位，尚未修

### 3.1 现象与证据（实时抓取，非推测）

设备当前就在系统里，实时串口抓到：

```
PICASSO_IPC Preferences pid=679
PICASSO_IPC Preferences receive=70 queued=5
PICASSO_IPC Preferences name=0xc03 msgs=1 qlimit=5 ... service=0
   (共 10 个 port，其中 5 个有排队消息)
PICASSO_WQ Preferences reqcount=0 nthreads=4 idle=1 scheduled=3
PICASSO_THR Preferences tid=11159 pid=679 name=Preferences state=0x1 pri=47 runnable_ms=0
   (7 个线程全部 runnable_ms=0)
```

- **不是 CPU 卡死**：所有线程 `runnable_ms=0`（不是上轮那种百万级饥饿），
  系统 CPU 只有 2~10%，用户能退出桌面。
- **是同步等待**：Preferences 主线程在等一个不会回来的 XPC 回复
  （5 个 port 有排队消息，没人应答）。

### 3.2 直接原因：驱动拒答 wifid 的两个控制面命令

`console-exp-dhcpok.cap` 的 apple80211 调用序列：

```
apple80211: wifid gets 12 (CARD_CAPABILITIES), value 0, 13 bytes: answered
apple80211: wifid gets 96 (?), value 0, 0 bytes: REFUSED      <-- 未实现
apple80211: wifid gets 97 (?), value 0, 16 bytes: REFUSED     <-- 未实现
apple80211: configd gets 12 (CARD_CAPABILITIES) ... answered
apple80211: configd gets 1 (SSID), value 0, 32 bytes: error 6  (ENXIO)
apple80211: the station says: the link, 1
```

- `REFUSED` = 驱动 `get()` 的 `default: error = EOPNOTSUPP;`
  （`wifi-wcn3990/source/hakios.apple80211.c:459-461`）。
- `type_name()` 表（`hakios.apple80211.c:63-78`）里**没有 96/97**，所以显示为 `?`。
- 参考实现 `work/refs/hakios/drivers/wifi/hakios.apple80211.c` **也没有实现 96/97**
  （两者的 `case` 列表只差一个 `case 106`）—— 说明这是 Apple 私有命令，
  参考项目同样没做。
- 逻辑链：设置 App 的 WiFi 详情页 → 问 wifid 网络状态 → wifid 发 apple80211
  请求 → 驱动对 96/97 回 `EOPNOTSUPP` → wifid 拿不到完整状态 →
  回给 Settings 的回复不完整 → Settings 主线程同步等 → 界面卡住。

### 3.3 下一轮怎么修（方向，不含未验证实现）

优先**让 wifid 拿到它要的**而不是让界面别等：
1. 先查清 96/97 到底是什么命令（Apple 私有 Apple80211 请求号）。
   线索：`wifid gets 97 (?), value 0, 16 bytes` —— 请求体 16 字节。
   查 Apple 私有 `Apple80211.h` 的请求号定义，或抓 wifid 二进制里的引用。
2. 若确认是 wifid 必需的能力（如 `PRIVATE_MAC_ADDRESS` / `AWDL` 相关），
   在 `get()` 里补实现，返回**真实数据**而不是空壳。
3. 若无法确认语义，**不要**用「未知命令一律返回 0 成功」这种糊弄法
   —— 那是 v13 之后被证明会把 `EOPNOTSUPP` 语义打乱的路。
   参考项目 `wifi.md` 提到过相关坑：「重复加入失败是 wifid 在
   `PRIVATE_MAC_ADDRESS_VALUE` 为空时退出」，说明 wifid 对某些字段为空的容忍度很低。
4. 设置 App 卡死**不影响上网**（§1），优先级排在 DNS 之后。

---

## 4. 组件总表（截至 v19）

| # | 组件 | 状态 | 证据 |
|---|---|---|---|
| 1 | 固件加载（WCN3990 / WLFW） | ✅ | `stage=3 state=5` |
| 2 | CE / HTC / WMI / HTT | ✅ | `htt=64 httidx=1127 mgmt=12` |
| 3 | 扫描（beacon/probe） | ✅ | `beacons=2214 mgmtev=2214` |
| 4 | 认证 + 关联 | ✅ | `eapol=3 bad=0/0` |
| 5 | VDEV_UP + WPA2 四次握手 | ✅ | `eapol=3` |
| 6 | 装钥 / PEER_AUTHORIZE | ✅ | chain-580 |
| 7 | 真实以太网收发进 Darwin 栈 | ✅ | `REAL DATA PLANE ARP` + AP ACK |
| 8 | en0 发布 / link up | ✅ | `en0 attached`、`link up` |
| 9 | 内核 DHCP（IP + 路由） | ✅ | `bound inet ... addr=0 rt=0` |
| 10 | **DNS（option 6 → resolver → mDNSResponder）** | ❌ **唯一功能缺口** | §2 |
| 11 | **设置 App WiFi 详情可用** | ❌ | §3（不影响上网） |
| 12 | IP 进 SystemConfiguration / 状态栏显示已连接 | ⬜ | 随 #10 |
| 13 | 凭据持久化（密码存盘） | ⬜ | `wpass=` 仍是 boot-arg 明文 |

---

## 5. 关键路径与位置速查

| 内容 | 位置 |
|---|---|
| 内核 DHCP 客户端 | `wifi-wcn3990/source/hakios.dhcp.c`（486 行） |
| option 解析（缺 case 6） | `hakios.dhcp.c` 的 `dhcp_option()` |
| 参数列表（已索要 option 6） | `hakios.dhcp.c:183-184` |
| 装地址 + 路由 | `hakios.dhcp.c` 的 `dhcp_install()` |
| apple80211 命令分发（default→EOPNOTSUPP） | `wifi-wcn3990/source/hakios.apple80211.c:294,409-461` |
| 命令名表（无 96/97） | `hakios.apple80211.c:63-78` |
| 提前绑定 IONetworkStack（**已提交未上机**） | `package/PicassoDisplay.cpp:350-363`，commit `2b941d3`/`b4c9616` |
| netsvc preferences.plist（DNS 字典为空） | `patches/picasso-network-timeout-xnu.patch` 的 `picasso_netsvc_plist` |
| 上机脚本 | `work/boot-variants.sh`（`SECS=400 bash work/boot-variants.sh <tag>`） |
| 打参脚本 | `work/mk-autojoin-args.py`、`work/mk-args.py` |
| 串口诊断 | `work/serial-cmd.py COM5 status 8` |
| 本地编译预检（**改 .c 后必跑**） | 见 §6 |

---

## 6. 下一步（按优先级，含可直接执行的命令）

### 步骤 1：补 DNS（唯一功能缺口）

1. **整份重写** `work/fix-wlan-dns.py`（§2.6 的签名与范例可直接用）。
   - DHCP `dhcp_option()` 加 `case 6`（存 `dhcp_dns[]`，第二组放 `dhcp_dns2[]`）
   - `dhcp_install()` 末尾调 `dhcp_publish_dns()`
   - `dhcp_publish_dns()` 按 Apple 格式拼 `nameserver %u.%u.%u.%u\n`，
     依次试写候选路径，每条都打印 vnode 错误码
   - 写成功后遍历 pid 找 `mDNSResponder` 发 `SIGHUP`
2. **本地编译预检**（省掉 10 分钟盲等 CI）：

```bash
wsl -e bash -lc '
cp ~/miphone/upstream/silenzio-picasso/wifi-wcn3990/source/hakios.dhcp.c /tmp/hakios.dhcp.c
python3 - <<"PY"
import json, subprocess
p="/root/miphone/device/out/picasso/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json"
raw=open(p).read().strip().rstrip(",")
d,_=json.JSONDecoder().raw_decode(raw)
a=list(d["arguments"]); out=[]; skip=False
for x in a:
    if skip: skip=False; continue
    if x=="-o": skip=True; continue
    if x=="-c": continue
    if x.startswith("-xc++"): out.append("-xc"); continue
    if x=="--driver-mode=g++": out.append("--driver-mode=gcc"); continue
    if x.startswith("-std=gnu++"): out.append("-std=gnu11"); continue
    if x.endswith("IOPlatformExpert.cpp"): out.append("/tmp/hakios.dhcp.c"); continue
    if x=="IOPlatformExpert.cpo": continue
    out.append(x)
out.append("-fsyntax-only")
res=subprocess.run(out, cwd=d["directory"], capture_output=True, text=True)
lines=(res.stdout+res.stderr).splitlines()
print("EXIT",res.returncode)
errs=[l for l in lines if "error:" in l]
print("ERRORS",len(errs))
for l in errs[:20]: print(l)
PY
'
```
   期望 `EXIT 0` / `ERRORS 0`。
   ⚠️ 已知坑：`vfs_context_current()` **无参数**；`SIGHUP` 要 `<sys/signal.h>`；
   新函数要在使用点**之前**有前向声明，否则
   `call to undeclared function` + `static declaration follows non-static declaration`。
3. 提交 → **单次**派 CI（`fast=true`，复用 seed，一次只派一个）→
   下载产物 → 上机 400 s。
4. **判据（checkpoint 明文）**：
   - `DHCP: dns servers <a.b.c.d>` ← option 6 真的解析到了
   - `DHCP: dns write <path>: error=0` ← 哪个路径能写（设备自己告诉我们）
   - `DHCP: dns published via <path>` + `DHCP: dns reload signalled pid=<n>`
   - 然后**用户实测**：Safari 输域名能否打开
   - 反面判据：0 `PICASSO CDC PANIC BEGIN`

### 步骤 2：上机验证已提交但从未验证的 `netstack=2`

`2b941d3`/`b4c9616`（提前绑定 IONetworkStack，让 configd 的 `InterfaceNamer`
能在 t+18 s 就 open，而不是 t+168 s）**从未上机跑过**（chain-598 的 A/B 被中断，
只留下对照组 `console-exp-n2ctl.cap` 250549 B）。

单变量 A/B 脚本：`work/chain-598.sh`（同镜像、同 995 B args，只差 1 字节 `netstack=1`↔`2`）。
**预期收益**：`InterfaceNamer` 能 open → 发 `quiet` → `PreferencesMonitor` 建立偏好
→ **IPConfiguration 风格的原生 service 出现** → 设置里的 WiFi 详情也能用（§3 可能一并解决）。
**但注意 §2.4：`IPConfiguration.bundle` 不在设备上，所以「原生 DHCP」这条路仍然不通，
这条修复的收益是 `quiet` + 设置界面，不是 DHCP。**

### 步骤 3：修设置 App 卡死（§3.3）

先查清 apple80211 请求号 96/97 的语义，再决定补实现还是别的解法。**不要**用
「未知命令返回 0」糊弄。

---

## 7. 铁律与假判据黑名单

**铁律**
1. 一次只派一个 CI；`fast=true`；复用 seed；不新建 userdata。
2. patch 一律 LF；改完用脚本重算 hunk 头；`git apply --check` 真实骨架验证。
3. 新增/修改 `.c` 后，**先跑 WSL 的 `-fsyntax-only`**，再派 CI。
4. 采集窗口 ≥ 400 s（HIF 在 t+168 s 启动，168 s 前结束的日志没有意义）。
5. **不改 launchd exec 路径**（chain-595/596 在此 panic 过）。
6. commit message 不写未实机验证的结论。
7. 用户自己那 6 处未提交改动（5 个 `docs/evidence/*.log` 删除 +
   `wifi-wcn3990/docs/diagnostic-charter.md`）原样保留，提交只 `git add` 自己的文件。

**假判据（不要再用）**
| 现象 | 为什么不能用 |
|---|---|
| `PICASSO_FBUC call=N` 停止 | patch 里有 `if (value < 160)` 打印上限，是诊断截断不是帧停止 |
| `PICASSO_GPU hangcheck: fence=2` | 所有组（含正常组）恒定，不是差异因素 |
| `PICASSO_IPC` / `PICASSO_THR` census | 诊断命令触发才有输出，有无取决于当时发没发命令 |
| `stack_open` 0 次 | `219b972` 里该探针**只在 entitlement 拒绝分支**，成功路径不打印 |
| `prefs_size=1102` | configd 重写过 plist（1786→1102），不能据此判定 service 建没建成 |
| 「0 panic」 | 只说明没崩，不代表功能通 |

---

## 8. 待补

- §2.5 的两个待验证点（可写路径、SIGHUP 是否生效）必须实机取证。
- §3.3 的 96/97 语义未查明。
- `netstack=2` 从未上机。
- 密码仍在 boot-arg 明文（`wpass=`），凭据持久化未做。
- 采集窗内从未实测过「Safari 打开 HTTPS」——这是最终验收动作，需要用户配合。
