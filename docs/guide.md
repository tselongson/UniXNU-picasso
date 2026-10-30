# 项目指南：怎么做这个移植

这份文档回答三个问题：**参考谁**、**现在在哪**、**接下来怎么走**。
状态快照见 [status.md](status.md)，排障细节见 [debugging.md](debugging.md)。

---

## 一、项目是什么

在 Redmi K30 5G（picasso / SM7250）上启动真正的 XNU 内核，让它跑起未修改的 iPadOS 17.7.11
用户态（launchd、守护进程、SpringBoard）。

不是兼容层、不是模拟器、不是换皮桌面。内核是 Apple 开源的 XNU，用户态是 IPSW 里的原始二进制。

---

## 二、参考项目地图

**这是本文档最重要的一节。** 每个项目能借鉴什么、不能借鉴什么，都标出来了。

### 1. `UniXNU/UniXNU`（上游框架）

所有移植的共同基座：构建系统（`unixnu`）、补丁体系、payload/DarwinBoot、文档。

- **能借鉴**：`docs/concepts/` 下的每一篇。`boot-chain.md`、`payload.md`、`userdata.md`、
  `security.md` 是权威设计说明，**动手前必须先读**。
- **注意**：本仓库是"钉住某个 revision + 覆盖设备文件"的模式（`upstream.rev`），
  上游的新特性不会自动进来。想用新特性必须升级 `upstream.rev`，而那是大动作。

### 2. `UniXNU/UniXNU-sheng`（SM8550，小米平板 6S Pro）—— 完整方案

**跑得最远的参考**，配置最全。

- **能借鉴**：`configs/sheng_defconfig`、`board.toml` 的完整结构、
  `[userdata]` 窗口配置、启动参数清单。
- **关键限制**：它的根挂载依赖**完整的 owner-root 栈**，其中 `OWNER_UNION_SYSTEMOS`
  需要 `libVerifiedRootProbe.a`，而 `build-verified-root-probe.py` 拒绝生成它：
  `The two-volume bundle requires the positive original OS policy`。
  **那份 policy 是苹果签发、绑定具体设备的材料，小米设备拿不到。**
- **结论：sheng 的核心路线对本板关闭。** 详见 status.md。

### 3. `Allenwdk/UniXNU-uke`（SM7675，小米平板 7）—— **当前最有价值的参考**

状态：UFS 根挂载成功、**launchd 启动**，只剩显示。

**它和 sheng 的关键区别**：用 `XNU_BRINGUP_OWNER_CACHE` 绕开了验证根，
只开缓存挂载和 cryptex 联合挂载：

```
CONFIG_XNU_BRINGUP_OWNER_CACHE=y
CONFIG_XNU_OWNER_CACHE_MOUNT=y
CONFIG_XNU_OWNER_CACHE_STARTUP=y
CONFIG_XNU_OWNER_DATA_MOUNT=y
CONFIG_XNU_OWNER_UNION_SYSTEMOS=y
# 下面这些全关
# CONFIG_XNU_VERIFIED_ROOT_PROBE is not set
# CONFIG_XNU_OWNER_ROOT_TRUST is not set
# CONFIG_XNU_OWNER_BOOT_MANIFEST is not set
```

- **能借鉴**：
  - `patches/xnu/offline-owner-root-auth-bypass.patch` —— 13 行，用启动参数跳过根认证
  - `patches/xnu/zzzz-offline-owner-systemos-bringup.patch` —— 7 行，放宽 `#error` 守卫
  - `scripts/build-xnu-4k-baseline.sh` 里 `BRINGUP_OWNER_CACHE` 的四处改动
  - `drivers/NativeUfs/NativeUfs.cpp:1825-1888` —— **`native_verified_block_*` 的 UFS 实现**
    （本仓库已移植）
- **做法**：`git clone https://github.com/Allenwdk/UniXNU-uke`，对照它的 `board.toml`
  和 `configs/uke-cpu_defconfig` 逐项比对。

### 4. `dipa-studio/ios-sweet`（SM7150，Redmi Note 10 Pro）—— **跑到主屏幕**

状态：能进主屏幕、设置、控制中心，CPU 软件渲染。

**完全不同的路线**，值得认真研究：

| 维度 | ios-sweet | 本移植 |
|---|---|---|
| 根文件系统 | **日志式 HFS+ 卷**（自建镜像） | APFS 原始 OS dmg |
| dyld 共享缓存 | 镜像里 44 个缓存文件的**硬链接** | 依赖 cryptex |
| 内核补丁 | **69 组** | 少量 |
| 安全机制 | 最小 AMFI、宽松沙盒、AppleKeyStore 桩 | 未做 |

