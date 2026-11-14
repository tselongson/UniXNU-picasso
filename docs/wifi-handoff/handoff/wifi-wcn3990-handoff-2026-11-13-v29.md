# WiFi (WCN3990) 交接 — 2026-11-13 22:5x（v29：DNS 发布实机闭环）

> **本文件是当前唯一权威状态源。** v28 及更早均为历史归档，冲突以本文为准。
> 判据口径：checkpoint 明文 + 用户可操作性，不是「0 panic」。
> 本版核心：`857e48f` 的 DNS 路径在**完整 400 s 实机采集**里全部通过；
> 采集末期的整机 panic 经取证是 **UFS 存储故障**，与 WiFi/DNS 无关。
> **最终「能上网」仍需用户在 Safari 实测域名，不得由日志代判。**

---

## 0. 一句话状态

```
固件/HIF/扫描/auth/assoc/VDEV_UP/WPA2 四次握手/PTK+GTK/数据面 rx+tx ✅
内核 DHCP(192.168.101.12/24) ✅   en0 link / KEV / SIOCSIFADDR ✅
注入链四道关 ✅   *TIMEOUT* 终止标记 ✅   wifid sets ×22 ✅
── verify-608 完整 400 s 判决（857e48f）：
   configd started with = 1（崩溃循环已修）
   service disabled = 1
   dns wait=155s / dns servers=1
   dns set=1  Setup:/Network/Service/picasso-wifi/DNS      ✅
   dns set=1  State:/Network/Service/picasso-wifi/DNS      ✅
   dns set=1  State:/Network/Global/DNS                    ✅
   VERDICT: configd stable + DNS published to IPMonitor input keys
── 22:52 已用同一镜像 fastboot boot 进桌面，等用户 Safari 实测
⚠ 采集末期一次 UFS transport failure → disk5 I/O error → panic（独立存储问题）
```

---

## 1. verify-608 判决（22:44–22:52，完整 400 s）

`console-exp-c1.cap` **608,731 B / 7,951 行**（`work/verify-608-attempt-1.out`）。
前两轮（20:04 采集被后台回收截断、22:34 CDC 未枚举 0 字节）均无效，本轮首次完整。

| 判据 | 实测 | 期望 |
|---|---|---|
| `configd started with` | **1** | 1（chain-607 是 6） |
| `loaded in configd` / `quiet KERN_DENIED` | 1 / 1 | 1 / 1 |
| `key=Plugin:InterfaceNamer` `truthful=1 publish=1` | **True**（整行精确） | True |
| `service begin` / `service disabled` | 1 / 1 | 1 / 1 |
| `dns wait=` | **155s** | ≥1 |
| `dns servers=` | **1**（192.168.101.1） | ≥1 |
| `dns set=1` ×3（Setup/State per-service + Global 状态） | **3 / 3** | 3 |
| DNS INPUT KEYS SET OK / GLOBAL STATUS KEY | **2 / 1** | 2 / 1 |
| `wifid sets` | 22 | ≥1 |
| `DHCP: bound inet` / `DHCP: dns` / `SIOCSIFADDR` | 1 / 9 / 2 | ✅ |
| `join=5` | 33 | ≥1 |
| `DATASTORE ready=1` / `Early boot complete` | 1 / 2 | ✅ |
| 心跳末值 | `polls=43000 rx=552 beacons=208 join=5 ord=55 msdu=56 eapol=3 bad=0/0 ring=1/1` | 稳定 |
| 判读脚本 | **VERDICT: configd stable + DNS published to IPMonitor input keys** | — |

关键时序：`DHCP: dns 192.168.101.1 count=1` → `write /private/var/run/resolv.conf error=0 bytes=77`
→ 独立 DNS 线程等 155 s 读到 → 三个 dynamic-store 键全部 `set=1`。
这证实 v27 §2 的源码级结论（`State:/Network/Global/DNS` 是输出键、
per-service 键才是输入）与 §9 的闭环（`ALLOW_EMPTY_STRING` 保留
`SupplementalMatchDomains:[""]`）在真机上生效。

---

## 2. 采集末期的 panic 与 WiFi 无关（取证，不猜）

日志 7917–7950 行：

