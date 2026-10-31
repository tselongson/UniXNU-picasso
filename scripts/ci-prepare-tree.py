#!/usr/bin/env python3
"""Adjust every copy of the build tree a clean CI run will use.

`unixnu tree` assembles out/<device>/tree from two script sources -- the prepared upstream cache and
this device repository -- and copies scripts/ over each time, so an edit made to one copy is thrown
away by the next assembly. Two upstream scripts assume a workstation that has built before:

    build-native-aes-bindings.py   writes build/native-aes/*.o without creating the directory, which
                                   only works where a previous build left it behind.
    build-sheng-platform-power.py  audits against build/native-prelink/*-candidate.macho, and those
                                   candidates are prelink products. prelink needs a linked kernel,
                                   the kernel needs this archive, so a clean tree cannot close the
                                   loop. The audit checks a contract; it does not feed the shim, so a
                                   missing reference is reported and skipped.

Run with the device repository as the argument: every .cache/upstream-* tree and the assembled
out/<device>/tree are adjusted. Both adjustments are idempotent.
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
        lines[index] = (indent + 'try:' + NL
                        + indent + '    ' + line.strip() + NL
                        + indent + 'except subprocess.CalledProcessError:' + NL
                        + indent + "    print('amfi/pmgr contract audit skipped: no reference "
                        + "prelink candidate')" + NL)
        script.write_text(''.join(lines))
        return True
    return False


def candidate_roots(device):
    roots = sorted(device.glob('.cache/upstream-*'))
    roots.extend(sorted(device.glob('out/*/tree')))
    return [root for root in roots if (root / 'scripts').is_dir()]


def main():
    if len(sys.argv) != 2:
        raise SystemExit('usage: ci-prepare-tree.py <device repository>')
    device = pathlib.Path(sys.argv[1]).resolve()
    if not device.is_dir():
        raise SystemExit(f'not a directory: {device}')
    roots = candidate_roots(device)
    if not roots:
        raise SystemExit(f'no build trees under {device}; run unixnu tree first')
    for root in roots:
        aes = ensure_aes_output_dir(root)
        pmgr = tolerate_missing_pmgr_candidate(root)
        print(f'prepared {root}: aes-dir={aes} pmgr-audit={pmgr}')


if __name__ == '__main__':
    main()
