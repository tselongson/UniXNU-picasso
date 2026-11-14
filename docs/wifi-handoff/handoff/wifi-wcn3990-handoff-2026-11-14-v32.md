# WiFi / UI 交接 v32 — 键盘已修好；WiFi 只剩「收」一个方向

**时间**：2026-11-14 13:4x
**HEAD**：`ebd4c1f`（fork `kd439-pool`）
**CI**：`43562360905` **SUCCESS**（HEAD `ebd4c1f`，`-f fast=true` 复用 seed-613，产出
`picasso-bootimg-614`，36.1 MB）——探针与键盘钩子均已确认在镜像内
**设备**：正在运行 kb3 镜像（未在 fastboot）；Data = seed-613，`PICASSO_DATA_INIT phase=2`
**权威结论**：键盘/输入已修复并经用户实测确认；WiFi 硬件层与发送方向全部打通，**仅缺单播接收**

---

## 1. 本轮做成了什么

| 项 | 状态 | 证据 |
|---|---|---|
| 键盘输入（320 vs 360 键面） | **已修复，用户实测确认** | `PICASSO_KEYBOARD_SIZE pid=26 reason=installed`；用户回「键盘倒是正常了」 |
| 面板密度 dot-pitch | **已发布** | `PICASSO_DISPLAY panel pitch=395 ppi` |
| 输入语言缓存（AFPreferences） | **已安装** | `PICASSO_INPUT_CACHE pid=26 reason=installed` |
| WiFi CE4 TX 保护（上一轮） | **实机 PASS** | chain-610：400 s，0 PANIC / 0 UFS failure / 0 SMMU fault，真实 TX 87 帧，`srri4==wr4` |
| WiFi 关联 / 密钥 / DHCP / DNS 发布 | **实机 PASS** | `join=5`、`DHCP: bound 192.168.101.18`、`dns set=1`×3、`State:/Network/Global/DNS` |
| WiFi 发送方向 | **实机 PASS** | AP 对 407 帧全部 ACK：`tx compl status=0 ack=407 noack=1 discard=0` |
| **WiFi 单播接收** | **未通（唯一缺口）** | `ord/msdu` 冻结 443、`bad=0`、beacon 仍涨 |

---

## 2. 键盘修复（已交付）

20H392 先返回 **320 pt** 键盘模板再展开 **360 pt** 键面，导致按键错位、键面被系统
尺寸通知重入 `setFrame` 重置回字母页。按 yincql `af9ea80`（同一 20H392 构建）定向移植：

- `package/PicassoDisplay.cpp`：1080×2400 面板发布 `dot-pitch=395`（69.5×154.4 mm）。
  **与上游刻意不同**：该属性是建议性的，在 `nub->init()` 之后调用，**不进入启动条件**，
  属性被拒也不会让 framebuffer 起不来。
- `package/picasso-springboard/picasso-keyboard-size.m`：swizzle
  `+[UIKeyboardLayoutStar keyboardSizeForInputMode:screenTraits:keyboardType:]`，
  仅当 320→360、1080×2400、scale=3、360×800 pt、idiom=0、非浮动/分裂时放大。
- `package/picasso-springboard/picasso-input-cache.m`：`-[AFPreferences
  offlineDictationStatus]` 在指定调用点 nil→`@{}`，完成语言缓存初始化。
- 两个钩子都有 **LC_UUID + 代码段哈希**双护栏；不匹配只打 `unavailable`，不替换实现。
- 接线：probe include + `recipe()` + ASTC 主机检查清单同步（`f994d37`）。

**为什么不整体 rebase**（用户曾要求「更新到 silenzio/yincql 最新版」）：
`origin/main` 只领先 11 个提交，但与我们在 `patches/unixnu-source-kernel.patch`
（我方 51 提交 / 对方 +1748 行）、`board.toml`（52 提交）等 19 个文件重叠；
`yincql/feat/fix-v1` 与我们的历史无共同祖先，且其 WiFi 仅 `drivers/NativeWlan`
SMEM 探测（官方文档自述 *does not yet scan, associate or carry packets*）。
整体换基会丢掉已验证的 WCN3990 数据面 ⇒ 只取 UI 修复。

---

## 3. Data seed 重建事故与恢复（重要运维结论）

改注入库 ⇒ `recipe()` 变 ⇒ 必须新建 seed。本轮 CI `43554167848` 产出 **seed-613**。
用 16 KiB 的 marker-clear sparse 触发内核用 boot 内嵌 seed 重建 20 GiB Data 时：

1. **kb（第一次启动）**：重建完成（`phase=1 complete=1`），但 20 GiB 写入压垮 UFS 队列，
   出现 `ocs=0f` 命令超时 → `NATIVE_UFS_FAILURE` ×5、`disk5: I/O error` ×**3768**，
   WiFi 关联后随即停摆。**PANIC 仍为 0**。
2. **kb2（第二次）**：`phase=2`（不重建），残留 2 次 UFS failure + 9 次 disk5 错误，
   `DHCP: send failed`，桌面未起来。
3. **kb3（第三次）**：`phase=2`、**UFS/disk/PANIC 全 0**，join=5、DHCP、DNS、键盘钩子
   全部正常 —— 系统完全恢复，用户实测键盘可用。

**结论/注意**：
- marker-clear 路径可行，但重建期的 UFS 超时是**瞬时**的；**再启动一次即可恢复**，不要重复清 marker。
- 本机磁盘仅 ~11 GB，**不要**下载/解压 5.87 GB 的 `picasso-userdata-613`（解压会超盘）。
- 历史上传的 `picasso-userdata-613` 仍在 CI，需要时可换机器刷。

---

## 4. 当前唯一缺口：单播接收

### 4.1 证据链（全部来自串口，可复算）

