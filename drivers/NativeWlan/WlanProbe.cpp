// SPDX-License-Identifier: BSD-2-Clause-Patent
// picasso (SM7250 "lito") WLAN bring-up probe. `sheng-wlan=<n>`:
//   1  map the icnss/WCSS block read-only, snapshot its identity window, validate the five regulator rails
//   2  read the apps-SMMU geometry and the SMR entry for stream 0xc0, compute the MSA page plan, touch MSA page 0
//   3  parse/verify a caller-supplied MBN and publish the WLFW/QMI handshake plan as far as it can be specified
//   4  publish the transport conclusion (integrated SNOC/CE; no PCIe link training or SDIO enumeration exists)
//   5  stop with the honest blocker: there is no Apple-side 802.11 stack for this Qualcomm CNSS/WCN part
// Every stage reports through native_wlan_* globals and a PICASSO_WLAN line. All work is synchronous and bounded;
// no loop waits for hardware and no stage can block the boot monitor for more than a few MMIO accesses.
// Board constants and their sources are in WlanBoard.h; the pure parsers/state machines are in WlanFirmware.c.
// Nothing here has run on hardware yet.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <pexpert/pexpert.h>
#include <string.h>
extern "C" {
#include "WlanBoard.h"
#include "WlanFirmware.h"
}
#include "WlanSmem.h"   // ported SMEM/SMP2P decoders (Trogdor, BSD-2)

extern "C" {
// Boot-monitor diagnostics. The names are the contract for the main session's boot report.
volatile uint32_t native_wlan_stage_req, native_wlan_stage, native_wlan_step;
volatile int native_wlan_error;
volatile uint32_t native_wlan_transport, native_wlan_attach;
volatile uint32_t native_wlan_regs[8];
volatile uint32_t native_wlan_rails;
volatile uint32_t native_wlan_smmu_idr[3];
volatile uint32_t native_wlan_smmu_slot = 0xffffffffu;
volatile uint32_t native_wlan_smmu_smr, native_wlan_smmu_s2cr;
volatile uint32_t native_wlan_sid = WLAN_SMMU_SID;
volatile uint64_t native_wlan_msa_base = WLAN_MSA_BASE, native_wlan_msa_size = WLAN_MSA_SIZE;
volatile uint32_t native_wlan_msa_pages, native_wlan_msa_word;
volatile uint32_t native_wlan_fw_present, native_wlan_fw_segments, native_wlan_fw_hash, native_wlan_fw_payload;
volatile uint32_t native_wlan_handshake_step;
volatile int native_wlan_handshake_error;

// Entry points declared before their definitions (XNU builds with -Wmissing-prototypes).
void native_wlan_set_firmware(const uint8_t *data, size_t size);
void native_wlan_probe(void);

// Firmware is supplied by the integrator (weak symbols or the setter); no vendor blob is stored in this tree.
extern const uint8_t native_wlan_firmware[] __attribute__((weak_import));
extern const uint32_t native_wlan_firmware_size __attribute__((weak_import));
}

