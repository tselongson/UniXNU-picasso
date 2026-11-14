# WiFi / UI 交接 v31 — 键盘不可输入（320 vs 360 键面）与基线决策

**时间**：2026-11-14 12:0x
**HEAD**：`f994d37`（fork `kd439-pool`）
**CI**：`43554167848`（`create_data_seed=true` + `build_userdata=true`，进行中）
**设备**：仍在 `64993333 fastboot`，**本轮尚未刷任何分区**

---

## 1. 本轮要解决的问题

11-14 11:45 用户实测反馈：**「输入法好卡，根本输不了网页」**，要求把基线更新到
silenzio 或 yincql 最新版。

这是与 WiFi 无关的**输入层缺陷**：WiFi 侧 chain-610 已在 400 s 内证明真实 TX
稳定（见 §2），但用户无法在 Safari 输入网址，所以 WiFi 的浏览器级验收一直被卡住。

---

## 2. 上一轮 WiFi 结论（已实机验证，勿重复怀疑）

`bash work/chain-610.sh 43468210876`（tg1 = `wifiui=1 netpath=1 txguard=1`）：

| 判据 | 结果 |
|---|---|
| PANIC BEGIN / NATIVE_UFS_FAILURE / disk5 I/O / SMMU FAULT | **0 / 0 / 0 / 0** |
| REAL DATA PLANE / HTT TX_FRM | **87 / 87**（真实 TX 全程在流） |
| `srri4` vs `wr4` | 始终相等（末次 6/6），`ce4 gather busy` 0 次 |
| DNS | `dns set=1` ×3（Setup / State service / State **Global**） |
| 其它 | DHCP bound 192.168.101.18、join=5、`truthful=1 publish=1` |

对照：11-13 的 `netpath=1`（无 guard）必现 1 PANIC + 5 UFS failure；`netpath=0`
不崩但无真实 TX。⇒ CE4 TX guard 修复有效，WiFi 数据面稳定。

---

## 3. 基线为什么不整体 rebase

用户要求「更新到 silenzio 或 yincql 最新版」。逐条量化后**不能整体换基**：

| 对象 | 事实 |
|---|---|
| `origin/main`（silenzio111） | 与我们同源，但只领先 merge-base 11 个提交，且 **19 个文件重叠**：`patches/unixnu-source-kernel.patch` 我方 51 个提交、对方 +1748 行；`board.toml` 我方 52 个提交；`unixnu` 24 个。硬合等于重写生成型补丁，风险直指刚验证稳定的内核与 WiFi。 |
| `yincql/feat/fix-v1` | **与我们的历史无共同祖先**（独立导入）。其 WiFi 仅 `drivers/NativeWlan/`：官方文档明写 *“does not yet scan, associate or carry packets”*，只有 SMEM 探测。换树 = 丢掉整套 WCN3990 数据面。 |

⇒ 采用**定向移植**：只把 yincql 的 UI 修复搬到我们树上，WiFi 与内核零改动。

---

## 4. 根因（键盘不可输入）

yincql `af9ea80` 在**同一 20H392 构建**上定位到：

- 20H392 工厂模板先返回 **320 pt** 键盘尺寸，再展开 **360 pt** 手机键面；
- 320 pt 容器套 360 pt 键面 ⇒ **按键落点错位**，点哪都不对；
- 系统尺寸通知随后把窄容器重新居中，并在键面切换时重入 `setFrame`，
  **把键面重置回字母页** ⇒ 输网址时数字/符号打不进去。
- 面板未发布 dot-pitch，UIKit 的物理停靠尺寸回落到通用手机值，放大错位。

本机实测吻合：`PICASSO_DISPLAY ... width=1080 height=2400 scale=3`，即 360×800 pt。

---

## 5. 本轮改动（`f994d37`，6 文件 +273/−12）

1. **`package/PicassoDisplay.cpp`**
   - 新增 `publishPanelPitch()`：1080×2400 面板发布 `dot-pitch=395`（69.5×154.4 mm）。
   - **与上游的刻意差异**：属性是**建议性**的，在 `nub->init()` 之后调用，
     **不进入启动条件**。上游把它串进 `if (...)`，属性被拒或换面板就会导致
     framebuffer 起不来；本仓改为失败也不影响显示。
   - `started` 分支加 `PICASSO_DISPLAY panel pitch=395 ppi` 作为实机判据。
