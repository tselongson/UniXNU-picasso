# Applying this package to a clean tree

Baseline: `silenzio111/UniXNU-picasso`, branch `stage12/picasso-ios16`,
commit `00ea4a0` (2026-11-06). From the repository root:

```bash
git apply wifi-wcn3990/integration/01-board-wifi-arg.patch
git apply wifi-wcn3990/integration/02-build-native-wlan.patch
git apply wifi-wcn3990/integration/03-bsd-init-hook.patch
git apply wifi-wcn3990/integration/04-native-ufs-raw-read.patch
git apply wifi-wcn3990/integration/05-rpmh-crypto-export.patch
```

`wifi-wcn3990/source/` must stay at the repository root. Patch 02 finds it by
walking upwards from `__file__`, because `--prepare` copies the build script into
`device/out/picasso/tree/scripts/` while the package stays at the checkout root.
The five patches are exactly `git diff 00ea4a0 -- <file>` for
the five host-tree files, so each can be reviewed, reverted or re-applied on its
own. No defconfig, workflow, UFS transport, display or cpufreq file is touched;
patch 05 is the single exception in a GPU file, and it only gives an existing
function C linkage (no GPU logic changes).

To uninstall: `git apply -R` the five patches in reverse order and delete the
`wifi-wcn3990/` directory. Alternatively set `wifi=0` to keep the code linked but
inert.

## Baseline history

The package first targeted `e790d5d` (`stage12/picasso-cpufreq-big`) because the
three commits after it (`ba1aefc`..`e4a5a51`) added a `picasso_ipc_census_rows()`
call without installing its patch, which broke the kernel link.  Upstream's
`stage12/picasso-ios16` (`00ea4a0`, 2026-11-06) supersedes that branch and builds
cleanly, so the five patches are now generated against it.

To move to a newer upstream tip:

1. `git fetch origin <branch>` and confirm the unmodified tip builds in CI.
2. Branch from that tip, `git checkout <this-branch> -- wifi-wcn3990/`, then
   replay the five host-file edits: `board.toml` (`wifi=1`),
   `scripts/build-arm-driver.py` (package lookup + source compile loop),
   `scripts/build-xnu-4k-baseline.sh` (one `bsd_init` hook) and
   `drivers/NativeUfs/NativeUfs.cpp` (read-only `native_ufs_raw_*` exports), and
   `drivers/NativeAdrenoPicasso/Adreno620Cp.cpp` (`extern "C"` on
   `rpmhVoteCryptoPath`, which the CE doorbell path must call).
3. Regenerate `integration/0*.patch` from `git diff <new-tip> -- <file>` and
   update the hashes in README.md / USAGE.txt / manifest.json / INTEGRATION.md.

If upstream changes `scripts/build-arm-driver.py` or `bsd_init.c`'s
`printf(copyright)` anchor, only patches 02 and 03 need attention.