namespace {
static const uint8_t *gFirmware;
static size_t gFirmwareSize;

struct Region {
    OSSharedPtr<IOMemoryMap> keep;
    uintptr_t va;
};

bool mapRegion(uint64_t phys, uint64_t length, Region &region) {
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    region.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    region.va = region.keep ? static_cast<uintptr_t>(region.keep->getVirtualAddress()) : 0;
    return region.va != 0;
}
uint32_t rd32(uintptr_t base, uint32_t reg) { return *reinterpret_cast<volatile uint32_t *>(base + reg); }

int fail(unsigned step, int code) {
    native_wlan_step = step;
    if (!native_wlan_error) native_wlan_error = code ? code : WLAN_ERR_IO;
    printf("PICASSO_WLAN fail step=%u rc=%d\n", step, code);
    return code ? code : WLAN_ERR_IO;
}

struct Session {
    Region icnss, smmu, msa;
};

// Stage 1: read-only identity. The register block has no public field map for this build, so the probe reports
// the first eight words verbatim and does not guess at names. No write of any kind.
int stage1(void *ctx) {
    Session *s = static_cast<Session *>(ctx);
    native_wlan_step = 11;
    if (!mapRegion(WLAN_ICNSS_BASE, WLAN_ICNSS_SIZE, s->icnss)) return fail(11, WLAN_ERR_NOMEM);
    for (unsigned i = 0; i < 8; ++i) native_wlan_regs[i] = rd32(s->icnss.va, 4 * i);

    native_wlan_step = 12;
    unsigned rails = 0;
    for (unsigned i = 0; i < WLAN_REGULATOR_COUNT; ++i) {
        const struct wlan_regulator *r = &wlan_regulators[i];
        const uint32_t target = wlan_regulator_target_uv(r);
        if (r->min_uv > r->max_uv) return fail(12, WLAN_ERR_INVAL);
        if (target && wlan_regulator_validate(r, target)) return fail(12, WLAN_ERR_INVAL);
        if (target) ++rails;
    }
    native_wlan_rails = rails;

    // There is no clock or interconnect specifier in the icnss DT node, and this tree has no PMIC/SPMI backend.
    // Nothing is written: see WlanBoard.h for why a guessed GCC write would be unsafe.
    native_wlan_step = 13;
    printf("PICASSO_WLAN stage1 icnss=%08x %08x %08x %08x %08x %08x %08x %08x rails=%u clocks=%u ic=%u\n",
           native_wlan_regs[0], native_wlan_regs[1], native_wlan_regs[2], native_wlan_regs[3],
           native_wlan_regs[4], native_wlan_regs[5], native_wlan_regs[6], native_wlan_regs[7],
           rails, static_cast<unsigned>(WLAN_CLOCK_SPECIFIERS),
           static_cast<unsigned>(WLAN_ICNSS_INTERCONNECTS));
    return 0;
}

// Stage 2: mapping plan. SMMU inspection is read-only; no SMR/S2CR is written. MSA page 0 is touched only after
// the page arithmetic accepts the fixed region.
int stage2(void *ctx) {
    Session *s = static_cast<Session *>(ctx);
    native_wlan_step = 21;
    if (!mapRegion(WLAN_APPS_SMMU_BASE, WLAN_APPS_SMMU_SIZE, s->smmu)) return fail(21, WLAN_ERR_NOMEM);
    const uint32_t idr0 = rd32(s->smmu.va, WLAN_SMMU_IDR0);
    const uint32_t idr2 = rd32(s->smmu.va, WLAN_SMMU_IDR2);
    native_wlan_smmu_idr[0] = idr0;
    native_wlan_smmu_idr[1] = rd32(s->smmu.va, WLAN_SMMU_IDR1);
    native_wlan_smmu_idr[2] = idr2;

    native_wlan_step = 22;
    // ARM SMMU: IDR0.SMRS(7:0) is the number of stream-match registers. Cap the scan at the 128 groups this
    // driver can name; a stream that is not mapped yet is reported as slot 0xffffffff, not treated as fatal.
    unsigned count = idr0 & 0xffu;
    if (count > 128) count = 128;
    uint32_t slot = 0xffffffffu, smr = 0, s2cr = 0;
    unsigned matches = 0;
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t candidate = rd32(s->smmu.va, WLAN_SMMU_SMR_BASE + 4 * i);
        if (!(candidate & 0x80000000u)) continue;
        if ((candidate & 0xffffu) != WLAN_SMMU_SID) continue;
        ++matches;
        if (slot == 0xffffffffu) {
            slot = i;
            smr = candidate;
            s2cr = rd32(s->smmu.va, WLAN_SMMU_S2CR_BASE + 4 * i);
        }
    }
    native_wlan_smmu_slot = slot;
    native_wlan_smmu_smr = smr;
    native_wlan_smmu_s2cr = s2cr;

