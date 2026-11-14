# WiFi (WCN3990) 交接 — 2026-11-14 00:5x（v30：真实 TX 的 SError 定位与修复）

> **本文件是当前唯一权威状态源。** v29 及更早为历史归档，冲突以本文为准。
> 判据口径：checkpoint 明文 + 用户可操作性，不是「0 panic」。
> 用户 11-14 00:17/00:32 指示：**今晚不动设备（用户睡觉），自行上网查其它机型解决**。
> 本轮遵守：未刷任何分区、未重启设备、未清 marker；只做本地代码修复 + 云端 CI。

---

## 0. 一句话状态

```
固件/HIF/扫描/auth/assoc/VDEV_UP/WPA2/PTK+GTK/数据面/内核 DHCP ✅
注入链 / *TIMEOUT* / wifid sets×22 / DNS 三键 dns set=1 ✅（v29 verify-608）
── 用户 23:0x 实测：「网页打不开、开网页会重启」，再「还没输网址就重启」
── netpath=0 单变量（console-exp-notx.cap）：0 PANIC / 0 UFS failure / 0 HTT TX_FRM
   netpath=1（console-exp-webtest.cap）：1 PANIC / 5 UFS failure / 20 disk5 I/O
   ⇒ 崩溃 100% 由运行期真实 TX 数据面触发，与用户操作、DNS、configd 无关
── 根因（三方独立证据）：CE4 gather 覆盖仍在 DMA 的 live 描述符/缓冲
── 两个 commit（35e7113 修复 + 31fc804 诊断）已推，最终 CI 43468210876 **SUCCESS**
   （picasso-bootimg-612，seed 与设备 Data 一致，明早无需清 marker/刷 userdata）
   **未上机，不得宣称已修好**
```

---

## 1. 本轮通读范围（用户要求「读取最新交接文档和全部项目日志」）

- 交接：`v29`（最新权威）、`v28`、`v27`、`v26`、`v25` 全文；v24 及更早脉络沿用上一轮通读。
- 日志：`.workbuddy-ai/memory/2026-11-13.md` **全文 46 KB / 23:12 版**（含 23:0x 用户实测
  与 `netpath=0` 实验，另一套 `.workbuddy/memory/2026-11-13.md` 只到 17:25，**内容不全**）；
  11-12、11-11、11-10…10-26 各份上一轮已逐份亲自读完。11-14 无新日志（本轮新建）。
- 证据：`console-exp-webtest.cap`(522,794 B)、`console-exp-notx.cap`(614,551 B)、
  `console-exp-c1.cap`(608,731 B)、`work/webtest-capture.out`、`work/notx-capture.out`、
  两份 `.cap.times`，以及 `out/full-flash/boot-exp-{c1,notx}.img` 的 args 区。
- 代码：`wifi-wcn3990/source/Wcn3990Hif.cpp`、`Trogdor-ipadOS`/`work/trogdor/port/WlanRadio.c`
  （同芯片 WCN3990 真机跑通的完整栈）、`work/refs/ath10k/snoc.c`。
- 外网（按 `docs/research-playbook.md` §2 短查询 + 用户指定的 unixnu.com/devices 逐机型）：
  设备目录全表、`pong`、`picasso`、`sheng` 设备页与 sheng 仓库 README、
  LKML 官方补丁原文 ×2、CVE-2022-50700。详见 §3。

---

## 2. ★ 瓶颈取证：不是 DNS，是真实 TX 触发的整机 SError

### 2.1 用户实测（11-13 23:0x，v29 之后，未写进 v29）

用户报告「网页打不开、开网页会重启」，随后「还没输网址就重启」。
`console-exp-webtest.cap`（23:02，600 s 全程抓取）在用户**未输网址**时即
`PICASSO CDC PANIC BEGIN`，`ESR_EL1=...be000411`（v29 那次是 `be000011`）。

### 2.2 单变量 A/B 判决（本轮重新严谨统计）

`boot-exp-notx.img` 与 `boot-exp-c1.img` 的 args 区**仅 9 字节不同**
（`netpath=0` 是唯一新增 token），`wifiui=1` / `wifiauto=1` 均保留。

