#!/usr/bin/env python3
"""Bind IOBlockStorageDevice for the picasso UFS driver.

The driver publishes the root device by subclassing IOBlockStorageDevice, but this kernel's
nlist points those symbols into an unpopulated part of __PLK_TEXT_EXEC, so the call lands in
zeroed memory ("Undefined kernel instruction, instr=0").

Earlier attempts searched the kernel image for the class dispatch table and matched its entries
against exported addresses. That search is fragile: it depends on the exact table layout and on
every known method lining up index for index, and it reported zero candidates on this image.
Binding the four symbols the driver actually references is enough -- the constructor installs
the vtable and every later call is virtual through it -- and it is deterministic.

Addresses come from the nlist and are offset by the kernel slide, because np.exports reports
pre-slide values while the image stores post-slide pointers.
"""
import json
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/NativePrelink'))
import native_prelink as np  # noqa: E402

OUT = ROOT / 'build/picasso-ufs-bindings'
OUT.mkdir(parents=True, exist_ok=True)
CANDIDATE = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT.parent / 'kernel/kernel.macho'

if not CANDIDATE.is_file():
    # The archive step runs before the kernel link, so the prelinked candidate only exists from the
    # second pass on. Emit an empty archive here; the workflow builds twice so the second pass links
    # the real bindings.
    empty = OUT / 'libUfsBindings.a'
    empty.unlink(missing_ok=True)
    stub_source = OUT / 'empty.c'
    stub_object = OUT / 'empty.o'
    stub_source.write_text('')
    subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-c', str(stub_source),
                    '-o', str(stub_object)], check=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(empty), str(stub_object)], check=True)
    print(f'picasso UFS bindings: no prelinked candidate at {CANDIDATE}, emitted an empty archive')
    raise SystemExit(0)

kernel = CANDIDATE.read_bytes()
plan = json.loads(CANDIDATE.with_suffix('.json').read_text())
native = np.macho(kernel)
slide = int(plan['slide'])
exports = np.exports(kernel, native)
by_name = {name: (va + slide) & 0xffffffffffffffff for va, names in exports.items() for name in names}
print(f'picasso UFS bindings: slide {slide:#x}, {len(by_name)} exports')

# Stock (pre-prelink) IOStorageFamily addresses, the same table build-arm-driver.py binds against.
# Deriving them from the export table does not work once the driver archive binds these names as
# absolutes: the linked kernel then reports them already slid, and sliding again lands on a mapped
# page that looks plausible but is the wrong one -- the vtable came back with sixteen zero entries
# and the metaclass came back 16 MiB low. Stock plus slide is unambiguous.
STOCK = {
    '__ZN20IOBlockStorageDeviceC2EPK11OSMetaClass': 0xfffffff00596ed98,
    '__ZN20IOBlockStorageDeviceD2Ev': 0xfffffff00596edb8,
    '__ZTV20IOBlockStorageDevice': 0xfffffff006cb44c0,
    '__ZN20IOBlockStorageDevice10gMetaClassE': 0xfffffff007add1d0,
}
required = list(STOCK)
for name, address in STOCK.items():
    by_name[name] = (address + slide) & 0xffffffffffffffff
for name in required:
    print(f'  {name} -> {by_name[name]:#x}')


def read(va, length):
    at = np.backed_offset(native['segments'], va, length, len(kernel))
    return kernel[at:at + length]


# Shape checks: a kext constructor stores its vtable, and a vtable starts with two zero words
# (offset-to-top and RTTI) followed by pointers into the module's own code.
constructor = by_name['__ZN20IOBlockStorageDeviceC2EPK11OSMetaClass']
words = struct.unpack('<8I', read(constructor, 32))
if words[:2] != (0xa9bf7bfd, 0x910003fd) or words[7] != 0xd65f03c0:
    raise SystemExit(f'constructor at {constructor:#x} is not a kext constructor: {[hex(w) for w in words]}')
vtable = by_name['__ZTV20IOBlockStorageDevice']
if read(vtable, 16) != bytes(16):
    raise SystemExit(f'vtable at {vtable:#x} does not start with two zero words')
entries = struct.unpack_from('<16Q', read(vtable, 128), 0)
resolved = sum(1 for entry in entries if entry in exports)
print(f'  constructor shape ok, vtable {vtable:#x} first entries {[hex(e) for e in entries[2:6]]}')
if resolved < 8:
    raise SystemExit(f'vtable entries do not resolve ({resolved}/16)')
metaclass = by_name['__ZN20IOBlockStorageDevice10gMetaClassE']
if not (metaclass & 0xffffffff00000000):
    raise SystemExit(f'metaclass address looks wrong: {metaclass:#x}')

bindings = {name: by_name[name] for name in required}
for name, value in by_name.items():
    if name.startswith('__ZN20IOBlockStorageDevice') or name.startswith('__ZNK20IOBlockStorageDevice'):
        bindings.setdefault(name, value)
print(f'picasso UFS bindings: {len(bindings)} symbols')

assembly = OUT / 'original-bindings.S'
assembly.write_text(''.join(f'.globl {name}\n.set {name}, {value:#x}\n' for name, value in sorted(bindings.items())))
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-Werror', '-Wno-incompatible-sysroot',
                '-c', str(assembly), '-o', str(OUT / 'original-bindings.o')], check=True)
archive = OUT / 'libUfsBindings.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(OUT / 'original-bindings.o')], check=True)
(OUT / 'bindings.json').write_text(json.dumps({k: hex(v) for k, v in sorted(bindings.items())}, indent=2) + '\n')
print(f'picasso UFS bindings: {len(bindings)} symbols -> {archive}')
