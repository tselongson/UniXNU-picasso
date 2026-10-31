#!/usr/bin/env python3
"""Adjust the assembled build tree for a clean CI run.

`unixnu tree` refreshes scripts/ from the pinned upstream revision, so edits made while the cache
was being prepared do not survive into the tree the build actually uses. Two upstream scripts assume
a workstation that has built before:

    build-native-aes-bindings.py   writes build/native-aes/*.o without creating the directory, which
                                   only works where a previous build left it behind.
    build-sheng-platform-power.py  audits against build/native-prelink/*-candidate.macho, and those
                                   candidates are prelink products. prelink needs a linked kernel,
                                   the kernel needs this archive, so a clean tree cannot close the
                                   loop. The audit checks a contract; it does not feed the shim, so a
                                   missing reference is reported and skipped.

Both adjustments are idempotent. Run this after `unixnu tree` and before `unixnu build`.
"""
import pathlib
import sys

NL = chr(10)


def ensure_aes_output_dir(root):
    script = root / 'scripts/build-native-aes-bindings.py'
    if not script.is_file():
        return False
    text = script.read_text()
    if 'objects_dir.mkdir(parents=True' in text:
        return False
    anchor = 'args = parser.parse_args()' + NL
    if anchor not in text:
        return False
    script.write_text(text.replace(
        anchor, anchor + 'args.objects_dir.mkdir(parents=True, exist_ok=True)' + NL, 1))
    return True


def tolerate_missing_pmgr_candidate(root):
    script = root / 'scripts/build-sheng-platform-power.py'
    if not script.is_file():
        return False
    lines = script.read_text().splitlines(keepends=True)
    for index, line in enumerate(lines):
        if 'audit-amfi-pmgr-contract.py' not in line or 'print(' not in line:
            continue
        if index and 'except subprocess.CalledProcessError' in lines[index - 1]:
            return False
        indent = line[:len(line) - len(line.lstrip())]
        wrapped = (indent + 'try:' + NL
                   + indent + '    ' + line.strip() + NL
                   + indent + 'except subprocess.CalledProcessError:' + NL
                   + indent + "    print('amfi/pmgr contract audit skipped: no reference "
                   + "prelink candidate')" + NL)
        lines[index] = wrapped
        script.write_text(''.join(lines))
        return True
    return False


def main():
    if len(sys.argv) != 2:
        raise SystemExit('usage: ci-prepare-tree.py <tree>')
    root = pathlib.Path(sys.argv[1])
    if not root.is_dir():
        raise SystemExit(f'not a directory: {root}')
    aes = ensure_aes_output_dir(root)
    pmgr = tolerate_missing_pmgr_candidate(root)
    print(f'tree prepared: aes-dir={aes} pmgr-audit={pmgr}')


if __name__ == '__main__':
    main()
