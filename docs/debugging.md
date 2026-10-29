# 调试方法、工具与踩过的坑

这一页写给下一个人。内容都是花掉真实构建轮次换来的，按"能省多少时间"排序。

---

## 一、读日志：USB CDC 控制台

### 为什么需要它

屏幕会滚动，`bsd_init` 之后卡住时更是什么都不打印。没有串口时每一轮都要拍照 + 猜。

### 怎么读

**不要用 pyserial。** 它在 `open()` 时发 `SET_LINE_CODING`，而设备在早期启动阶段不保证及时
应答，于是报 `OSError(22) 信号灯超时`，即使端口在列表里、设备正在输出。

用 Win32 原始读取（`work/raw-com-read.py` / `work/boot-and-capture.py`）：
`CreateFile` + `ReadFile`，不做任何配置传输。

```sh
# 刷机 + 抓取一体，90 秒
python work/boot-and-capture.py COM5 90 work/console.log out/vNN/.../boot.img
```

脚本会：刷机 → 反复尝试打开端口（设备重新枚举要十几秒）→ 读到断开 → 自动重连。

### 关键限制

**串口只在设备重新枚举后才出现**（约 14 秒后）。所以必须"先刷机、再持续重试打开"，
不能"先连上再刷机"——fastboot 状态下没有这个端口。

---

## 二、启动参数：每一个为什么在那里

| 参数 | 作用 | 去掉会怎样 |
|---|---|---|
| `native-ufs-probe=12` | 调用 `startRoot`，失败时 panic 并打印媒体报告 | 不走这条路径 |
| `native-ufs-diagnostics=1` | 命令级跟踪 | 少日志，功能不变 |
| `nointr_consio=1` | 允许在关中断上下文输出控制台 | XNU 在 `serial_console.c:175` panic |
| `nointr_iolog=1` | 允许 `IOLog` 在关中断上下文运行 | `IOLib.cpp` 断言触发 |
| `picasso_usb=2` | 启用 CDC gadget | 无串口 |
| `picasso_usb_smmu=6` | 驱动自己编程 SMMU 路由并打印计划 | **串口不出现**（实测 `smmu=0` 无 COM 口） |
| `picasso_usb_hold=0` | **必须为 0**，见下 | 内核停在 `bsd_init` 永不继续 |
| `picasso_usb_reset=pshold` | panic 后经 PSHOLD 回 fastboot | 失败后要手动断电 |

### `picasso_usb_hold` 是个陷阱

```c
picasso_usb_park(int hold) {
    if (!hold) return;
    for (;;) {                       // 无限循环
        ud_poll_serialized();
        picasso_usb_watchdog_pet();
    }
}
```

`hold=1` 是**上游的调试模式**：故意把内核停在 `bsd_init`，好让你通过串口发
`status` / `panic-test` / `reboot-fastboot` 命令。上游的"完整抓取"就是这么来的。

照搬他们的参数会把启动流程一起停住。日志表现为：

```
PICASSO USB: holding at bsd_init; SMMU guard PASS
PICASSO USB: configured, streaming the console
（之后什么都没有）
```

### 看门狗

```
PICASSO USB: APSS watchdog armed ticks=491519 bark=491519 bite=491519 ctl=1
```

内核停住 → 不喂狗 → 到期复位。**手机"自己重启"不是崩溃，是看门狗到期。**
控制台线程在跑的时候会喂狗，所以有串口时设备能一直挂着不重启。

---

## 三、补丁工程：这一块烧掉最多轮次

这个仓库是**补丁套补丁**：`patches/picasso-usb-console-xnu.patch` 会被
`unixnu` 驱动嵌进 `patches/unixnu-source-kernel.patch`，构建时再展开到 XNU 源码树。

### 坑 1：hunk 头的行数必须重算

新建文件的 hunk 头写着行数（`@@ -0,0 +1,N @@`），`git apply` **严格按 N 截断**。
改了内容却忘了改 N，生成的文件就在半路被切断，编译器报的是"缺右括号"，
**和真实原因完全无关**。

校验方法：把补丁 apply 到空目录，用忽略字符串/注释的解析器数花括号。

### 坑 2：补丁按文件名排序应用

`patches/xnu/*.patch` 按名字顺序应用。如果补丁里带着**原始行号**，就必须排在前面，
否则前面的补丁会把行号推歪。我们的补丁名以 `aaa-` 开头就是这个原因。

报错会出现**补丁里根本不存在的行号**（比如 `bsd_init.c:987`），因为那是 git 尝试匹配的位置。

### 坑 3：Windows 的 `write_text` 会把 LF 变成 CRLF

`Path.write_text()` 在 Windows 上默认把 `\n` 转成 `\r\n`。补丁变 CRLF 后，
`git apply` 的上下文里会出现 `?` 字符，看起来像"上下文不匹配"，**实际是换行符问题**。

写补丁文件一律 `newline='\n'`。

### 坑 4：`patch --fuzz` 能应用，但可能落在错误位置

`git apply` 严格，行号偏移就拒绝；`patch --fuzz=5` 宽松，能应用但可能**落在语义错误的位置**
——实测出现过变量被插进函数体内部，导致作用域错误。

正确做法：**用 fuzz 应用 → 人工核对每个 hunk 的落点 → 用 `git diff` 重新生成补丁**。
重新生成的补丁 `git apply --check` 能过，而且落点是核对过的。

### 工具

```sh
python work/sync-usb-patch.py
```

一条命令完成：重算 hunk 头 → 重建内嵌副本 → apply 到临时目录 → 检查花括号平衡。
**改完补丁就跑一次**，不要等 CI 报错。

---

## 四、其他

- **只 `fastboot boot`，从不 `fastboot flash`**：镜像只进内存，不写分区。设备刷不坏。
- **panic 屏幕是好用的诊断通道**：它渲染寄存器和 `far`，即使日志滚掉了。
  `far = 映射基址 + 偏移` 直接指出越界读。
- **不要从驱动里去碰 DWC3**：控制器此时未开时钟，读它会挂住总线、被看门狗复位
  （v60 就是这么把手机弄进 recovery 的）。