| 判据 | `netpath=1`（webtest） | `netpath=0`（notx，对照） |
|---|---|---|
| `PANIC BEGIN` / `ESR_EL1` | **1 / 1** | **0 / 0** |
| `NATIVE_UFS_FAILURE` / `disk5: I/O error` | **5 / 20** | **0 / 0** |
| `FAULT sample_mask=7 (HC=1 SMMU=2 CB=4)` | 1 | 0 |
| `cb=0->400 syn=3f0411 far=4516387a9850` | 1 | 0 |
| `REAL DATA PLANE` / `HTT TX_FRM` | **48 / 48** | **0 / 0** |
| `WPA: tx frame` | 58 | 3 |
| 伪造回包 `delivering DHCPOFFER/DHCPACK/ARP` | 0 | 3 |

⇒ **崩溃与运行期真实 TX 帧数强相关**，后台自发流量（IPv6 MLD / DHCPv6 / ARP）即可触发，
与用户是否操作 Safari 无关。`netpath=0` 走伪造 DHCP/ARP 路径，DNS 线程也因此
`dns resolver file absent after 240s`——它只是对照组，不是可用配置。

### 2.3 时序（逐行，不是推测）

```
6678  picasso_wlan_tx: [REAL DATA PLANE] ... over HTT TX_FRM
6679  WCN3990-HIF: WPA: tx frame ... slot=1
6680  NATIVE_UFS_FAILURE: first transport failure; mapped registers sampled read-only
      FAULT sample_mask=7 (HC=1 SMMU=2 CB=4)
      FAULT global=0->0 syn=0 cb=0->400 syn=3f0411 far=4516387a9850 id2=5111 cba2r=1
7932-7934 disk5: I/O error ×3  →  7948 PANIC BEGIN → 7950 ESR ...be000011
```
`notx` 组里这些行**一条都没有**。另有历史排除项（11-13 日志 §23:0x）：
`disk3: device is not ready` / `apfs_nstream_read` / `ChunkCompression.cpp` 在
13 份历史 cap 出现（最多 3499 次）且 0 PANIC；`wifid sets 383` 是 Trogdor
`docs/hardware-tests.md:1549` 记录的正常请求；CE 主体寄存器均已用 shadow doorbell。

---

## 3. ★ 精准定位：CE4 gather 复用 live DMA 缓冲（三方独立证据）

### 3.1 Linux 主线官方补丁（LKML 原文，WCN3990 SNOC 专属）

- `[RFC 1/2] ath10k: Set tx credit to one for wcn3990 snoc based devices`
  （Youghandhar Chintala, 2022-01-21, `lkml.indiana.edu/2201.2/05616.html`）：
  > "when copy complete interrupt for first DMA reaches HOST, CE has already
  > updated SRRI for both DMA transfers and is in the middle of 2nd DMA.
  > HOST uses SRRI to interpret how many DMA's have been completed and tries
  > to unmap/free both the DMA entries, but CE is still in the middle of
  > 2nd DMA which can cause SMMU issues or corruption."
  修法：`HTC_HOST_MAX_CREDIT_COUNT 1`，即同一时刻只允许 1 个未完成 TX DMA。
- `[RFC 2/2] ath10k: Delay the unmapping of the buffer`
  （`.../05614.html`）：`ath10k_htc_notify_tx_completion()` 对
  `ep->ul_pipe_id == 3` 加 `mdelay(2)` 再释放缓冲。
- CVE-2022-50700 记录同一崩溃签名：`fsr=0x402, fsynr=0x500003,
  cbfrsynra=0xc1, cb=6` + `cmnos_thread.c:3998:Asserted in copy_engine`。
- 两个补丁都**只对 WCN3990 打开**（`tx_credit_limit` / `delay_unmap_buffer`），
  Douglas Anderson `Tested-by`，固件 `WLAN.HL.3.2.2.c10-...`（与本板同族）。

### 3.2 同芯片、同 XNU、真机跑通的参考实现（unixnu 生态「其它机型」）

`Trogdor-ipadOS` / `work/trogdor/port/WlanRadio.c`（SC7180 + **WCN3990**）：

| 位置 | 做法 |
|---|---|
| `wc_send()` :2403-2404 | `sw = wc.rri[id] & 0xffff`；`if (!(((sw-1u)-s->write)&s->mask)) return -2`（ring 满拒发） |
| **`wc_send_gather()` :2428-2429** | **要求至少 2 个空闲描述符，否则 `return -2`** |
| `wc_send_gather()` :2430-2431 | header 写入 **`s->buf + write * src_max`**，每个描述符独立缓冲 |
| `wj_tx_put()` :4003-4008 | 空闲表为空就 `return -2` **丢帧**，不绕回复用 busy 槽 |
| `WJ_TX_SLOTS` :2913 | **256** 个 TX 槽 + free list + `wj_tx_compl()` 显式归还 |

