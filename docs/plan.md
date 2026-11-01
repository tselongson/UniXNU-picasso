# 启动计划

这份文件是唯一的进度账本。每一步都必须留下：**问题是什么、怎么观测、怎么修、验证结果**。
没有日志或屏幕证据的结论一律不算数。

分支：`tselongson/UniXNU-picasso` 的 `boot-evidence`（**我们自己的基线**，不是 silenzio 的 `stage7-v2`）。

---

## 0. 铁律

1. **只用 `fastboot boot`，从不 `fastboot flash`。** 镜像只加载进内存，不写任何分区。
   作者的全部证据文档都写着 "Only `fastboot boot` was used"。
2. **一次只改一个变量**，除非改动共享同一份构建输入（例如 seed 的多个修正）。
3. **能在本地验证的，绝不靠刷机验证。** 每次构建约 10 分钟，每次启动约 2 分钟，
   而 `work/check-seed.sh` 读 seed UUID 只要 1 秒。
4. **先看证据再下结论。** 今天多次因为「看起来应该是这样」而浪费了一整轮构建。

---

## 1. 观测手段

| 通道 | 怎么用 | 能看到什么 | 局限 |
|---|---|---|---|
| **CDC 控制台** | `work/cdc-capture.py --out work/console-X.log` | 内核启动全过程、launchd 启动任务、驱动日志 | 需要内核走到 USB 初始化（`picasso_usb=2`） |
| **framebuffer** | 直接看屏幕 / 拍照 | panic 信息、`-v` 启动日志 | 只有 `-v` 时才有；显示驱动缺失时只有内核早期 framebuffer |
| **构建日志** | `gh run view <id> --log` | 归档构建、seed 生成、prelink、payload | 看不到运行时 |
| **本地 seed 验证** | `work/check-seed.sh <boot.img>` | seed 里真实的 APFS 卷 UUID | 只覆盖 seed 这一项 |
| **panic 读取** | `dump-picasso-panic.py --fastboot` | PRAM 里的 panic 记录 | **这个 bootloader 不支持**，已验证无效 |

---

## 2. 已验证的事实

以下每条都有证据，不是推测。

| 事实 | 证据 |
|---|---|
| 基线 `boot-evidence` 能起内核 | CDC 控制台输出 206 KB，framebuffer 出字 |
| framebuffer 控制台可用 | 用户拍照：`offline scratch Data mount failed: 2 @kern_exec.c:8054` |
| CDC 控制台可用（COM5） | `work/console-base.log`，30 秒枚举 |
| launchd 能起来，14 个启动任务 | `mount-phase-1/2`、`init-with-data-volume`、`fips`、`keybag`、`usermanagerd` 等 |
| `fips` / `fixup-mobile-tmp` / `MSUEarlyBootTask` 通过 | 日志有 `Finished` / `PASSED` |
| 上次能跑到 launchd 的提交是 `01c7424`（`--gib 3`） | run `41186490589`，`headSha=01c7424` |
| 改成 `--gib 1` 后 seed 挂载失败 | run `41280975761`，panic |
| `apfs_vol_uuid` 在 APFS 卷超级块 **+240（0xF0）** | 本地读上一版 seed：+240 是随机 UUID，+260 是构建日志里那个假值 |
| 设备没有 Secure Enclave | `init_data_protection: No SEP present on this device` |
| bootloader 不支持内存转储 | `fastboot oem dump` / `ramdump` / `fetch pram` 全部失败 |
| silenzio 的 `userdata-root.img` 只有 OS + SystemOS，**没有 Data 卷** | `userdata-root.json` 的 `windows` 字段 |
| 基线只有 `ArmCpu`、`NativeUfs` 两个驱动 | `ls drivers/` |

---

## 3. 问题台账

状态：`待修` / `修完待验` / `已验通过` / `阻塞`

### P1 · Data 卷挂载失败 —— 修完待验

**现象**
```
offline scratch Data mount failed: 2 @kern_exec.c:8054
```
`2` = ENOENT。panic 发生在 launchd 之前，整个启动就此终止。

**怎么观测**
- framebuffer 上的 panic（本次就是靠它发现的）
- `work/check-seed.sh <boot.img>` 读 seed 的真实 UUID，与构建日志里 `data volume uuid:` 对比

