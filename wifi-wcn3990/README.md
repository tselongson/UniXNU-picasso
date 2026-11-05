# UniXNU Picasso — WCN3990 Wi-Fi driver package

A self-contained Wi-Fi driver package for the Xiaomi picasso / SM7250 (WCN3990,
"hakios") port of UniXNU, modelled on the layout of
[u0er/unixnu-wcn7860-wifidriver](https://github.com/u0er/unixnu-wcn7860-wifidriver):
one source tree, one archive, a short manifest, and a small set of integration
patches against a known-good upstream revision. **The main tree keeps no Wi-Fi
implementation of its own.**

## What is in here

| path | contents |
|---|---|
| `source/` | 13 files, ~7.9k lines: PAS/PIL, SMP2P/GLINK, QRTR, WLFW QMI, BDF, rmtfs, CE2, `en0` (IONetworkInterface), Apple80211 `kern_control` control port, and the UniXNU compatibility shim (GPT/UFS partition reads, RPMh/AOP rail votes) |
| `manifest.json` | entry point, boot argument, platform dependencies, link method, verified/unverified features |
| `USAGE.txt` | the two supported integration methods |
| `integration/` | the four patches a clean `e790d5d` tree needs, one concern each |

The archive itself (`build/native-wlan/libNativeWlan.a`) is produced by CI with
the tree's recorded XNU kext flags. It is deliberately **not** committed: it is
built for one specific XNU/ABI pair, exactly as `libNezhaPeachWifi-0452.a` is.

## Boundary with the host tree

The host tree supplies the XNU kernel, `native_ufs_raw_read`, the boot-arg
parser, `IOSleep`/thread primitives and the `-force_load` link line. Everything
Wi-Fi specific lives in `source/`. Nothing in this package touches GPU, UFS
transport internals, display, cpufreq or the payload's trust/addon chain.

Only four host-tree integration points exist; see `integration/`. Applying them
to a clean upstream tree is the whole install. The baseline is pinned to the last
upstream revision that links on its own; see `integration/INTEGRATION.md` for why
we are waiting on upstream before moving to a newer tip. Removing them (or setting
`wifi=0`) returns the tree to a stock upstream build with no Wi-Fi behaviour.

## Honest status

Verified on hardware: MPSS PAS (`init_image` / `mem_setup` / `auth_and_reset`
all return 0), WLFW BDF download, `FW_READY_IND`, mission mode, QRTR/rmtfs,
`en0` attach and Apple80211 ioctls from `wifid`.

**Not implemented:** real over-the-air scanning and association. The upstream
`hakios-picasso` implementation this was ported from feeds six hard-coded BSSIDs
in `picasso_sta_scan()` and forges DHCP/ARP replies locally; there is no
HTC/WMI/HTT host stack. CE2 only fills descriptors without programming the
target's copy-engine registers. Real scanning needs CE pipe setup, an HTC
handshake, `WMI_INIT`/`WMI_START_SCAN` and `WMI_MGMT_RX_EVENT` parsing. The
package is structured so that work can land entirely inside `source/`.

## Licence

See the header of each file in `source/` (BSD-2-Clause-Patent where stated).
