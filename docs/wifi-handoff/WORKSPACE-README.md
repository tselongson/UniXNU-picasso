# picasso XNU 移植 — 工作区

> **发布说明**：本文是本地工作区 README 的快照，链接已重定位到本目录。文中以行内代码出现的 `src/...`、`docs/build-handoff-...` 路径属于本地其它检出的文件，未随本仓库上传；`handoff/`、`logs/` 链接在本目录内有效。

Redmi K30 5G（picasso / SM7250）上运行 XNU（Darwin 23 / iPadOS 17.7.11）。
目标是从 UEFI 引导走到挂载根卷、启动 `launchd`，并把外设（当前主攻 WCN3990 WiFi）做到可用。

> **当前唯一权威状态源**：[`handoff/wifi-wcn3990-handoff-2026-11-14-v35.md`](handoff/wifi-wcn3990-handoff-2026-11-14-v35.md)（v35，2026-11-14 23:3x；en0 lladdr 与空口 STA MAC 不一致是根因，已修复并默认化，PC ping 6/6、`uni=41` 实机闭环；`pspoke`（chain-619）、`pskeep`（chain-621）均实机证伪；chain-622 拿到 CE1 的 `drri1`，证明停摆是**固件停止整个 target→host 方向**（CE1+CE2 一起停、`drri1==sw1`、`nb1=0`）而 TX 仍工作，下一刀是停摆时发 WMI 命令探活 + 主动 VDEV_DOWN/UP 重关联，见 §5.4）。
> 本文件与 `docs/*.md` 里的阶段状态是历史归档，冲突时以交接文档为准。
> 最新逐轮事实：`logs/2026-11-14.md`（工作日志，追加不覆盖）。

## 当前状态（2026-11-14 18:0x）

```
固件/HIF/握手/密钥/DHCP/DNS/键盘/发送/广播接收 ✅
  ── ★ 单播双向不通 ❌（心跳无上限计数 uni=0）
  ── ★★ 协议栈不回 ARP ❌ 已定位到 if_input，但本树 struct ifnet 不透明、不可赋值
  ── 运行期卷 I/O 挂死 → UI 冻结（另一条线，未取证）
```

本轮（v33→v34）关键结论，全部有实机或源码依据：

1. **PTK 推导完全正确**：用 `console-exp-y0.cap`（wpadiag=1）的 M1/M2 原文，
   Python 独立复算 PMK[0:4]=`a28efd65`、KCK[0:4]=`4a0a0614`，与设备日志逐字相同；
   完整 PTK/TK 也由同一 PRF 得出。**"密钥算错"排除。**
2. **主机侧与两条参考逐项一致**：key 命令布局、cipher ID、`0x5003` 语义、
   WMI INIT、join 序列、TX 帧构造（含 TID）、RX 路径，均与 ath10k/Trogdor 相同。
   **"驱动算法不如参考"排除。**
3. **发现新故障：协议栈不回 ARP**。网关在 DHCP 装址后两次广播问
   `192.168.101.18`，驱动收了、交给了 `ifnet_input()`，协议栈一次 reply 都没发；
   PC 侧 `ping 192.168.101.18` 6/6 超时、ARP 表为空。⇒ 别人也无法找到我们。
4. **`if_input` 这条路探到底**：本树导出 `if_var.h` 但 `struct ifnet` 不透明
   （`struct __ifnet` 仅前向声明），`if_ethersubr.h` 不导出
   ⇒ 驱动层不可赋值，需改内核侧。省掉重复尝试。
5. **三个假设已实机证伪，不要再试**：`rekey`（chain-613）、
   `keyfix`/TKIP MIC（chain-614）、`pspoke`（chain-615）。
6. 已交付 `net: in #n len=.. rc=.. netin=..` 探针（`58cd878`，CI `43581180860` success）：
   一次启动即可判定 `ifnet_input()` 是否失败。

下一棒第一步：跑一次 400 s 看 `rc`；`rc!=0` ⇒ 改内核侧挂 `ether_input`；
`rc==0` ⇒ 转查 `arp_input` 的地址匹配与 `SIOCAIFADDR` 的广播地址。
ARP 通后，**PC ping 手机**就是单播 TX 的独立判据。

模块已合并到上游最新分支 `stage12/picasso-ios16`（`00ea4a0`）：
fork 分支 `wifi-wcn3990-stage12` @ `2cb67cd`，CI success，5 个 integration
补丁，详见交接文档。

