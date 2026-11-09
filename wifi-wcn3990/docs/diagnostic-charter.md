# WCN3990 bring-up: evidence charter and current symptom

This document exists because several earlier "green" results were not
results.  It is the rule set for every further experiment, derived from
UniXNU's official porting docs, the Pong and WCN7860 device repositories,
and HakiOS's verification notes.

## 1. What counts as a result

- UniXNU `porting/network`: **"Firmware startup and successful scanning are
  separate checkpoints."**  A scan is successful only when it reports BSS
  entries; the full checkpoint list is scan -> auth -> assoc -> address ->
  rx -> tx, each recorded separately.
- UniXNU `porting/drivers`: "Exercise one complete operation": request,
  hardware completion, OS-visible result, then release/repeat; record the
  **last successful stage**.
- Pong `pong-wifi/AGENTS.md`: "Report hardware observations separately from
  build results ... **A successful link is not evidence that the hardware
  works**" and "never claim a device result that a log does not show".
- Pong `pong-wifi/README.md`: "does not substitute an empty success for
  firmware, scan or link results"; the network interface is published only
  after station readiness and a real hardware MAC.
- HakiOS `docs/air.md`: **"Green has to mean 'looked at and agreed', never
  'did not look'"**, "a measurement without a control answers a question
  nobody asked".  Every skipped case must reach the summary with its own
  number and its own reason; each stage of a state machine is counted and a
  failure stops at the first failing stage with a reason.

Consequences for this port:

1. `START_SCAN` accepted, `SCAN_EVENT_COMPLETED reason=0` and zero panics
   prove **only** that the scan state machine ran.  They are not a scan
   result.
2. `WMI_SCAN_EVENT_BSS_CHANNEL` does **not** mean an AP was found.  Linux
   `wmi.c:ath10k_wmi_event_scan_bss_chan()` only does
   `ar->scan_channel = NULL` (back on the home channel).  Earlier notes
   that read 32 BSS_CHANNEL events as "32 BSS found" were wrong.
3. An event that arrives and is dropped must be counted separately from an
   event that never arrived.  The WCN7860 driver keeps
   `mgmt_rx_received / mgmt_rx_discarded / bss_count / bss_dropped` as
   separate counters for exactly this reason.
4. Every experiment needs a control that differs by one variable, and a
   170 s capture so a late event is not mistaken for "never happened".

## 2. Current symptom (builds 526-530, identical across j1/j2/e/d)

```
STARTED(1) -> FOREIGN_CHANNEL(8) x39 -> FOREIGN_CHANNEL_EXIT(256) x39
           -> BSS_CHANNEL(4) x32 -> CHAN_INFO(0x4002) x40
           -> COMPLETED(2) reason=0
panic=0  smp2p bit1=0
mgmtev=0  mgmt drops=0  beacons=0  httidx=0  rx=169
```

The scan engine completes, but the host receives **zero 802.11 frames**.
`WMI_MGMT_RX_EVENTID (0x7001)` is never seen; the whole set of WMI event ids
in the log is `{0x3, 0x4009, 0x4022, 0x3a001, 0x401f, 0x4002}`.
`0x3a001` is a regdomain-class event (payload contains "US"), not a frame.

CHAN_INFO parsed from the existing log: 40 events, freq 2412..5825 MHz,
noise floor -101..-117 dBm, err_code=0.  The radio is tuning and measuring
each channel; this is not a "radio did not tune" failure.

Excluded by single-variable boots: our MGMT_RX parser (chain-527), wrong
event id, another CE pipe carrying the frames, vdev state
(`vdstart=1` still mgmtev=0; `vdevup=1` panics with both 14B and 16B TLVs),
the three 6-byte `wmi_mac_addr` ABI errors (chain-529, A/B identical), and
HTC credit handling.

## 3. Why it happens (working hypothesis, three independent sources)

`Wcn3990Hif.cpp:hif_htt_rx_ring_cfg()` fills 2047 RX buffer addresses but
then sets `g_dma->htt_alloc_idx = 0` and sends `fw_idx_init_val = 0`.  That
word is the `fw_idx_shadow_reg_paddr` handed to the target: it tells the
firmware how many buffers the host has posted.  **We tell it zero.**

- Linux `htt_rx.c:__ath10k_htt_rx_ring_fill_n()` advances
  `*alloc_idx.vaddr` as it posts buffers; `htt_tx.c:938` reads that same
  word and sends it as `fw_idx_init_val`.
- trogdor (`SC7180 + the same WCN3990`) writes `posted = WS_RX_FILL = 1023`
  into the index page and puts `1023` in `values[3]` of RX_RING_CFG, before
  START_SCAN.
- WCN7860 (`PeachWmi.c:peach_wmi_start_management`) allocates
  `MGMT_RX_STORAGE`, posts 127 management buffers and configures a dedicated
  HTT ring; only then does `mgm_rx_received` become non-zero.

We also send only one of trogdor's three HTT H2T messages: RX_RING_CFG.
Missing: FRAG_DESC_BANK_CFG (6) and AGGR_CFG (5).  `httidx=0` (the firmware
never writing the HTT ring) is consistent with "the target believes no
receive resource exists".

The WCN7860 driver is a different chip family (ath12k / MHI), so its code is
not portable here; the portable part is the **mechanism**: management frames
need host-posted receive buffers and a configured ring before the firmware
will deliver them.  The same conclusion is available from Linux and from
trogdor, which is the same WCN3990.

## 4. Verification rule for the next experiment

One build, one new boot-arg bit, two 170 s boots:

- control: `scanfix=31` (current best configuration)
- test:    `scanfix=63` (+ HTT receive resources posted correctly)

Pass condition is **not** "no panic" and **not** "COMPLETED".  It is
`mgmtev > 0` plus at least one parsed beacon with an SSID/BSSID.  If
`mgmtev` stays 0, the next split is FRAG_DESC_BANK_CFG and AGGR_CFG as
separate bits, then the WMI INIT words where Linux and trogdor disagree
with us (`ast_skid_limit 16`, `num_wds_entries 2`).
