# GPU 用户态（Metal 插件）交接 — 2026-11-23

面向：换到 M1 Max 本地编译后的下一轮工作。
结论先行：**WiFi 已完成并验证；GPU 内核侧全通；唯一缺口是用户态 `NativeAdrenoMetal.bundle`，源码在上游、需要 macOS 工具链编译。云端 CI 已铺到"Turnip 编译通过、卡在 Rust 桥"的位置，本地 Mac 上继续最快。**

---

## 1. 现在确定的事实（不要再重复验证）

### 1.1 网络：已修好，字节精确
```
PICASSO_NETPROBE [SpringBoard] https  status=200 bytes=68      apple.com
PICASSO_NETPROBE [SpringBoard] https2 status=200 bytes=69      captive.apple.com
PICASSO_WEBKIT_PULL get status=200 bytes=1234567 err=0         本机 HTTP，1,234,567 字节一字节不差
```
唯一失败的 `http err=-1001` 指向 neverssl.com（外部端点），同轮对本机 HTTP 成功且字节精确，协议栈问题可排除。

### 1.2 GPU 内核侧：全链路跑通
```
PICASSO_GPU gmu: cold start complete
PICASSO_GPU hfi: run status=0 last-stage=7
PICASSO_GPU gx: oob polls=15 ... on
PICASSO_GPU cp: hw_init done / ring programmed / me_init idle
PICASSO_GPU zap: pas auth-and-reset a0=0 a1=0 / loaded=1
PICASSO_GPU cp: secure mode left=1 / noop ib fence=1 / ready fence=1
PICASSO_GPU cp: metal stage=5 status=0 registered=1 plugin=absent   ← 唯一缺口
```
`AGXAccelerator` 注册成功（`class AGXAccelerator : IOAcceleratorES`，类名故意与 Apple 一致，
让原始 sandbox profile 放行插件 open）。加 boot-arg `sheng-gpu-addon=1` 后日志变
`plugin=declared`，但 `MTLCreateSystemDefaultDevice()` 仍是 nil —— 因为仓库里没有 bundle 本体。

### 1.3 缺口的确切形态
内核在 addon 挂载后打印：
```
GPU addon: extensions=1/2 daemons=1/2 wallpaper=1/2
```
`<stage>/<errno>`：stage=1 表示**源目录不存在**（2=ENOENT）。即 payload 里的 NGPUADD2 addon
没有 `addons/` 内容。挂载关系（`patches/xnu/zzy-native-gpu-addon-mount.patch`）：

| addon 内路径 | 挂到 | 校验 |
|---|---|---|
| `addons/` | `/System/Library/Extensions` | 要求 `AGXMetalA10.bundle` 仍可见，且 `NativeAdrenoMetal.bundle/Info.plist` 可见（stage 5） |
| `daemons/` | `/System/Library/LaunchDaemons` | — |
| `wallpaper/` | 默认壁纸集合 | — |

---

## 2. 上游代码在哪（关键：不在本仓库）

| 内容 | 仓库 | pin |
|---|---|---|
| 超项目 + 全部构建脚本 + mesa 补丁 | `UniXNU/UniXNU` | `8542980d02b4d2cc5c32ee3542de1760b2562aac`（2026-11-19 main） |
| Metal 前端（Metal→metal2vulkan→Turnip） | `UniXNU/AdrenoMetal`（子模块） | `c9ebd2fb17253996f0a8d835c9ab7c2f740778e1` |
| Turnip darwin 后端的 ABI 头来源 | `UniXNU/Adreno740`（子模块） | `1c9029bc3216a39d9e514ba41dee12e95e8f3598` |
| PM4 工具 | `UniXNU/AdrenoPM4`（子模块） | `282606b528b2595bc4274ea7490b62b323f35a5f` |
| Mesa | `gitlab.freedesktop.org/mesa/mesa` | `85fda3b36405bcaea8bffc64aa4f2651dc90770d` |
| metal2vulkan（Rust） | `steelbrain/metal2vulkan` | `46e7cc96a284150bdf19b4a6a7890b0a8bc0ef71` |

本仓内核侧**早已适配**这套栈：Turnip 的 darwin 后端 `#include "NativeAdrenoABI.h"`（v1），
通过 `IOAcceleratorES` 服务 + open type `0x4147` 的 `{magic, method, payload}` 隧道调 external method
（sandbox 只放行个别 selector，所以隧道化）。不需要改内核 ABI。

---

## 3. 已在本仓 main 落地的东西

| 文件 | 作用 |
|---|---|
| `.github/workflows/build-metal-plugin.yml` | `workflow_dispatch`，输入 `targets`（`17.0` / `16.7` / `17.0+16.7`），两个 job 分别构建并上传 bundle + 日志 |
| `scripts/build-metal-plugin-ci.sh` | 完整构建脚本；**本地 Mac 可直接跑**（brew/rustup/xcrun） |