- `ord=443 msdu=444` 在 5 分钟内**完全不动**（beacons 5383→继续增长，`rx=5546`）。
- `bad=0/0`：连「解密失败被拒」的帧都没有 —— 说明帧**没送到主机**，不是协议栈丢包。
- 发送方向正常：`ack=407 noack=1 discard=0`，86 字节 SYN 持续发出（AP 在 802.11 层 ACK）。
- 广播路径正常：DHCP 拿到 192.168.101.18；`dns set=1`×3 已发布。
- 组钥是 **TKIP**（`install group key idx=2 len=32 cipher=2`），单播是 CCMP（`cipher=4`）。

### 4.2 两个候选（不猜，等探针数据）

1. **固件侧 PTK 未真正生效**：日志里 peer 被映射三次（`id=55 → id=7 → id=72`），
   **最后一次在装完密钥之后**。若固件重建了 peer，密钥会挂在旧 peer 上 ——
   广播走 vdev 组钥仍通，单播全部解不开，与现象完全吻合。
2. **数据接收环停摆**：`httidx` 在涨但 `ord` 不动，需排除 ring 未被 repost。

### 4.3 已安排的探针（CI `43562360905`）

`ebd4c1f` 在驱动里加了两处**有界**日志：

- `hif_rx_trace()`：前 64 个被接受帧，打印 `decap/bytes/type/dst/src`，IPv4 再打印
  五元组与 TCP flags（`tcp 443>52111 fl=12` 即 SYN-ACK），ARP 打印 opcode 与双方地址。
- `hif_wr_bad()`：前 16 个被拒帧，打印 reason/attention/start/decap/前 8 字节。

**该改动只动 `wifi-wcn3990/`，不在 seed recipe 内**，所以 CI 复用 seed-613，
**不需要刷 userdata**，只需 `fastboot boot` 新 boot.img。

判读：
- 有 `RX: ... tcp ... fl=12` ⇒ 单播收得到 ⇒ 问题在协议栈/交付；
- 有 `DP: reject #` ⇒ 收到了但被拒（看 attention 位定位解密/长度）；
- 一条 `RX:` 都没有 ⇒ 固件没往主机送 ⇒ 优先查 §4.2 候选 1（peer/key 生命周期）。

---

## 5. 代码与产物

| 项 | 值 |
|---|---|
| HEAD | `ebd4c1f`（wlan RX 诊断）、`f994d37`（UI 移植） |
| seed | **seed-613**（`43554167848`），设备 Data 与之一致（`phase=2`） |
| 可启动镜像 | `out/full-flash/boot-exp-kb.img`（= seed-613 + UI 修复，参数 995 B） |
| 诊断镜像 | CI `43562360905` 的 `picasso-bootimg-*`（HEAD ebd4c1f） |
| 上机链 | `bash work/chain-612.sh 43562360905`（下载→校验 RX 探针→生成参数→boot→抓 400 s） |
| 上一轮链 | `bash work/chain-611.sh <run> [--flash]`（含 marker-clear，**不要再跑 --flash**） |
| 设备 | 正在运行 kb3；串口 COM5；userdata 几何 `0x1AC2BFB000` |

---

## 6. 复现命令

```sh
# 1) 取 RX 诊断镜像并上机（不刷 Data）
bash work/chain-612.sh 43562360905

# 2) 抓包判读（脚本会打印 RX: / DP: reject # / tx compl 汇总）

# 3) 用户侧验收
#    键盘：设置搜索框输入 + 数字页
#    WiFi：Safari http://192.168.101.1  →  域名
```

---

## 7. 下一步（按证据分支）

- **若探针显示无任何 RX**：按 ath10k 的 peer/key 生命周期核对——
  在 `PEER_MAP` 事件之后重新装 PTK（或改为按 peer id 装），并确认 vdev/peer 未在
  `PEER_AUTHORIZE` 后被重建。这是最可能的一刀。
- **若探针显示有 RX 但 reject**：按 attention 位定位（27 TKIP MIC / 29 FCS / 30 len），
  对应修 decap 或长度校验。
- **若探针显示 SYN-ACK 正常但协议栈不通**：查交付路径（`hakios_net_input` 的 mbuf/
  校验和/接口状态），与 pong/sheng 的 ifnet 集成对照。

**不要**在没有新证据前重复清 marker 或整树 rebase。

---

## 8. 用户可见状态（如实）

- 键盘：**已可用**（用户确认）。
- WiFi：能连上、能拿 IP、能发帧且被 AP ACK、DNS 已发布；**但打不开网页**（收不到回包）。
- 设备当前可正常进桌面，未变砖；所有改动已推送 `fork/kd439-pool`。

---

## 9. 补记：诊断镜像已构建成功（13:49）

- CI `43562360905` **SUCCESS**，`Reuse persistent Data seed` = **success**
  ⇒ recipe 未变、**seed 未重建、不需要刷 userdata**。
- 产物：`picasso-bootimg-614`（36,127,596 B）。下载后核验
  （`sha256 356f65e9f02bd351…`）：
  - `RX: #%u decap=` ×1、`DP: reject #%u` ×1 → 探针在新内核里；
  - `PICASSO_KEYBOARD_SIZE` ×1、`panel pitch=395` ×1 → 键盘修复仍在；
  - 默认 args 995 B，`wifiui=1` 由链脚本注入（与历次一致）。
- 一条命令上机（**只 boot、不动 Data**）：
  ```sh
  bash work/chain-612.sh 43562360905
  ```
  脚本会：校验探针字符串 → 生成 995 B 参数镜像 → `fastboot boot` → 抓 400 s →
  打印 `RX: #` / `DP: reject #` / `tx compl` 汇总。
- 判读口径见 §4.3；三种结果分别对应 §7 的三条分支。
