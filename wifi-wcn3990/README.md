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
| `source/` | the modem loader and, in `Wcn3990Hif.cpp`, the host-side CE / HTC / WMI-TLV stack: PAS/PIL, SMP2P/GLINK, QRTR, WLFW QMI, BDF, rmtfs, CE0/2/3 rings, HTC handshake, WMI INIT + START_SCAN, MGMT_RX beacon parsing, `en0`, Apple80211 `kern_control` and the UniXNU compatibility shim |
| `manifest.json` | entry point, boot argument, platform dependencies, link method, verified/unverified features |
| `USAGE.txt` | the two supported integration methods |
| `docs/wcn3990-protocol.md` | every CE/HTC/WMI constant with its Linux ath10k file:macro source |
| `integration/` | the four patches a clean `00ea4a0` tree needs, one concern each |

The archive itself (`build/native-wlan/libNativeWlan.a`) is produced by CI with
the tree's recorded XNU kext flags. It is deliberately **not** committed: it is
built for one specific XNU/ABI pair, exactly as `libNezhaPeachWifi-0452.a` is.

## Boundary with the host tree

The host tree supplies the XNU kernel, `native_ufs_raw_read`, the boot-arg
parser, `IOSleep`/thread primitives and the `-force_load` link line. Everything
Wi-Fi specific lives in `source/`. Nothing in this package touches GPU, UFS
transport internals, display, cpufreq or the payload's trust/addon chain.

Only four host-tree integration points exist; see `integration/`. Applying them
to a clean upstream tree is the whole install. The baseline is the current upstream
tip of `stage12/picasso-ios16` (`00ea4a0`); see `integration/INTEGRATION.md` for
the regeneration steps. Removing them (or setting
`wifi=0`) returns the tree to a stock upstream build with no Wi-Fi behaviour.

## Honest status

Verified on hardware before the HIF work: MPSS PAS (`init_image` /
`mem_setup` / `auth_and_reset` all return 0), WLFW BDF download,
`FW_READY_IND`, mission mode, QRTR/rmtfs, `en0` attach and Apple80211 ioctls
from `wifid`.

**New in this revision (pending hardware confirmation):** `Wcn3990Hif.cpp`
programs CE0/CE2/CE3 with ath10k's WCN3990 register map and 64-bit
descriptors, completes the HTC READY → CONNECT_SERVICE → SETUP_COMPLETE
handshake, sends WMI-TLV `INIT` and `START_SCAN`, and turns `MGMT_RX` beacons
into real scan results. `picasso_sta_scan()` no longer fabricates networks.
`wcn3990-hif=0` disables CE register programming without rebuilding.

**Still not implemented:** association and the data path. The upstream
`hakios-picasso` transmit function still forges DHCP/ARP replies locally and
there is no HTT/HTC data pipe, so a successful scan does not yet mean
connectivity. See `docs/wcn3990-protocol.md` section 5.

## Licence

See the header of each file in `source/` (BSD-2-Clause-Patent where stated).
