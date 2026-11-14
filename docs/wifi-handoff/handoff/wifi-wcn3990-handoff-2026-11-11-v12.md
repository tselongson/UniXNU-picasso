# WiFi (WCN3990) 项目交接文档 — 2026-11-11 15:4x（v12）【历史归档】

> ⚠️ **已被 `wifi-wcn3990-handoff-2026-11-11-v13.md` 取代，仅作历史对照。**
> v12 写于「修复已提交、CI 在跑」的时刻；v13 补齐了实机验证结果与剩余 4 阶段清单。
> 冲突时以 v13 为准。

---

## 0. 用户真正的问题（v11 判读漏掉的部分）

用户现场三连问：
1. 「之前 WiFi 也能开机，但是会卡死在锁屏没反应，现在也是，完全没反应」
2. 「之前试过把 WiFi 启动推迟到 UI 启动完毕（进锁屏以后），但是 WiFi 一启动还是又卡死了」
3. 「WiFi 还没启动的时候有反应」

**v11 只修好了「加 WiFi 就 panic 开不了机」，并把 `0 PANIC` +
`Early boot complete` 当成成功——这是判读失误。** 用户要的是「WiFi 和系统并存」
= 系统能正常操作 + WiFi 也在，不是不崩就行。上面第 ③ 条直接锁定了因果：
**WiFi 一启动就锁死 UI，与开机顺序无关；推迟启动只是把卡死时刻后移。**

---

## 1. 当前状态（2026-11-11 15:4x）

| 层 | 状态 | 证据 |
|---|---|---|
| 加 WiFi 开不了机（Data seed 不匹配 panic） | ✅ 已修 + 实机闭环 | `9572b30` / CI `42973205561` / `console-exp-wifidfix.cap` 0 PANIC、`DATASTORE ready=1`、`stage=4 error=0` |
| WiFi 与系统「不崩地并存」 | ✅ 已达成 | 同上：en0 attach、心跳到 polls=187000 |
| **WiFi 启动后 UI 可操作** | ❌ **本轮定位到根因，修复已提交、CI 在跑** | 见 §2/§3；`171c883` / CI `42981481378` |
| 设置里 Wi-Fi 开关 | ❌ | `PICASSO_JOBS error=22`（v11 §4），独立遗留 |
| DHCP / 凭据持久化 | ⬜ | 依赖 wifid |

---

## 2. 锁屏卡死的精准根因（有实机证据，不是猜）

### 2.1 时间线（`.times` 字节偏移→墙钟，精确）

| 组 | 采集窗 | HIF 启动 | `d1/d2 busy` @HIF 前 → 后 |
|---|---|---|---|
| wifidfix | 349 s | **168 s** | 8/12 → **100/100（持续到 349 s）** |
| m3 | 349 s | 168 s | 同型 100/100 |
| k13 | 349 s | 168 s | 同型 100/100（用户当时就报「完全没反应」） |
| k12 | 169.6 s | 168 s | 69/66（**采集在卡死前就结束了**） |
| g13 / g7 | 169 s | 88 s | 11/6、51/52 |

→ **这解释了为什么历史上 k12/g7 被判成「能进系统」，而长窗口的 k13 被用户判「没反应」：**
前者采集窗口恰好在 168 s 卡死点之前结束。交接文档 v10 §4.2 把 150 s 延迟
当成修复，实际只是把卡死推迟到采集窗外。

### 2.2 卡死瞬间的实时串口（设备当前就冻在锁屏，`serial-cmd.py COM5 status`）

```
PICASSO_CPUFREQ ... d1 mhz=2208 busy=100 ps=7 ... d2 mhz=2400 busy=100 ps=7 ...
TOUCH d_frames=0 d_hid=0 d_begin=0 d_edge=0 d_skip=0 phys=0 count=0
PICASSO_THR runnable total=46
  SpringBoard  runnable_ms=1438818   backboardd runnable_ms=...
  configd      runnable_ms=1421155   launchd   runnable_ms=...
  kernel_task  runnable_ms=0..8      (内核线程正常拿 CPU)
PICASSO_GPU hangcheck: polls=3197 fence=2 hang_fence=2
WCN3990-HIF: heartbeat polls=1547000   (实测 ~1078 polls/s，永不停止)
```
即：**两大核 100% 忙、46 个用户态线程可运行却拿不到 CPU、触摸事件全零。**

### 2.3 代码级根因

`wifi-wcn3990/source/PicassoWlan.cpp`：
```c
#undef IOSleep
#define IOSleep(ms) picasso_sleep_ms(ms)     /* -> machine_delay_until(): 纯自旋 */
```
`machine_delay_until()` 是 XNU `osfmk/kern/clock.c` 的**忙等自旋分支**
（仅在 `ml_delay_should_spin()` / 抢占关闭 / 中断关闭时使用）；让出 CPU 的
路径是 `clock_delay_until -> assert_wait_deadline + thread_block`。
Apple 官方文档明确：`IOSleep` 阻塞调用线程给其他进程时间，`IODelay` 才自旋。

**两个无限循环用了这个忙等**：
| 循环 | 条件 | 每轮 |
|---|---|---|
| `modem_thread` GLINK watch | `glink_ms` 默认 **0** ⇒ `for(ms=0; glink_ms==0 || ms<glink_ms; ms++)` **永不退出** | `glink_poll` + `picasso_wlan_ce_poll` + `picasso_usb_console_drive_poll` + `IOSleep(1)` 忙等 |
| `modem_monitor` | `for(;;)` | `IOSleep(500)` 忙等 |