### 3.3 本仓代码的三处确定性缺失（逐行对照，不是猜测）

`wifi-wcn3990/source/Wcn3990Hif.cpp`（修复前）：

1. `hif_ce_send_gather()`（:771-800）**完全没有 SRRI / 空闲描述符检查**，
   无条件覆写 `ring[idx]` 与 `ring[next]`，即使 CE 仍在读取它们。
2. `hbuf = g_dma->tx_head[idx & 3]`：`tx_head[4][64]` 只有 4 个共享 header，
   而 gather 每次把写索引 +2 ⇒ **实际只用 2 个槽**连续复用；
   且 `head_len` 上限是 256 字节，**槽本身只有 64 字节**（潜在越界）。
3. `hif_wpa_tx_frame()`（:2876）8 个 `g_wpa_tx_busy[]` 全占满时，
   循环结束后 `slot` 绕回 `g_wpa_st.tx_slot`，**复用仍在 DMA 的帧槽**。

`netpath=0` 时只有少量 EAPOL 帧、间隔长，不触发；`netpath=1` 打开后台流量后
连续发帧 ⇒ 覆盖 live DMA ⇒ SMMU context fault ⇒ UFS 传输失败 ⇒ SError。
**所有观测与三方证据一致。**

### 3.4 按用户指定路径做的机型调研（unixnu.com/devices）

| 机型 | SoC / WiFi | 结论 |
|---|---|---|
| 设备目录全表 | 100+ 机型 | 官方维护仅 `sheng`；社区机型各有独立 porting record |
| `pong` | SM8475 / QCA6490 | pong 页 "PongQCA6490 nativeWiFi"；本地 `UniXNU-pong` 已用于 InterfaceNamer/DNS（v23~v29），但 QCA6490 是 ath11k/PCIe，**CE 架构不同** |
| `picasso` | SM7250 / WCN3990 | 8 个源；本地主线是 `tselongson/UniXNU-picasso`，其它仓库无真 WiFi 栈（11-05 已证 k20/hakios 是伪造 DHCP） |
| `sheng` | SM8550 / `Ath12kWifi` | README 明确 "`Ath12kWifi` supplies the Wi-Fi stack through board PCIe, MSI, DMA, and network interfaces"；设备页 Network=`implemented`，另有 "Wi-Fi DMA support" 子模块 ⇒ 其它机型确实把 **WiFi DMA 生命周期当独立组件**处理，但 ath12k/PCIe 寄存器不可直接搬 |
| **Trogdor（本地同芯片）** | SC7180 / **WCN3990** | 唯一同 SNOC/ath10k 且 XNU 真机跑通的完整栈，`wc_send_gather()` 就是本轮修复的蓝本 |

⇒ 结论：其它机型给的是**方法论与架构分层**，真正能修本板 CE race 的是
「同芯片 Trogdor 实现 + Linux 主线 WCN3990 官方补丁」这一对组合。

---

## 4. 修复（commit `35e7113`，1 file +74/−6）

`wifi-wcn3990/source/Wcn3990Hif.cpp`，**不触碰 `recipe()` 覆盖的任何文件**
（不改 `.m` / scripts / board.toml ⇒ seed recipe 不变 ⇒ 可复用 seed-610，
**无需 marker 清零、无需刷 userdata**）：

1. `tx_head[4][64]` → **`tx_head[32][256]`**：CE4 每个源描述符一个独立
   256 B header；加编译期断言 `hif_ce4_head_check` 锁住几何。
2. `hif_ce_send_gather()` 发送前 volatile 读 DDR RRI 的 CE4 SRRI
   （`g_dma->rri[4] & 0xffff`，`hif_program_bases()` 已给 12 个 CE 全开
   `CE_CTRL1_RRI_UPD`），不足 2 个空闲描述符就 `return -2` 丢帧；
   有界日志 `ce4 gather busy=<n> srri=<n> wr=<n>` 便于判读 SRRI 是否推进。
3. `hif_wpa_tx_frame()` 槽耗尽时 `return -1` 丢帧，不绕回复用 busy 槽。
4. 新 boot-arg **`txguard`**（默认 1，`=0` 恢复旧行为），与 `netpath=N` 等长
   9 B，同一镜像即可做单变量 A/B；`wifiui/wifiauto` 门控与 DNS 链完全不动。
