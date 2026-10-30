# 移植状态

最后更新：2026-10-30 22:20（commit `9faba75`，**launchd 已运行并执行启动任务**）

## 一句话

XNU 已经跑过 BSD 层、USB 控制台可用、**UFS 根设备成功发布、APFS 读到容器 checkpoint**。
当前卡在 **APFS 容器方案探测失败**：读盘正常，但 `AppleAPFSContainerScheme::probe` 不认这块盘，
所以根卷从未发布成 `Apple_HFS`。

## 阶段表

| # | 阶段 | 状态 | 证据 |
|---|---|---|---|
| 1 | UEFI → DarwinBoot → XNU 跳转 | ✅ | 内核横幅 |
| 2 | IOKit 匹配、kext 加载（AMFI / AppleImage4 / MAC） | ✅ | `Security policy loaded` |
| 3 | BSD 层、mbufs、dlil、lo0 | ✅ | `dlil_init` / `ifnet_attach` |
| 4 | APFS 注册 | ✅ | `apfs_module_start: v2236.140.6` |
| 5 | USB CDC 控制台 | ✅ | `COM5`，见 `evidence/` |
| 6 | UFS 控制器 bring-up（NOP OUT / 查询 / LU 就绪） | ✅ | `host bring-up ok` |
| 7 | **根设备发布** | ✅ | `published SystemOS` |
| 8 | **APFS 容器挂载** | ✅ | `nx_mount: checkpoint search: largest xid 14` |
| 9 | **APFS 容器方案匹配** | ✅ | `Got boot device = .../AppleAPFSContainerScheme/AppleAPFSMedia/...` |
| 10 | **根卷挂载** | ✅ | `mount-complete volume DawnUpdate21H461...` |
| 11 | **根哈希认证** | ✅ | 加 `allow-root-hash-mismatch` + bsd_init 补丁后越过 |
| 12 | **`load_init_program`** | ✅ | `attempting to load /sbin/launchd` |
| 13 | **launchd 读取与解压** | ✅ | 加 `AppleFSCompression` kext 后 `AMFI: is adhoc signed` |
| 14 | **launchd 签名信任** | ✅ | `loaded external trust cache module: 0/1` |
| 15 | **launchd 运行（用户态）** | ✅ | `Darwin Ignition Sequence Version 1.0.0` |
| 16 | ignition 后续阶段 | ❌ | 启动后无输出，系统存活但静默 |
| 17 | 苹果标 / 图形栈 | ❌ | 未到达 |

第 7、8 阶段是 2026-10-29 打通的，见 [ufs-root.md](ufs-root.md)。
上游三个 Picasso 移植目前停在**第 6 与第 7 之间**（README 自述"存储 bring-up 是当前阶段"）。

## 当前阻塞点

**不是 root hash。** 这一点有外部证据。

### 外部参照：qemu-t8030