    native_wlan_step = 23;
    const uint64_t pages = wlan_msa_pages(WLAN_MSA_BASE, WLAN_MSA_SIZE);
    if (!pages) return fail(23, WLAN_ERR_INVAL);
    native_wlan_msa_pages = static_cast<uint32_t>(pages);
    if (!mapRegion(WLAN_MSA_BASE, 0x1000, s->msa)) return fail(23, WLAN_ERR_NOMEM);
    native_wlan_msa_word = rd32(s->msa.va, 0);

    printf("PICASSO_WLAN stage2 idr=%08x/%08x/%08x sid=%x slot=%u matches=%u smr=%08x s2cr=%08x "
           "msa=%llx pages=%u word=%08x\n",
           native_wlan_smmu_idr[0], native_wlan_smmu_idr[1], native_wlan_smmu_idr[2], native_wlan_sid,
           native_wlan_smmu_slot, matches, native_wlan_smmu_smr, native_wlan_smmu_s2cr,
           (unsigned long long)native_wlan_msa_base, native_wlan_msa_pages, native_wlan_msa_word);
    return 0;
}

// Stage 3: firmware. A staged MBN is parsed and hash-checked; the WLFW request order is published. The actual
// PIL/SCM load is deliberately not faked: this tree has no secure-monitor call, and the picasso DT has no wlan PIL
// node, so the load step reports ENOTSUP. A missing or unloadable image is a diagnostic here, not a crash; the
// final stop belongs to stage 5.
struct HandshakeContext { int unused; };

int hsLoad(void *, const struct wlan_mbn *) {
    printf("PICASSO_WLAN stage3 PIL load unavailable: no SCM/TZ call in this tree and no wlan PIL node in "
           "picasso.dts (modem.mdt/modem_wlan_mem is the only staged modem image)\n");
    return WLAN_ERR_NOTSUP;
}
int hsRequest(void *, unsigned request, uint64_t, uint64_t) {
    printf("PICASSO_WLAN stage3 WLFW request %u skipped: no QMI/GLINK client\n", request);
    return WLAN_ERR_NOTSUP;
}
int hsWait(void *, unsigned indication, unsigned) {
    printf("PICASSO_WLAN stage3 WLFW indication %u skipped: no QMI/GLINK client\n", indication);
    return WLAN_ERR_NOTSUP;
}

int stage3(void *ctx) {
    (void)ctx;
    native_wlan_step = 31;
    size_t size = 0;
    const uint8_t *firmware = gFirmware && gFirmwareSize ? gFirmware : nullptr;
    if (!firmware && &native_wlan_firmware && &native_wlan_firmware_size && native_wlan_firmware_size) {
        firmware = native_wlan_firmware;
        size = native_wlan_firmware_size;
    } else if (firmware) {
        size = gFirmwareSize;
    }
    if (!firmware || !size) {
        native_wlan_fw_present = 0;
        native_wlan_handshake_step = WLAN_HS_LOAD;
        native_wlan_handshake_error = WLAN_ERR_NOENT;
        printf("PICASSO_WLAN stage3 no firmware staged: link native_wlan_firmware[]/native_wlan_firmware_size or "
               "call native_wlan_set_firmware() with the device's MBN (wlanmdsp/athwlan are not in this tree)\n");
        return 0;
    }
    native_wlan_fw_present = 1;
    native_wlan_step = 32;
    struct wlan_mbn mbn;
    int rc = wlan_mbn_parse(&mbn, firmware, size);
    if (rc) return fail(32, rc);
    rc = wlan_mbn_verify_hashes(&mbn, firmware, size);
    if (rc) return fail(32, rc);
    native_wlan_fw_segments = mbn.segments;
    native_wlan_fw_hash = mbn.hash_index == WLAN_MBN_NO_HASH ? 0 : mbn.hash_count;
    native_wlan_fw_payload = static_cast<uint32_t>(mbn.payload_size);

    native_wlan_step = 33;
    struct HandshakeContext hctx = {0};
    const struct wlan_handshake_ops ops = {&hctx, hsLoad, hsRequest, hsWait};
    struct wlan_handshake_state state;
    rc = wlan_handshake_run(&ops, &mbn, WLAN_MSA_BASE, WLAN_MSA_SIZE, 50, &state);
    native_wlan_handshake_step = state.step;
    native_wlan_handshake_error = state.error;
    printf("PICASSO_WLAN stage3 fw=%u bytes segs=%u hash=%u payload=%u load=not-run handshake_step=%u error=%d\n",
           static_cast<unsigned>(size), native_wlan_fw_segments, native_wlan_fw_hash, native_wlan_fw_payload,
           native_wlan_handshake_step, native_wlan_handshake_error);
    // A malformed image is fatal; an unavailable secure load is an environment fact reported by stage 5.
    return 0;
}

