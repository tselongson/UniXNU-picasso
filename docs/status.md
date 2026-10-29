# 移植状态

最后更新：2026-10-30 00:10（v86 回退，commit `294ad75`）

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
| 14 | launchd 签名信任 | ❌ | `unsuitable CT policy 0 for this platform/device` |
| 15 | launchd 运行 / 用户态 | ❌ | 未到达 |
| 16 | 苹果标 / 图形栈 | ❌ | 未到达 |

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

### 新的阻塞点：AMFI 拒绝 launchd 的签名

launchd 被读出、解压、解析之后，AMFI 拒绝它：

```
load_init_program: attempting to load /sbin/launchd
AMFI: '/sbin/launchd' is adhoc signed.
AMFI: '/sbin/launchd': unsuitable CT policy 0 for this platform/device, rejecting signature.
AMFI: hook..execve() killing pid 1: dyld signature cannot be verified.
```

随后内核 panic：`unexpected SIGKILL of launchd`（namespace 9 = CODESIGNING）。

**三个启动参数都试过，输出一字未变**：

| 尝试 | 结果 |
|---|---|
| `amfi_get_out_of_my_way=1` | ❌ |
| ADT `amfi-only-platform-code` 1→0 | ❌ |
| `amfi_allow_any_signature=1` | ❌ |

三次完全相同，说明这些开关管不到这个判断。`CT policy 0` 里的 0 是"没有策略数据"，
不是"策略太严"——所以没有东西可以放宽。

### 试过并失败的路：StaticTrustCache

正确机制应当是提供信任缓存（IPSW 里的 `StaticTrustCache` 含所有系统二进制的 cdhash）。
workflow 能提取它（`Firmware/044-76040-038.dmg.trustcache`，59519 字节），
但传进 payload 后 **DarwinBoot 直接拒绝整个 payload**：

```
'Boot' returned: Compromised Data          ← EFI_COMPROMISED_DATA
```

`docs/concepts/boot-chain.md` 的失败表里，这一状态只对应一个原因：
"invalid kernel, ADT or trust cache"。

原因在 `make-picasso-adt.py:137`：`--trust-cache` 在那里是**布尔开关**，写的是
**16 字节零占位符**（注释原文 "Zeroed TrustCache sidecar handoff placeholder"）。
DarwinBoot 会校验它，一串零过不去。

**该接线已回退（`294ad75`）**，提取步骤保留但不再传给构建。
下一步要提供 DarwinBoot 能接受的缓存，而不是占位符。

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
