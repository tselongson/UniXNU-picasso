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
    parser.add_argument('--defconfig', type=Path,
                        help='picasso_defconfig to receive the generated volume UUID')
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
            # The kernel resolves the Data volume by APFS UUID (NativeRamBlock's selectVolumeSeed
            # matches IOMedia on that property), so the UUID this container gets has to reach the
            # build. hdiutil picks it at random, so it is read here rather than assumed -- an earlier
            # build had the all-zero placeholder in the config and the mount failed with ENOENT.
            volume_uuid = info.get('VolumeUUID')
            if not volume_uuid:
                for suffix in ('s1', 's2'):
                    try:
                        vinfo = plistlib.loads(run('diskutil', 'info', '-plist',
                                                   device + suffix, capture=True).encode())
                    except subprocess.CalledProcessError:
                        continue
                    if vinfo.get('VolumeUUID'):
                        volume_uuid = vinfo['VolumeUUID']
                        break
            if not volume_uuid:
                raise SystemExit('could not read the Data volume UUID from diskutil')
            # Copy in page-sized blocks, not whole MiB: the attached size is not a multiple of 1 MiB
            # (3011469312 bytes for a nominal 3 GiB sparse image), so a MiB count stopped 984 KiB
            # short and the length check below rejected it. Rounding down to a page keeps the image
            # page aligned, which the packer requires.
            size = size // 4096 * 4096
            # Reading a raw device needs root; the macOS runners allow passwordless sudo.
            run('sudo', 'dd', f'if={raw_device}', f'of={raw}', 'bs=4096',
                f'count={size // 4096}')
        finally:
            # The image has already been read by this point, so a busy detach is not a reason to fail
            # the build -- the runner is ephemeral. Retry once, then warn.
            for attempt in range(2):
                try:
                    run('hdiutil', 'detach', device, '-force')
                    break
                except subprocess.CalledProcessError:
                    if attempt:
                        print(f'warning: could not detach {device}; continuing', file=sys.stderr)

        # The Data volume is mounted at /private/var, so its root is what /private/var contains. A bare
        # container has none of the directories launchd's boot tasks expect -- syslogd cannot bind
        # /var/run/syslog, dirs_cleaner and usermanagerd have nothing to work with -- so the skeleton
        # is laid down here, before the image is packed.
        # The mount point comes from the attach output; VolumeName is empty for this container and
        # joining an empty name gave /Volumes itself.
        mount = re.search(r'<key>mount-point</key>\s*<string>([^<]+)</string>', attached)
        if not mount:
            raise SystemExit(f'could not find the mount point in://n{attached}')
        volume_root = Path(mount.group(1))
        for relative in ('run', 'db', 'db/com.apple.directoryservices', 'tmp', 'logs', 'cores',
                         'mobile', 'mobile/Library', 'mobile/Media', 'root', 'containers',
                         'containers/Data', 'containers/Shared', 'preferences', 'keychains',
                         'lib', 'spool', 'folders', 'vm', 'hardware', 'audit', 'backups',
                         'wireless', 'networkd', 'protect', 'MSUData', 'MobileSoftwareUpdate',
                         'DeviceRegistry', 'Caches'):
            (volume_root / relative).mkdir(parents=True, exist_ok=True)
        print(f'data volume skeleton: {volume_root}')

        if args.defconfig:
            config = Path(args.defconfig)
            text = config.read_text(encoding='utf-8')
            replaced, count = re.subn(r'^CONFIG_XNU_OWNER_DATA_VOLUME_UUID=.*$',
                                      f'CONFIG_XNU_OWNER_DATA_VOLUME_UUID="{volume_uuid}"',
                                      text, flags=re.MULTILINE)
            if count != 1:
                raise SystemExit(f'{config}: expected one CONFIG_XNU_OWNER_DATA_VOLUME_UUID line, '
                                 f'found {count}')
            config.write_text(replaced, encoding='utf-8')
        print(f'data volume uuid: {volume_uuid}')

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
