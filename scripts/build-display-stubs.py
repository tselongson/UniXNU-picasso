#!/usr/bin/env python3
"""Build the display stub archive.

When the display layer is off, the USB console still calls its three entry points, and the link needs
them from somewhere. This compiles the no-op definitions in the tree and archives them, so the link
resolves on a clean machine rather than only on one that has a display archive from an earlier run.
"""

import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = ROOT / "drivers/NativeMobileDisplay/DisplayStubs.cpp"
OUT = ROOT / "build/display-stubs"
ARCHIVE = OUT / "libDisplayStubs.a"
RECORD = ROOT / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json"


def main() -> int:
    if not RECORD.is_file():
        print(f"no compile record yet: {RECORD}", file=sys.stderr)
        return 1
    record = json.loads(RECORD.read_text().strip().rstrip(","))
    OUT.mkdir(parents=True, exist_ok=True)

    arguments = [a for a in record["arguments"]]
    # Swap the recorded source and output for ours, and keep every flag the kernel objects use.
    result = []
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value == "-o":
            result.extend(["-o", str(OUT / "DisplayStubs.o")])
            index += 2
            continue
        if value.endswith((".c", ".cpp", ".iig")):
            index += 1
            continue
        result.append(value)
        index += 1
    result[0] = "xcrun"
    result[1] = "-sdk"
    # The recorded invocation is xcrun -sdk <sdk> clang++ ...; keep it and add ours after.
    result.extend(["-c", str(SOURCE)])

    completed = subprocess.run(result, capture_output=True, text=True)
    if completed.returncode:
        print(f"Failed to compile {SOURCE}:\n{completed.stdout}{completed.stderr}", file=sys.stderr)
        return 1

    ARCHIVE.unlink(missing_ok=True)
    subprocess.run(
        ["xcrun", "ar", "rcs", str(ARCHIVE), str(OUT / "DisplayStubs.o")], check=True
    )
    print(f"DisplayStubs: stub archive built ({ARCHIVE})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
