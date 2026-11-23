#!/usr/bin/env python3
"""Add exactly the compiled status bar library to the original trust bundle."""
import argparse
import json
from pathlib import Path
from picasso_ui_trust import make_bundle


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original', type=Path, required=True)
    parser.add_argument('--library', type=Path, action='append', required=True,
                        help='owned Mach-O image to trust; repeat for more than one')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    bundle, report = make_bundle(args.original.read_bytes(), [path.read_bytes() for path in args.library])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(bundle)
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('PASS: unchanged Apple modules plus %d exact owner CDHash(es): %s'
          % (report['owned_entries'], ' '.join(identity['cdhash'] for identity in report['libraries'])))


if __name__ == '__main__':
    main()
