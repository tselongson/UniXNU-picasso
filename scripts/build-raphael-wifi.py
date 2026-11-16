#!/usr/bin/env python3
"""Compile raphael's kernel-side WiFi glue into an archive for force-load.

Route 2 (complete replacement with raphael's code). Compiles the 9 WiFi core
hakios.*.c files with the XNU compile record (so they see the real kernel
headers), plus our include dir for the raphael sys/hakios.*.h headers.

The 9 files are self-contained: all 56 hakios_* functions they need are defined
among themselves (verified: hakios.patch adds only 6 non-WiFi hakios_* funcs).
Platform constants are patched separately (see raphael-port patch)."""
import json
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def compile_file(source: Path, destination: Path, record: dict, extra_flags: list):
    destination.parent.mkdir(parents=True, exist_ok=True)
    command = []
    arguments = record['arguments']
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value == '-xc++':
            command.append('-xc')
        elif value == '-fapple-kext':
            pass
        elif value == record['file']:
            command.append(str(source))
        elif value == '-o':
            command.extend(['-o', str(destination)])
            index += 1
        elif value == '-Xclang' and index + 1 < len(arguments) and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W'):
            command.append(value)
        index += 1
    command.append('-std=gnu11')
    command.extend(['-Wno-everything'])  # raphael code is warning-noisy on XNU 16.6
    command.extend(['-I', str(ROOT / '.cache/xnu-work/osfmk')])
    command.extend(extra_flags)
    result = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(f'Failed to compile {source}:\nSTDOUT:\n{result.stdout}\nSTDERR:\n{result.stderr}\nCMD: {" ".join(map(str, command))}')
    data = destination.read_bytes()
    if struct.unpack_from('<4I', data) != (0xfeedfacf, 0x100000c, 0, 1):
        raise SystemExit(f'Expected a plain ARM64 Mach-O object for {destination}')


def main():
    recipe = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    record = json.loads(recipe.read_text().strip().rstrip(','))
    # This script is copied into the prepared tree (out/picasso/tree/scripts/),
    # but wifi-ath10k/ source lives in the device checkout, not in the tree.
    # Walk up from __file__ to find it (same pattern as build-arm-driver.py).
    src = None
    for parent in (ROOT, *Path(__file__).resolve().parents):
        candidate = parent / 'wifi-ath10k' / 'kernel-full'
        if candidate.is_dir():
            src = candidate
            break
    if src is None:
        raise SystemExit('cannot find wifi-ath10k/kernel-full source')
    out = ROOT / 'build/raphael-wifi'
    out.mkdir(parents=True, exist_ok=True)

    # 8 WiFi core files from raphael (hakios.input.c excluded: it also owns
    # touchscreen init which collides with picasso's Novatek driver). Its two
    # globals used by the WiFi stack (hakios_is_raphael, hakios_tl) are supplied
    # by our picasso-wifi-glue.c below.
    names = ['hakios.wifi', 'hakios.mpss', 'hakios.qmi', 'hakios.net',
             'hakios.sta', 'hakios.apple80211', 'hakios.wlfw', 'hakios.lx',
             'picasso-wifi-glue']
    inc = ['-I', str(src)]  # resolves <sys/hakios.*.h> via kernel-full/bsd/sys/
    objects = []
    for name in names:
        s = src / 'bsd/dev' / (name + '.c')
        o = out / (name.replace('.', '_') + '.o')
        compile_file(s, o, record, inc)
        objects.append(o)
        print(f'compiled {name}.c')

    archive = out / 'libRaphaelWifi.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
    print(f'RaphaelWifi: archive built ({len(objects)} objects): {archive}')


if __name__ == '__main__':
    main()