- **能借鉴**：`NOTES.md` 是逐次启动的工程日志，**记录了每一个坑和解法**，
  包括 ignition 各阶段、共享缓存、SIP、launchd 缓存、HFS+ 的选择理由。
  **遇到新问题先搜这个文件。**
- **核心洞察**：它**根本不碰 APFS 封印和 cryptex**，所以避开了本移植卡住的整类问题。

### 5. `TrungNguyen1909/qemu-t8030`（QEMU 跑 iOS）

- **能借鉴**：真实启动日志样本。证明 `system-volume-auth-blob` 缺失**非致命**，
  证明 `launchd_unsecure_cache=1` 的用法。
- **限制**：虚拟机场景，有真实 iBoot 链的部分能力。

### 6. 其他 UniXNU 移植

`docs/devices/` 下有 30 多个机型。**看它们的状态表判断参考价值**：
- `uke` —— launchd 启动（最有价值）
- `nx729j` —— 派生自 sheng，用 owner bundle 签根策略
- `phy120` —— userdata 几何还没测完（早期）
- `picasso`（本机）—— 官方记录还停留在"早期骨架"，已严重过时

---

## 三、三条技术路线

### 路线 A：APFS + cryptex 联合挂载（sheng / uke）

根是 IPSW 里的原始 APFS 卷，dyld 共享缓存从 SystemOS cryptex 联合挂载。

- **优点**：不修改系统镜像，最接近真机
- **难点**：APFS 封印、根哈希、cryptex 挂载
- **状态**：本移植已走到根卷挂载 + launchd 启动，卡在共享缓存不可用
- **sheng 的完整栈对本板关闭**（缺 Apple policy）；**uke 的 bringup 变体可用**（正在试）

### 路线 B：HFS+ 自建根（ios-sweet）

从 IPSW 取出文件，自己建一个 HFS+ 卷当根，共享缓存用硬链接放进去。

- **优点**：**绕开整个 APFS/封印/cryptex 问题**，已被验证能到主屏幕
- **代价**：要自建镜像工具链 + 大量内核补丁
- **状态**：未开始

### 路线 C：纯内存根（不实用）

不适合本项目，列出仅为完整性。

**判断**：路线 A 已经投入很多且接近突破，**先用 uke 的 bringup 变体把 A 走完**；
如果 A 的共享缓存问题解决不了，再切 B。

---

## 四、当前状态（精确）

```
UEFI → DarwinBoot → XNU            ✅
BSD 层、IOKit、APFS 注册            ✅
USB CDC 控制台                     ✅
UFS 控制器 bring-up                ✅
根设备发布（SystemOS / OS）         ✅
APFS 容器匹配、根卷挂载             ✅
根哈希认证（跳过）                  ✅
load_init_program                  ✅
launchd 启动（PID 1）               ✅  ← Darwin Ignition Sequence 打印
ignition 后续阶段                   ❌  ← 卡在这里
```

**卡点的硬证据**（块设备计数器测得）：

```
整个启动 252~264 次块请求，ignition 开始后一次都没有
读的区间集中在 10710970–10711602，长度杂乱（32/40/56/64/72/80）→ 随机访问元数据
```

**所以是阻塞，不是"安静地工作"。**

**推断**：dyld 共享缓存不可用，launchd 加载不了 libSystem。
共享缓存位于 SystemOS cryptex，需要联合挂载到根上——这正是 uke 的 bringup 配置在做的事。

---

## 五、已排除的假设（不要重复）

| 尝试 | 结果 | 证据 |
|---|---|---|
| `IODelay` 可用 | ❌ 在本平台不返回 | 恢复它就卡在 `nop: cleared` |
| 控制台 poller 饿死 IODelay | ❌ | 无控制台时同样卡 |
| root hash 缺失是致命错误 | ❌ 导入阶段非致命 | qemu-t8030 报同样错误仍启动 |
| 缺 Data 卷导致卡住 | ❌ | seed 进 payload 后输出一字未变 |
| `-apfs_shared_datavolume` | ❌ | 输出不变 |
| `launchd_unsecure_cache=1` 单独够用 | ❌ | 输出不变 |
| ADT `/chosen/asmb` 放宽 SIP | ❌ | 节点确认写入，252 次 I/O 一字不差 |
| sheng 的完整 owner-root 栈 | ❌ 关闭 | 需要 Apple 签发的 OS policy |

---

## 六、项目规划

### 阶段 1：把 launchd 走通（当前）

**目标**：ignition 越过 level 5，launchd 开始跑启动任务。

**做法**：完成 uke 的 bringup 配置移植。

- [x] `BRINGUP_OWNER_CACHE` 开关（构建脚本 4 处改动）
- [x] `kern_exec.c` 的 `#error` 放宽
- [x] `native_verified_block_*` 的 UFS 实现（从 uke 移植）
- [ ] **让构建通过** —— 当前卡在 `NativeRamBlock requires the qualified original storage/AES prelink closure`
- [ ] 刷机验证：`ignition level: 0x5` 之后是否出现新日志
- [ ] 块计数器确认：I/O 是否恢复

