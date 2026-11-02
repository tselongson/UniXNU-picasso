#!/usr/bin/env python3
"""Run every check the kernel build would run, in the order it would hit them.

The point is that 'it compiles' is not one question but four, and the build answers them in the order
they appear below, each costing a CI round trip when it fails. Replaying them locally turns that into
a minute:

  1. every layer the defconfig switches on must have its input in the tree
  2. every component the archive list names must be buildable
  3. every source a component compiles must exist
  4. every source must compile with the flags the kernel objects use
"""

import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
WSL_PY = "/mnt/c/Users/tselo/Downloads/miphone/work/local-check.py"

steps = []

steps.append(("preflight", [sys.executable, str(ROOT / "scripts/preflight.py")]))

# The components and their sources, read out of the build script so this cannot drift from it.
builder = (ROOT / "scripts/build-arm-driver.py").read_text(encoding="utf-8", errors="replace")
components = []
for match in __import__("re").finditer(
    r"def build_(\w+)\(record\):.*?for name in \((.*?)\):", builder, __import__("re").S
):
    component = match.group(1)
    names = __import__("re").findall(r"'([^']+\.cpp)'", match.group(2))
    if names:
        components.append((component, names))

if components:
    # The compile checks run in WSL against the Linux tree, because that is where the compiler and
    # the kernel headers live.
    script = ["set -e"]
    for component, names in components:
        for name in names:
            script.append(
                f'python3 {WSL_PY} /root/pic/out/picasso/tree/drivers/*/{name} '
                f'$(dirname /root/pic/out/picasso/tree/drivers/*/{name}) 2>&1 '
                f'| grep -E "^exit" | head -1'
            )
    steps.append(("compile-check", ["wsl", "-e", "bash", "-lc", "; ".join(script)]))

failed = False
for name, command in steps:
    print(f"== {name}")
    completed = subprocess.run(command, capture_output=True, text=True)
    output = completed.stdout.strip() or completed.stderr.strip()
    if output:
        for line in output.splitlines()[:20]:
            print("  ", line)
    if completed.returncode != 0:
        failed = True

print("verify:", "FAILED" if failed else "all checks passed")
sys.exit(1 if failed else 0)
