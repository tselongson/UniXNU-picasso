#!/usr/bin/env python3
"""Create a Data seed: an empty APFS container, packed for DarwinBoot.

The boot needs a writable Data volume and neither container in the userdata image has one -- the OS
window holds the System volume and SystemOS holds the cryptex, measured with work/read-apfs-uuid.py.
The payload has only a few MiB free, so the seed has to be the packed DBRDISK1 form rather than a
dense image.

This runs on macOS because only hdiutil can create an APFS container. It attaches a sparse image,
copies the raw device out, detaches, and packs the result with the packer DarwinBoot's own tests use
(drivers/DarwinBoot/Test/packed_ramdisk.py), so the format is the one the firmware already validates.

    make-data-seed.py --tree <upstream tree> --gib 3 --output data.dbr
"""
import argparse
import plistlib
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def run(*command, capture=False):
    result = subprocess.run(command, check=True, text=True,
                            stdout=subprocess.PIPE if capture else None,
                            stderr=subprocess.STDOUT if capture else None)
    return result.stdout if capture else ''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, required=True,
                        help='upstream tree, for drivers/DarwinBoot/Test/packed_ramdisk.py')
    parser.add_argument('--gib', type=int, default=3, help='container size in GiB')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()

    packer = args.tree / 'drivers/DarwinBoot/Test/packed_ramdisk.py'
    if not packer.is_file():
        raise SystemExit(f'packer not found: {packer}')

    with tempfile.TemporaryDirectory(prefix='data-seed-') as work:
        work = Path(work)
        sparse = work / 'seed.sparseimage'
        raw = work / 'seed.img'

        run('hdiutil', 'create', '-size', f'{args.gib}g', '-fs', 'APFS', '-volname', 'Data',
            '-type', 'SPARSE', '-o', str(sparse))

        attached = run('hdiutil', 'attach', str(sparse), '-nobrowse', '-plist', capture=True)
        match = re.search(r'<key>dev-entry</key>\s*<string>(/dev/disk\d+)</string>', attached)
        if not match:
            raise SystemExit(f'could not find the attached device in:\n{attached}')
        device = match.group(1)
        # The raw character device bypasses the block cache; rdisk is the conventional choice.
        raw_device = device.replace('/dev/disk', '/dev/rdisk')
        try:
            # diskutil is the macOS-native way to ask a device its size; blockdev is util-linux and
            # does not exist on the runner, which is how the first attempt failed.
            info = plistlib.loads(run('diskutil', 'info', '-plist', device, capture=True).encode())
            size = int(info['TotalSize'])
            # Copy in page-sized blocks, not whole MiB: the attached size is not a multiple of 1 MiB
            # (3011469312 bytes for a nominal 3 GiB sparse image), so a MiB count stopped 984 KiB
            # short and the length check below rejected it. Rounding down to a page keeps the image
            # page aligned, which the packer requires.
            size = size // 4096 * 4096
            # Reading a raw device needs root; the macOS runners allow passwordless sudo.
            run('sudo', 'dd', f'if={raw_device}', f'of={raw}', 'bs=4096',
                f'count={size // 4096}')
        finally:
            run('hdiutil', 'detach', device, '-force')

        if raw.stat().st_size != size:
            raise SystemExit(f'copied {raw.stat().st_size} bytes, expected {size}')
        if size % 4096:
            raise SystemExit(f'raw image is {size} bytes, not page aligned')

        sys.path.insert(0, str(packer.parent))
        import packed_ramdisk
        packed_ramdisk.pack(raw, args.output)

    print(f'{args.output}: {args.output.stat().st_size} bytes packed from {args.gib} GiB')


if __name__ == '__main__':
    main()
