# WCN3990 bring-up: evidence charter and current symptom

This document exists because several earlier "green" results were not
results.  It is the rule set for every further experiment, derived from
UniXNU's official porting docs, the Pong and WCN7860 device repositories,
and HakiOS's verification notes.

## 0. Current state (2026-11-15 23:1x, v41) — read this before §2

The authoritative write-up is
`handoff/wifi-wcn3990-handoff-2026-11-15-v41.md`.  What matters for every
future experiment in this driver:

- The data plane is up: join, WPA2, unicast both ways, DHCP binding
  (`DHCP: bound inet 192.168.101.18 router 192.168.101.1`) and DNS
  publication all appear in real boot logs.  **"Cannot open a web page" is
  therefore not a DNS or AP problem** — the earlier rounds that chased
  resolver configuration were wrong, and `pkg-config`-level checks cannot
  show it.
- The remaining defect is **runtime liveness of the receive path**, and it
  has two shapes that must not be conflated:

  | shape | evidence | state |
  |---|---|---|
  | **A. whole RX stops** | `rx` / `beacons` / `mgmtev` freeze together (`RX STALL-GLOBAL`), `nb1=0` and `post1=sw1-1` prove buffers were still posted, `srri4` still advancing proves TX was alive | target only; `7750b02` asks Q6 for a `wlan_pd` restart instead of tearing the link down |
  | **B. RX alive, no downlink from the AP's own stack** | beacons keep arriving for 32 min, `uni` only rises when the PC pings, DNS queries leave (ACKed at L2) and nothing comes back, never a frame > 100 B | **unresolved** |

- The rule this replaces: a driver that reports `join=5` is not a working
  network.  The verdict is **"after it breaks, does it come back by
  itself"**, and the only acceptable proof is the log chain
  `RX DEAD -> asked Q6 to restart wlan_pd -> WLAN PD RESTART -> rejoin ->
  DHCP: bound` with no `addr=none` in between.
- Consequence for instrumentation: never gate a liveness detector on
  `g_link_up` or `g_scan_active`.  In shape A both are exactly what the
  failure leaves behind (`link=0`, `scan=1`), so any such gate silences the
  detector precisely when it is needed.

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

## 2. Current symptom (2026-11-10 20:45, chain-573 in flight)

Sections 2-4 were written for builds 526-530 and are kept below as a record
of the reasoning; the **current** state is in
`handoff/wifi-wcn3990-handoff-2026-11-10.md` (v6).  Summary:

- scan / auth / assoc / VDEV_UP are all verified checkpoints.  The
  VDEV_UP SError (un-clocked CE register reads in the 1 ms poll) was fixed
  in `62572ba` and verified on device (chain-564/n1, 0 PANIC over 220 s).
- chain-572 proved the zero-datapath cause with a four-way single-variable
  split: WMI INIT `cfg[6] ast_skid_limit` was 0; with 16 the target emits
  HTT peer map (type 3), WMI `PEER_ASSOC_CONF (0x6006)`,
  `RX_IN_ORD_PADDR_IND (type 0x12)` and real EAPOL frames.
  `initfix=0/1` had none of it, `initfix=3/7` passed.
- The remaining defect is that `hif_wr_msdu()` folded the native-WiFi
  Ethernet frame at `frame+0` instead of the reference `frame+18`, so the
  EAPOL payload was read 18 bytes early and M1 was discarded; the AP
  deauthed with reason 6.  `804f651` adds `rxfix` to A/B this.

Historical symptom (builds 526-530, now resolved):

```
STARTED(1) -> FOREIGN_CHANNEL(8) x39 -> FOREIGN_CHANNEL_EXIT(256) x39
           -> BSS_CHANNEL(4) x32 -> CHAN_INFO(0x4002) x40
           -> COMPLETED(2) reason=0
panic=0  smp2p bit1=0
mgmtev=0  mgmt drops=0  beacons=0  httidx=0  rx=169
```

That was `fw_idx_init_val = 0` (host posting no buffers).  Verified by
chain-531 A/B: `scanfix=63` / posted=1023 produced 10+ HEARD BEACON with
real SSIDs.  See §3 below for the original reasoning, which held up.

## 3. Original hypothesis (526-530), confirmed by chain-531

`hif_htt_rx_ring_cfg()` filled the RX ring but sent `fw_idx_init_val = 0`,
telling the target that no buffers were posted.

- Linux `htt_rx.c:__ath10k_htt_rx_ring_fill_n()` advances
  `*alloc_idx.vaddr` as it posts buffers; `htt_tx.c:938` reads that same
  word and sends it as `fw_idx_init_val`.
- trogdor (`SC7180 + the same WCN3990`) writes `posted = WS_RX_FILL = 1023`
  into the index page and puts `1023` in `values[3]` of RX_RING_CFG.
- WCN7860 (`PeachWmi.c`) posts 127 management buffers before any
  `mgm_rx_received` becomes non-zero.

Two later corrections to this section, both from on-device A/B:

