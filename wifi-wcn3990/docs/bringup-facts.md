# Picasso WLAN bring-up: what the device says, and what each build proved

This is the ledger the earlier attempts did not keep. Every entry is either a
register/device-tree value read from *this* phone or the outcome of a build
that was flashed and captured. Anything taken from another board is marked as
such, because that is where the two-month detour came from.

## 1. The authority: this device's own device tree

Extracted from `Resources/DTBs/picasso.dtb` (the blob the bootloader hands XNU):

```
/soc/apps-smmu@15000000          phandle 0x18, compatible qcom,qsmmu-v500
                                  reg 0x15000000, #iommu-cells 2

/soc/qcom,icnss@18800000
        reg              = <0x18800000 0x800000 0xb0000000 0x10000>
        reg-names        = "membase", "smmu_iova_ipa"
        iommus           = <&apps_smmu 0xc0 0x1>        <-- stream 0xc0/0xc1
        qcom,iommu-dma-addr-pool = <0xa0000000 0x10000000>
        qcom,iommu-dma   = "fastmap"
        qcom,iommu-faults= "stall-disable", "HUPCF", "non-fatal"
        interrupts       = 132, 81, 415, 416, 417, 86, 94, 96, 82, 119, 120, 87
        qcom,wlan-msa-fixed-region = <&pil_wlan_fw_region>

/soc/ipa_smmu_wlan     iommus = <&apps_smmu 0x521 0x0>

reserved-memory:
        pil_wlan_fw_region@8ba00000   2 MiB   (the WLAN's MSA)
        modem_wlan_region@8c000000    248 MiB (the modem's WLAN window)
        ...  0xa0000000..0xb0000000 is *not* reserved: it is the DMA
             address window the WLAN is expected to use, not memory.
```

Two of these numbers had been taken from an upstream sc7180 description and
were wrong for this board:

| value | what we used | what this device says |
| --- | --- | --- |
| copy-engine stream id | `0x20` / `0x21` | **`0xc0` / `0xc1`** |
| CE interrupt lines | `414..425` | **132, 81, 415, 416, 417, 86, 94, 96, 82, 119, 120, 87** |

## 2. Why "touch the copy engines and the SoC dies"

The failure surface is always the same: the first *write* to a CE register
kills the machine, and the console shows either

* `[pic] 00000000be000411 ...` + `Unhandled implementation specific error`
  (ESR EC = 0x2F: an asynchronous SError), or
* nothing at all, followed by an APSS watchdog reset.

What is established:

1. Reading CE registers works (`CE0 ctrl1 00000080`), so the block is powered
   and its AHB slave answers.
2. The write that matters is `CTRL1` bit 19 (`RRI_UPD`): with it set the CE
   performs the read-index DMA, with it clear the machine still dies - just
   silently instead of with an SError. Either way the *next* CE access never
   happens.
3. The SError is asynchronous. Build 456's ELR (`_ml_set_interrupts_enabled_with_debug+0x50`)
   is the `IOSleep(30)` that follows the store, i.e. the abort is delivered at
   the next interrupt-enable, not at the store itself.
4. The apps-SMMU is in the path: `sCR0 = 0x00270406` (CLIENTPD = 0, USFCFG = 1).
   `CLIENTPD` is TZ-locked (a write reads back unchanged), so the CoachZ
   recipe - "the SMMU is off, plain physical addresses work" - is not available
   here.
5. `S2CR` stores land and read back; `SMR` stores do **not** (they read back 0).
   `TYPE = 1` (bypass) is coerced to `TYPE = 2` (fault) by this silicon, so the
   only usable encoding is `TYPE = 0` plus a bank whose `SCTLR.M` is 0.
6. Bank 60 is such a bank (`cbar = 00010000`, `sctlr = 00000000`) and it is the
   one USB already DMAs through; bank 61 is what XBL binds the WLAN streams to
   when it binds them (`SMR[4] id = 0c0 -> S2CR = 3d`).

## 3. Hypotheses that are now falsified

| hypothesis | experiment | result |
| --- | --- | --- |
| XBL leaves the CE stream as `TYPE=FAULT`, reroute `S2CR[0x20]` | modes 1/3/4 (CB61, CB60, CLIENTPD) | identical death in all three |
| the S2CR write needs a TLB invalidate | mode 3 + TLBIALLNSNH | still dies (and the sync registers were also wrong, see §4) |
| an SMR must be added for the stream | SEC-phase bind at slots 12..15 | stores never land, table unchanged |
| the CE DMA block is outside the DMA pool | `kmem_alloc_contig(0xa0000000, 0xb0000000)` | the allocator ignores the bounds and returns 0xcdca4000 anyway |
| any stream below 128 is the culprit | mode 6: 103 faulting slots defaulted to bank 60 | still dies - the engines are simply not in 0..127 |
| a wrong CE register offset (RRI wrapper) | compare against CoachZ `hw.c` (`0x24C004`) | matches; the readback holds the address we wrote |

## 4. Instrumentation now in the tree

* **Panic banner** (`osfmk/kern/debug.c` -> `picasso_wlan_panic_report()`):
  one line, SMMU first, because the panic console on this board only survives
  about three writes:
  `[wlan] gfsr=.. fsynr1=.. fsynr0=.. cr0=.. elr=..`
  `sGFSR != 0` means the SMMU rejected a transfer and `sGFSYNR1` names the
  stream id that tried; `sGFSR == 0` means the transaction never reached the
  SMMU (NoC / clock / XPU) and no SMMU work will fix it.
* **Stream-match table dump** before the engines are touched
  (`picasso_wlan_smmu_table_dump`), including both context banks' `SCTLR`,
  `TTBR0`, `TCR`, `FSR`, `FAR`.
* **Bind modes** (`wcn3990-wlan-smrubind`), all repointing only `S2CR`:
  `1`..`6` the historical experiments on `0x20`; `7` (default) stream `0xc0`
  through every matching SMR *and* the direct `S2CR[0xc0]` slot, to bank 60;
  `8` the same to bank 61; `9` an explicit `SMR` entry for `0xc0`.
* **CE bisect knobs**: `wcn3990-hif` (0 off, 1 no register touched, 3 normal),
  `-cecount`, `-cewait`, `-pace`, `-rri`, `-sleep`, `-trace`, `-post`.

Two register offsets were wrong in build 455 and are fixed since: `sTLBGSYNC`
is `0x070` and `sTLBGSTATUS` is `0x074`; `0x050`/`0x058` are `sGFSYNR0/2`, so
"tlbgstatus = 0 after 0 us" was reading a status register that never moves.

## 5. Open questions, in the order they must be answered

1. Does `S2CR[0xc0]` accept a store at all (index 192 is above the 128 the
   driver used to scan, but inside the 4 KiB GR0 page)?
2. If `0xc0` is routed to bank 60 and the CE still aborts, is `sGFSR` set? With
   `sGFSYNR1 = 0xc0` the SMMU is the one refusing; with `sGFSR = 0` the refusal
   is at the NoC/XPU level and only a VMID assignment (`hyp_assign`, which TZ
   rejected with `ret=-2 res=86` for VMIDs 24/25) could help.
3. Bank 61 is bound to `0xc0` on some boots and its stage-2 table is TZ's, not
   ours: `qcom,iommu-dma-addr-pool = <0xa0000000 0x10000000>` says the WLAN
   expects *I/O* addresses in that window, which only a stage-2 table can map.
   If bank 60 (identity) turns out to be unusable for the WLAN, building that
   table is the remaining option.
