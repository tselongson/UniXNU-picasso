# 交接

## 五分钟了解现状

1. 读 [status.md](status.md) 的阶段表 —— 11 个阶段，到第 8 个
2. 读 [ufs-root.md](ufs-root.md) —— UFS 两个沉默失败是怎么被证明的
3. 读 [debugging.md](debugging.md) —— 怎么抓日志、参数含义、补丁的坑

## 当前唯一阻塞点

根卷认证：设备树缺 `system-volume-auth-blob`，APFS 无法导入根卷哈希。

```
import_iboot_forwarded_roothash:3491: importing root hash (basesystem ? 0)...
apfs_extract_root_hash_arm:13275: could not retrieve system-volume-auth-blob from device tree
import_iboot_forwarded_roothash:3496: apfs_extract_root_hash_and_manifest failed with error: No such file or directory (2)
```

三个候选方案，**一个都还没验证**：

| 方案 | 位置 | 风险 |
|---|---|---|
| 往 ADT 注入 `system-volume-auth-blob` | payload 的设备树 | 需要知道 blob 的格式与来源 |
| 二进制 patch APFS kext 跳过校验 | `kernel.macho` 里的 kext | 改动二进制，需重新签名流程配合 |
| 找 APFS 的相关 boot-arg | 启动参数 | 可能不存在 |

`apfs_extract_root_hash_arm` 在 APFS kext 二进制里，**不在源码里**，所以改不了源码。

## 已验证 / 未验证

**已验证（有日志证据）**

- 内核跑过 BSD 层、APFS 注册
- USB CDC 控制台可用，日志能完整抓到
- UFS 发布 `SystemOS`，APFS 读到容器 checkpoint（xid 14）
- `picasso_usb_hold=1` 会把内核停在 `bsd_init`（源码 + 日志双重确认）
- `picasso_usb_smmu=0` 时串口不出现

**已用单变量实验证实（2026-10-29 晚）**

- **`delay()` 换自旋是唯一原因**：分支 `probe/iodelay-restored` 只把 `delay()` 换回
  `IODelay(2)`，其余不动 —— 结果卡在 `nop: cleared` 之后，`submit returned` 打不出来。
  恢复自旋即发布成功。
- **NOP 三行埋点是惰性的**：分支 `probe/no-breadcrumbs` 只删埋点，结果与 v71 完全一致
  （`published SystemOS` + `checkpoint xid 14`）。

**已排除**

- **"控制台 busy poller 饿死 `IODelay`"**：`IODelay` 版本 + `picasso_usb=0`（无控制台）
  仍然停在 `Waiting on Apple_HFS`。假设被推翻。

**未验证 / 未解释（不要在这些之上做假设）**

- **为什么 `IODelay` 在 v55 能返回、在 v70 不能** —— 同一个调用，两个版本行为不同。
  poller 饿死已被排除，剩下的差异只有 USB 补丁版本本身，但**没有证据指认**。
- poll 上限 20000 与 NOP 失败诊断是否必需：从未被触发过，属于安全网
- 上游补丁里 `nointr_consio` / `nointr_iolog` 之外的部分是否有副作用

## 建议的下一步顺序

1. **先把 v71 的改动逐项回退验证**，得到干净基线：自旋延时、NOP 埋点、poll 上限 20000、
   NOP 失败诊断 —— 四项里哪些是必需的。这花两三版构建，但之后每一步都站得稳。
2. 再动 root hash。

**不要跳过第 1 步。** 当前驱动里混着调试残留，直接往前推会在不牢的地基上盖楼。

## 纪律（来自踩过的坑）

- **一次只改一个变量**，单独构建、单独留日志、单独记结论
- **沉默不等于卡死**：先确认超时机制本身可靠，再下"卡住了"的结论
- **报错位置经常是假的**：987、1114 这些行号在补丁里根本不存在。
  定位方法是把补丁还原成文件看实际内容
- **补丁改完先跑 `work/sync-usb-patch.py`**，不要等 CI 报错
- **只 `fastboot boot`**，从不 flash，设备刷不坏

## 设备操作

- 进 fastboot：长按电源 10 秒 → 电源 + 音量下
- 正常刷机后**不需要手动断电**：panic 会经 `picasso_usb_reset=pshold` 自动回 fastboot
- 只有设备完全死透（既无串口也无 fastboot）才需要物理断电