## 目录

| 路径 | 内容 |
|---|---|
| `upstream/silenzio-picasso/` | **当前主线设备仓库**，分支 `kd439-pool`，GitHub Actions 出可 `fastboot boot` 的 `boot.img` |
| `handoff/` | 交接文档；`wifi-wcn3990-handoff-2026-11-11.md` 是 v10 权威版，11-10 v9 及更早为归档 |
| `logs/` | 按日工作日志（10-26 → 11-10），追加不覆盖，是逐轮实验的事实来源 |
| `docs/` | 硬件资料、早期移植计划、构建交接、GPU 路径；多为 11-02/11-03 的历史归档 |
| `work/` | 实验链 `chain-5*.sh`、抓日志/改参数/ABI 门禁脚本；`work/refs/ath10k/`、`work/refs/ath10k-full/` 是 Linux 主线本地参考树 |
| `work/trogdor/port/WlanRadio.c` | 同芯片（SC7180+WCN3990）在 XNU 上真机跑通的完整栈，5627 行，最重要的逐字对照对象 |
| `out/` | CI 产物、镜像、串口日志 `.cap`；`out/full-flash/logs/` 是判读输入 |
| `upstream/` | 四个上游移植克隆，只读参考（`UniXNU/` 为编排器本体） |
| `src/YinCQL_UniXNU-picasso/` | 另一条移植线（YinCQL 派生），停在 APFS 容器方案探测 |
| `archive/` / `tools/` | 原始交接包 / `gh.exe` 等本地工具 |

## 构建与上机

CI 约 9–14 分钟，**必须带 `-f fast=true`**（否则 host-check 失败、compose 被 skip、拿不到 boot.img）；
`git push` 不触发构建：

```sh
tools/bin/gh.exe workflow run compose-bootimg.yml -R tselongson/UniXNU-picasso --ref kd439-pool -f fast=true
tools/bin/gh.exe run list -R tselongson/UniXNU-picasso --limit 3
```

下载产物、打槽、生成单变量镜像、上机 220s 并判读，走现成的链脚本：

```sh
bash work/chain-573.sh        # 见脚本头部注释：改 RUN / COMMON / 变量即可复用
```

关键约束（详见交接文档 §4、§6）：

- **只用 `fastboot boot`，从不 `fastboot flash`** —— 镜像只加载到内存，不写分区。
- boot-args 区是**硬上限 995 B**：先 `work/make-545.py` 打槽，再 `work/mk-args.py` 重排；
  新 CI 镜像必须打槽，旧镜像已打过、再打会溢出。
- 单变量 A/B + 控制组 + **220s 采集**（HIF 约 t+94s 才启动）；判据是 checkpoint 明文，
  不是「0 panic」。组名跨日复用前必须删旧 `.img`/`.cap`。
- 提交前 `python3 work/abi-check.py` 必须 0 HARD；未实机验证的提交写
  「Device verification pending」。
- 实验后设备不会自己回 fastboot：`work/ensure-fastboot.sh` 或
  `python work/serial-cmd.py COM5 reboot-fastboot 3`。
- 证据规则见 `upstream/silenzio-picasso/wifi-wcn3990/docs/diagnostic-charter.md`。

构建侧的历史状态与已修问题见 `docs/build-handoff-2026-11-02.md`。

## 另一条线：`src/YinCQL_UniXNU-picasso`

内核跑过 BSD 层、USB 控制台可用、UFS 已发布根设备、APFS 读到容器 checkpoint（xid 14），
卡在 **APFS 容器方案探测失败**。文档入口：
`src/YinCQL_UniXNU-picasso/docs/status.md`、
`src/YinCQL_UniXNU-picasso/docs/handoff.md`、
`src/YinCQL_UniXNU-picasso/docs/ufs-root.md`、
`src/YinCQL_UniXNU-picasso/docs/debugging.md`。

## 设备操作

- 进 fastboot：长按电源 10 秒 → 电源 + 音量下
- 正常失败不需要手动断电：panic 会经 `picasso_usb_reset=pshold` 自动回 fastboot
- 只有设备完全死透（无串口、无 fastboot）才需要物理断电
- 串口：CDC-ACM，Windows 上是 `COM5`，macOS 上是 `/dev/cu.usbmodemPICASSO011`