// Stage 4: transport. The evidence says integrated SNOC/CE, so there is no PCIe link to train and no SDIO
// function to enumerate. Listing the conclusion explicitly keeps the absence of an attach from looking like a bug.
int stage4(void *) {
    native_wlan_step = 41;
    const int transport = wlan_transport_classify(1, 0, 0, 1);
    native_wlan_transport = static_cast<uint32_t>(transport);
    native_wlan_attach = 0;   // 0 = not applicable
    if (transport != WLAN_BOARD_TRANSPORT) return fail(41, WLAN_ERR_INVAL);
    printf("PICASSO_WLAN stage4 transport=%s attach=none (no PCIe node, no SDIO WLAN child; integrated WCSS/SNOC)\n",
           wlan_transport_name(transport));
    return 0;
}

// Stage 5: the honest stop. There is no IO80211/mac80211 replacement in this tree and no WLAN data path.
int stage5(void *) {
    native_wlan_step = 51;
    printf("PICASSO_WLAN stage5 stop: no Apple 802.11 stack (IO80211) or mac80211-style data path for this "
           "Qualcomm CNSS/WCN part; WLAN cannot be brought up by this driver\n");
    return WLAN_ERR_NOTSUP;
}

void copyState(const struct wlan_probe_state &state) {
    native_wlan_stage = state.stage;
    native_wlan_step = state.step;
    native_wlan_error = state.error;
}
}  // namespace

extern "C" void native_wlan_set_firmware(const uint8_t *data, size_t size) {
    gFirmwareSize = size;
    __atomic_store_n(&gFirmware, data, __ATOMIC_RELEASE);
}

// Callable from the boot monitor. `sheng-wlan=<n>` selects the last stage to run; without it the probe does
// nothing. The entry point depends only on IOKit and its own pure helpers, so a boot monitor can declare it
// weak_import and link whether or not this driver is included.
// Stage 1 reads the WCSS block, and on this board that block hangs the bus when it is touched
// before its rail has been voted (the v266-v272 boot stalls were exactly this access). The MMIO
// therefore needs an explicit sheng-wlan-mmio=1 on top of sheng-wlan=<n>; without it the stage
// reports why it was skipped and the text-only stages still run.
static int stage1_gated(void *ctx) {
    unsigned mmio = 0;
    if (!PE_parse_boot_argn("sheng-wlan-mmio", &mmio, sizeof mmio) || mmio != 1) {
        printf("PICASSO_WLAN stage1 skipped: WCSS MMIO requires sheng-wlan-mmio=1 (rail not voted)\n");
        return 0;
    }
    return stage1(ctx);
}

// SMEM inventory: safe on every boot (SMEM lives in DDR and needs no WLAN rail). This is the first
// real bring-up rung from the migration plan - it tells us whether the modem partition, the SMP2P
// pair and the GLINK items exist, which is what the WLFW transport will later sit on.
static int smemRead(void *context, uint32_t offset, void *buffer, uint32_t bytes) {
    if (offset > 0x200000u || bytes > 0x200000u - offset) return -1;
    memcpy(buffer, (const uint8_t *)context + offset, bytes);
    return 0;
}