2. **`package/picasso-springboard/picasso-keyboard-size.m`**（新，124 行）
   - swizzle `+[UIKeyboardLayoutStar keyboardSizeForInputMode:screenTraits:keyboardType:]`；
   - 仅在 `320 → 360`、`1080×2400`、`nativeScale=scale=3`、`360×800 pt`、
     `idiom=0`、非浮动、非分裂、`keyboardWidth=360`、高度 <400 时放大；
   - 其余几何（横屏、iPad、浮动/分裂、已正确）原样返回。
3. **`package/picasso-springboard/picasso-input-cache.m`**（新，108 行）
   - `-[AFPreferences offlineDictationStatus]` 在**指定调用点**返回 nil 时改为 `@{}`，
     让输入语言缓存完成初始化，避免每次弹出键盘都重新查询；
   - 其它调用点的 nil 语义、所有非 nil 结果保持不变。
4. **接线**：probe 在 `picasso-astc-guard.m` 之后 include 两个新文件；
   `recipe()` 与 `check-picasso-astc-guard.py` 的输入清单同步加入两个文件。

**安全设计（关键）**：两个钩子都用 **LC_UUID + 目标区域代码哈希**双重校验，
构建不匹配时只打印 `unavailable` 并**不做任何替换**。因此即使 UUID 假设错误，
后果是「不生效」，不是崩溃。

---

## 6. 门禁

- 两个新文件：`clang -target arm64-apple-ios16.0 -fobjc-arc -fblocks
  -Wall -Wextra -Werror` + UIKit/objc/mach-o stub，**RC 0**（`work/tmp-keyboard-check.py`）。
- `PicassoDisplay.cpp` / probe / 两个新文件：括号 0/0/0、深度 0。
- `work/check-src.py` OK；`work/abi-check.py` 0 HARD。
- 提交 blob 均为 **LF**（CRLF=0）。

---

## 7. 为什么必须换 Data seed

`recipe()` 哈希 `package/picasso-springboard/*.m` 与 `settings.injected_library`，
注入库一变 ⇒ recipe 变 ⇒ 旧 seed 不可复用（`validate()` 会拒绝）。
CI 因此带 `create_data_seed=true`，新 seed 的 APFS `VolumeUUID` 由 `hdiutil`
随机生成 ⇒ **Data 身份变化**，设备旧 Data 无法与新内核配对（11-11 m1 panic 已证）。

**⇒ 必须 `fastboot flash userdata <新 sparse>`，属不可逆操作，需用户明确确认。**

---

## 8. 下一步（按序）

1. 等 CI `43554167848` success，确认产物：`picasso-bootimg-*` + `picasso-data-seed-*`
   + `picasso-userdata-*`；`Reuse persistent Data seed` 应显示新建而非复用。
2. **征得用户同意后**：`fastboot flash userdata <新 sparse>`。
3. `fastboot boot <新 boot.img>`（参数取 `work/mk-autojoin-args.py` 日用基线 +
   `wifiui=1 netpath=1 txguard=1`），抓 400 s。
4. 判据：
   - 崩溃类：PANIC / UFS failure / SMMU 全 0；
   - WiFi：`REAL DATA PLANE`、`HTT TX_FRM` > 0，`dns set=1` ×3；
   - **本轮新增**：`PICASSO_DISPLAY panel pitch=395 ppi`；
     `PICASSO_KEYBOARD_SIZE ... reason=installed`（App 进程内）；
     `PICASSO_INPUT_CACHE ... reason=installed`。
   - 若出现 `reason=unavailable` ⇒ 该构建的 UIKit/AssistantServices 与 yincql
     假设不同，钩子未生效（不是崩溃），需按其日志重新定位。
5. **用户实测**：Safari 地址栏输入域名能正常落字 → 打开 `http://192.168.101.1`
   → 打开域名 → 设置 > WLAN 详情不卡。

---

## 9. 风险与备选

- **UUID/偏移不匹配**：钩子 no-op。备选是按日志里的实际 UUID 重算偏移与哈希
  （需要设备侧二进制；可先用 `ps`/`snapshot` 与崩溃日志定位）。
- **键盘仍卡**：下一刀是 `picasso-input-cache.m` 的调用点判定（`0x37e8`）或
  SpringBoard 侧 `UIKeyboardLayoutStar` 的实例方法路径；也可评估 yincql
  `picasso-power-button.m` / 快照相关修复是否需要一并带入。
- **不做**整树 rebase，除非用户明确要求并接受 WiFi 回归风险。
