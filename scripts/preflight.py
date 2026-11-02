#!/usr/bin/env python3
"""Preflight: answer 'will this build fail?' before pushing.

Every failure this pipeline has had that was not a plain compile error was the same shape: a build
layer whose switch is on while its input is something only a workstation has -- a firmware blob under
/tmp, a prelink candidate left by the previous build, a script that stops when the seed is missing.
The kernel build itself reports those one at a time, twenty minutes apart, so this checks them all in
one pass by asking the same question each script asks and looking at the same files.
"""

import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent   # silenzio-picasso

# Layers whose switch lives in the defconfig and whose build needs an input outside the tree.
LAYERS = [
    {
        "config": "CONFIG_XNU_SHENG_HIDINPUT",
        "env": "XNU_SHENG_HIDINPUT",
        "script": ROOT / "scripts/build-sheng-hid-input.py",
        # The HID builder seeds itself from the kernel the previous run linked.
        "needs": [ROOT / "src/kernel/kernel.macho"],
        "why": "the HID layer seeds its candidate from a previously linked kernel",
    },
    {
        "config": "CONFIG_XNU_NOVATEK_TOUCH",
        "env": "XNU_NOVATEK_TOUCH",
        "script": ROOT / "scripts/build-native-novatek-touch.py",
        # The touch probe embeds a firmware image the probe reads from a vendor directory.
        "needs": [pathlib.Path("/tmp/picasso-vendor/blobs/candidate1.bin")],
        "why": "the touch probe embeds a Novatek firmware blob that is not in the repository",
    },
    {
        "config": "CONFIG_XNU_BOOT_PROGRESS",
        "env": "XNU_BOOT_PROGRESS",
        "script": ROOT / "scripts/build-boot-progress.py",
        "needs": [],
        "requires_configs": ["XNU_NATIVE_UFS", "XNU_OWNER_ROOT_AUDIT", "XNU_SOURCE_CORECRYPTO14"],
        "why": "the boot progress build requires the ufs, owner-root and corecrypto layers",
    },
]


def defconfig_value(name: str) -> bool:
    text = (ROOT / "configs/picasso_defconfig").read_text(encoding="utf-8", errors="replace")
    return bool(re.search(rf"^{re.escape(name)}=y\s*$", text, re.M))


def build_script_accepts_env(env: str) -> bool:
    """Reads the kernel build script for the default it gives this layer's switch."""
    text = (ROOT / "scripts/build-xnu-4k-baseline.sh").read_text(encoding="utf-8", errors="replace")
    match = re.search(rf"{re.escape(env)}=\$\{{XNU_{re.escape(env)[4:]}:-([01])\}}", text)
    return bool(match and match.group(1) == "1") or f"${{XNU_{env[4:]}:-1}}" in text


def run_check() -> int:
    problems = []
    for layer in LAYERS:
        enabled = defconfig_value(layer["config"])
        if not enabled:
            continue
        if not layer["script"].is_file():
            problems.append(f"{layer['config']}: {layer['script'].name} is missing from the tree")
            continue
        for needed in layer.get("needs", []):
            if not needed.exists():
                problems.append(
                    f"{layer['config']}: {layer['why']} -- {needed} is missing\n"
                    f"    build would stop at {layer['script'].name}\n"
                    f"    fix: comment out {layer['config']} in configs/picasso_defconfig, or supply {needed.name}"
                )
        for required in layer.get("requires_configs", []):
            if not defconfig_value(f"CONFIG_{required}"):
                problems.append(
                    f"{layer['config']}: needs CONFIG_{required}=y, which is not set"
                )

    # Every component the archive list names must be a component the build script knows how to build.
    board = (ROOT / "board.toml").read_text(encoding="utf-8", errors="replace")
    listed = re.findall(r'"(build-arm-driver\.py\s+\w+)"', board)
    builder = (ROOT / "scripts/build-arm-driver.py").read_text(encoding="utf-8", errors="replace")
    choices = re.search(r"add_argument\('component',\s*choices=\[([^\]]*)\]", builder)
    for entry in listed:
        component = entry.split()[-1]
        if choices and f"'{component}'" not in choices.group(1):
            problems.append(f"archive list names component '{component}', which build-arm-driver.py does not build")

    # The source files each component compiles must exist.
    for component_match in re.finditer(r"def (\w+)\(record\):.*?for name in \((.*?)\):", builder, re.S):
        names = re.findall(r"'([^']+)'", component_match.group(2))
        for name in names:
            # Sources can sit in either of the two driver roots the build script uses.
            if not list(ROOT.glob(f"drivers/*/{name}")):
                problems.append(f"component {component_match.group(1)} compiles {name}, which is not in drivers/")

    if problems:
        print(f"preflight: {len(problems)} problem(s)")
        for problem in problems:
            print(" -", problem)
        return 1
    print("preflight: clean")
    return 0


if __name__ == "__main__":
    sys.exit(run_check())
