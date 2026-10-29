#!/usr/bin/env python3
"""Unwrap an IMG4-wrapped trust cache into the bare version-2 module.

build-original-system-trust.py wants .cache/firmware/target-volumes/static-trustcache.bin to be a
version-2 trust module already -- it checks module[0] == 2 and length == 24 + count * 24 -- while the
IPSW ships the same cache inside an IM4P/IMG4 envelope. extract-target-volume.py returns that
envelope, so the two do not line up without this step.

The envelope is DER: SEQUENCE { 'IM4P', 'iP…', 'iP…', OCTET STRING <module> }. Only the payload of
the last field is written; the envelope and its signatures are discarded, which is what the consumer
expects and what its sha256 pin was recorded against.

    python3 unwrap-img4-payload.py <wrapped> <output>
"""
import sys
from pathlib import Path


def der(data, at):
    if at + 2 > len(data):
        raise ValueError('truncated DER header')
    tag, size = data[at], data[at + 1]
    at += 2
    if size & 0x80:
        count = size & 0x7F
        if not 0 < count <= 4 or at + count > len(data):
            raise ValueError('invalid DER length')
        size = int.from_bytes(data[at:at + count], 'big')
        at += count
    if at + size > len(data):
        raise ValueError('DER field runs past the input')
    return tag, data[at:at + size], at + size


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    wrapped = Path(sys.argv[1]).read_bytes()
    tag, sequence, end = der(wrapped, 0)
    if tag != 0x30 or end != len(wrapped):
        raise SystemExit(f'not a single DER SEQUENCE: tag {tag:#x}, consumed {end} of {len(wrapped)}')

    fields, cursor = [], 0
    while cursor < len(sequence):
        kind, body, cursor = der(sequence, cursor)
        fields.append((kind, body))

    kinds = [kind for kind, _ in fields]
    if kinds != [0x16, 0x16, 0x16, 0x04]:
        raise SystemExit(f'unexpected IMG4 field layout: {[hex(k) for k in kinds]}')
    if fields[0][1] != b'IM4P':
        raise SystemExit(f'not an IM4P envelope: {fields[0][1][:8]!r}')

    module = fields[3][1]
    version, count = int.from_bytes(module[0:4], 'little'), int.from_bytes(module[20:24], 'little')
    expected = 24 + count * 24
    if version != 2:
        raise SystemExit(f'payload is not a version-2 module: version {version}')
    if len(module) != expected:
        raise SystemExit(f'payload length {len(module)} does not match 24 + {count} * 24 = {expected}')

    Path(sys.argv[2]).write_bytes(module)
    print(f'{sys.argv[2]}: version 2, {count} entries, {len(module)} bytes')


if __name__ == '__main__':
    main()