**验收标准**：日志里出现 launchd 的启动任务（如 `Doing boot task: fsck`）。

### 阶段 2：用户态服务

**目标**：launchd 拉起守护进程。

参考 ios-sweet 的 NOTES.md 里 0036–0050 号补丁对应的问题：
- `CS_SIGNED` / 平台二进制
- `boot-manifest-hash`（ADT 里加）
- bindfs（`mount` 需要）
- IOWatchdog（`watchdogd` 会主动崩溃触发 panic）
- Sandbox 策略

**验收标准**：`lockdownd`、`configd` 等守护进程有 PID。

### 阶段 3：显示

**目标**：屏幕不再停在苹果标。

需要 `IOMobileFramebuffer` + `IOSurface`。ios-sweet 用 CPU 软件渲染（无 GPU 驱动），
uke 也卡在这一步。**参考 uke 的现状**。

**验收标准**：屏幕上出现 launchd 的进度或 SpringBoard 的启动画面。

### 阶段 4：SpringBoard

**目标**：进主屏幕。

难点：数据迁移（ios-sweet 花了很久，最后选择跳过）、`backboardd` 的 check-in。

---

## 七、工程方法（血泪教训）

### 调试原则

1. **先读文档，再动手。** `docs/concepts/` 里一句话可能省掉几天。
2. **先找现成实现，再自己推导。** 30 多个机型的 `docs/devices/` + 上游三个移植，
   大部分问题别人踩过。
3. **一次只改一个变量。** 多变量改动出问题时无法归因。
4. **沉默 ≠ 卡死。** 必须用可测量的判据（本项目的块计数器就是这么来的）。
5. **埋点取样前，确认缓冲区装的是什么。** 本项目被自己的埋点骗过三次：
   内存描述符回读、花括号计数、`bounce[0]` 取错偏移。

### 构建工程

- **改完补丁跑 `work/sync-usb-patch.py`**，不要等 CI 报错
- **hunk 头的行数必须重算**，`git apply` 严格按它截断
- **补丁按文件名排序应用**，依赖行号的补丁要排前面
- **Windows 的 `write_text` 会把 LF 变 CRLF**，写补丁一律 `newline='\n'`
- **确认命令在目标平台存在**（`blockdev` 是 Linux 的，macOS 没有）
- **`$GITHUB_ENV` 只对后续步骤有效**，同一步读不到
- **`actions/cache` 路径按 workspace 根解析**，checkout 到子目录时要加前缀

### 设备操作

- **只 `fastboot boot`，从不 flash**，镜像只进内存，设备刷不坏
- panic 会经 `picasso_usb_reset=pshold` 自动回 fastboot
- ignition 卡住不会 panic，**需要手动断电**（长按电源 10 秒 → 电源 + 音量下）

---

## 八、工具速查

```sh
# 构建（GitHub Actions，约 5-10 分钟，缓存已开）
tools/bin/gh.exe workflow run compose-bootimg.yml -R tselongson/UniXNU-picasso --ref boot-evidence
tools/bin/gh.exe run download <run-id> -R tselongson/UniXNU-picasso -D out/vNN

# 刷机 + 抓串口（一条命令）
python work/boot-and-capture.py COM5 170 work/console.log out/vNN/.../boot/boot.img

# 驱动本地编译检查（WSL，秒级）
wsl -e bash -lc "bash /root/miphone/check-driver.sh"

# 解析 userdata 镜像里的 APFS 卷 UUID
python work/read-apfs-uuid.py

# 从 ADT 里确认属性（FDT 结构）
python work/dtb_dump.py <adt-file>
```

**关键脚本**：
- `work/boot-and-capture.py` —— 刷机 + 串口抓取一体
- `scripts/make-picasso-adt.py` —— ADT 生成（`/chosen` 下的所有属性）
- `scripts/make-data-seed.py` —— 从零创建 Data seed（hdiutil + DBRDISK1 打包）
- `scripts/build-arm-driver.py` —— 驱动归档编译（组件表在 `out_map`）
- `unixnu` —— 构建包装器，负责钉版本、打补丁、注入设备补丁

---

## 九、接手第一步

1. 读 `docs/status.md` 的阶段表和卡点
2. 读本文档第二节，把 `upstream/uke` 和 `upstream/sheng` 的 `board.toml` 对照一遍
3. 读 `docs/concepts/boot-chain.md`（上游设计说明）
4. 跑一次构建，确认能出 boot.img
5. 刷机，确认能复现"launchd 启动后卡住"
6. 从阶段 1 的未完成项继续
