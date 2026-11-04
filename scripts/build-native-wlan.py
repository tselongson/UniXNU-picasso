#!/usr/bin/env python3
"""Build the picasso WLAN bring-up probe (WCSS mapping, rails, SMMU geometry).

Compiles every source in drivers/NativeWlan with this tree's recorded XNU kext
flags and archives them as build/native-wlan/libNativeWlan.a, which
scripts/build-xnu-4k-baseline.sh force-loads so BootProgress.c can call
native_wlan_probe(). The probe is inert without sheng-wlan=<n>, does read-only
MMIO and never blocks the boot monitor.
"""
import argparse
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if not (ROOT / 'build/xnu').is_dir() and (ROOT / 'out/picasso/tree/build/xnu').is_dir():
    ROOT = ROOT / 'out/picasso/tree'

DRIVER = ROOT / 'drivers/NativeWlan'
RECORD = 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
OUT = ROOT / 'build/native-wlan'
OUT.mkdir(parents=True, exist_ok=True)


def run(command, cwd=None):
    print('+', ' '.join(map(str, command)))
    subprocess.run([str(value) for value in command], check=True, timeout=900, cwd=cwd)


def flags(record_path, source, obj, language):
    """Recorded IOPlatformExpert compile line, retargeted to our source."""
    info = json.loads(Path(record_path).read_text().strip().rstrip(','))
    command, index, arguments = [], 0, info['arguments']
    while index < len(arguments):
        value = arguments[index]
        if value == info['file']:
            command.append(str(source))
        elif value == '-o':
            command.extend(['-o', str(obj)])
            index += 1
        elif value in ('-MF', '-MT', '-MQ', '-MJ', '--serialize-diagnostics'):
            index += 1
        elif value in ('-MD', '-MMD', '-MP', '--driver-mode=g++', '-fno-odr-hash-protocols',
                       '-fno-c++-static-destructors'):
            pass
        elif value == '-xc++':
            command.append('-xc++' if language == 'c++' else '-xc')
        elif value == '-std=gnu++2b':
            command.append('-std=gnu++2b' if language == 'c++' else '-std=gnu11')
        elif value == '-Xclang':
            index += 1
        elif value.startswith('-W'):
            pass
        else:
            command.append(value)
        index += 1
    command.extend(['-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-unknown-warning-option',
                    '-Wno-missing-designated-field-initializers',
                    '-I', str(ROOT / '.cache/xnu-work/osfmk'), '-I', str(source.parent),
                    '-I', str(DRIVER)])
    return command, info['directory']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--record', default=str(ROOT / RECORD))
    args = parser.parse_args()

    sources = sorted(DRIVER.glob('*.cpp')) + sorted(DRIVER.glob('*.c'))
    if not sources:
        raise SystemExit(f'no sources under {DRIVER}')
    objects = []
    for source in sources:
        language = 'c' if source.suffix == '.c' else 'c++'
        obj = OUT / (source.stem + '.o')
        command, cwd = flags(args.record, source, obj, language)
        run(command, cwd=cwd)
        objects.append(obj)

    archive = OUT / 'libNativeWlan.a'
    archive.unlink(missing_ok=True)
    run(['xcrun', 'ar', 'rcs', archive] + objects)
    print('native-wlan:', archive)


if __name__ == '__main__':
    main()