1. `alloc_idx` is the **host refill pointer**, not target progress
   (`htt_rx.c:151/209`, `htt_tx.c:940`).  `hif_service_htt_ring()` used it
   as a write index and therefore ran only once; `be91f28` fixed the drain
   to be attention-driven.  chain-571 then showed the target wrote zero
   MSDUs anyway, so this was a real bug but **not** the zero-datapath cause.
2. The four-way `initfix` split (chain-572) found the actual remaining
   cause: `ast_skid_limit=0` in WMI INIT.  This is exactly the
   "WMI INIT words where Linux and trogdor disagree with us" that §4
   below proposed checking - that suggestion was correct and is now done.

## 4. Verification rule for the current experiment (chain-573)

One build, one new boot-arg bit, two 220 s boots (HIF starts at ~t+94 s):

- control: `rxfix=0` (old fold, reproduces 572's garbage EAPOL headers)
- test:    `rxfix=1` (wlan-wip `frame+18` fold)
- both:    `initfix=3 dpfix=5 wpa=1 joinsel=4 scanfix=127 httring=7`

Pass condition is **not** "no panic", **not** "an EAPOL frame was counted"
(572 already counted four), and **not** "COMPLETED".  It is the WPA
state-machine plaintext: `WPA: msg1 seen`, `msg2 sent`, then `msg3` /
`msg4` / `keys installed` / `peer authorized`.  `WPA: MIC mismatch -
wrong passphrase` is an informative checkpoint, not a code failure, since
`joinsel=4` picks an arbitrary visible 2.4 GHz AP and the passphrase in
`wpass=` is not yet tied to a confirmed SSID.

If `msg1 seen` never appears, compare the EAPOL log's
`ver/type/len/desc/info` against `wr_msdu()` byte-for-byte; the remaining
suspects are the rx_desc offsets (`buf+36` in `hif_wr_msdu` vs `buf+28`
in `hif_wr_rx_ind`) and the `decap` field position.

## 5. CE register access rules (authoritative, WCN3990 SNOC)

From the 2018-04-17 IMPS patch set (Rakesh Pillai / Govind Singh,
`linux-wireless.vger.kernel.org` archive) and Douglas Anderson's
"Keep track of which interrupts fired, don't poll them"
(Tested-on: WCN3990 SNOC WLAN.HL.3.2.2):

1. The always-on shadow window `0x32000+` is **WRITE-ONLY**.
   `CE_HOST_IE` (0x2c) and `CE_MISC_IE` (0x34) must never be read;
   read-modify-write on them hangs the AHB on this board.
2. `CE_CURRENT_SRRI` (0x44) / `CE_CURRENT_DRRI` (0x48) are **not mapped**
   into the shadow window.  Reading them while the target has idled is an
   un-clocked access ("can lead to un-clocked access ... Read SRRI/DRRI
   from DDR instead of doing an actual hardware read").  Use the DDR RRI
   word: `CTRL1` bit19 plus wrapper 0x24C004/0x24C008, SRRI in [15:0],
   DRRI in [31:16].  This driver already programs both.
3. Destination doorbells must be rung through the shadow addresses
   (`shadow_dst_wr_ind_addr()`: CE1 0x32034, CE2 0x32038, CE5 0x32044,
   CE7 0x3204C, CE8..CE11 0x32050..0x3205C), not the CE-body 0x40.
4. Receive completion is decided **only** by the host-memory descriptor's
   `nbytes` (`_ath10k_ce_completed_recv_next_nolock_64()`, ce.c:806;
   CoachZ `wc_service()` @2489; `ce-irq-service` `hif_ce_service()`).
   In Linux `dest_ring_read_index_get()` is called at init (:1416) and
   dump (:1807) only, never on the running path.
5. `CE_STATUS` (0x30) is W1C and may only be written for an engine that
   actually completed something (CoachZ services one engine at a time).
   An unconditional W1C on every 1 ms tick is itself an unsafe access.
6. Douglas Anderson, verbatim: "if _none_ of the Copy Engines have an
   interrupt pending then they might go into low power mode and
   **reading from their address space will cause a full system crash**".
   The upstream fix records which IRQs fired instead of polling them.

Applied to `Wcn3990Hif.cpp` on 2026-11-10: zero CE-body register reads in
the 1 ms poll loop; CE_STATUS ACK moved inside the `nbytes != 0` branch
for both CE1 and CE2; both reposts ring the always-on shadow doorbell;
heartbeat/scan/quiet diagnostics read DDR RRI.  Stage-2/3 init and
`hif_dump_regs()` still touch CE registers (the engine is awake there);
that is intentional and matches Linux.

**Verified on device 2026-11-10** — chain-564/n1 ran the fixed poll loop
with `assoc resp status=0 -> vdev up sent -> 0 PANIC` across a full 220 s
capture; chain-570/571/572/573 all kept 0 PANIC with VDEV_UP reached.
The SError that blocked builds 535-560 is gone.  The rule above remains
authoritative: do not reintroduce CE-body register reads into the 1 ms loop.
