# WiFi bring-up: authoritative notes (do not re-derive by trial and error)

Everything in this file comes from upstream Linux source or the three reference projects.
Read this before changing anything in `drivers/NativeWlan/WlanMpss.cpp`.

## 1. Which boot path applies to SM7250 / WCN3980

Two entirely different Qualcomm modem boot paths exist. Confusing them cost us
many hours of failed `PAS_INIT_IMAGE` calls.

| | MBA self-authentication (Trogdor / SC7180) | PAS authentication (SM7250 and newer) |
|---|---|---|
| Linux driver | `drivers/remoteproc/qcom_q6v5_mss.c` | `drivers/remoteproc/qcom_q6v5_pas.c` |
| Firmware | `mba.mbn` + `qdsp6sw.mbn`/`modem.mdt` | `modem.mdt` + `modem.bXX` only |
| `mba.mbn` present in NON-HLOS | yes (Trogdor: 283296 B) | **no** (our FAT has only modem.mdt + b00..b33) |
| SCM calls | none for auth; `qcom_scm_assign_mem` for mpss_mem then mba_mem | `PAS_INIT_IMAGE`, optional `PAS_MEM_SETUP`, `PAS_AUTH_AND_RESET` |
| Q6 start | HLOS writes QDSP6SS/RMB registers | done by TrustZone inside AUTH_AND_RESET |

`docs/handoff-20261104-statusbar-gpu.md` in this tree and Trogdor's
`docs/plans/wifi.md` both state TF-A on SC7180 implements **no PAS calls at all**,
so Trogdor had to use MBA self-auth. Our Xiaomi XBL/ABL is a different firmware:
our on-device `PAS_IS_SUPPORTED` scan returned supported ids
`1 4 9 13 15 18 19 20`, i.e. PAS is present. `Adreno620Cp.cpp` already boots the
GPU ZAP with `PAS_AUTH_AND_RESET` (pas-id 13) on this exact device, which proves
the PAS path works here.

**Consequence: do not port Trogdor's MBA/RPMh/QDSP6SS code. Port its WLFW/GLINK/
CE/HTC/WMI/scan/en0 layers instead, and use Linux's PAS sequence for MPSS.**

## 2. Exact PAS sequence (Linux `qcom_mdt_pas_load` -> `qcom_pas_prepare_and_auth_reset`)

Sources:
- `drivers/soc/qcom/mdt_loader.c`: `qcom_mdt_pas_load`, `__qcom_mdt_pas_init`,
  `qcom_mdt_read_metadata`, `qcom_mdt_load_no_init`
- `drivers/firmware/qcom/qcom_pas.c`: thin ops dispatch
- `drivers/firmware/qcom/qcom_scm.c`: `__qcom_scm_pas_init_image`,
  `qcom_scm_pas_mem_setup`, `qcom_scm_pas_auth_and_reset`

Order is **init -> mem_setup -> load segments -> auth_and_reset**:

```
metadata = ELF header (phdrs[0].p_filesz bytes)
         + hash segment (phdrs[hash].p_filesz bytes), NO padding
  hash is the phdr whose (p_flags & 0x07000000) == 0x02000000
  for split firmware the hash sits at fw->data + phdrs[0].p_filesz
metadata must be physically contiguous, 4K-aligned, non-cacheable
  (Linux uses dma_alloc_coherent; we use IOBufferMemoryDescriptor + kIOMapInhibitCache)

PAS_INIT_IMAGE  (pas_id, metadata_phys)     arginfo 0x82 = ARGS(2, VAL, RW)
if any PT_LOAD phdr has QCOM_MDT_RELOCATABLE (BIT(27) = 0x08000000):
    PAS_MEM_SETUP (pas_id, mem_phys, max_addr - min_addr)   arginfo 0x03
for each loadable phdr: copy modem.bNN to carveout + (p_paddr - min_addr)
    loadable = PT_LOAD && (flags & TYPE_MASK) != HASH && p_memsz != 0
    **the hash phdr is NOT loaded into the carveout**
PAS_AUTH_AND_RESET (pas_id)                 arginfo 0x01
```

SMC convention: `function = 0x42000000 | (0x02 << 8) | cmd`
(SMCCC_SMC_64 bit 30 + ARM_SMCCC_OWNER_SIP<<24 + QCOM_SCM_SVC_PIL<<8 + cmd),
x1 = arginfo, x2..x5 = args, x6 = session id. Cmd values:
INIT_IMAGE 0x01, MEM_SETUP 0x02, AUTH_AND_RESET 0x05, SHUTDOWN 0x06,
IS_SUPPORTED 0x07, MSS_RESET 0x0A. If x0 == 1 (QCOM_SCM_INTERRUPTED), retry the
same call with x6 = returned session id (see `Adreno620Cp.cpp`).

### Our previous mistakes (all measured on device)

1. **`pas_id = 1` is wrong.** Linux `mpss_resource_init`, sm6350, sm8150, sm8350,
   sm8450, sm8550, sm8650, sm8750 all use **`pas_id = 4`** for `modem.mdt`.
   pas-id 1 is ADSP/LPASS. Our scan showed 4 supported. `0xffcfffbb` was TZ
   rejecting an image/metadata pair for the wrong peripheral.