两个各钉住一个大核 → 正好对应 `d1/d2 busy=100`；另有
`hakios.net.c net_thread` 每 2 ms 经 `link->poll` 再调一次同一个 HIF poll。

**关键的板级佐证**：`HakiosCompat.cpp` 用的是**原生** `IOSleep(150000)`
（该文件没有宏覆盖），本板 HIF 准时在 ~168 s 启动，且这 150 s 内
UI 完全有反应 ⇒ 原生 `IOSleep` 在本板确实会阻塞让出 CPU。
这也直接解释了用户第 ③ 条观察。

### 2.4 已排除的假判据（避免下一轮重踩）

- `PICASSO_FBUC call=N` 在 `unixnu-source-kernel.patch` 里是
  `if (value < 160)` 才打印 → **160 是硬上限**，"FBUC 停止"是诊断截断，
  不是帧停止。k12/k13/g7 都恰好 160 就是证据。
- `PICASSO_GPU hangcheck fence=2` 在**所有组（含正常组）**恒定，非差异因素。
- `PICASSO_IPC / PICASSO_THR / PICASSO_STK` census 是 `status` 命令触发的
  诊断输出，有无取决于采集时是否发命令，不能当判据；但**一旦发出，其内容
  （runnable_ms）是有效证据**。
- SMMU / CE / SError：本轮 0 PANIC、`gfsr=0`，与 11-08~10 那批崩溃无关。

---

## 3. 修复（`171c883`，CI `42981481378` 在跑）

- 在 `#undef IOSleep` **之前**用 `picasso_yield_ms()` 捕获原生 `IOSleep`
  （定义在宏之前，否则会被展开成自旋版本 → 修复变 no-op；脚本有顺序自检）。
- `modem_monitor` 的 500 ms、GLINK watch 的 1 ms 两处忙等改为阻塞等待；
  GLINK tick 由 1 ms 改 **4 ms**（确保超过 `ml_delay_should_spin` 阈值，
  轮询量降 4 倍，CE ACK 仍在 CoachZ 的几 ms 服务窗口内）。
- 微秒级硬件握手 `IODelay(10/100)` 保持 `machine_delay_until` 不变。
- `wlan_spin=1` 恢复旧忙等，供单变量 A/B。
- 门禁：`work/abi-check.py` 0 HARD；可复现脚本 `work/fix-wlan-cpu-starve.py`。

**验收判据（不是 0 panic，必须实测）**：
1. HIF 启动（t+168 s）后 `PICASSO_CPUFREQ d1/d2 busy` **不再 100/100**；
2. `TOUCH d_*` 出现非零计数（自动 home/swipe 注入 + 用户实际触摸）；
3. `runnable_ms` 不再累积到百万级；
4. **用户实测锁屏可滑动、能进桌面、能操作** —— 这一条是最终判据。

---

## 4. 前序修复（v11，仍有效）

`9572b30`：恢复 `offline_disabled_jobs` 中 wifid 原位 → Data seed 身份不变，
`runtime_enabled_jobs` 加 wifid；补上 patch 的**双 entry 循环删除**
（v10 §5.3 漏掉，否则 wifid 放行无效、host test 会挂）。
CI `42973205561` success、复用 seed（1.18 MB，**不需要刷 5.87 GB userdata**）。
剩余 `PICASSO_JOBS error=22`：100/100 历史日志全失败，v10 §5.2 假设的
runtime 放行从未在真机生效；m3 基线同样如此，与本轮修复无关。
时间线取证已把范围压到 `picasso_jobs_metadata()` 的 `vnode_vtype != VDIR`
（iOS Data 卷 `/private/var` 是 firmlink），但**尚未实机确认**，
下一步需加分步诊断打印，不要直接放宽 SHA 白名单。

---

## 5. 关键文件 / 命令

| 用途 | 路径 |
|---|---|
| 本轮修复脚本 | `work/fix-wlan-cpu-starve.py` |
| v11 修复脚本 | `work/fix-wifid-runtime-jobs.py` |
| 卡死证据日志 | `out/full-flash/logs/console-exp-wifidfix.cap` (+`.times`) |
| 实时取证 | `python work/serial-cmd.py COM5 status 8` |
| 刷机采集 | `SECS=400 bash work/boot-variants.sh <TAG>` |
| 派 CI | `gh workflow run compose-bootimg.yml -R tselongson/UniXNU-picasso --ref kd439-pool -f fast=true` |

铁律：一次只派一个 CI；`fast=true` 复用 seed；单变量 A/B + 控制组；
**采集窗口必须长于 168 s（HIF 启动点），否则会得到「能进系统」的假阳性**；
判据是 checkpoint 明文 + 用户可操作性，不是 0 panic。

---

## 6. 下一步

1. CI `42981481378` 出包 → 刷 `boot-exp-wifidyield.img` → 400 s 采集
   → 按 §3 四条判据验收；同时跑 `wlan_spin=1` 组做单变量对照。
2. 若 UI 恢复：再攻 `PICASSO_JOBS error=22`（设置里 Wi-Fi）与 DHCP。
3. 若仍卡：在 `picasso_yield_ms` 里加分步打印，确认原生 `IOSleep(4)`
   是否真被 `thread_block`；并排查 net_thread 2 ms 轮询是否仍需降频。
