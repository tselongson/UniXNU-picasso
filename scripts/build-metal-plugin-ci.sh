#!/usr/bin/env bash
# Build the Adreno Metal plugin bundle for the picasso port on a macOS runner.
#
# The userspace GPU stack is a component of the UniXNU core tree, not of this
# device repository: drivers/AdrenoMetal (Metal frontend -> metal2vulkan ->
# Turnip -> NativeAdreno ABI v1), drivers/Adreno740 (the ABI header the Turnip
# darwin backend compiles against), kits/AdrenoPM4 and the pinned Mesa source
# with patches/mesa/turnip-darwin-kmd.patch. This script resolves that tree at
# a pinned revision, adapts the deployment target when asked, and runs the
# upstream build entry points unchanged.
#
#   scripts/build-metal-plugin-ci.sh WORKDIR [MINOS]
#
# WORKDIR receives `unixnu/` (the resolved core tree). MINOS defaults to the
# upstream iOS 17.0; pass 16.7 for an iOS 16.7.16 rootfs such as this port's.
# Output: WORKDIR/unixnu/build/adreno-metal-plugin/NativeAdrenoMetal.bundle
set -euo pipefail

WORK="${1:?usage: build-metal-plugin-ci.sh WORKDIR [MINOS]}"
MINOS="${2:-17.0}"

CORE_REPO="https://github.com/UniXNU/UniXNU.git"
# Pinned core revision: 2026-11-19 main, the revision whose submodules match
# the AdrenoMetal 1.98-era bridge. Changing it means re-qualifying every pin.
CORE_COMMIT="8542980d02b4d2cc5c32ee3542de1760b2562aac"
SUBMODULES="drivers/AdrenoMetal drivers/Adreno740 kits/AdrenoPM4 kits/AirNir kits/AdrenoFirmware"
ROOT="$WORK/unixnu"

step() { printf '\n=== %s  [%s] ===\n' "$1" "$(date -u +%H:%M:%S)"; }

step "host"
sw_vers
xcodebuild -version || true
xcrun --sdk iphoneos --show-sdk-path
sysctl -n hw.ncpu

step "core $CORE_COMMIT"
rm -rf "$ROOT"
mkdir -p "$WORK"
git init -q "$ROOT"
git -C "$ROOT" remote add origin "$CORE_REPO"
git -C "$ROOT" fetch -q --depth 1 origin "$CORE_COMMIT"
git -C "$ROOT" checkout -q FETCH_HEAD
echo "checked out $(git -C "$ROOT" rev-parse HEAD)"

step "submodules"
for m in $SUBMODULES; do
  echo "-- $m"
  git -C "$ROOT" submodule update --init "$m"
done
git -C "$ROOT" submodule status

step "toolchain"
export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_CLEANUP=1
need() { brew list --versions "$1" >/dev/null 2>&1 || brew install "$1"; }
for pkg in ninja glslang shaderc spirv-tools llvm bison flex pkg-config; do
  need "$pkg"
done
export PATH="/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/flex/bin:/opt/homebrew/opt/llvm/bin:$PATH"
command -v glslangValidator glslc spirv-val llvm-as bison flex ninja

if ! rustup toolchain list 2>/dev/null | grep -q '^1\.98\.0'; then
  rustup toolchain install 1.98.0 --profile minimal || true
fi
if rustup toolchain list 2>/dev/null | grep -q '^1\.98\.0'; then
  rustup target add aarch64-apple-ios --toolchain 1.98.0
else
  echo "1.98.0 unavailable; falling back to the installed stable toolchain"
  rustup target add aarch64-apple-ios
  for f in scripts/build-adreno-metal-bridge.py; do
    sed -i '' "s/'+1\.98\.0'/'+stable'/" "$ROOT/$f"
  done
fi
cargo --version

step "mesa venv"
python3 -m venv "$ROOT/.cache/mesa-venv"
"$ROOT/.cache/mesa-venv/bin/pip" install -q --upgrade pip
"$ROOT/.cache/mesa-venv/bin/pip" install -q -r "$ROOT/scripts/requirements-adreno-compiler.txt"
"$ROOT/.cache/mesa-venv/bin/meson" --version

step "pinned sources"
MESA_COMMIT=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['mesa']['commit'])" "$ROOT/scripts/data/adreno-compiler-inputs.json")
TRANSLATOR_COMMIT=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['translator']['commit'])" "$ROOT/scripts/data/gpu-metal-vulkan-inputs.json")
echo "mesa=$MESA_COMMIT translator=$TRANSLATOR_COMMIT"
mkdir -p "$ROOT/.cache/mesa-work" "$ROOT/.cache/gpu-reference/metal2vulkan"
git -C "$ROOT/.cache/mesa-work" init -q
git -C "$ROOT/.cache/mesa-work" remote add origin https://gitlab.freedesktop.org/mesa/mesa.git
git -C "$ROOT/.cache/mesa-work" fetch -q --depth 1 origin "$MESA_COMMIT"
git -C "$ROOT/.cache/mesa-work" checkout -q FETCH_HEAD
git -C "$ROOT/.cache/gpu-reference/metal2vulkan" init -q
git -C "$ROOT/.cache/gpu-reference/metal2vulkan" remote add origin https://github.com/steelbrain/metal2vulkan.git
git -C "$ROOT/.cache/gpu-reference/metal2vulkan" fetch -q --depth 1 origin "$TRANSLATOR_COMMIT"
git -C "$ROOT/.cache/gpu-reference/metal2vulkan" checkout -q FETCH_HEAD
git -C "$ROOT/.cache/gpu-reference/metal2vulkan" rev-parse HEAD

