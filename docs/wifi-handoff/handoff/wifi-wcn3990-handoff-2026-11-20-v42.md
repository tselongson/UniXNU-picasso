# WiFi 全链路交接 v42（2026-11-20 13:15）

> **状态**：**WiFi 网络栈完全打通（应用层实证）**，代码已提交推送至 `kd439-pool`（`f11728d`），
> 镜像已上机验证。UI（SpringBoard 重启循环 / FBUC=0）是独立的显示问题，不属于 WiFi 范围。
> **唯一权威状态源**：本文件；v41 及更早为历史归档。

---

## 0. 一句话

```
v41 之后WiFi 从「有 IP 但 0 应用流量」走到「应用层 TCP/TLS 会话真实成功」：
内核自检（netprobe）证明驱动→固件→AP→NAT→公网→回程无断点，
四个用户态侧修复（netsvc 时序 / ifaceuc / ifaddr / 服务类型）让 configd 真正接管 en0，
最终日志里 DNS、NTP、TLS 的请求与响应逐条配对 —— 手机自己在跟 Apple 的公网服务器对话。
```

---

## 1. 决定性证据（round-final，f11728d 镜像，2026-11-20 13:00 实测）

| 层 | 出向 | 入向 | 说明 |
|---|---|---|---|
| DHCP | `0.0.0.0:68 -> 255.255.255.255:67` | `192.168.101.1:67 -> ...:68` | `bound inet 192.168.101.32 router 192.168.101.1 addr=0 rt=0` |
| DNS | `192.168.101.32:52340 -> 192.168.101.1:53` (87B) | `192.168.101.1:53 -> :52340` (110B) | 6 次查询全部配对响应 |
| NTP | `192.168.101.32:123 -> 17.253.60.37:123` (90B) | `17.253.60.37:123 -> :123` (90B) | **Apple 公网 NTP**，`timed` 同步成功 |
| TLS | `192.168.101.32:49153 -> 17.23.96.10:443` (78/54B) | （17.x:443 有回包） | TCP/TLS 握手 |
| TLS | `192.168.101.32:49154 -> 184.27.207.235:443` (583B ClientHello) | 1502B 满帧应用数据 | **完整 TLS 会话** |

- 内核自检（上一轮 netprobe）：`apple.com is 17.253.144.10`、`ICMP reply from 223.5.5.5`
- PC 侧 ping 设备：3/3、5ms
- 56 条 `:443` 会话记录

---

## 2. 今天的提交（全部已推送）

| commit | 内容 | 结果 |
|---|---|---|
| `8c65a81` | **netprobe**：内核自造 DNS 查询 apple.com + ICMP echo 223.5.5.5，走 `picasso_dhcp_send()` 同路径 | 链路全通实证 |
| `05dc7a4`→`99d8302` | iface80211（IOInterfaceType 71）实验 | **失败已回退**：WiFi 卡 GLINK doorbell，不重试 |
| `32d18d8` | netsvc 时序修复（首读 boot-arg=0 时 done 锁死永不重试）+ exec 日志 600 + `.gitattributes` 锁 patch LF | configd 启动时能读到服务 |
| `40f0755` | SIOCSIFADDR 返回 EOPNOTSUPP 交还协议栈（`ifaddr=1`）；preferences.plist Hardware/Type 改 Ethernet | configd 走完 IPConfiguration 第一步 |
| `47a2cc6` | `net_ports=1`（所有 IPv4 端口日志，96 条）+ XNU hunk 打印 connectx 失败码 | 看到了真实流量 |
| `f11728d` | **netpoll 真实生效**（此前只在参考树里，本树硬编码 2/10ms）+ 出向端口也打印 | 证据链闭环 |

## 3. 本轮证伪/修正的三个旧结论

1. **"netpoll=8 解开了 UI 饿死"** —— 错。实际编译的 `wifi-ath10k/.../hakios.net.c`
   从不读 netpoll（硬编码 `delay_for_interval(quiet<100?2:10)`）。那轮 UI 变好是
   netsvc/ifaceuc 修复的功劳。f11728d 才真正接上，默认仍 2ms（CE ring 实证值）。
2. **"ifaceuc=1 无效"** —— 错。带上后 `configd_open where=denied class=IOEthernetInterface`
   消失（早期启动阶段的 2 条 denied 仍会打，属正常时序）。
3. **"4283 帧/28MB 是应用联网"** —— 错。出现在 GLINK doorbell 风暴 + SpringBoard
   启动之前，下一轮同参数仅 78 帧，判定为 AP 侧广播/组播突发，不要当成功证据。

## 4. 遗留（不属于 WiFi）

- **UI**：SpringBoard 每轮 327-882 次重启循环、FBUC=0（黑屏）。这是显示/注入链路问题
  （v41 §12A 已有记录），与 WiFi 无关。手机黑屏时用户无法点 Safari，但后台服务
  （timed/nesession 等）的网络活动已实证正常。
- **NECP 警告**：`connectx() - failed necp_set_socket_domain_attributes` 只是
  tracker-domain 元数据设置失败的 printf（uipc_socket.c:1836），**不阻塞连接**，
  TLS 会话照常建立。无需修。
- **XNU patch hunk 已验证**：`git apply --check` 对 xnu-10063.141.1 真源文件通过。

## 5. 运维

- 构建：dispatch `tselongson/UniXNU-picasso`，workflow `compose-bootimg.yml`，`raphael_wifi=true`，分支 `kd439-pool`
- 最新 run：44675604973（`f11728d`）；本地镜像 `work/round-final.img`
- 常用上机参数（7 槽全用）：
  `wifissid=... wpass=... netpoll=8 netsvc=1 ifaceuc=1 ifaddr=1 net_ports=1`
  （dhcp 默认开，可不占槽；net_ports 打印双向 IPv4 端口，netpoll 现在真的生效）
- 未刷任何分区；测试失败用 `fastboot boot` 回 `out/full-flash/boot-normal.img`
- 诊断抓手速查：
  - 链路自检：`netprobe=1`（DNS apple.com + ICMP 223.5.5.5）
  - 应用是否开 socket：`net_ports=1`（双向端口，96 条）
  - connectx 失败：`PICASSO_NET connectx failed error=... ifscope=... flags=... pid=...`
  - configd 是否接管：`SIOCSIFADDR on en0 by configd (hand_to_stack=1)`
