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
import time
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
    parser.add_argument('--os-dmg', type=Path,
                        help='original OS image; its own /usr/share/zoneinfo is copied into the seed')
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
            # The volume UUID is also listed by the APFS container itself, which does not depend on
            # how the slices happen to be named. This is tried first because the slice lookup below
            # is what failed on a 2 GiB container: the disk answered, no slice did.
            def apfs_volume_uuid(target):
                try:
                    listing = run('diskutil', 'apfs', 'list', '-plist', target, capture=True)
                except subprocess.CalledProcessError:
                    return None
                try:
                    parsed = plistlib.loads(listing.encode())
                except Exception:
                    return None
                for container in parsed.get('Containers', []):
                    for volume in container.get('Volumes', []):
                        uuid = volume.get('APFSVolumeUUID')
                        if uuid:
                            return uuid
                return None

            volume_uuid = apfs_volume_uuid(device) or info.get('VolumeUUID')
            # The volume is not necessarily published by the time the disk answers, and at 1 GiB the
            # first query came back without one and the build stopped here. Poll instead of assuming
            # the layout is ready: the disk itself first, then its slices.
            for attempt in range(30):
                if volume_uuid:
                    break
                found = apfs_volume_uuid(device)
                if found:
                    volume_uuid = found
                    break
                for name in (device, device + 's1', device + 's2'):
                    try:
                        vinfo = plistlib.loads(run('diskutil', 'info', '-plist', name,
                                                   capture=True).encode())
                    except subprocess.CalledProcessError:
                        continue
                    if vinfo.get('VolumeUUID'):
                        volume_uuid = vinfo['VolumeUUID']
                        break
                if not volume_uuid:
                    time.sleep(1)
            if not volume_uuid:
                raise SystemExit('could not read the Data volume UUID from diskutil after 30 attempts')
            # Copy in page-sized blocks, not whole MiB: the attached size is not a multiple of 1 MiB
            # (3011469312 bytes for a nominal 3 GiB sparse image), so a MiB count stopped 984 KiB
            # short and the length check below rejected it. Rounding down to a page keeps the image
            # page aligned, which the packer requires.
            size = size // 4096 * 4096

            # The Data volume is mounted at /private/var, so its root is what /private/var contains.
            # A bare container has none of the directories launchd's boot tasks expect -- launchd
            # cannot open its own log, syslogd cannot bind /var/run/syslog, tzinit has no data -- so
            # the skeleton is laid down BEFORE the image is copied out.
            #
            # This ordering is the whole point. The copy used to happen first and the directories were
            # created afterwards, so every seed shipped an empty container regardless of what was
            # added here, and the boot reported the consequences one at a time.
            mount = re.search(r'<key>mount-point</key>\s*<string>([^<]+)</string>', attached)
            if not mount:
                raise SystemExit(f'could not find the mount point in:\n{attached}')
            volume_root = Path(mount.group(1))
            # The mounted volume root is not writable by the runner user, and these directories are
            # owned by root on a real device anyway.
            for relative in ('run', 'db', 'db/com.apple.directoryservices', 'tmp', 'logs', 'cores',
                             'mobile', 'mobile/Library', 'mobile/Media', 'root', 'containers',
                             'containers/Data', 'containers/Shared', 'preferences', 'keychains',
                             'lib', 'spool', 'folders', 'vm', 'hardware', 'audit', 'backups',
                             'wireless', 'networkd', 'protect', 'MSUData', 'MobileSoftwareUpdate',
                             'DeviceRegistry', 'Caches', 'log/com.apple.xpc.launchd',
                             'db/timezone', 'db/timezone/tz'):
                run('sudo', 'mkdir', '-p', str(volume_root / relative))
            # tzinit rebuilds /private/var/db/timezone from the system partition and fails here, so
            # the zoneinfo tree is copied in directly. It has to come from the OS image, not the build
            # host: the macOS tree is a different layout and version.
            if args.os_dmg:
                attached_os = run('hdiutil', 'attach', str(args.os_dmg), '-readonly', '-nobrowse',
                                  '-noautoopen', '-plist', capture=True)
                os_mount = re.search(r'<key>mount-point</key>\s*<string>([^<]+)</string>',
                                     attached_os)
                if not os_mount:
                    raise SystemExit('could not find the OS image mount point')
                try:
                    source_zoneinfo = Path(os_mount.group(1)) / 'usr/share/zoneinfo'
                    if not source_zoneinfo.exists():
                        raise SystemExit(f'{source_zoneinfo} is not in the OS image')
                    run('sudo', 'cp', '-R', str(source_zoneinfo),
                        str(volume_root / 'db/timezone/zoneinfo'))
                finally:
                    run('hdiutil', 'detach', os_mount.group(1), '-force')
            else:
                run('sudo', 'cp', '-R', '/usr/share/zoneinfo',
                    str(volume_root / 'db/timezone/zoneinfo'))
            run('sudo', 'ln', '-sfn', '/private/var/db/timezone/zoneinfo/UTC',
                str(volume_root / 'db/timezone/localtime'))
            print(f'data volume skeleton: {volume_root}')

            # Unmount so the filesystem is flushed and the device is quiescent. Reading the raw
            # device bypasses the page cache, so a copy taken while the volume is still mounted can
            # miss everything just written -- which is the failure this whole block exists to avoid.
            run('sync')
            for attempt in range(3):
                try:
                    run('diskutil', 'unmount', 'force', str(volume_root))
                    break
                except subprocess.CalledProcessError:
                    if attempt == 2:
                        raise
            run('sync')
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

        # Verify what was actually copied rather than what was intended. The seed is packed from a raw
        # copy, and a copy that silently omits the skeleton is indistinguishable from a working one
        # until the boot fails three tasks later -- which is exactly what happened for several builds.
        check = run('hdiutil', 'attach', str(raw), '-readonly', '-nobrowse', '-noautoopen',
                    '-plist', capture=True)
        check_mount = re.search(r'<key>mount-point</key>\s*<string>([^<]+)</string>', check)
        if not check_mount:
            raise SystemExit('the copied image does not mount; the seed would arrive unreadable')
        try:
            copied_root = Path(check_mount.group(1))
            missing = [name for name in ('run', 'log/com.apple.xpc.launchd', 'db/timezone/zoneinfo',
                                         'mobile', 'containers', 'tmp', 'preferences')
                       if not (copied_root / name).exists()]
            if missing:
                raise SystemExit(f'the copied seed is missing {missing}; '
                                 f'the copy did not carry the skeleton')
            print(f'seed verified: {copied_root} carries the skeleton')
        finally:
            run('hdiutil', 'detach', check_mount.group(1), '-force')


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