**根因**
`NativeRamBlock::selectVolumeSeed` 拿配置 `XNU_OWNER_DATA_VOLUME_UUID` 去匹配 IOMedia 的
`UUID` 属性，必须**恰好命中一个**。配置里的值来自 `make-data-seed.py` 对 seed 镜像的扫描，
而扫描偏移算错了 20 字节（`apfs_meta_crypto_t` 是 20 字节，代码当成 40），
读到的是检查点区域的字节，格式像 UUID 但不是。

**怎么修**
- `51f23d6`：偏移 `260` → `240`（`0xF0`）
- 优先问挂载点：`diskutil info -plist /Volumes/Data` → `diskutil apfs list -plist /Volumes/Data`
  → 设备切片 → 镜像扫描（兜底）

**验证标准**
`check-seed.sh` 读出的 UUID == 构建日志里的 `data volume uuid:` == 内核不 panic

---

### P2 · `/private/var/keybags` 不存在 —— 修完待验

**现象**
```
MKB_INIT: Could not open /private/var/keybags/systembag.kb: No such file or directory
UMD_INIT: Failed to atomically write /private/var//keybags/usersession.kb: ... [2]
UMD_INIT: Failed to atomically write /private/var/keybags/persona.kb: ... [2]
```

**根因**
seed 的目录骨架里没有 `keybags`。三条错误都指向同一棵树，但没有一条说「目录不存在」。

**怎么修**
`5739637`：骨架补 `keybags`、`mobile/Library/Keychains`、`mobile/Library/Caches`

**验证标准**
MKB_INIT 不再报 `No such file or directory`，或 keybag 任务 `Finished`

---

### P3 · 没有 Secure Enclave —— 未处理

**现象**
```
init_data_protection: No SEP present on this device
```

**影响**
keybag / data-protection 在原理上不可能真正成功。可能挡住用户会话（`usermanagerd`），
但未必挡住启动链。

**怎么观测**
keybag 任务是否 `Finished`；`UMD_INIT` 是否还报 persona 失败。

**怎么修（预案）**
照 silenzio 的做法上 AppleKeyStore 桩（他有 `drivers/AppleKeyStoreState` 和 PMGR shim）。
**先不动**——等 P1/P2 验证完，看它到底挡不挡。

---

### P4 · 没有显示驱动 —— 阻塞（目标 B）

**现象**
基线 `drivers/` 只有 `ArmCpu`、`NativeUfs`。屏幕上的字来自内核早期 framebuffer（ADT 配的），
不是 IOKit 视频控制台。

**影响**
没有 IOMobileGraphicsFamily → 没有 backboardd / SpringBoard → **没有界面、没有 logo**。

**阻塞点**
`build-native-legacy-display.py` 需要 `build/native-prelink/native-owner-mobile-graphics-candidate.macho`，
而产出它依赖手工 GDB 会话的 `gdb-stop.json`，CI 无法复现。

**怎么修（预案）**
让 `emit_prelink.py` 在 pass 1 之后、用同一个 kernel + 原始 kernelcache、以
`--root com.apple.iokit.IOMobileGraphicsFamily` 单独产出一份候选。**未验证可行。**

---

### P5 · 启动停在 init_exclavekit —— 进行中

**现象**
```
Couldn't build index for com.apple.init_exclavekit: Not eligible for acceleration
```
之后日志静止（92 KB 后不再增长），设备不 panic、CDC 心跳正常。

**怎么观测**
- CDC 日志最后一条非噪声行
- `grep -av PICASSO | grep -av "NativeUfs blk"` 过滤心跳后看尾部
- 数 `Event: <name>` 去重后的启动任务个数（当前 18）

**候选原因（未验证，按可能性排序）**
1. `exclaves-boot` / `init_exclavekit` 在等 Exclave 硬件或 SEP，而本机**没有 SEP**
2. launchd 在等某个 `Doing` 后没有 `Finished` 的任务
3. 某个守护进程 exec 后卡住，launchd 阻塞