2. `PAS_SHUTDOWN` before INIT **hangs forever** when the modem is off. Do not call it.
3. `PAS_MEM_SETUP` was called before INIT and unconditionally. Linux calls it
   **only after a successful INIT and only for relocatable images**.
4. The hash phdr (type 0, paddr 0x9b800000) was treated as a loadable segment via
   a `paddr >= 0x80000000` heuristic. It must be metadata only.
5. Segments were staged into the carveout *before* INIT; Linux authenticates the
   metadata first, then loads segments, then authenticates the whole image.

## 3. Where the WLAN firmware actually is

Source: Linux `drivers/net/wireless/ath/ath10k/snoc.c` and `qmi.c`.

For WCN3990/WCN3980 (SNOC/QMI), the AP driver **does not load `wlanmdsp.mbn`
or `athwlan.bin`**. That executable is loaded by the modem/PIL/WLFW side from its
own partition. The AP supplies only:

- `board-2.bin` (BDF / board calibration, from linux-firmware
  `ath10k/WCN3990/hw1.0/board-2.bin`; Trogdor used this exact file),
- `firmware-5.bin` (ath10k API/feature metadata, not a target image),
- MSA memory (our `WlanBoard.h`: `0x8ba00000`, 2 MiB; the SM7250 DTS has
  `wlan_fw_mem: wlan-fw@8ba00000` and `modem_wlan_mem: modem-wlan@8c000000`,
  matching our stage-2 SMMU probe `msa=8ba00000 pages=512`),
- host capability, CE config, service-to-pipe map, shadow regs, firmware mode.

WLFW QMI (service 69 on QRTR node 1) then loads the WLAN FW, applies BDF,
resets the WLAN core and emits `FW_READY_IND`. Only then does the AP register
the ath10k core, init CE/HTC/WMI and scan.

**So searching NON-HLOS.bin / vendor.img / system.img for `athwlan.bin` was the
wrong question.** Those images contain `WCNSS_qcom_cfg.ini`, `wlan_mac.bin`,
`aoa_cldb_*.bin` and `qca_cld3_wlan.ko`, but no WLAN executable — exactly as the
Linux driver predicts. `board-2.bin` can be fetched from the public
linux-firmware tree; no private blob needs to be vendored.

## 4. Stage ladder (from Trogdor `docs/plans/wifi.md`, all stages verified there)

1. SMEM/ADT inventory — **done on our device** (`layout rc=0 entries=14`).
2. MPSS boot — in progress here; use section 2.
3. SMP2P (in 435 / out 428), GLINK SMEM (478 desc, 479 TX, 480 RX), QRTR node 1,
   rmtfs service 14 — Trogdor's `coachz-prelink-wifi.patch` has working XNU code.
4. WLFW QMI: indication register -> CAP -> host cap -> MSA info/ready -> BDF ->
   WLAN enable (CE config) -> FW_READY_IND.
5. WCN3980 supplies, 12 copy engines, HTC, WMI service-ready / WMI_INIT, passive
   scan -> BSS table (Trogdor: ~70 frames, 40+ APs on 2.4/5 GHz).
6. `en0` via `CoachzWifiNet.cpp` (adapted from UniXNU-sheng `WifiNet.cpp`):
   ifnet + Apple80211 ioctls + `bpf_attach`/`bpf_tap`, `nw_wifi_*` boundary.
   Association, WPA2 4-way handshake, HTT RX/TX, DHCP, DNS.
   Trogdor's documented result: Safari HTTPS works, saved network auto-joins.

Known Trogdor pitfalls to reuse:
- No IPv4 after join = driver did not `bpf_attach`; DLIL won't tap DHCP for you.
- Repeated join failure = wifid exits on empty `PRIVATE_MAC_ADDRESS_VALUE`;
  write `/chosen/mac-address-wifi0`.
- SMEM item lookups must re-read the partition header each time: the modem
  appends ~20 KiB of new items after boot, so a cached partition tail misses them.
- `sheng-early-retire` interacts with frame lifetime; see this tree's
  `docs/handoff-20261104-statusbar-gpu.md` for the backboardd SIGBUS analysis.

## 5. Reference files in this workspace

- `Trogdor-ipadOS/docs/plans/wifi.md` — the stage table and hardware results
- `Trogdor-ipadOS/docs/hardware-tests.md` — k78..k107 detailed MPSS/QRTR/scan logs
- `Trogdor-ipadOS/platform/coachz-prelink/patches/xnu/coachz-prelink-wifi.patch`
  — 6138 lines of working XNU C: SMEM alloc, SMP2P, GLINK, QRTR, WLFW, CE, WMI
- `Trogdor-ipadOS/platform/coachz-prelink/package/CoachzWifiNet.cpp`
  — 591 lines, en0 + Apple80211 + nw_wifi bridge
- `unixnu-wcn7860-wifidriver/source/` — complete ath12k/PCIe stack, layer map
- `UniXNU-pong/`, `pong-wifi/` — ath11k PCIe/MHI platform + firmware staging
