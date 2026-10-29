# UniXNU on Redmi K30 5G (picasso, SM7250)

> **Status: the UFS root device publishes and APFS mounts its container.** The
> UEFI firmware boots the UniXNU menu, the xnu entry starts the kernel through
> DarwinBoot, and the kernel reaches the BSD layer, loads the AMFI /
> AppleImage4 / MAC-framework and pthread kexts, brings up mbufs, dlil and lo0,
> and registers APFS. The UFS driver then brings the controller up, publishes
> `SystemOS`, and APFS reads the container checkpoints on both disks.
>
> **The current blocker is root-volume authentication**: APFS cannot import the
> root hash because the device tree carries no `system-volume-auth-blob`, which
> iBoot normally derives from a personalised IM4M. Without it the root volume
> never publishes as `Apple_HFS` and `bsd_init` waits forever.
>
> The USB CDC console works, so the kernel log is readable over `/dev/cu.*`
> without a screen. See [docs/](docs/) for the stage table, the evidence behind
> each conclusion, and what is still unverified.
>
> UEFI firmware: https://github.com/YinCQL/uefi_picasso

## Progress

- **UEFI firmware: working.** Boots to the UniXNU menu. UFS Mass Storage, GOP
  framebuffer, button input and stable boot all work. Built via the UniXNU build
  driver; produces a flashable `Mu-picasso-unixnu-0.img`.
- **XNU kernel: boots to the root-device wait.** Build workflow, first-console
  config and the SM7250 ADT generator are in place, and the SM7250 kernel patch
  set (`patches/unixnu-source-kernel.patch`) builds the kernel that DarwinBoot
  starts. AMFI early-security init, the `com.apple.kec.pthread` kext and the
  NativeUfs driver are linked in; the verbose boot log scrolls live on the
  framebuffer and ends at the IOMedia/root wait. UFS bring-up (stage 6) is in
  progress.
- **USB kernel console: working.** The kernel exposes a CDC-ACM device named
  `picasso XNU USB console` and streams the buffered/live console over DWC3.
  Capture it with `scripts/capture-picasso-usb-console.py`; see
  `docs/usb-console-bringup.md` for hardware results and safeguards.


## Build

- Firmware: `Build UniXNU UEFI for picasso` workflow -> `fastboot boot Mu-picasso-unixnu-0.img`.
- Kernel + payload: `picasso XNU kernel` workflow (macos-latest).

## Tooling merged from the silenzio111 fork

`silenzio111/UniXNU-picasso` forked the same bring-up baseline (`189c01e`).
Its host-side build tooling and drivers are merged here:

| File | Purpose |
| --- | --- |
| `scripts/build-arm-driver.py` | Builds an archive from the kernel's own `IOPlatformExpert.cpo.json` compile record: ArmCpu, ArmDTPlatform, NativeRamBlock, NativeUfs, NativeAdreno. Replaces the copy that only existed inside the device patch set. |
| `scripts/prepare-xnu-source.py` | Restores the omitted DriverKit / TrustCache headers, with `scripts/headers-cache/` as a seed so a rate-limited CI egress IP does not fail the fetch. A real repository file instead of a patched-in script. |
| `drivers/ArmCpu/` | Upstream ArmCpu `1951cc84` plus a `/arm-io/intc/virtual-timer-intid` override (validated little-endian u32 PPI, 16..31; absent keeps upstream's 27) and the SPI attach helper. |
| `scripts/make-picasso-adt.py` | Adds `/arm-io/serial` (GENI UART 0x988000), `/pram` panic-log region, absolute `reg` values under a DRAM `ranges`, UFS and UFS-PHY `reg`s, and the Adreno/GMU/SMMU nodes. Each new node is inert unless the board asks for it. |
| `scripts/dump-picasso-panic.py` | Host-side reader for the `0x46554E4B` panic header over fastboot / adb. |
| `scripts/derive-a620-hwinit.py` | Derives the A620 hw_init register table from the pinned a6xx sources; needs `.cache/adreno-source`, so it is not part of the CI build. |
| `patches/unixnu-source-kernel.patch` | Rebuilt on the new pin: guards the `native_legacy_display_probe` call with `XNU_NATIVE_LEGACY_DISPLAY` (instead of the earlier no-op stub, which would collide with the archive's definition once legacy display is enabled) and adds the `XNU_CRYPTO_BOOTSTRAP="source"` prelink bypass. |

## Reference

- UniXNU: https://github.com/UniXNU/UniXNU