脚本做的事：解析 pinned 核心树 → 初始化 4 个子模块 → brew 装 ninja/glslang/shaderc/spirv-tools/llvm/bison/flex
→ rustup 1.98.0 + `aarch64-apple-ios` → mesa venv（`requirements-adreno-compiler.txt`，meson 1.9.1）
→ 取 pinned Mesa / metal2vulkan → 应用 36 个 mesa 补丁 → `build-turnip-darwin.py --target ios`
→ `build-adreno-metal-plugin.py --target ios --no-replay`。

### 3.1 云端已经验证到的位置
- Mesa 配置 + 全量编译**通过**，`libvulkan_freedreno.dylib` 已产出（Xcode 26.6 / iPhoneOS 26.5 SDK）
- 失败点在 Rust 桥：`metal2vulkan-runtime` 的 `copied.rs` 缺 `read_pages`（见 3.2 第 3 条）
- 之后未再继续（本轮被叫停，run 已 cancel）

### 3.2 三个必须知道的坑（脚本里都已处理）
1. **iPhoneOS 26 SDK 的 `<endian.h>` 陷阱**：头存在但不再定义 `__BYTE_ORDER`，Mesa 的探测走 glibc 分支
   → `UTIL_ARCH_LITTLE_ENDIAN and/or UTIL_ARCH_BIG_ENDIAN were unset`。
   修法：交叉编译参数追加 `-UHAVE_ENDIAN_H -D_DARWIN_C_SOURCE=1`（让 `u_endian.h` 走 `__APPLE__` 分支）。
2. **`MinimumOSVersion` 是条件表达式**：新版脚本写成
   `{'MinimumOSVersion': '27.0' if IOS27 else '17.0'}`，降 target 要替换的是 `else '17.0'`，
   不是 `'MinimumOSVersion': '17.0'`（旧写法已不存在）。
3. **metal2vulkan 没有 iOS 的 `read_pages`**：`crates/metal2vulkan-runtime/src/copied.rs` 里只有
   linux/macos/windows 三份实现，`target_os="ios"` 直接编译失败。
   修法：把 macOS 那份的 cfg 扩成 `any(target_os = "macos", target_os = "ios")`
   （`mach_vm_read_overwrite` iOS 也有）。**必须作为 translator 补丁序列的一员**（脚本写成
   `bridge/patches/0065-ios-read-pages.patch` 并追加进 `PATCHES` 列表），否则 bridge 的
   源码身份校验会以 "Translator source differs" 拒绝。

---

## 4. 本地 M1 Max 上怎么继续（建议顺序）

### 4.1 先拿 iOS 17.0 的 bundle 验证链路（最快）
```bash
git clone https://github.com/tselongson/UniXNU-picasso && cd UniXNU-picasso
bash scripts/build-metal-plugin-ci.sh "$HOME/metal-work" 17.0
# 产物：$HOME/metal-work/unixnu/build/adreno-metal-plugin/NativeAdrenoMetal.bundle
```
这一步只验证"能编出来、链接、4K 签名、CDHash"。17.0 与设备无关，但能证明工具链对不对。

### 4.2 再编 16.7（对齐本机 rootfs：iOS 16.7.16 / 20H392）
```bash
bash scripts/build-metal-plugin-ci.sh "$HOME/metal-work" 16.7
```
预期阻力（按可能性排序）：
- `-Werror` + 可用性警告：前端 391KB ObjC++ 若用了 iOS 17 才有的 API，会以 `-Werror` 报错。
  处理：逐个加 `@available`/`#if __IPHONE_OS_VERSION_MAX_ALLOWED` 守卫，或把 `-Werror` 去掉先跑通。
- 前端可能还有类似 `read_pages` 的"只考虑了 macOS"的分支（`#if TARGET_OS_OSX` 已有一批，需按 iOS 逐个看）。
- Rust 运行时其它 `cfg(target_os)` 分支（桥里已有一批 iOS 处理，未必全）。

### 4.3 设备侧打包（脚本已写好，本地在 `work/ci/`）
1. **addon 打包器**：`work/ci/prepare-metal-plugin-addon.py`
   ```bash
   python3 prepare-metal-plugin-addon.py --bundle <bundle 目录> out/picasso/gpu-addon.bin          # 安全模式（无 .publish）
   python3 prepare-metal-plugin-addon.py --bundle <bundle 目录> --publish out/picasso/gpu-addon.bin # 发布真设备
   ```
   产出的条目是 `addons/NativeAdrenoMetal.bundle/<相对路径>`（Mach-O 0755，其余 0644），
   `--publish` 追加 `addons/NativeAdrenoMetal.publish`。
   注意 `.publish` 必须是**挂在 `/System/Library/Extensions` 根下**的文件，插件用 `dladdr` 从自己路径
   推导 `<dir>/NativeAdrenoMetal.publish` 判断是否发布真设备。

