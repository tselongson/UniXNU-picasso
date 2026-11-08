#!/usr/bin/env python3
"""Fetch and authenticate the stock touch firmware, or verify a local copy."""
import argparse
import json
from pathlib import Path
import urllib.request
import picasso_touch_firmware as firmware

# The upstream release asset occasionally answers 504 for hours at a time.
# The exact same bytes are already embedded uncompressed in every composed
# boot image, so keep a verified copy in-tree as a fallback.  firmware.verify()
# enforces the pinned SHA256 and the Novatek markers either way.
BUNDLED = Path(__file__).resolve().parent.parent / 'firmware' / 'touch' / firmware.NAME


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        data = args.output.read_bytes()
    elif args.source:
        data = args.source.read_bytes()
    else:
        request = urllib.request.Request(firmware.URL, headers={'User-Agent': 'UniXNU-picasso'})
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                data = response.read(firmware.SIZE + 1)
        except Exception as exc:
            if not BUNDLED.exists():
                raise
            print('touch firmware download failed (%s); using verified bundled copy' % exc)
            data = BUNDLED.read_bytes()
    result = firmware.verify(data)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists():
        with args.output.open('xb') as stream:
            stream.write(data)
    args.output.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