5. 只改 CE4 **gather**（运行期数据帧），未动 `hif_ce_send()` 的 CE0/CE3/CE4
   启动期 HTT H2T 消息，避免破坏已实机验证的初始化时序。

**门禁（全过）**：`work/check-src.py` 两个 driver 文件 ok；括号/圆括号/方括号
486/486、2465/2465、973/973、final depth 0；`work/abi-check.py` **0 HARD**；
独立 `clang++ -std=c++11 -fsyntax-only -Wall -Wextra -Werror` 编译
`hif_dma_block` + 两条静态断言 **RC 0**（`work/tmp-hif-dma-check.py`）；
用户 6 处未提交改动原样保留。

### CI 调度（两个 commit，一个最终镜像）

- `35e7113` 核心修复 → 先派 `43466626356` 做编译验证；其
  `Reuse persistent Data seed` 步骤已 **success**，证明 recipe 不变、
  seed-610 可复用（明早无需清 marker / 刷 userdata）。
- 随后补 `31fc804`（心跳增加 `srri4=`/`wr4=`，纯诊断，便于明早判定 CE4
  读索引是否推进）。为不留下两个易混淆的镜像，**已取消** `43466626356`
  （conclusion=`cancelled`），改派最终 CI **`43468210876`**（HEAD `31fc804`，
  `-f fast=true`，复用 seed-610）。
- **最终 CI `43468210876` SUCCESS**（11m33s）：`Reuse persistent Data seed`
  **success**、0 failed steps，产物 `picasso-data-seed-612`(1,189,509 B) /
  `picasso-bootimg-612`(36,126,077 B)。真实 iPhoneOS SDK 编译/链接/签名通过，
  两条编译期断言（`hif_dma_window_check` / `hif_ce4_head_check`）也随之通过。
- **最终镜像硬核验**（`out/run-43468210876-v610-1794590016/boot/boot.img`，
  sha `1df663317ab67397`，65,105,920 B）：内含
  `ce4 gather busy`×1 / `srri4=`×1 / `wr4=`×1 / `txguard`×3 /
  `tx slots busy`×1 ⇒ 修复与诊断确实进了内核；注入库
  `libpicasso-sb.dylib` 94,080 B 仍含 `dns wait=` / `dns servers=` /
  `service disabled` / `SupplementalMatchDomains` / `State:/Network/Global/DNS`
  ⇒ v29 的 DNS 链未回退。
- **★ seed 身份一致（明早不用动 Data 的关键证据）**：最终镜像的
  `volume_uuid = 1A80E883-C753-471E-8A5B-3142D26F0EA0`、
  `seed_identity_sha256 = 6cb910d0e777…` 与 v29 设备当前所用的
  `picasso-bootimg-610`（CI `43418179396`）**逐字相同** ⇒ 新 boot 与设备
  Data 配对，**禁止清 marker / 刷 userdata**（清了反而丢弃用户 Data）。
- **最终镜像离线单变量预演**：`mk-autojoin-args.py` →
  `mk-args.py "wifiui=1 netpath=1 txguard=1/0"` ⇒ 两组各 995 B、
  必需 token 全在、全镜像**仅 1 字节不同**（offset 60229782）。
- 门禁补记：`check-src.py` 两文件 ok、括号 486/486 + 2469/2469 + 974/974、
  心跳 printf 22 个格式符对应 22 个参数。

---

## 5. 明早一键验证（用户醒来后）

设备今晚**没有动过**。执行：

```sh
bash work/chain-610.sh 43468210876                 # 只跑修复组，400 s
bash work/chain-610.sh 43468210876 --with-control  # 可选：+ txguard=0 对照，共 800 s
```

链已 `bash -n` 通过，并用 CI 原始 boot.img 做过离线预演：
`wifiui=1 netpath=1 txguard=1/0` 两组各 **995 B**、必需 token 全在、
**全镜像仅 1 字节不同**（offset 60348566）。

⚠ 链脚本曾漏 `wifiui=1`（CI 默认 args 不含它，历来由链注入），离线预演抓到并已修。
⚠ 同 seed ⇒ **禁止清 marker / 刷 userdata**（会丢弃用户 Data）。
⚠ 采集 400 s 后**不重启**，留桌面给用户实测。