[TrungNguyen1909/qemu-t8030 issue #44](https://github.com/TrungNguyen1909/qemu-t8030/issues/44)
在 QEMU 里启动 iOS，报**完全相同**的错误：

```
import_iboot_forwarded_roothash:2577: importing root hash ...
apfs_extract_root_hash_arm:10001: could not retrieve system-volume-auth-blob from device tree
import_iboot_forwarded_roothash:2580: apfs_extract_root_hash_and_manifest failed with error: No such file or directory (2)
```

**但系统继续启动了**，最终到达 launchd 并能 SSH：

```
apfs_vfsop_mount:2197: disk0s1s1:0 mounted volume: System
Darwin Bootstrapper Version 7.0.0
com.apple.xpc.launchd[1] <Notice>: launchd logging initialized
```

**结论：`system-volume-auth-blob` 缺失是非致命的噪声，不是阻塞点。**
这条错误在我们日志里出现，说明不了为什么没启动。

### 真正的症状（v77 实测）

两个窗口的内容**都是正确的**，已逐字节确认：

```
blk[SystemOS]: block=0  head=1a 3a 25 60 ...   kind=NXSB        裸 APFS 容器
blk[OS]:       block=0  head=00 00 ...         保护性 MBR
blk[OS]:       block=1  head=45 46 49 20 50 41 52 54 = "EFI PART"  GPT 头
blk[OS]:       block=2  head=ef 57 34 7c ...   分区项
```

`OS` 窗口是带 GPT 的磁盘镜像，`SystemOS` 是裸容器——与 `docs/concepts/userdata.md`
的描述完全一致。**镜像没问题。**

而且分区方案**成功**了：

```
nx_mount: disk1s1 checkpoint search: largest xid 418, best xid 418 @ 3
nx_mount: disk0   checkpoint search: largest xid 14,  best xid 14  @ 9
```

`disk1s1` 说明 GUID 分区方案发布了分区，两个容器都挂载了。

### 剩下的问题

**`AppleAPFSContainerScheme::probe` 从未成功**（日志里失败三次），所以 APFS 没有把卷
发布成 `IOMedia`，`bsd_init` 等不到 `Content=Apple_HFS`。

`nx_mount` 是 VFS 在 BSD 设备上直接请求挂载触发的，不是容器方案触发的。

### 突破：根卷挂载成功（v78）

给 ADT 加上 `/chosen/root-matching`（UUID `04769D26-2CA0-4825-B443-E2E5F88300F2`，
从 userdata 镜像里读出来的）之后，死锁解开了：

```
Waiting on <dict ... IOPropertyMatch ... UUID 04769D26-...>
Got boot device = IOService:/ArmDTPlatform/ufshc/NativeUfs/NativeUfsBlock/IOBlockStorageDriver/
                  Native OS Media/IOGUIDPartitionScheme/Untitled 1@1/AppleAPFSContainerScheme/
                  AppleAPFSMedia/AppleAPFSContainer/DawnUpdate21H461.J120J121J207J208OS@1
BSD root: disk2s1, major 1, minor 4
apfs_vfsop_mountroot: apfs: mountroot called!
handle_mount: disk2s1 mounting volume DawnUpdate21H461...
mount-complete volume DawnUpdate21H461...
```

**整条存储链打通**：UFS → IOBlockStorageDriver → 分区方案 → APFS 容器 → APFS 卷。

### 突破：用户态跑起来了（v87）

按 `docs/concepts/payload.md` 的要求提供 **trust bundle**（不是裸 cache）之后，AMFI 放行：

```
AMFI: attempting to load 2 external trust cache modules
loaded external trust cache module: 0
loaded external trust cache module: 1
completed loading external trust cache modules
```

然后 launchd 作为 PID 1 启动：

```
load_init_program: attempting to load /sbin/launchd
Darwin Ignition Sequence Version 1.0.0: Fri Jul 11 17:33:51 PDT 2025
libignition: 1:   ignition level    : 0x5
```

**这是第一次进入用户态。** 设备保持存活（串口心跳持续），内核没有 panic。

### 之前为什么失败

传的是裸的 `.dmg.trustcache`（IMG4 外壳）。文档规定 DarwinBoot 校验的是 **bundle**：
32 位计数、偏移表、每个 cache 以 version 2 开头、每项 24 字节、hash 升序。
**占位符与输入不匹配就是 `EFI_COMPROMISED_DATA`** —— 正是 v86 看到的。

正确链路（已实现）：

```
extract-target-volume.py StaticTrustCache  → IMG4
scripts/unwrap-img4-payload.py             → bare version-2 module
scripts/build-original-system-trust.py     → bundle（用 sha256 钉死两个输入）
unixnu build --trust-cache <bundle>
```

生成的 bundle sha256 是 `7416ca4f...`，与 `make-owner-trust-bundle.py` 里钉死的常量一致。

### 当前状态：ignition 静默

`ignition level: 0x5` 之后再无输出，但**设备活着**（心跳持续、串口在）。
不是崩溃，是在等待或静默工作。

### 已提供 Data seed（v92），但 ignition 仍停在同一处

`data_ramdisk_sha256` 之前一直是 null。经测量确认**两个容器各只有一个卷**（OS 是系统卷
`04769d26`，SystemOS 是 cryptex `8846fe78`），没有 Data 卷；`-apfs_shared_datavolume` 也无效。
所以 Data 卷必须由 seed 提供。

仓库里没有 seed，也没有从零创建的工具（`seed-data-grow.py` 只能扩展现有 `.dbr`，sheng 无发布）。
但**打包器是现成的**：`drivers/DarwinBoot/Test/packed_ramdisk.py` 的 `pack()`，
DarwinBoot 自己的测试就用它。

`scripts/make-data-seed.py` 用 hdiutil 建空 APFS 容器、dd 出原始设备、打包成 DBRDISK1：

```
data.dbr: 110704 bytes packed from 3 GiB
```

payload 里确实有了（`payload.json` 的 `data_ramdisk_sha256` 非空，payload.img 里能搜到
`PackedRAMDisk` 占位符）。**但 ignition 依然停在 `ignition level: 0x5` 之后，输出与 v89 完全相同。**

所以"缺 Data 卷"这个假设**已被证伪**，或者至少不是唯一原因。

### 用块计数器测出的确定事实（v93/v94）

给驱动加了 I/O 计数器之后，答案很干脆：

```
NativeUfs blk[OS]: #263 block=10711530 count=72
shared_region: [1(launchd)] vm_shared_region_start_address() failed
NativeUfs blk[OS]: #264 block=10711314 count=40      ← 最后一次 I/O
Darwin Ignition Sequence ... ignition level: 0x5
（之后零 I/O）
```

**整个启动只有 252~264 次块请求，ignition 开始后一次都没有。**
所以 launchd 不是"安静地工作"，是**阻塞**，而且阻塞在不再触碰磁盘之后。

读的模式也记下来了：全部集中在 `10710970`–`10711602` 这个约 630 块的窄区间，长度杂乱
（32/40/56/64/72/80），是**随机访问元数据**，不是顺序读大文件。

### 试过无效的（都来自能启动的参考项目）

| 尝试 | 来源 | 结果 |
|---|---|---|
| `-apfs_shared_datavolume` | sheng | 输出不变 |
| `launchd_unsecure_cache=1` | ios-sweet / qemu-t8030 | 输出不变 |
| ADT `/chosen/asmb`（`lp-sip0`=0x6f，`lp-sip1`=1） | ios-sweet | **输出不变，252 次 I/O 一字不差** |

`asmb` 节点已确认写进了 ADT（`lp-sip0` 8 字节值 0x6f、`lp-sip1` 4 字节值 1），
但启动路径毫无变化——**SIP 不是这个内核在这个位置的阻塞点**。

### 与 ios-sweet 的结构性差异

`dipa-studio/ios-sweet` 在 Redmi Note 10 Pro 上跑到 SpringBoard，但它和我们有本质区别：

| 维度 | ios-sweet | 本移植 |
|---|---|---|
| 根文件系统 | **日志式 HFS+ 卷**（自建镜像，`mkiosroot.sh`） | APFS 原始 OS dmg |
| dyld 共享缓存 | 镜像里做了 **44 个 cryptex 缓存文件的硬链接** | 未处理 |
| 内核补丁 | **69 组**（含最小 AMFI、宽松沙盒、AppleKeyStore 桩） | 少量 |
| 启动参数 | `ignition_level=1`、`wdt=-1`、`-noprogress` | 无 |

**它绕开了整个 APFS 封印/根哈希/cryptex 问题，代价是自建根镜像和大量内核补丁。**
本移植走的是相反的路线，而现在的卡点（launchd 启动后阻塞、dyld 共享缓存不可用）
正是它用那套方案解决掉的问题。

**继续调参数收益很低，需要决定是否转向它的方案。**

### sheng 的路线：试过了，是关闭的

sheng 能跑是因为它启用了整个 owner-root 栈。逐项试下来的结论：

| 障碍 | 结果 |
|---|---|
| `XNU_OWNER_ROOT_TRUST` 要求 `XNU_NATIVE_VIRTIO_BLOCK` | **可解**。那是构建期配置检查，不是硬件要求。sheng 也是开 virtio 满足它、实际从 UFS 启动。已给 `build-arm-driver.py` 加上 `NativeVirtioBlock` 组件，归档能编出来 |
| `XNU_VERIFIED_ROOT_ARCHIVE`（`libVerifiedRootProbe.a`） | **关闭**。`scripts/build-verified-root-probe.py` 直接拒绝：`The two-volume bundle requires the positive original OS policy` |

那份 policy 是**苹果签发、绑定具体设备和系统的材料**——和根哈希同一类输入，小米设备永远拿不到。

**所以 sheng 的 `OWNER_UNION_SYSTEMOS`（把 cryptex 联合挂载到根上、提供 dyld 共享缓存）
这条路走不通，不是因为构建细节，是因为路线本身关闭。**

### 突破：SystemOS cryptex 联合挂载成功（uke 路线）

用 uke 的 bringup 配置之后，`native_owner_cache_prepare` **通过了全部校验**，cryptex 挂载并联合到根：

```
apfs_log_op_with_proc: disk3s1 mounting volume DawnUpdate21H461.J120J121J207J208SystemCryptex, requested by: init (pid 1)
apfs_log_op_with_proc: disk3s1 mount-complete volume DawnUpdate21H461.J120J121J207J208SystemCryptex
load_init_program: attempting to load /sbin/launchd
```

**这是本项目第一次把 cryptex 挂起来。** 之前 launchd 卡住正是因为共享缓存在 cryptex 里而没挂载。

关键改动（全部照 uke）：
- `CONFIG_XNU_BRINGUP_OWNER_CACHE=y` + 缓存挂载/联合挂载，不开验证根
- `build-xnu-4k-baseline.sh` 四处改动（`picasso-bringup-owner-cache.patch`）
- `kern_exec.c` 的 `#error` 放宽（`picasso-bringup-kern-exec.patch`）
- `native_verified_block_*` 由 UFS 驱动提供（`native-ufs-allow-unverified=1`）

### 当前卡点：读共享缓存后复位，无 panic 文本

挂载之后 launchd 开始从 SystemOS 读共享缓存，请求量从 264 涨到 **2560+**（每次 1 MB）：

```
NativeUfs blk[SystemOS]: #2304 block=1984944 count=2048
NativeUfs blk[SystemOS]: #2560 block=6238200 count=8
```

然后设备复位回 fastboot，**控制台没有任何 panic 文本**。

已排除：
- **不是我们驱动的 pshold 复位** —— 那条路径会先打印 `PICASSO CDC: reset begin`
- **`wdt=-1` 无效** —— `wdt` 只在 `osfmk/arm/arm_init.c` 和 `zalloc.c` 里解析，**arm64 没有处理路径**，是空操作

两个候选方向：
1. **panic 输出被吞掉** —— panic 走了别的控制台（屏幕上是帧缓冲花屏，可能就是 panic 文本用了错误格式）
2. **硬件看门狗** —— 读 2.5 GB 缓存耗时够长，而内核无法关它

下一步优先验证方向 1：让 panic 路径强制冲刷 USB 控制台，否则后面每次复位都是瞎的。

### Data 卷：UUID 打通，APFS 开始挂载

上一版的失败是 `offline scratch Data mount failed: 2`（ENOENT）。原因很具体：

- `NativeRamBlock` 的 `selectVolumeSeed` 按 **APFS 卷 UUID** 匹配 IOMedia
- 配置里是 `CONFIG_XNU_OWNER_DATA_VOLUME_UUID="00000000-..."`（全零占位符）
- 而 seed 由 `hdiutil` 生成，**UUID 每次随机**

修复链：
1. `make-data-seed.py` 从 `diskutil` 读出卷 UUID，写进 `configs/picasso_defconfig`
2. **`unixnu defconfig` 会把 defconfig 复制成工作副本，而副本在 seed 生成之前就做了** —— 所以 seed 之后要重新执行一次 `unixnu defconfig`
3. 验证方式：直接在 `kernel.macho` 里搜生成的 UUID（v118 出现 1 次，全零占位符消失）

**结果：Data 卷被识别了。**

```
dev_init:306: disk5 device_handle block size 4096 block count 735222   ← 3.01 GB，正是 seed
nx_mount:1275: disk5 initializing cache w/hash_size 1024
nx_mount:1617: disk5 checkpoint search: largest xid 2
```

### 当前卡点：ram block I/O 返回 not ready

```
IOBlockStorageDriver; executeRequest: request failed to start!
disk5: device is not ready.
```

`NativeRamBlock::doAsyncReadWrite` 在 `online` 为 false 或 `big`/`sparse` 取页失败时返回
`kIOReturnNotReady`。媒体已经发布（尺寸正确、APFS 读到了 superblock 和 checkpoint），
说明 `registerService` 跑过了，问题在**读路径的取页**。

下一步查 `isBig` / `sparse.pages` 的初始化——我们的 seed 是 3 GB packed，应该走 `isBig` 分支。

### 里程碑：launchd 运行起来了

Data 卷挂载成功后，launchd 第一次真正跑起来并输出日志：

```
apfs_log_op_with_proc: disk5s1 mounting volume Data, requested by: init (pid 1)
handle_mount:817: disk5s1 vol-uuid: AE98A1AA-... (unencrypted; features: 1.0.2)
mount-complete volume Data
load_init_program: attempting to load /sbin/launchd
com.apple.xpc.launchd| <Notice>: boot-mode set by sysctl: main
com.apple.xpc.launchd| <Notice>: hello, launchd UUID: 4B525992-...
com.apple.xpc.launchd| <Notice>: Darwin Bootstrapper Version 7.0.0
com.apple.xpc.launchd| (system) <Notice>: entering ondemand mode
com.apple.xpc.launchd| (exclaves-boot) <Notice>: Doing boot task
com.apple.xpc.launchd| (fsck) <Notice>: Doing boot task
com.apple.xpc.launchd| (mount-phase-1) <Notice>: Doing boot task
```

**`Doing boot task` 就是 guide.md 里阶段 1 的验收标准，已达成。**

关键修复（`9faba75`）：`NativeRamBlock::selectVolumeSeed` 只查一次 IOMedia 就放弃，
而 Data 卷的媒体是异步出现的（屏幕上 `disk5s2` 在失败之后才出现）。
改成 100 × 100 ms 的等待循环——和 uke 的 `native_verified_block_volume_device` 同样的做法。

### 当前卡点：mount-phase-1 退出 66

```
pid 3 exited -- no exit reason available -- (signal 0, exit 66)
coredump (mount, pid 3): failed to open core dump file /private/var/cores/mount.core: error 2
```

`mount`（pid 3）是 launchd 的 mount-phase-1 任务。退出码 66 且 `/private/var` 仍不存在。

另外 `disk5: device is not ready` 仍在反复出现——ram block 的部分读仍然失败
（可能是 seed 里没有的非零页，读路径返回 not ready 而不是补零）。

下一步：
1. 让 ram block 对缺失页返回零而不是 not ready
2. 查 `/private/var` 的 graft 是否发生（`native_owner_data_prepare` 挂载后应该 graft 到 /private/var）

### 剩下的路：ios-sweet 的 HFS+ 方案

它的做法是**根本不碰 APFS 和 cryptex**：

- 自建日志式 HFS+ 根镜像（`mkiosroot.sh`），从 IPSW 的 dmg 里取文件
- 在 `/System/Library/Caches/com.apple.dyld/` 下给 44 个缓存文件建硬链接
- 根可写（`rd=ufsroot0 ufsrw=1 rootrw=1`），`/private/var` 直接在根上，不需要 Data 卷
- 内核打 69 组补丁（最小 AMFI、宽松沙盒、AppleKeyStore 桩、bindfs 等）

**没有 cryptex、没有封印、没有 policy。** 我们昨晚和今天在 APFS 上打的所有仗，它全部绕开。

### 下一步计划（不需要设备）

1. 用 `extract-target-volume.py` 从 IPSW 取出 OS 卷的 dmg
2. 在 CI 里用 `hdiutil` 挂载、建 HFS+ 卷、拷入文件、做 dyld 缓存硬链接（对应 `mkiosroot.sh`）
3. 把根改成 HFS+ 卷，去掉 APFS/root-matching/cryptex 相关配置
4. 补上 `bindfs`、`IOWatchdog` 等内核侧缺口

第 1–2 步可以完全在 CI 里验证，**不需要真机**。

### 另一条可借鉴的参数

qemu-t8030 的启动参数里有 `wdt=-1`（关看门狗）。我们这边之前"手机自己重启"就是 APSS 看门狗到期，
如果后续需要设备长时间挂着，这个参数值得试。

## 最后一条等待是什么意思

`Waiting on <dict ... Content=Apple_HFS>` 是 `bsd_init` 在向 IOKit 索取根卷，
**没有超时**（没有根设备系统就无法启动）。它后面不会再有任何输出，所以屏幕定格在这行。
它是症状，不是故障点。

## 构建与运行

```sh
# 构建（GitHub Actions，约 9-10 分钟）
tools/bin/gh.exe workflow run compose-bootimg.yml -R tselongson/UniXNU-picasso --ref boot-evidence
# 产物：out/vNN/picasso-bootimg-XX/boot/boot.img

# 刷机 + 抓串口（一条命令，自动等设备重新枚举）
python work/boot-and-capture.py COM5 90 work/console.log out/vNN/.../boot/boot.img
```

**只使用 `fastboot boot`，从不 `fastboot flash`。** 镜像只加载到内存，不写任何分区。

## 启动参数

```
-v debug=0x14e cpus=1
native-ufs-probe=12 native-ufs-diagnostics=1
nointr_consio=1 nointr_iolog=1
io=0x232
picasso_usb=2 picasso_usb_smmu=6 picasso_usb_hold=0 picasso_usb_reset=pshold
```

每个参数为什么在那里、去掉会怎样，见 [debugging.md](debugging.md)。