static void wlanSmemInventory(void) {
    IODeviceMemory *memory = IODeviceMemory::withRange(0x80900000, 0x200000);
    IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    if (!map) { printf("PICASSO_WLAN smem: window could not be mapped\n"); return; }
    struct cw_reader reader = { (void *)map->getVirtualAddress(), 0x200000u, smemRead };
    struct cw_smem smem = {};
    const int rc = cw_smem_layout(&reader, &smem);
    printf("PICASSO_WLAN smem: layout rc=%d global=%d modem=%d entries=%u\n",
           rc, smem.has_global, smem.has_modem, smem.count);
    if (smem.has_modem) {
        struct cw_item item = {};
        const int irc = cw_smem_find(&reader, &smem.modem, CW_SMP2P_IN, &item);
        printf("PICASSO_WLAN smem: smp2p item %u rc=%d off=%u bytes=%u\n",
               CW_SMP2P_IN, irc, item.offset, item.bytes);
        if (irc == CW_OK) {
            struct cw_smp2p s = {};
            const int src = cw_smp2p_read(&reader, &item, &s);
            printf("PICASSO_WLAN smem: smp2p rc=%d version=%u state=%u valid=%u\n",
                   src, s.version, s.state, s.valid);
        }
    }
    const uint32_t items[3] = { CW_GLINK_DESC, CW_GLINK_TX, CW_GLINK_RX };
    for (unsigned i = 0; i < 3; ++i) {
        struct cw_item item = {};
        const int irc = smem.has_modem ? cw_smem_find(&reader, &smem.modem, (uint16_t)items[i], &item) : CW_ABSENT;
        printf("PICASSO_WLAN smem: glink %u rc=%d off=%u bytes=%u\n", items[i], irc, item.offset, item.bytes);
    }
}

// PAS availability scan, probe only: SVC_PIL/PAS_IS_SUPPORTED (cmd 0x07) for every pas id.
// It reads no memory and touches no subsystem state (Adreno620Cp.cpp uses the same query as a
// control probe). The SMC convention is that file's: STD_CALL|SMC_64|OWNER_SIP, x6 carries the
// session id of an interrupted secure call (x0==1 = INTERRUPTED; a retry without x6 wedges PIL).
struct ScmReply { uint32_t a0, a1, a2, a3; uint64_t a6; };

static ScmReply wlanScmCall(uint32_t function, uint32_t arginfo, uint64_t arg0) {
    register uint64_t x0 __asm__("x0") = function;
    register uint64_t x1 __asm__("x1") = arginfo;
    register uint64_t x2 __asm__("x2") = arg0;
    register uint64_t x3 __asm__("x3") = 0;
    register uint64_t x4 __asm__("x4") = 0;
    register uint64_t x5 __asm__("x5") = 0;
    register uint64_t x6 __asm__("x6") = 0;
    register uint64_t x7 __asm__("x7") = 0;
    __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3),
        "+r"(x6), "+r"(x7)
        : "r"(x4), "r"(x5)
        : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16",
          "x17", "memory");
    return ScmReply{uint32_t(x0), uint32_t(x1), uint32_t(x2), uint32_t(x3), x6};
}

// Read-only raw block access exported by NativeUfs (additive integration hook).
extern "C" uint32_t native_ufs_raw_max_blocks(void);
extern "C" int native_ufs_raw_read(uint32_t lun, uint64_t lba, uint32_t blocks, void *dst);

// Modem partition identity probe. picasso stores the modem FAT on LUN 4 starting at LBA 61606
// (rawprogram4.xml: physical_partition_number=4, start_sector=61606, SECTOR_SIZE_IN_BYTES=4096).
// Reading one block tells us whether the kernel can reach partitions outside the root window -
// the prerequisite for loading MPSS firmware at runtime without touching silenzio's root path.
static constexpr uint32_t kModemLun = 4;
static constexpr uint64_t kModemLba = 61606;

