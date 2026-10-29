# UFS 根设备：从「静默卡死」到发布

这一页记录两个问题，以及各自是怎么被证明的。两个都是**沉默失败**——没有 panic、没有错误码，
只有屏幕停住。所有基于"屏幕不动"的推断都错过一次。

---

## 一、`OSTypeAlloc` 跳进空页（2026-10-26 ~ 10-28）

### 症状

`publish` 流程走到 `OSTypeAlloc` 就再没有输出。构造函数里的日志一行都没打。

### 排除过程

逐一排除且都被证伪：构造函数本身（反汇编跟到底）、vtable（190 项全扫）、日志锁、线程环境、
内存分配器。全都"正确"，但都没解决问题。

### 真正原因

`build-arm-driver.py` 里 IOStorageFamily 的 11 个绑定地址是**硬编码的厂商原始值**，
而 prelink 会把整个组件下移 16 MB。**每个符号都指向真实代码上方 16 MB 的空页。**

构造函数看起来"无害"，是因为**调用根本没到构造函数**——第一跳就飞出去了。

`nm` 里符号类型是 `A`（absolute），那是绑定产物，不是代码，必须检查它有没有加过 slide。

### 修法

从厂商 stock 表取原始地址，加上 `board.toml` 里的 slide（`-0x1000000`），作为绝对符号绑定。
生成器是 `build-picasso-ufs-bindings.py`，接在 prelink 之后。

### 教训

**"地址有段支撑"不等于"地址正确"** —— 空页也有段支撑。

---

## 二、`IODelay` 永远不返回（2026-10-29）

### 症状

`NativeUfs: stage NOP OUT` 之后什么都没有。不是 panic，不是错误，就是停住。

### 三次错误推断

1. **"轮询超时太长"** —— 把 `pollUfsTransfer` 的迭代上限从 200000 降到 20000。
   没有任何变化。诊断日志一条都没打出来。
2. **"控制台干扰 UFS"** —— 用 `picasso_usb=0` 完全关掉控制台。UFS 照样卡在 NOP OUT。
3. **"补丁改坏了驱动"** —— 对比 v55，`NativeUfs.cpp` 一字未改，只少了日志。

三次都错了，因为**沉默被我当成了"没有进展"，而实际是"代码根本没执行到那里"**。

### 真正原因

```c
void delay () { IODelay (2); }     // RootCommands.h，轮询循环里每次迭代都调
```

`IODelay` 靠**时间基准**自旋等待。在这个上下文里它**第一次调用就不返回**，于是：

- 轮询上限根本没机会执行
- 失败诊断、panic 全部执行不到
- 表现就是屏幕定格

换成不依赖时间基准的有限自旋后立刻通了：

```c
void delay ()
{
  for (volatile unsigned spin = 0; spin < 200; ++spin) {
    __asm__ __volatile__ ("" ::: "memory");
  }
}
```

### 证据

v71 日志（`evidence/console-20261029-v71-ufs-published.log`）：

```
NativeUfs: stage NOP OUT
NativeUfs nop: enter
NativeUfs nop: cleared
NativeUfs nop: submit returned 0 ocs=0     ← 命令成功完成
NativeUfs: stage power mode query
NativeUfs: stage LU0 ready
NativeUfs: host bring-up ok
NativeUfs: root layout first=2c0000 last=1d82bfa manifest=0 windows=2 magic=474e4853 544f4652
NativeUfs: startRoot returned 1 (blocks=1 step=3)
NativeUfs: published SystemOS
dev_init:303: disk0 device_handle block size 4096 block count 806400
nx_mount:1617: disk0 checkpoint search: largest xid 14, best xid 14 @ 9
nx_mount:1644: disk0 stable checkpoint indices: desc 8 data 16
```

### 遗留疑点（未解释）

**同样的 `IODelay` 在 v55 是能返回的**，那时读盘成功。v55 与 v70 的差异只有
USB 补丁版本和几个启动参数。为什么同一个调用在两个版本里行为不同，**目前没有证据能指认**。

这一条被明确记录为未解释项，不要在它之上做假设。

### 教训

**沉默不等于卡死。先确认超时机制本身是否可靠，再下结论。**
这个板上所有基于时间的超时都要打折扣——`IODelay` 的实际时长比参数大得多。

---

## 三、当前状态

UFS 已完全工作：发布 `SystemOS` → APFS 挂载容器 → 读到 checkpoint（xid 14）。
阻塞点已转移到根卷认证，见 [status.md](status.md)。
