# Metal 插件：设备侧接入材料（未集成，待本机验证）

这些文件是 2026-11-23 交接时留下的，**没有接进现有构建**，只作为下一步的起点。
配套说明见 `handoff/gpu-metal-plugin-handoff-2026-11-23.md`。

| 文件 | 状态 | 用途 |
|---|---|---|
| `prepare-metal-plugin-addon.py` | 新 | 把编译好的 `NativeAdrenoMetal.bundle` 打成 NGPUADD2 GPU addon；`--publish` 才写发布标记 |
| `picasso_ui_trust.py` | 改 | `make_bundle` 接受多个库，CDHash 升序写进同一个 owner 模块；单库输出与旧版逐字节相同 |
| `prepare-picasso-ui-trust.py` | 改 | `--library` 可重复，把 bundle 可执行文件与 `libvulkan_freedreno.dylib` 一起纳入信任 |
| `0065-ios-read-pages.patch` | 新 | metal2vulkan 的 `read_pages` 只实现了 linux/macos/windows，iOS 编译失败；把 macOS 的 cfg 扩成 `any(macos, ios)` |

## 使用顺序

```bash
# 1. 打包（先不加 --publish，插件只跑内置自测，返回 nil，不会影响 backboardd）
python3 handoff/metal-plugin/prepare-metal-plugin-addon.py \
  --bundle "$HOME/metal-work/unixnu/build/adreno-metal-plugin/NativeAdrenoMetal.bundle" \
  out/picasso/gpu-addon.bin

# 2. 信任：把两个 Mach-O 的 CDHash 一起放进 owner 模块
python3 handoff/metal-plugin/prepare-picasso-ui-trust.py \
  --original out/picasso/original-static-system.bundle \
  --library out/picasso/libpicasso-sb.dylib \
  --library "$HOME/metal-work/unixnu/build/adreno-metal-plugin/NativeAdrenoMetal.bundle/NativeAdrenoMetal" \
  --library "$HOME/metal-work/unixnu/build/adreno-metal-plugin/NativeAdrenoMetal.bundle/libvulkan_freedreno.dylib" \
  --output out/picasso/ui-trust.bundle

# 3. payload 带上 addon，boot args 带 sheng-gpu-addon=1
#    ./unixnu build payload ... --gpu-addon out/picasso/gpu-addon.bin
```

## 回归

`check-picasso-ui-trust.py`（仓库 scripts/ 里那份，未改动）覆盖 34 项结构检查；
改完信任模块后先跑它，单库路径必须仍然逐字节一致。