2. **信任**：`work/ci/picasso_ui_trust.py`（本地改好，**未推送**，已通过原有 34 项自检）
   - `make_bundle(original, libraries)` 现在接受一个列表；CDHash **升序**放进**同一个** owner 模块
     （v2 模块：`<I16sI` 头 + N×24 字节条目；上游 `make-owner-trust-bundle.py` 就是这么做的，
     并有与 boot loader 同构的结构校验）。
   - 单库时输出与旧版**逐字节相同**，不影响现有 `libpicasso-sb.dylib` 的既有产物。
   - `work/ci/prepare-picasso-ui-trust.py` 同步改成 `--library` 可重复。
   - 需要信任的至少两个：bundle 可执行文件 + `libvulkan_freedreno.dylib`。
3. **payload**：核心 CLI 支持 `unixnu build payload --gpu-addon FILE`
   （`scripts/unixnu/cli.py` 的 `--gpu-addon`），boot args 里要带 `sheng-gpu-addon=1`
   （本地用 `work/patch-args3.py` 往现成镜像注入过该参数）。

### 4.4 上机顺序（务必按这个坡度，别一上来就 publish）
1. **无 `.publish`**：插件只会跑内置自测（`NativeAdrenoUserTest.h`）然后返回 nil。验证
   `GPU addon: extensions=5/0`、插件被 dlopen、`0x68xx` 进度标记（6800 进入 / 6801 找到 Turnip /
   6802 造出 device / 68f1 无 Turnip / 68f2 无 device）。这一步失败不会打死 backboardd。
2. **带 `.publish`**：backboardd 拿到真设备，CARenderServer 开始走 Metal。
   这一步有把 SpringBoard 打进重启循环的风险 —— 始终保留 `out/full-flash/boot-normal.img`，
   用 `fastboot boot` 试，失败就回正常镜像，**不要刷分区**。

---

## 5. 已知风险 / 未验证项（交接时必须带着）

| 项 | 状态 |
|---|---|
| iOS 16.7 能否用 iOS 17 目标的前端编译并运行 | **未验证**。`MTLAddDevice` 未被使用（走的是老式 `IOAcceleratorES → MetalPluginName → NSBundle → principal class → initWithAcceleratorPort:` 路径，iOS 16 上成立），但 API 可用性仍需实测 |
| Rust 运行时是否还有其它 macOS-only 分支 | 未知，`read_pages` 只是第一个撞上的 |
| 内核能否接受"一个 owner 模块里 3 个 CDHash" | 格式上支持（N 条 24 字节），但本仓的内核加载路径未实测；先单 CDHash 跑通再逐步加 |
| GPU 扫描输出（`GpuScanoutClient.h`）在 picasso 的 DPU 上是否成立 | 上游在 A740 上验证过；picasso 是 A620 级别，Turnip 支持，但扫描输出路径要实测 |
| 现有 `FBUC=0` 黑屏 / SpringBoard 重启循环 | 大概率是"没有 Metal 设备"的后果；插件装上后要重新观察，不要假定它自己会好 |

---

## 6. 设备/仓库常用参数

- 设备 serial `64993333`；正常镜像 `out/full-flash/boot-normal.img`；正常时 IP `192.168.101.25`
- 串口 `COM5`（XNU CDC 控制台，只认内建命令）：`python3 work/serial-cmd.py COM5 reboot-fastboot 4`
- 抓串口：`python3 work/boot-cap.py COM5 work/xxx.cap 420`
- 网络验证：`python3 work/check-net.py`（不要只用 ping）
- 提交 CI 前：`python3 work/preflight-c.py`
- 分支：工作分支 `kd439-pool`；`main` 上放着 workflow 与构建脚本（workflow_dispatch 只认 main）

## 7. 本轮留下的文件（已同步到仓库 `handoff/metal-plugin/`）

```
work/ci/build-metal-plugin-ci.sh        # 与 main 上一致
work/ci/build-metal-plugin.yml          # 与 main 上一致
work/ci/prepare-metal-plugin-addon.py   # 新，未推送
work/ci/picasso_ui_trust.py             # 改（多库支持），未推送
work/ci/prepare-picasso-ui-trust.py     # 改（--library 可重复），未推送
work/ci/check-picasso-ui-trust.py       # 原样，用作回归（34 项，全过）
work/refs/adrenometal/                  # 上游 Metal 前端全量源码（142 文件）
work/refs/build-scripts/                # 上游构建脚本快照
work/refs/unixnu.git                    # 上游超项目 bare clone（50 分支）
```

设备侧四件（打包器 / 信任模块 / 信任准备 / 0065 补丁）已推到 `kd439-pool` 的 `handoff/metal-plugin/`，
换机后直接 `git clone` 取，不用从这台机器搬。

