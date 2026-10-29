# 移植状态

最后更新：2026-10-29 19:15（v71，commit `3321517`）

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
| 9 | APFS 容器方案匹配 | ❌ | `AppleAPFSContainerScheme::probe fails` |
| 10 | APFS 卷挂载 | ❌ | 缺 `apfs_vfsop_mount: ... mounted volume: System` |
| 11 | `load_init_program` / launchd | ❌ | 未到达 |
| 12 | 苹果标 / 图形栈 | ❌ | 未到达 |

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

### 真正的症状

我们的日志里缺了一行关键输出——qemu 有而我们没有：

```
apfs_vfsop_mount:2197: disk0s1s1:0 mounted volume: System
```

而且紧跟在发布之后有：

```
NativeUfs: published SystemOS
IOGUIDPartitionScheme::probe fails
IOFDiskPartitionScheme::probe fails
IOApplePartitionScheme::probe fails
AppleAPFSContainerScheme::probe fails        ← 容器方案没认这块盘
```

`nx_mount` 的 checkpoint 搜索是成功的（xid 14），说明**读盘没问题**；
但 `AppleAPFSContainerScheme::probe` 失败，意味着 APFS 卷没有被发布成 IOMedia，
所以 `bsd_init` 等不到 `Apple_HFS`。

### 下一步：查容器方案为什么 probe 失败

需要确认我们发布的 IOMedia 的 `Content` 属性和 `Whole` 标记，以及
`AppleAPFSContainerScheme` 的匹配条件。读盘已经证明可用，所以问题在**媒体属性**或**探测时的数据**。

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