判据（不是 0 PANIC  alone）：
- `PANIC BEGIN`=0、`NATIVE_UFS_FAILURE`=0、`disk5: I/O error`=0；
- `REAL DATA PLANE`>0 且 `HTT TX_FRM`>0（真实 TX 必须仍在跑，不能靠关 TX 换「不崩」）；
- `dns set=1`×3、`DHCP: bound`、`join=5`、`wifid sets`、`DATASTORE ready=1`；
- `ce4 gather busy=` 可观测（若 SRRI 恒 0 且持续丢帧，日志会明示，下一刀按官方
  `tx credit=1` 串行化 CE4，或给 TX_COMPL 加 `mdelay(2)`）；
- **最终判据**：用户依次打开 `http://192.168.101.1`、一个域名、设置 > Wi-Fi 详情。

对照 `txguard=0` 若复现 panic ⇒ 坐实因果；若两组都不崩 ⇒ 保护有效但需查
是否因丢帧过多导致 TX 事实上停摆（看 `REAL DATA PLANE` 计数）。

---

## 6. 未通清单（如实）

1. **Safari 端到端上网** —— ⬜ 当前唯一关键动作，需用户实测；
2. 设置 App 点 Wi-Fi 详情卡死 —— 未复测（`*TIMEOUT*` 已发布、`wifid sets`×22）；
3. 状态栏 / SystemConfiguration 显示已连接 —— 与原生 service 同源（v26 §4.8：
   configd 进程内建 SCPreferences service 结构上不可行，需独立 root 进程）；
4. 凭据持久化 —— `wpass=` 仍是 boot-arg 明文；
5. 5G MGMT_TX `status=3`（非阻塞）；
6. UFS transport failure 的**独立存储可靠性**问题 —— v29 §2 已取证与 WiFi 正交，
   但本轮证明触发条件是 WiFi TX DMA，修好 TX 后应一并消失。

---

## 7. 铁律（v29 §5 全部沿用，新增 3 条）

21. **「开着 WiFi 会整机重启」先做 `netpath` 单变量**：真实 TX 与伪造栈是
    两条完全不同的数据面，判据不是「有没有 PANIC」，而是
    「真实 TX 仍在流（`HTT TX_FRM`>0）且 0 PANIC」。
22. **WCN3990 的 CE 会提前推进 SRRI**（Linux 主线官方补丁 + CVE-2022-50700）：
    任何 CE 发送前必须检查空闲描述符，DMA 缓冲在完成前不得复用/释放；
    丢帧可恢复，覆盖 live DMA 是整机 SError。
23. **改 `wifi-wcn3990/source/*.cpp` 不改 recipe**：只有
    `picasso-springboard-probe.m` / 7 个 scripts / board.toml payload 进
    `recipe()`；这类修复可复用当前 seed，严禁再清 marker 或刷 userdata。

---

## 8. 关键路径

| 用途 | 路径 |
|---|---|
| 唯一权威 | 本文 v30 |
| 本轮修复 | commit `35e7113` / `wifi-wcn3990/source/Wcn3990Hif.cpp` |
| 判决证据 | `console-exp-{webtest,notx,c1}.cap`、`work/{webtest,notx}-capture.out` |
| 修复蓝本 | `work/trogdor/port/WlanRadio.c:2403/2428/2913/4003/4090` |
| 官方依据 | LKML `2201.2/05616`（tx credit=1）、`2201.2/05614`（mdelay 2）、CVE-2022-50700 |
| 机型调研 | `https://unixnu.com/devices/`、`.../pong`、`.../models/picasso`、`.../sheng`、`UniXNU/UniXNU-sheng` README |
| 本地门禁 | `work/check-src.py`、`work/abi-check.py`、`work/tmp-hif-dma-check.py` |
| CI | 最终 `43468210876` **SUCCESS**（HEAD `31fc804` = 35e7113+诊断，`-f fast=true`，复用 seed-610），产物 `picasso-bootimg-612`；`43466626356` 已取消、仅用于确认 seed 可复用 |
| 最终镜像 | `out/run-43468210876-v610-1794590016/boot/boot.img`（sha `1df663317ab67397`）；离线单变量 995 B / 1 字节差 offset 60229782 |
| 明早上机链 | `work/chain-610.sh <run-id> [--with-control]` |
| 语法/参数工具 | `work/mk-autojoin-args.py`、`work/mk-args.py` |