**怎么修（预案）**
先区分「等」还是「卡」：用 `init_exclavekit` 的日志时间戳和后续心跳时间戳对比。
如果确认是 Exclave/SEP，考虑在 seed 或 launchd plist 里绕过该任务；
如果是阻塞，找出被等的那一项。

---

## 4. 步骤计划

### Step 1 —— 验证 Data 卷（进行中）

- 构建：`41283484096`（`51f23d6` + `5739637`）
- 刷前先跑 `check-seed.sh` 确认 UUID 与构建日志一致
- `fastboot boot` → 抓 CDC → 看是否还 panic
- **通过标准**：没有 `offline scratch Data mount failed`，日志越过 `kern_exec.c:8054`
- **失败则**：把 CDC 日志里 `NativeRamBlock` 相关的行全部取出来，看是 `selectVolumeSeed` 返回 0
  还是 `attachStore` / `seedInfo` 失败

### Step 2 —— 验证 /private/var 可见

- 看 `mount: data volume missing` 是否消失
- 看 `mount-phase-1/2` 是否 `Finished`
- 看 `keybags` 相关错误是否消失
- **通过标准**：`/private/var` 有内容，`init-with-data-volume` 返回

### Step 3 —— 验证用户态推进

- 数启动任务个数（当前 14 个不同任务）
- 看 `keybag`、`data-protection`、`usermanagerd` 各自结果
- **通过标准**：launchd 不再停在等某个任务，或明确指认下一个卡点

### Step 4 —— 处理 P3（keybag / 无 SEP）

只在 Step 3 证明它真的挡路时才做。

### Step 5 —— 目标 B（界面）

单独立项，见 P4。不在今天的范围。

---

## 5. 每步的记录格式

每完成一步，在下面追加一条：

```
### [时间] Step N —— 标题
- 提交：
- 改动：
- 观测（原话日志/屏幕）：
- 结果：通过 / 失败
- 下一步：
```

---

## 记录

### 2026-11-01 15:20 Step 1 开始
- 提交：`51f23d6`（UUID 偏移 240）、`5739637`（keybags 骨架）
- 改动：seed 的 UUID 来源与目录骨架
- 观测：待构建 `41283484096` 完成
- 结果：进行中
- 下一步：刷前跑 `check-seed.sh` 对比 UUID

### 2026-11-01 15:40 Step 1 —— **通过**

- 提交：`51f23d6` + `5739637`，构建 `41283484096`
- 刷前本地验证（关键，省了一次瞎刷）：
  ```
  构建日志 data volume uuid (mount point info): 288B3D01-668A-404C-AEA1-3E9190A95600
  check-seed.sh  seed volume uuid:              288B3D01-668A-404C-AEA1-3E9190A95600
  ```
  两条完全一致 —— 这是第一次配置值与 seed 真实值对上。
- 观测：
  - **`offline scratch Data mount failed` 消失，不再 panic**
  - `PICASSO EXEC[0]/[1]`：launchd 正常 exec
  - `mount: data volume missing` 消失
  - 启动任务从 14 个涨到 **18 个**，新增：`dirs_cleaner`、`init_featureflags`、
    `xpcroleaccountd`、`auearlyboot`、`exclaves-boot`
  - `auearlyboot` 走完（`End Early Boot` / `Exiting Early Boot`）
  - launchd 缓存发送成功（`Cache sent to launchd successfully`）
- 结果：**通过**。P1、P2 都过了。`diskutil info /Volumes/Data` 是有效的 UUID 来源，
  镜像扫描那条路没被用到。
- 下一步：见 P5。

### 2026-11-01 15:45 Step 2 —— 新卡点

- 观测：日志停在
  ```
  Couldn't build index for com.apple.init_exclavekit: Not eligible for acceleration
  ```
  之后不再增长（92 KB 后静止）。设备仍活着（CDC 心跳正常），没有 panic。
- 同时存在但不致命的错误：
  ```
  tzinit: Unable to locate data; resetting to system partition   （之后自行恢复）
  failed to load kernel prng seed: (-76) (2) No such file or directory
  failed to load virtual random: (-147) (-536870212)
  ```
- 结果：进行中，见 P5。
- 下一步：查 `init_exclavekit` 与 `exclaves-boot` 是不是在等某个东西。