```
7917 NATIVE_UFS_FAILURE: first transport failure; mapped registers sampled read-only
7918 stage=media publication result=e00002d6      (kIOReturnTimeout)
7925 FAULT sample_mask=7 (HC=1 SMMU=2 CB=4)
7926 FAULT ... cb=0->400 syn=3f0412 far=4d16787bd950 ...
7931 PICASSO_LOG save failed; stopped
7932-7934 disk5: I/O error. ×3
7936 heartbeat polls=43000 join=5 msdu=56 ...   (数据面仍正常)
7948 === PICASSO CDC PANIC BEGIN ===
7950 ESR_EL1=...be000011 FAR_EL1=
```

⇒ 是 **UFS/NVMe 传输失败 → Data 卷 I/O error → 整机 panic**，
不是 SError / CE 寄存器 / VDEV_UP / DNS 发布引起（DNS 早在 6590–6592 行完成，
之后系统又稳定跑了 ~1300 行日志）。`be000011` 在历史所有 cap 中从未出现，
属本次新增的存储层事件；设备 panic 后按 `pshold` 自动回 fastboot。
**这是独立的存储可靠性问题，需单独跟踪，但不能据此判定 WiFi 失败。**

---

## 3. 22:52 已重新启动设备供用户实测

```
fastboot devices -> 64993333 fastboot
fastboot boot out/full-flash/boot-exp-c1.img
  Sending 'boot.img' (63580 KB) OKAY [1.428s]
  Booting OKAY [0.129s]   Finished 1.665s
```

同一 seed / 同一镜像，**未重新清 marker**（journal 已完整，v25：完整 journal
不再 grant reset；重清反而会重置 Data）。启动后 DNS 线程会再次自动发布
（每次 boot 都跑，不依赖上次状态）。

### 请用户按顺序实测（最终判据，串口日志判不了）

1. `http://192.168.101.1` —— 纯 IP 层（网关管理页）；
2. 一个域名，如 `https://www.apple.com/library/test/success.html` —— DNS + HTTPS；
3. 设置 > Wi-Fi 详情是否还卡（v28 §5 未闭环项）。

判读：
- **IP 通 + 域名通** ⇒ WiFi 全链路（上网）打通，剩余仅 UI/凭据/状态栏；
- IP 通 / 域名不通 ⇒ 原生 resolver 仍未消费 dynamic-store 配置，
  执行 v27 §9.1 独立 root 进程方案（pong `wifi-bootstrap.c --configure-only`）；
- IP 都不通 ⇒ 回到数据面，前面的 DNS 结论需重新审视；
- 设备黑屏/再次 panic ⇒ UFS 故障，属存储问题，与 WiFi 正交。

---

## 4. 未通清单（如实）

1. **Safari 端到端实测** —— ⬜ 从未在「关联稳定 + DNS 已发布」下做过，**当前唯一关键动作**；
2. 设置 App 点 Wi-Fi 详情卡死 —— 未复测（`ifaceuc=1` 已证伪，96/97 已 answered）；
3. 状态栏 / SystemConfiguration 显示已连接 —— 未验证；
4. 凭据持久化 —— `wpass=` 仍在 boot-arg 明文；
5. **UFS transport failure**（v29 §2）—— 新增独立存储可靠性问题；
6. 5G MGMT_TX `status=3`（非阻塞）。

---

## 5. 铁律（v28 §6 全部沿用）

新增 19：**采集窗口不完整（0 字节 / 早于 t+168 s）不得当判决**；
`work/verify-608.sh` 已内置 `< 20000 B` → `CAPTURE_FAILED_USB` 提前退出。
新增 20：日志末尾 panic 必须取证到具体子系统（UFS vs WiFi），
不能因「有 PANIC BEGIN」就否定整条 WiFi 链路。

---

## 6. 关键路径

| 用途 | 路径 |
|---|---|
| 唯一权威 | 本文 v29 |
| 有效判决 | `out/full-flash/logs/console-exp-c1.cap`（608,731 B）、`work/verify-608-attempt-1.out` |
| 修复提交 | `857e48f`（CI `43418179396`，产物 `picasso-bootimg-610`） |
| 上机链 | `work/verify-608.sh`（空采集会明确报 USB 失败） |
| 当前镜像 | `out/full-flash/boot-exp-c1.img`（995 B / wifiui=1 / wifiauto=1） |
| 源码依据 | v27 §2/§9、v28 §2、`work/refs/apple-oss/configd/Plugins/IPMonitor/` |
| 参考项目 | `UniXNU-pong/userland/wifi-bootstrap.c`、`work/refs/hakios/userland/src/net_test.m` |
