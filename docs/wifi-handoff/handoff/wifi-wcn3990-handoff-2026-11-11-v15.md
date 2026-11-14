# WiFi (WCN3990) 交接文档 — 2026-11-11 23:3x（v15）

> **本文件是当前唯一权威状态源。** v14 及更早为历史归档。
> 判据口径：**checkpoint 明文 + 用户可操作性，不是「0 panic」**。

## 0. 一句话状态

驱动侧全通（实机：WPA2 四次握手 / PTK+GTK / PEER_AUTHORIZE / HTT TX 被 ACK /
帧进协议栈）。**本轮攻的是 software 侧**：KEV 链路事件报文、configd 的
IPv4 服务、InterfaceNamer 命名握手，外加一个让多轮上机白跑的确定性
启动 bug（rmtfs 高位地址）。四个新提交都在 `kd439-pool`，CI 全绿。
最终一轮 `chain-593.sh` 待上机判据。

## 1. 本轮四个提交

| commit | 内容 | 实机判决 |
|---|---|---|
| `8703f5b` | radio `.up=true→false`，出生如实 link down | `console-exp-linkup2.cap`：`attached ... link down` → `keys installed` → `en0's link is up`；**SIOCSIFADDR/DHCP/inet 全 0** |
| `54b5d8d` | ① KEV 报文 4 字节错位 ② IFF_UP/IFF_RUNNING ③ configd ioctl 可见 | `console-exp-fix3b.cap`：`KEV_DL_LINK_ON size=48 hdr=24 family=2 unit=0 name=en fix=3`，configd `SIOCGIFMEDIA` ×8；**SIOCSIFADDR 仍 0** |
| `6285168` | 按同芯片 CoachZ 移植免 entitlement 的 `IONetworkStack` user client（`netstack=1`） | `console-exp-ns1.cap`：`IONetworkStack user client bound (netstack=1)`，0 PANIC；该轮被 rmtfs bug 截断，未走到握手 |
| `c75f96d` | rmtfs 缓冲限制在 4 GiB 以下（`max_pnum=0x100000`） | CI `43049291288`，待上机 |

## 2. 两个被实机钉死的根因

### 2.1 KEV_DL_LINK_ON 报文布局（`54b5d8d`）
`struct kern_event_msg` 尾部有 `event_data[1]` 占位；`sizeof()` 比线头
`KEV_MSG_HEADER_SIZE` 大一个 word。原 `hakios.net.c` 把 `net_event_data`
放在 `(header+1)` 且 `total_size=sizeof(message)` → `if_family/unit/name`
整体偏移 4 字节，configd 匹配不到 en0。已按已联网机型
`unixnu-wcn7860/PeachNet.c:246-260` 修正，用
`__builtin_offsetof(..., event_data[0])` 求线头（不依赖头文件宏）。
同时补 `IFF_UP`（attach）与 `IFF_RUNNING`（link）以及 configd ioctl 的
有界日志。`netfix` 位掩码可单变量 A/B，默认 3。

### 2.2 DHCP 不通 = 设置主页点 Wi-Fi 详情卡死（同一个根因）
证据链（本地 Apple OSS 源码，已 clone 到 `work/refs/apple-oss/`）：
- 上游 `docs/build168-settings-brightness.md:3-7`：Preferences 卡在
  `PSGAboutDataSource::_macAddress -> SCNetworkInterfaceCopyAllWithPreferences
  -> dispatch_once`，等 `Plugin:InterfaceNamer`。
- `configd/.../ifnamer.c:3965` `updateInterfaces()` 在
  `S_connect == MACH_PORT_NULL` 时直接 return；`:4108` `S_connect` 来自
  `IOServiceOpen(IONetworkStack)`；`:4159` `quietCallback` 同样硬门槛。
  `WaitStackTimeout` 超时只发布 TIMEOUT 状态，**不会创建 S_connect**。
- `kernel.macho` 有 `IONetworkStack::initWithTask - No Entitlement`：
  原装 user client 要求 entitlement，本移植没有 → open 永不成功。
- 用户现场：只有设置 App 卡、可回桌面、清后台后能滑动、再点 Wi-Fi 又卡
  → 正是这个进程内一次性同步等待，不是系统级死锁、不是 WiFi 开关。