static void wlanModemPartitionProbe(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("sheng-wlan-ufs", &enabled, sizeof enabled) || enabled != 1) return;
    const uint32_t maxBlocks = native_ufs_raw_max_blocks();
    uint8_t *sector = static_cast<uint8_t *>(IOMalloc(4096));
    if (!sector) return;
    memset(sector, 0, 4096);
    const int rc = native_ufs_raw_read(kModemLun, kModemLba, 1, sector);
    const unsigned bps = sector[11] | (sector[12] << 8);
    const unsigned spc = sector[13];
    const unsigned rsv = sector[14] | (sector[15] << 8);
    const unsigned nfat = sector[16];
    const unsigned rootEnt = sector[17] | (sector[18] << 8);
    unsigned total16 = sector[19] | (sector[20] << 8);
    const unsigned total32 = sector[32] | (sector[33] << 8) | (sector[34] << 16) | (static_cast<unsigned>(sector[35]) << 24);
    if (!total16) total16 = total32;
    printf("PICASSO_WLAN ufs modem lun=%u lba=%llu rc=%d maxblk=%u oem=%.5s bps=%u spc=%u rsv=%u nfat=%u root=%u total=%u\n",
           kModemLun, static_cast<unsigned long long>(kModemLba), rc, maxBlocks,
           reinterpret_cast<const char *>(sector + 3), bps, spc, rsv, nfat, rootEnt, total16);
    IOFree(sector, 4096);
}

static void wlanPasScan(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("sheng-wlan-pas", &enabled, sizeof enabled) || enabled != 1) return;
    constexpr uint32_t kStdSipSmc64 = 0x42000000u;   // STD_CALL | SMC_64 | OWNER_SIP
    constexpr uint32_t kSvcPil = 0x02u, kPasIsSupported = 0x07u;
    for (uint32_t id = 0; id <= 20; ++id) {
        const ScmReply r = wlanScmCall(kStdSipSmc64 | (kSvcPil << 8) | kPasIsSupported, 0x01u, id);
        printf("PICASSO_WLAN pas: id=%u a0=%08x a1=%08x\n", id, r.a0, r.a1);
    }
}

extern "C" void native_wlan_probe(void) {
    unsigned stage = 0;
    if (!PE_parse_boot_argn("sheng-wlan", &stage, sizeof stage) || stage < 1) return;
    if (native_wlan_stage_req) return;
    if (stage > WLAN_PROBE_STAGE_MAX) stage = WLAN_PROBE_STAGE_MAX;
    native_wlan_stage_req = stage;

    // DDR-only inventory first: it cannot hang a bus and is the ground truth for the transport.
    wlanSmemInventory();
    // PAS scan (opt-in): tells us which subsystem ids the secure monitor can boot.
    wlanPasScan();
    // Modem partition reachability (opt-in): proves LUN 4 is readable from the kernel.
    wlanModemPartitionProbe();

    Session session = {};
    const struct wlan_probe_ops ops = {&session, stage1_gated, stage2, stage3, stage4, stage5};
    struct wlan_probe_state state;
    const int rc = wlan_probe_run(&ops, stage, &state);
    copyState(state);
    printf("PICASSO_WLAN end stage=%u step=%u error=%d transport=%s\n", native_wlan_stage, native_wlan_step,
           native_wlan_error, wlan_transport_name(static_cast<int>(native_wlan_transport)));
    (void)rc;
}

// The integrator stages firmware through native_wlan_set_firmware(); the weak externs above bind
// only when a blob was compiled into the image, which this tree never does. Define the absent-blob
// objects so the final link resolves the data symbols; size 0 keeps stage3 on the honest
// "no firmware staged" path.
extern "C" {
const uint8_t native_wlan_firmware[1] = {0};
const uint32_t native_wlan_firmware_size = 0;
}
