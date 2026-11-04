#!/usr/bin/env python3
"""Build the picasso power/volume key probe.

Compiles drivers/NativeButtons/ButtonProbe.cpp with this tree's recorded XNU kext flags and archives
it as build/buttons/libNativeButtons.a, which scripts/build-xnu-4k-baseline.sh force-loads so its
native_buttons_probe() constructor is reachable from the boot-progress monitor.

The probe is inert without the native-buttons=1 boot argument.
"""
import argparse
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if not (ROOT / 'build/xnu').is_dir() and (ROOT / 'out/picasso/tree/build/xnu').is_dir():
    ROOT = ROOT / 'out/picasso/tree'

DRIVER = ROOT / 'drivers/NativeButtons'
RECORD = 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
OUT = ROOT / 'build/buttons'
OUT.mkdir(parents=True, exist_ok=True)


def run(command, cwd=None):
    print('+', ' '.join(map(str, command)))
    subprocess.run([str(value) for value in command], check=True, timeout=900, cwd=cwd)


def flags(record_path, source, obj, language):
    """Recorded IOPlatformExpert compile line, retargeted to our source.

    C++: keep the recorded language. C: swap -xc++/-std=gnu++2b for C and drop the C++-only vendor
    features (the -Xclang pairs) instead of enumerating them.
    """
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

    source = DRIVER / 'ButtonProbe.cpp'
    obj = OUT / 'ButtonProbe.o'
    command, cwd = flags(args.record, source, obj, 'c++')
    run(command, cwd=cwd)

    archive = OUT / 'libNativeButtons.a'
    archive.unlink(missing_ok=True)
    run(['xcrun', 'ar', 'rcs', archive, obj])
    print('buttons:', archive)


if __name__ == '__main__':
    main()