- 参考实现：`work/trogdor/port/CoachzWifiNet.cpp:499-529`（同 WCN3990）与
  `unixnu-wcn7860/.../PeachWLAN.cpp:33-48,99-115` 都自己提供
  `IONetworkStack` + 宽松 user client（`setProperties` 返回成功）。
  本机预链接了真 IONetworkingFamily，定义同名类会 duplicate-metaclass
  panic（11-06 教训），所以保留真服务，只把 `IOUserClientClass` 指向
  自定义的 `HakiosNetworkStackUserClient`（`netstack=1`）。
- `ifnamer.c:1242` `registerInterfaceWithIORegistryEntryID` 用的是
  `IOConnectSetCFProperties` → 对应 user client 的 `setProperties`，与实现吻合。

### 2.3 确定性启动 bug：rmtfs 缓冲 ≥4 GiB（`c75f96d`）
| 轮次 | 采集跨度 | rmtfs pa | iovec | 走到握手 |
|---|---|---|---|---|
| linkup2 / fix3b | 349 s | `0xd99da000` / `0xcdbb3000` (<4G) | ok | ✅ |
| fix3 / fix0 / ns1 | **169 s** | `0x100bff000` (>4G) | `rmtfs iovec invalid phys_offset bff200` | ❌ |

modem Q6 只发 32 位物理地址；`kmem_alloc_contig` 第 5 参 `max_pnum=0`
是无限制（XNU `osfmk/vm/vm_resident.c:7146`）。高位页让每个 iovec 被判
越界 → MPSS 起不来 → HIF 不启动 → 15 s APSS watchdog 在 t+169 s 复位。
这解释了 3 轮"断流"，不是偶发竞态。修为 `max_pnum=0x100000`。

## 3. 实机证据文件

- `out/full-flash/logs/console-exp-linkup2.cap`（8703f5b，589 KB，跑满）
- `console-exp-fix3b.cap`（54b5d8d + netfix=3，671 KB，跑满，KEV 正确）
- `console-exp-fix0.cap` / `fix3.cap` / `ns1.cap`（被 rmtfs bug 截断，勿当反例）
- `work/chain-590.sh`、`work/chain-593.sh`（可复现链）

## 4. 待办（按顺序）

1. **`chain-593.sh`**：CI `43049291288` 产物 + `netfix=3 netstack=1`，400 s。
   判据：`rmtfs: buffer ... pa < 0x100000000` → 无 `iovec invalid` →
   `user client bound` → 握手 → `KEV_DL_LINK_ON` → **`SIOCSIFADDR`** →
   DHCP → `inet`。若 `SIOCSIFADDR` 仍 0，查真实 `IONetworkStack` 是否
   override `newUserClient` 忽略 `IOUserClientClass`（必要时改为在
   `is_io_service_open_extended` 里定向放行，别猜）。
2. **用户实测**：设置主页点 Wi-Fi 详情是否还卡（DHCP 那一轮上机后请用户点一次）。
3. **凭据持久化**：`wpass=` 目前明文写在 boot-arg（不进仓库、不打印），
   正式使用应移出；参考 CoachZ 的已保存网络。
4. **真实上网验证**：DNS / HTTP / 路由（拿到 inet 之后才有意义）。
5. 阶段 4 剩余：`IO80211Interface is absent` 导致接口回退
   `IOEthernetInterface`，若设置里 WiFi 类型显示不对再处理。

## 5. 关键文件 / 铁律

| 用途 | 路径 |
|---|---|
| 本轮脚本 | `work/fix-net-service.py`、`work/fix-netstack-userclient.py`、`work/fix-rmtfs-lomem.py`、`work/mk-netfix-variants.py`、`work/chain-593.sh` |
| 本地 Apple 源码（已 clone，别再逐次 WebFetch） | `work/refs/apple-oss/configd`（9.8 M）、`work/refs/apple-oss/xnu`（41 M sparse） |
| 参考实现 | `work/trogdor/port/CoachzWifiNet.cpp`（同 WCN3990）、`unixnu-wcn7860/source/bsd/dev/qcom/PeachNet.c`、`.../iokit/Kernel/arm/PeachWLAN.cpp` |
| 上机 | `SECS=400 bash work/boot-variants.sh <TAG>`；`python work/serial-cmd.py COM5 status 8` |
| CI | `gh workflow run compose-bootimg.yml -R tselongson/UniXNU-picasso --ref kd439-pool -f fast=true` |

