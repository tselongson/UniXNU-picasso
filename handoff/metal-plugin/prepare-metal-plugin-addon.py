#!/usr/bin/env python3
"""Pack the built Adreno Metal bundle into an NGPUADD2 GPU addon.

  prepare-metal-plugin-addon.py --bundle DIR [--publish] OUTPUT

The kernel's addon installer (bsd/kern/kern_exec.c, only with boot-arg
sheng-gpu-addon=1) writes every entry below /private/var/ngpu and mounts:

  /private/var/ngpu/addons    over /System/Library/Extensions
  /private/var/ngpu/daemons   over /System/Library/LaunchDaemons
  /private/var/ngpu/wallpaper over the default wallpaper collection

so the entries here are `addons/NativeAdrenoMetal.bundle/...` plus, with
--publish, `addons/NativeAdrenoMetal.publish`. The marker is what makes the
plugin return the native Metal device instead of running its in-process test
and returning nil, so it is only packed when the caller asks for it.

Executables (any entry whose first four bytes are a Mach-O magic) are stored
0755, everything else 0644, like scripts/add-gpu-addon-file.py.
"""
import argparse
import plistlib
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b'NGPUADD2'
MACHO_MAGICS = (b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe')
BUNDLE = 'NativeAdrenoMetal.bundle'
PREFIX = 'addons'


def build(files):
    body = b''
    for name, mode, data in files:
        encoded = name.encode() + b'\0'
        if len(encoded) > 160 or name.startswith('/') or '..' in name.split('/'):
            raise SystemExit('bad addon path ' + name)
        packed = zlib.compress(data, 9)
        name_pad = encoded + bytes(-len(encoded) % 8)
        data_pad = packed + bytes(-len(packed) % 8)
        body += struct.pack('<IIQQ', len(encoded), mode, len(data), len(packed)) + name_pad + data_pad
    return MAGIC + struct.pack('<IIQ', len(files), 0, 24 + len(body)) + body


def macho(data, what):
    if len(data) < 32 or data[:4] not in MACHO_MAGICS:
        raise SystemExit('%s is not a thin Mach-O' % what)
    cpu = struct.unpack_from('<I', data, 4)[0]
    if cpu != 0x100000c:
        raise SystemExit('%s is not arm64 (cputype 0x%x)' % (what, cpu))


def collect(bundle):
    if not bundle.is_dir():
        raise SystemExit('no bundle at ' + str(bundle))
    plist = bundle / 'Info.plist'
    if not plist.is_file():
        raise SystemExit('bundle has no Info.plist')
    info = plistlib.loads(plist.read_bytes())
    if info.get('NSPrincipalClass') != 'NativeAdrenoMetalDevice':
        raise SystemExit('Info.plist principal class is %r, expected NativeAdrenoMetalDevice'
                         % info.get('NSPrincipalClass'))
    executable = info.get('CFBundleExecutable')
    if not executable or not (bundle / executable).is_file():
        raise SystemExit('bundle executable %r missing' % executable)
    files = []
    for path in sorted(bundle.rglob('*')):
        if path.is_symlink():
            raise SystemExit('bundle entry is a symlink: ' + str(path))
        if not path.is_file():
            continue
        data = path.read_bytes()
        relative = path.relative_to(bundle).as_posix()
        mode = 0o755 if data[:4] in MACHO_MAGICS else 0o644
        files.append(('%s/%s/%s' % (PREFIX, BUNDLE, relative), mode, data))
    macho((bundle / executable).read_bytes(), executable)
    turnip = bundle / 'libvulkan_freedreno.dylib'
    if turnip.is_file():
        macho(turnip.read_bytes(), turnip.name)
    else:
        print('warning: no libvulkan_freedreno.dylib in the bundle', file=sys.stderr)
    return files, executable


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--publish', action='store_true', help='add the marker that publishes a real device')
    parser.add_argument('--check-only', action='store_true', help='validate the bundle and print its entries')
    parser.add_argument('output', type=Path, nargs='?')
    args = parser.parse_args()
    files, executable = collect(args.bundle)
    if args.publish:
        files.append(('%s/NativeAdrenoMetal.publish' % PREFIX, 0o644, b''))
    if args.check_only:
        for name, mode, data in files:
            print('%04o %9d %s' % (mode, len(data), name))
        return
    if args.output is None:
        raise SystemExit('an output path is required unless --check-only is given')
    blob = build(files)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    print('PASS: %d entries, %d bytes -> %s (executable %s%s)'
          % (len(files), len(blob), args.output, executable,
             ', published' if args.publish else ', test mode (no marker)'))


if __name__ == '__main__':
    main()
