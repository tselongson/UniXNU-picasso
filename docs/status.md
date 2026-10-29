# 移植状态

最后更新：2026-10-29 18:45（v71，commit `3321517`）

## 一句话

XNU 已经跑过 BSD 层、USB 控制台可用、**UFS 根设备成功发布、APFS 读到容器 checkpoint**。
当前卡在根卷认证：设备树缺少 `system-volume-auth-blob`，APFS 因此无法导入根卷哈希。

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
| 9 | 根卷认证（root hash） | ❌ | `could not retrieve system-volume-auth-blob` |
| 10 | `load_init_program` / launchd | ❌ | 未到达 |
| 11 | 苹果标 / 图形栈 | ❌ | 未到达 |

第 7、8 阶段是 2026-10-29 打通的，见 [ufs-root.md](ufs-root.md)。
上游三个 Picasso 移植目前停在**第 6 与第 7 之间**（README 自述"存储 bring-up 是当前阶段"）。

## 当前阻塞点

```
import_iboot_forwarded_roothash:3491: importing root hash (basesystem ? 0)...
apfs_extract_root_hash_arm:13275: could not retrieve system-volume-auth-blob from device tree
import_iboot_forwarded_roothash:3496: apfs_extract_root_hash_and_manifest failed with error: No such file or directory (2)
Waiting on <dict ... IOProviderClass=IOMedia ... Content=Apple_HFS>
```

`system-volume-auth-blob` 正常由 iBoot 从个性化 IM4M 生成并写进设备树。我们没有真实
iBoot 启动链，所以 ADT 里没有这个属性。`apfs_extract_root_hash_arm` 在 APFS kext 二进制里，
不在源码里。

待选方案（尚未验证任何一个）：

1. 往 ADT 注入 `system-volume-auth-blob`
2. 二进制 patch APFS kext，跳过根卷哈希校验
3. 找 APFS 的相关 boot-arg

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