铁律（沿用 v14）：一次只派一个 CI、`fast=true` 复用 seed；采集窗口必须
>168 s（默认 400 s）；判据是 checkpoint 明文；patch 必须 LF、hunk 头重算、
改 C 后跑括号平衡 + `abi-check.py`；commit message 不写未实机验证的结果；
用户自己的 3 处未提交改动 + 5 个 `docs/evidence/*.log` 删除 +
`diagnostic-charter.md` 未提交改动**一律不提交**；磁盘 ~6 GB 可用，
未删工作区文件、未清回收站。

## 6. 23:5x 最终判决（`chain-593.sh`，CI 43049291288 / c75f96d，400 s）

`console-exp-final.cap`（587765 B / 7089 行，跑满 349 s，0 PANIC）：

```
net: en0 attached on Picasso WCN3990 Wi-Fi (a radio), link down
HakiosWLAN: IONetworkStack user client bound (netstack=1)
PicassoModem: rmtfs: buffer 2097152 bytes at pa cdde0000      <- <4 GiB, 修复生效
net: en0's link (Picasso WCN3990 Wi-Fi) is up
net: en0 KEV_DL_LINK_ON size=48 hdr=24 family=2 unit=0 name=en fix=3
WCN3990-HIF: WPA: msg3 seen, msg4 sent, keys installed, peer authorized, link up
```

`rmtfs iovec invalid` = 0，`stack_open` = 0，`by configd`（SIOCGIFMEDIA）= 8，
**`SIOCSIFADDR` / `DHCP` / `inet` 仍全为 0**。

⇒ 三个已定位的 bug 全部按预期生效，但 **InterfaceNamer 从未调用
`IOServiceOpen(IONetworkStack)`**（`stack_open` 0 次），因此：
- configd 不给 en0 建 IPv4 服务 → 无 DHCP；
- `Plugin:InterfaceNamer` 完成通知不发布 → Preferences 的
  `SCNetworkInterfaceCopyAllWithPreferences / dispatch_once` 继续等待。

**新增离线证据**：CI 产物的 `linux-inputs/source-var-manifest.json` 里
**没有** `/Library/Preferences/SystemConfiguration/preferences.plist`
（只有 `.GlobalPreferences.plist`），即 seed 未预置任何网络 service；
`data-audit.json` 的 `preferences` 列表也不含网络项。这是下一轮要验证的
候选根因（configd 无 service 可配置），不是已确认结论。

**下一轮取证方向（按优先级，别猜）**：
1. 从 Data 卷 `db/diagnostics` + `db/uuidtext` 读 configd unified log
   （v14 §5.2 的 CoachZ `log show` 路径），确认 InterfaceNamer 插件是否
   加载、`stackCallback` 是否触发、`Plugin:InterfaceNamer` 是否写过。
2. 若插件未加载：查 `picasso_ifnamer_mount()` 的 bindfs union 是否让
   `InterfaceNamer.bundle` 的签名/加载失败（`config stage=5 error=0`
   只证明 mount 成功，不证明 bundle 被加载）。
3. 若插件已加载但无 service：在 seed/运行时补
   `SystemConfiguration/preferences.plist` 的 Wi-Fi IPv4 service。
4. 请用户在**当前仍运行的设备上**点一次「设置 → Wi-Fi 详情」，确认
   `netstack=1` 是否已解除 UI 卡死（串口日志看不到 UI，只能用户判）。

`IOService::newUserClient` 默认实现（XNU `iokit/Kernel/IOService.cpp:8196`）
确实按 `IOUserClientClass` 属性创建实例，`netstack=1` 的绑定路径无误；
`SCNetworkInterface.c:1790` 判 Wi-Fi 看的是 controller 是否
`IOObjectConformsTo("IO80211Controller")`（我们是），
故 `IO80211Interface absent` 回退 `IOEthernetInterface` 不是当前阻塞点。
