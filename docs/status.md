# 移植状态

最后更新：2026-10-29 21:20（v77，commit `54a1c3d`）

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

### 下一个要验证的假设

`/chosen/root-matching` **没有设置**（`scripts/make-picasso-adt.py` 支持
`--root-match-volume-uuid`，但 `board.toml` 没传）。没有它，XNU 用默认的
`Content=Apple_HFS` 等待，而这要求卷已经被发布——但发布卷是容器方案的职责，
容器方案又没成功。**这是个循环。**

两条候选路线：

1. 提供 OS 卷的 UUID，让 XNU 按 UUID 匹配（UniXNU 的做法）。UUID 需要从镜像里读出来
2. 查清容器方案为什么在 block 0 有 `NXSB` 的情况下仍然 probe 失败
   （怀疑是媒体的 `Whole` 属性，但媒体由基类创建，我们设不了）

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
