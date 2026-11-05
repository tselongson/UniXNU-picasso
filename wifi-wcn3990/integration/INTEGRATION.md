# Applying this package to a clean tree

Baseline: `silenzio111/UniXNU-picasso`, branch `stage12/picasso-cpufreq-big`,
commit `e790d5d`. From the repository root:

```bash
git apply wifi-wcn3990/integration/01-board-wifi-arg.patch
git apply wifi-wcn3990/integration/02-build-native-wlan.patch
git apply wifi-wcn3990/integration/03-bsd-init-hook.patch
git apply wifi-wcn3990/integration/04-native-ufs-raw-read.patch
```

`wifi-wcn3990/source/` must stay at the repository root. Patch 02 finds it by
walking upwards from `__file__`, because `--prepare` copies the build script into
`device/out/picasso/tree/scripts/` while the package stays at the checkout root.
The four patches are exactly `git diff e790d5d -- <file>` for
the four host-tree files, so each can be reviewed, reverted or re-applied on its
own. No defconfig, workflow, GPU, UFS transport or display file is touched.

To uninstall: `git apply -R` the four patches in reverse order and delete the
`wifi-wcn3990/` directory. Alternatively set `wifi=0` to keep the code linked but
inert.

## Why this baseline, and when to upgrade

`e790d5d` (2026-11-05 16:34) is the last `stage12/picasso-cpufreq-big` revision
that links cleanly. Upstream's next three commits (`ba1aefc`, `f3e6518`,
`e4a5a51`, 19:35-19:37) add a `picasso_ipc_census_rows()` call to
`drivers/NativeMobileDisplay/boot-progress/BootProgress.c` but never install
`patches/picasso-ipc-census-xnu.patch` through `unixnu --prepare`, so the kernel
link fails with `_picasso_ipc_census_rows` undefined. We deliberately do not
carry a local fix for an upstream build break inside a Wi-Fi package.

Wait for upstream to wire that patch up (or drop the call), then upgrade:

1. `git fetch origin stage12/picasso-cpufreq-big` and confirm a CI kernel build
   of the *unmodified* tip succeeds.
2. Branch from that tip, `git checkout <this-branch> -- wifi-wcn3990/`, then
   replay the four host-file edits: `board.toml` (one `wifi=1` line),
   `scripts/build-native-wlan.py` (PACKAGE lookup + source compile loop),
   `scripts/build-xnu-4k-baseline.sh` (one bsd_init hook), and
   `drivers/NativeUfs/NativeUfs.cpp` (read-only `native_ufs_raw_*` exports).
3. Regenerate `integration/0*.patch` from `git diff <new-tip> -- <file>`,
   update the commit hashes in README.md / USAGE.txt / manifest.json /
   INTEGRATION.md, and run one CI build before testing on hardware.

If upstream changes `scripts/build-native-wlan.py` or `bsd_init.c`'s
`printf(copyright)` anchor, only patches 02 and 03 need attention.