step "apply recorded mesa patches"
python3 - "$ROOT" <<'PY'
import json, os, subprocess, sys
root = sys.argv[1]
pins = json.load(open(os.path.join(root, 'scripts/data/adreno-compiler-inputs.json')))
source = os.path.join(root, '.cache/mesa-work')
for patch in pins['patches']:
    subprocess.run(['git', '-C', source, 'apply', '--whitespace=nowarn', os.path.join(root, patch)], check=True)
    print('applied', patch)
PY

step "deployment target $MINOS"
python3 - "$ROOT" "$MINOS" <<'PY'
import pathlib, sys
root, minos = pathlib.Path(sys.argv[1]), sys.argv[2]

def edit(name, pairs):
    path = root / name
    text = path.read_text()
    for old, new in pairs:
        if old not in text:
            raise SystemExit('%s: %r not found; the pin changed, re-check the edit' % (name, old))
        text = text.replace(old, new)
    path.write_text(text)
    print('patched', name)

# The iPhoneOS 26 SDK ships <endian.h> without the glibc __BYTE_ORDER macros,
# so Mesa's header probe takes the glibc branch and leaves UTIL_ARCH_* unset;
# undefining the probe sends u_endian.h down its __APPLE__ branch instead. The
# C11 build also needs _DARWIN_C_SOURCE for dlfcn/POSIX declarations.
edit('scripts/build-turnip-darwin.py',
     [("'-isysroot', sdk, '-fstack-protector-strong']",
       "'-isysroot', sdk, '-fstack-protector-strong', '-UHAVE_ENDIAN_H', '-D_DARWIN_C_SOURCE=1']")])
if minos != '17.0':
    edit('scripts/build-turnip-darwin.py', [("'arm64-apple-ios17.0'", "'arm64-apple-ios%s'" % minos)])
    # The bundle plist picks the minimum OS with a conditional, so the default
    # branch of that expression is the one to move.
    edit('scripts/build-adreno-metal-plugin.py',
         [("'arm64-apple-ios17.0'", "'arm64-apple-ios%s'" % minos),
          ("else '17.0'", "else '%s'" % minos)])
    edit('scripts/build-adreno-metal-bridge.py', [("else '17.0'", "else '%s'" % minos)])
else:
    print('keeping the upstream 17.0 target')
PY
grep -n "apple-ios\|UHAVE_ENDIAN_H" "$ROOT/scripts/build-turnip-darwin.py" "$ROOT/scripts/build-adreno-metal-plugin.py" | head

step "build Turnip (ios)"
python3 "$ROOT/scripts/build-turnip-darwin.py" --target ios

step "iOS read_pages patch"
# metal2vulkan's copied-buffer first read has a kernel-side reader for Linux,
# macOS and Windows only; an iOS build finds no `read_pages` at all. The macOS
# one is mach_vm_read_overwrite of the process's own task, which iOS has too, so
# the fix is extending that cfg. It enters as the next recorded translator patch
# so the bridge's source-identity check stays exact.
cat > "$ROOT/drivers/AdrenoMetal/bridge/patches/0065-ios-read-pages.patch" <<'PATCH'
diff --git a/crates/metal2vulkan-runtime/src/copied.rs b/crates/metal2vulkan-runtime/src/copied.rs
--- a/crates/metal2vulkan-runtime/src/copied.rs
+++ b/crates/metal2vulkan-runtime/src/copied.rs
@@ -2640,8 +2640,8 @@ pub(crate) fn read_pages(pages: NonNull<u8>, into: &mut [u8]) -> bool {
     true
 }
 
-/// [`read_pages`] on macOS.
-#[cfg(target_os = "macos")]
+/// [`read_pages`] on macOS and iOS.
+#[cfg(any(target_os = "macos", target_os = "ios"))]
 pub(crate) fn read_pages(pages: NonNull<u8>, into: &mut [u8]) -> bool {
     unsafe extern "C" {
         /// `<mach/mach_init.h>`: the port of this process's task.
PATCH
python3 - "$ROOT" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1]) / 'scripts/build-adreno-metal-bridge.py'
text = path.read_text()
old = "    '0064-copies-leave-unreachable-pages-alone.patch']]"
new = "    '0064-copies-leave-unreachable-pages-alone.patch',\n    '0065-ios-read-pages.patch']]"
if old not in text:
    raise SystemExit('bridge patch list changed; re-check where 0065 goes')
path.write_text(text.replace(old, new))
print('bridge patch list now carries 0065')
PY

step "build plugin bundle (ios)"
python3 "$ROOT/scripts/build-adreno-metal-plugin.py" --target ios --no-replay

step "result"
OUT="$ROOT/build/adreno-metal-plugin"
ls -la "$OUT"
ls -la "$OUT/NativeAdrenoMetal.bundle"
cat "$OUT/trust.json"
shasum -a 256 "$OUT/NativeAdrenoMetal.bundle/NativeAdrenoMetal" "$OUT/NativeAdrenoMetal.bundle/libvulkan_freedreno.dylib"
