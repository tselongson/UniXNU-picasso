/* SPDX-License-Identifier: MIT */
/*
 * The two modem services the picasso WLAN stack calls before and while it takes
 * MPSS through PAS: the application processor's mailbox to the always-on
 * processor (AOP QMP) and the RPMh level votes for the DSP's rails.
 *
 * Both are ports of the code this device's HakiOS bring-up used
 * (drivers/modem/hakios.mpss.c: mailbox_open/mailbox_send and rail_position/
 * rails), with the SM7250 board constants and without the rest of that loader:
 * PicassoWlan.cpp already carries its own PAS sequence, it only needs these two
 * pieces to actually cast the votes. Returning true from a stub, as this file
 * replaces, made the WLAN stack believe cx.lvl/mss.lvl were raised while the
 * DSP was still at retention.
 *
 * Everything here runs before the modem is out of reset, so nothing waits on
 * anything the DSP owns; every wait is bounded and every failure is reported.
 */

#include <IOKit/IOLib.h>
#include <kern/locks.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" void nanoseconds_to_absolutetime(uint64_t nanoseconds, uint64_t *result);
extern "C" void machine_delay_until(uint64_t interval, uint64_t deadline);
extern "C" uint64_t mach_absolute_time(void);

namespace {

/* ---- register windows ---------------------------------------------------- */

struct Block {
    const char *name;
    uint32_t phys;
    uint32_t size;
    vm_offset_t base;
};

Block gAoss = { "mailbox", 0x0c300000u, 0x400u, 0 };
Block gIpcc = { "ipcc", 0x00408000u, 0x1000u, 0 };
Block gRsc = { "rsc", 0x18220000u, 0x10000u, 0 };

inline uint32_t rd(vm_offset_t base, uint32_t offset)
{
    return *reinterpret_cast<volatile uint32_t *>(base + offset);
}

inline void wr(vm_offset_t base, uint32_t offset, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

bool need(Block *block)
{
    if (block->base == 0) {
        block->base = ml_io_map(block->phys, block->size);
    }
    if (block->base == 0) {
        printf("PICASSO_WLAN qmp: %s window 0x%x did not map\n", block->name, block->phys);
        return false;
    }
    return true;
}

void sleepUs(uint32_t us)
{
    uint64_t interval = 0;
    nanoseconds_to_absolutetime(static_cast<uint64_t>(us) * 1000ull, &interval);
    machine_delay_until(interval, mach_absolute_time() + interval);
}

lck_grp_t *gLockGroup;
lck_mtx_t *gMailboxLock;
lck_mtx_t *gRscLock;

void locksInit()
{
    if (gLockGroup) return;
    gLockGroup = lck_grp_alloc_init("picasso-wlan-qmp", LCK_GRP_ATTR_NULL);
    gMailboxLock = lck_mtx_alloc_init(gLockGroup, LCK_ATTR_NULL);
    gRscLock = lck_mtx_alloc_init(gLockGroup, LCK_ATTR_NULL);
}

/* ---- the mailbox to the always-on processor (Linux qmp_open/qmp_send) ---- */

constexpr uint32_t kQmpMagic = 0x00;
constexpr uint32_t kQmpMagicValue = 0x4d41494cu;
constexpr uint32_t kQmpVersion = 0x04;
constexpr uint32_t kQmpTheirLink = 0x0c;
constexpr uint32_t kQmpTheirLinkAck = 0x10;
constexpr uint32_t kQmpTheirChannel = 0x14;
constexpr uint32_t kQmpTheirChannelAck = 0x18;
constexpr uint32_t kQmpOurLink = 0x24;
constexpr uint32_t kQmpOurLinkAck = 0x28;
constexpr uint32_t kQmpOurChannel = 0x2c;
constexpr uint32_t kQmpOurChannelAck = 0x30;
constexpr uint32_t kQmpOurBoxSize = 0x34;
constexpr uint32_t kQmpOurBox = 0x38;
constexpr uint32_t kQmpUp = 0x0000ffffu;
constexpr uint32_t kQmpDown = 0xffff0000u;
constexpr uint32_t kQmpMessage = 64;

constexpr uint32_t kIpccSend = 0x0c;
constexpr uint32_t kIpccAop = 0u;

uint32_t gOurBox;

uint32_t boxGet(const char *name, uint32_t offset)
{
    const uint32_t value = rd(gAoss.base, offset);
    printf("PICASSO_WLAN qmp: mailbox %s (0x%x) = 0x%x\n", name, gAoss.phys + offset, value);
    return value;
}

void boxPut(const char *name, uint32_t offset, uint32_t value)
{
    printf("PICASSO_WLAN qmp: mailbox %s (0x%x) <- 0x%x\n", name, gAoss.phys + offset, value);
    wr(gAoss.base, offset, value);
}

void ringAop()
{
    wr(gIpcc.base, kIpccSend, kIpccAop << 16 | 0u);
}

bool mailboxWait(uint32_t offset, uint32_t value, uint32_t ms)
{
    for (uint32_t i = 0; i < ms * 10u; i++) {
        if (rd(gAoss.base, offset) == value) return true;
        sleepUs(100);
    }
    return rd(gAoss.base, offset) == value;
}

bool mailboxOpen()
{
    if (!need(&gAoss) || !need(&gIpcc)) return false;
    if (boxGet("magic", kQmpMagic) != kQmpMagicValue ||
        boxGet("version", kQmpVersion) != 1u) {
        printf("PICASSO_WLAN qmp: not a mailbox this knows\n");
        return false;
    }
    const uint32_t box = boxGet("our mailbox", kQmpOurBox);
    const uint32_t size = boxGet("our mailbox's size", kQmpOurBoxSize);
    if (size < kQmpMessage + 4u || box < 0x3cu || box + size > gAoss.size || (box & 3u)) {
        printf("PICASSO_WLAN qmp: our mailbox is not where a message fits\n");
        return false;
    }
    if (rd(gAoss.base, kQmpOurLink) == kQmpUp && rd(gAoss.base, kQmpOurLinkAck) == kQmpUp &&
        rd(gAoss.base, kQmpOurChannel) == kQmpUp && rd(gAoss.base, kQmpTheirChannel) == kQmpUp &&
        rd(gAoss.base, kQmpOurChannelAck) == kQmpUp) {
        printf("PICASSO_WLAN qmp: the mailbox is open already\n");
        gOurBox = box;
        return true;
    }
    boxPut("their link, seen by us", kQmpTheirLinkAck, rd(gAoss.base, kQmpTheirLink));
    boxPut("our link", kQmpOurLink, kQmpUp);
    ringAop();
    if (!mailboxWait(kQmpOurLinkAck, kQmpUp, 1000)) {
        printf("PICASSO_WLAN qmp: AOP does not acknowledge our link within a second\n");
        boxPut("our link", kQmpOurLink, kQmpDown);
        ringAop();
        return false;
    }
    boxPut("our channel", kQmpOurChannel, kQmpUp);
    ringAop();
    if (!mailboxWait(kQmpTheirChannel, kQmpUp, 1000)) {
        printf("PICASSO_WLAN qmp: AOP does not open its channel within a second\n");
        boxPut("our channel", kQmpOurChannel, kQmpDown);
        boxPut("our link", kQmpOurLink, kQmpDown);
        ringAop();
        return false;
    }
    boxPut("their channel, seen by us", kQmpTheirChannelAck, kQmpUp);
    ringAop();
    if (!mailboxWait(kQmpOurChannelAck, kQmpUp, 1000)) {
        printf("PICASSO_WLAN qmp: AOP does not acknowledge our channel within a second\n");
        boxPut("our channel", kQmpOurChannel, kQmpDown);
        boxPut("our link", kQmpOurLink, kQmpDown);
        ringAop();
        return false;
    }
    printf("PICASSO_WLAN qmp: the mailbox to the always-on processor is open\n");
    gOurBox = box;
    return true;
}

/* ---- RPMh: the DSP's rails ------------------------------------------------ */

constexpr uint32_t kCmdDbPhys = 0x80860000u;   /* DT cmd-db memory region */
constexpr uint32_t kCmdDbSize = 0x20000u;

struct CmdDbSlave {
    uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct CmdDbHeader {
    uint32_t version;
    uint8_t magic[4];
    CmdDbSlave slave[8];
    uint32_t checksum, reserved;
    uint8_t data[];
};
struct CmdDbEntry {
    char id[8];
    uint32_t priority[2];
    uint32_t address;
    uint16_t length, offset;
};

const CmdDbHeader *gCmdDb;

constexpr uint32_t kRscTcsBase = 0xd00;
constexpr uint32_t kRscIrqStatus = kRscTcsBase + 0x04;
constexpr uint32_t kRscIrqClear = kRscTcsBase + 0x08;
constexpr uint32_t kTcsWaitForDone = 0x10;
constexpr uint32_t kTcsControl = 0x14;
constexpr uint32_t kTcsStatus = 0x18;
constexpr uint32_t kTcsEnabled = 0x1c;
constexpr uint32_t kTcsActiveMode = 1u << 16;
constexpr uint32_t kTcsTrigger = 1u << 24;
constexpr uint32_t kMsgIdWriteWaited = 8u | (1u << 8) | (1u << 16);
constexpr uint32_t kRpmhTcs = 0;

uint32_t tcsOffset(uint32_t index, uint32_t within)
{
    return kRscTcsBase + 672u * index + within;
}

uint32_t tcsCommand(uint32_t command, uint32_t within)
{
    return tcsOffset(kRpmhTcs, 0x30u + 20u * command + within);
}

/* ---- READ-ONLY TCS survey (copied from raphael xnu_files_bsd_dev_hakios.wifi.c
 *      :1005-1045, the step that prints the sequencer's TCSs).  Gated by
 *      wlan_rails_probe=1, default off, reads registers only -- never writes,
 *      so it cannot freeze the panel.  Purpose: establish whether TCS1 is free
 *      on picasso before porting raphael's TCS1 bulk-write (wifi_rpmh_write). */
void surveyTcs(void)
{
    constexpr uint32_t kRscId = 0x000u;       /* raphael RSC_ID  :993 */
    constexpr uint32_t kRscConfig = 0x00cu;   /* raphael RSC_CONFIG :994 */
    static const char *const kinds[] = {
        "at once", "at once", "asleep", "asleep", "asleep", "awake", "awake", "awake"
    };

    if (!need(&gRsc)) return;
    const uint32_t id = rd(gRsc.base, kRscId);
    const uint32_t config = rd(gRsc.base, kRscConfig);
    if (((id >> 16) & 0xffu) >= 3u) {
        printf("PICASSO_WLAN qmp: rsc sequencer version %u: layout not read\n", (id >> 16) & 0xffu);
        return;
    }
    const unsigned count = (config >> 12) & 0x3fu;
    const unsigned commands = (config >> 27) & 0x1fu;
    printf("PICASSO_WLAN qmp: rsc has %u TCSs of %u commands (id %#x config %#x)\n",
        count, commands, id, config);
    for (unsigned t = 0; t < count && t < 8; t++) {
        const uint32_t enabled = rd(gRsc.base, tcsOffset(t, kTcsEnabled));
        printf("PICASSO_WLAN qmp: TCS %u (%s): enabled %#x, control %#x, status %#x\n",
            t, kinds[t], enabled, rd(gRsc.base, tcsOffset(t, kTcsControl)),
            rd(gRsc.base, tcsOffset(t, kTcsStatus)));
        for (unsigned c = 0; c < commands && c < 16; c++) {
            const uint32_t flags = rd(gRsc.base, tcsOffset(t, 0x30u + 20u * c));
            const uint32_t address = rd(gRsc.base, tcsOffset(t, 0x30u + 20u * c + 4u));
            const uint32_t value = rd(gRsc.base, tcsOffset(t, 0x30u + 20u * c + 8u));
            if (address == 0 && !(enabled & (1u << c))) continue;
            printf("PICASSO_WLAN qmp:   TCS %u command %u%s: addr %#x = %#x, flags %#x\n",
                t, c, (enabled & (1u << c)) ? "" : " (not enabled)", address, value, flags);
        }
    }
}

/* The level position RPMh takes for that rail and level; -1: not found. */
int railPosition(const char *name, uint32_t level, uint32_t *address)
{
    char id[8] = { 0 };

    if (!gCmdDb) {
        gCmdDb = reinterpret_cast<const CmdDbHeader *>(ml_io_map(kCmdDbPhys, kCmdDbSize));
        if (!gCmdDb) {
            printf("PICASSO_WLAN qmp: the command database did not map\n");
            return -1;
        }
    }
    if (!(gCmdDb->magic[0] == 0xdb && gCmdDb->magic[1] == 0x30 &&
          gCmdDb->magic[2] == 0x03 && gCmdDb->magic[3] == 0x0c)) {
        printf("PICASSO_WLAN qmp: the boot loader's command database is not there\n");
        return -1;
    }
    strlcpy(id, name, sizeof(id));
    for (unsigned s = 0; s < 8; s++) {
        const CmdDbSlave *slave = &gCmdDb->slave[s];
        const CmdDbEntry *entry = reinterpret_cast<const CmdDbEntry *>(
            gCmdDb->data + slave->header_offset);

        if (slave->id == 0 || slave->count > 512) continue;
        for (unsigned i = 0; i < slave->count; i++) {
            if (memcmp(entry[i].id, id, sizeof(id)) != 0) continue;
            if (static_cast<unsigned>(slave->data_offset) + entry[i].offset + entry[i].length >
                kCmdDbSize - 0x1000u) {
                return -1;
            }
            const uint8_t *data = gCmdDb->data + slave->data_offset + entry[i].offset;
            char levels[160];
            int at = snprintf(levels, sizeof(levels), "%s at 0x%x, levels:", name, entry[i].address);
            for (unsigned l = 0; l < entry[i].length / 2u && l < 16; l++) {
                const unsigned value = data[2 * l] | data[2 * l + 1] << 8;
                if (l > 0 && value == 0) break;
                at += snprintf(levels + at, sizeof(levels) - static_cast<size_t>(at), " %u", value);
            }
            printf("PICASSO_WLAN qmp: RPMh has %s\n", levels);
            for (unsigned l = 0; l < entry[i].length / 2u && l < 16; l++) {
                if (static_cast<uint32_t>(data[2 * l] | data[2 * l + 1] << 8) >= level) {
                    *address = entry[i].address;
                    return static_cast<int>(l);
                }
            }
            return -1;
        }
    }
    printf("PICASSO_WLAN qmp: RPMh has no \"%s\"\n", name);
    return -1;
}

/* cmd-db base address of a named rail, with raphael's LDO-class check
 * (icnss_power :4158: (entry->address >> 16 & 0xf) == 4).  Unlike
 * railPosition() this does not interpret the level table; icnss_power writes
 * the desired millivolts directly. */
bool railAddress(const char *name, uint32_t *address)
{
    char id[8] = { 0 };
    if (!gCmdDb) {
        gCmdDb = reinterpret_cast<const CmdDbHeader *>(ml_io_map(kCmdDbPhys, kCmdDbSize));
        if (!gCmdDb) return false;
    }
    if (!(gCmdDb->magic[0] == 0xdb && gCmdDb->magic[1] == 0x30 &&
          gCmdDb->magic[2] == 0x03 && gCmdDb->magic[3] == 0x0c)) return false;
    strlcpy(id, name, sizeof(id));
    for (unsigned s = 0; s < 8; s++) {
        const CmdDbSlave *slave = &gCmdDb->slave[s];
        const CmdDbEntry *entry = reinterpret_cast<const CmdDbEntry *>(gCmdDb->data + slave->header_offset);
        if (slave->id == 0 || slave->count > 512) continue;
        for (unsigned i = 0; i < slave->count; i++) {
            if (memcmp(entry[i].id, id, sizeof(id)) != 0) continue;
            if (static_cast<unsigned>(slave->data_offset) + entry[i].offset + entry[i].length >
                kCmdDbSize - 0x1000u) return false;
            if (((entry[i].address >> 16) & 0xfu) != 4) return false; /* not an LDO-class rail */
            *address = entry[i].address;
            return true;
        }
    }
    printf("PICASSO_WLAN qmp: cmd-db has no LDO rail \"%s\"\n", name);
    return false;
}

/* One RPMh write through TCS 0: the message id, address and value, enabled,
 * put in active mode and triggered, then its bit in the completion status
 * waited for. Same shape as the two-rail request in hakios_mpss_rails(). */
bool rpmhWrite(uint32_t address, uint32_t value)
{
    if (!address) return false;
    locksInit();
    if (!need(&gRsc)) return false;
    lck_mtx_lock(gRscLock);
    if (rd(gRsc.base, tcsOffset(kRpmhTcs, kTcsStatus)) == 0) {
        printf("PICASSO_WLAN qmp: TCS busy, 0x%x = %u not sent\n", address, value);
        lck_mtx_unlock(gRscLock);
        return false;
    }
    wr(gRsc.base, kRscIrqClear, 1u << kRpmhTcs);
    wr(gRsc.base, tcsCommand(0, 0), kMsgIdWriteWaited);
    wr(gRsc.base, tcsCommand(0, 4), address);
    wr(gRsc.base, tcsCommand(0, 8), value);
    wr(gRsc.base, tcsOffset(kRpmhTcs, kTcsWaitForDone), 1);
    wr(gRsc.base, tcsOffset(kRpmhTcs, kTcsEnabled), 1);
    wr(gRsc.base, tcsOffset(kRpmhTcs, kTcsControl),
        rd(gRsc.base, tcsOffset(kRpmhTcs, kTcsControl)) & ~kTcsTrigger);
    wr(gRsc.base, tcsOffset(kRpmhTcs, kTcsControl),
        rd(gRsc.base, tcsOffset(kRpmhTcs, kTcsControl)) | kTcsActiveMode);
    wr(gRsc.base, tcsOffset(kRpmhTcs, kTcsControl),
        rd(gRsc.base, tcsOffset(kRpmhTcs, kTcsControl)) | kTcsTrigger);
    uint32_t done = 0;
    uint64_t deadline = 0;
    const uint64_t began = mach_absolute_time();
    nanoseconds_to_absolutetime(100ull * 1000ull * 1000ull, &deadline);
    while (mach_absolute_time() - began < deadline) {
        done = rd(gRsc.base, kRscIrqStatus) & (1u << kRpmhTcs);
        if (done) break;
        sleepUs(50);
    }
    printf("PICASSO_WLAN qmp: RPMh 0x%x = %u: %s (status 0x%x)\n", address, value,
        done ? "done" : "no answer", rd(gRsc.base, tcsCommand(0, 12)));
    wr(gRsc.base, kRscIrqClear, 1u << kRpmhTcs);
    wr(gRsc.base, tcsOffset(kRpmhTcs, kTcsControl),
        rd(gRsc.base, tcsOffset(kRpmhTcs, kTcsControl)) & ~(kTcsTrigger | kTcsActiveMode));
    wr(gRsc.base, tcsOffset(kRpmhTcs, kTcsEnabled), 0);
    lck_mtx_unlock(gRscLock);
    return done != 0;
}

/* ---- TCS1 bulk write, copied verbatim from raphael
 *      xnu_files_bsd_dev_hakios.wifi.c wifi_rpmh_write() :1052-1092.
 *
 * The existing rpmhWrite() above uses TCS0 (ACTIVE), whose direct programming
 * is documented at :378-384 to replace the display MDSS's CX vote and freeze
 * the panel (measured in v310/v312).  Raphael deliberately uses TCS1 for the
 * Wi-Fi rail votes and keeps TCS0 only for the IRQ/completion handshake.
 * On-device survey (wlan_rails_probe=1, 648-tcs-probe.cap) confirmed TCS1 is
 * free here: status 0x1, enabled 0, command flags 0x10108 -- identical to
 * raphael.  This is a literal port: same register sequence, same flags, same
 * completion poll.  The only substitution is our gRsc/tcsOffset/rd/wr/sleepUs
 * for raphael's rsc/RSC_TCS/rd/wr/sleep_us. */
bool wifiRpmhWriteTcs1(const uint32_t *addresses, const uint32_t *values, unsigned count)
{
    if (!need(&gRsc)) return false;
    if (count == 0 || count > 16) return false;
    const uint32_t all = (1u << count) - 1u;
    const uint32_t tcs1 = static_cast<uint32_t>(tcsOffset(1, 0)); /* RSC_TCS(1) */
    const uint32_t tcs0 = static_cast<uint32_t>(tcsOffset(0, 0)); /* RSC_TCS(0) */
    if (rd(gRsc.base, tcs1 + kTcsStatus) == 0) return false;
    const uint32_t irq = rd(gRsc.base, tcs0);
    wr(gRsc.base, tcs1 + kTcsControl, rd(gRsc.base, tcs1 + kTcsControl) & ~(1u << 24));
    wr(gRsc.base, tcs1 + kTcsControl, rd(gRsc.base, tcs1 + kTcsControl) & ~(1u << 16));
    wr(gRsc.base, tcs0 + 8, 2);
    for (unsigned i = 0; i < count; i++) {
        const uint32_t cmd = static_cast<uint32_t>(tcsOffset(1, 0x30u + 20u * i));
        wr(gRsc.base, cmd, kMsgIdWriteWaited);        /* 0x10108, same as raphael */
        wr(gRsc.base, cmd + 4, addresses[i]);
        wr(gRsc.base, cmd + 8, values[i]);
    }
    wr(gRsc.base, tcs1 + 0x10, all);
    wr(gRsc.base, tcs1 + kTcsEnabled, all);
    wr(gRsc.base, tcs0, irq | 2);
    __asm__ volatile("dsb sy" ::: "memory");          /* raphael settle() */
    wr(gRsc.base, tcs1 + kTcsControl, 1u << 16);
    wr(gRsc.base, tcs1 + kTcsControl, (1u << 16) | (1u << 24));
    uint32_t done = 0;
    for (unsigned n = 0; n < 2000 && !done; n++) {
        done = rd(gRsc.base, tcs0 + 4) & 2;
        if (!done) sleepUs(100);
    }
    printf("PICASSO_WLAN qmp: RPMh TCS1 %u commands %s; first/last status %#x/%#x\n",
        count, done ? "accepted" : "timed out",
        rd(gRsc.base, tcsOffset(1, 0x30u)), rd(gRsc.base, tcsOffset(1, 0x30u + 20u * (count - 1))));
    wr(gRsc.base, tcs1 + kTcsControl, rd(gRsc.base, tcs1 + kTcsControl) & ~(1u << 24));
    wr(gRsc.base, tcs1 + kTcsControl, rd(gRsc.base, tcs1 + kTcsControl) & ~(1u << 16));
    wr(gRsc.base, tcs1 + kTcsEnabled, 0);
    wr(gRsc.base, tcs1 + 0x10, 0);
    wr(gRsc.base, tcs0 + 8, 2);
    wr(gRsc.base, tcs0, irq);
    return done != 0;
}

} /* namespace */

/* The RPMh writes below bypass the RPMh client model: they program the ACTIVE
 * TCS directly, so they replace whatever aggregate the other clients (the
 * display's MDSS, which sits on CX) had voted, instead of adding a vote. v310
 * and v312 measured the effect: the panel stopped taking new frames
 * (submit frozen at 5) as soon as these ran, and v295 booted the same modem
 * through PAS with no votes at all. So they are off unless wlan_rails=1 says
 * the bootloader left the rails too low on this boot. */
static bool
railsWanted(void)
{
    uint32_t value = 0;
    return PE_parse_boot_argn("wlan_rails", &value, sizeof(value)) && value != 0;
}

/* The two rails at that level (Linux's proxy request), or given back (level 0).
 * True: RPMh has said done. */
extern "C" bool
hakios_mpss_rails(uint32_t level)
{
    if (!railsWanted()) {
        printf("PICASSO_WLAN qmp: rails left as the bootloader set them (wlan_rails=1 to vote)\n");
        return true;
    }
    static const char *const names[2] = { "cx.lvl", "mss.lvl" };
    uint32_t addresses[2] = { 0, 0 }, values[2] = { 0, 0 }, done = 0;

    locksInit();
    for (unsigned r = 0; r < 2; r++) {
        if (level == 0) railPosition(names[r], 0, &addresses[r]);
        const int position = level == 0 ? 0 : railPosition(names[r], level, &addresses[r]);
        if (position < 0 || addresses[r] == 0) {
            printf("PICASSO_WLAN qmp: no position for %s at level %u: the rails are not asked\n",
                names[r], level);
            return false;
        }
        values[r] = static_cast<uint32_t>(position);
    }
    if (!need(&gRsc)) return false;
    lck_mtx_lock(gRscLock);
    if (rd(gRsc.base, tcsOffset(kRpmhTcs, kTcsStatus)) == 0) {
        printf("PICASSO_WLAN qmp: TCS %u is busy (status 0x%x control 0x%x): rails not asked\n",
            kRpmhTcs, rd(gRsc.base, tcsOffset(kRpmhTcs, kTcsStatus)),
            rd(gRsc.base, tcsOffset(kRpmhTcs, kTcsControl)));
        lck_mtx_unlock(gRscLock);
        return false;
    }
    printf("PICASSO_WLAN qmp: asking RPMh, TCS %u, cx.lvl (0x%x) = %u, mss.lvl (0x%x) = %u (level %u)\n",
        kRpmhTcs, addresses[0], values[0], addresses[1], values[1], level);
    wr(gRsc.base, kRscIrqClear, 1u << kRpmhTcs);
    for (unsigned r = 0; r < 2; r++) {
        wr(gRsc.base, tcsCommand(r, 0), kMsgIdWriteWaited);
        wr(gRsc.base, tcsCommand(r, 4), addresses[r]);
        wr(gRsc.base, tcsCommand(r, 8), values[r]);
    }
    wr(gRsc.base, tcsOffset(kRpmhTcs, kTcsWaitForDone), 3);
    wr(gRsc.base, tcsOffset(kRpmhTcs, kTcsEnabled), 3);
    wr(gRsc.base, tcsOffset(kRpmhTcs, kTcsControl),
        rd(gRsc.base, tcsOffset(kRpmhTcs, kTcsControl)) & ~kTcsTrigger);
    wr(gRsc.base, tcsOffset(kRpmhTcs, kTcsControl),
        rd(gRsc.base, tcsOffset(kRpmhTcs, kTcsControl)) | kTcsActiveMode);
    wr(gRsc.base, tcsOffset(kRpmhTcs, kTcsControl),
        rd(gRsc.base, tcsOffset(kRpmhTcs, kTcsControl)) | kTcsTrigger);
    const uint64_t began = mach_absolute_time();
    uint64_t deadline = 0;
    nanoseconds_to_absolutetime(100ull * 1000ull * 1000ull, &deadline);
    while (mach_absolute_time() - began < deadline) {
        done = rd(gRsc.base, kRscIrqStatus) & (1u << kRpmhTcs);
        if (done) break;
        sleepUs(50);
    }
    printf("PICASSO_WLAN qmp: RPMh %s; command status 0x%x 0x%x, TCS status 0x%x\n",
        done ? "HAS DONE IT" : "HAS NOT SAID DONE",
        rd(gRsc.base, tcsCommand(0, 12)), rd(gRsc.base, tcsCommand(1, 12)),
        rd(gRsc.base, tcsOffset(kRpmhTcs, kTcsStatus)));
    wr(gRsc.base, kRscIrqClear, 1u << kRpmhTcs);
    wr(gRsc.base, tcsOffset(kRpmhTcs, kTcsControl),
        rd(gRsc.base, tcsOffset(kRpmhTcs, kTcsControl)) & ~(kTcsTrigger | kTcsActiveMode));
    wr(gRsc.base, tcsOffset(kRpmhTcs, kTcsEnabled), 0);
    lck_mtx_unlock(gRscLock);
    return done != 0;
}

/* A message to the always-on processor (Linux's qmp_send()). */
extern "C" bool
hakios_aop_mailbox_send(const char *text)
{
    uint32_t words[kQmpMessage / 4] = { 0 };
    bool sent = false;

    if (!text) return false;
    locksInit();
    lck_mtx_lock(gMailboxLock);
    if (gOurBox == 0 && !mailboxOpen()) goto out;
    strlcpy(reinterpret_cast<char *>(words), text, sizeof(words));
    printf("PICASSO_WLAN qmp: telling the AOP \"%s\"\n", text);
    if (rd(gAoss.base, gOurBox) != 0) {
        printf("PICASSO_WLAN qmp: our mailbox still holds %u bytes: not sent\n",
            rd(gAoss.base, gOurBox));
        goto out;
    }
    for (unsigned i = 0; i < kQmpMessage / 4; i++) {
        wr(gAoss.base, gOurBox + 4 + 4 * i, words[i]);
    }
    wr(gAoss.base, gOurBox, kQmpMessage);
    (void)rd(gAoss.base, gOurBox);
    ringAop();
    if (!mailboxWait(gOurBox, 0, 1000)) {
        printf("PICASSO_WLAN qmp: the AOP does not take the message within a second\n");
        wr(gAoss.base, gOurBox, 0);
        goto out;
    }
    printf("PICASSO_WLAN qmp: the AOP has taken it\n");
    sent = true;
out:
    lck_mtx_unlock(gMailboxLock);
    return sent;
}

/* The WLAN power-on path asks for one ARC level on one rail
 * ("cx.lvl"/"mss.lvl" at 0x180); PicassoWlan.cpp's sweet_rpmh_arc_level() forwards
 * here. Same request hakios_mpss_rails(384) makes for both, one resource at a
 * time. */
bool mpssProxyTcs1(void); /* defined below; TCS1 proxy vote for wlan_rails=2 */
extern "C" int
hakios_wlan_arc_level(const char *res, unsigned level)
{
    uint32_t address = 0;

    /* READ-ONLY TCS survey, gated, default off.  Runs once per boot to decide
     * whether raphael's TCS1 path can be ported.  No register writes. */
    {
        uint32_t probe = 0;
        static bool surveyed = false;
        if (!surveyed && PE_parse_boot_argn("wlan_rails_probe", &probe, sizeof(probe)) && probe) {
            surveyed = true;
            surveyTcs();
        }
    }

    /* wlan_rails=2: raphael-style proxy vote over TCS1, once.  Avoids the
     * TCS0 rpmhWrite below, whose direct ACTIVE-TCS programming is documented
     * (:378-384) to displace the display's CX vote and freeze the panel. */
    {
        uint32_t mode = 0;
        PE_parse_boot_argn("wlan_rails", &mode, sizeof(mode));
        if (mode >= 2) {
            static bool proxyDone = false;
            if (!proxyDone) { proxyDone = true; return mpssProxyTcs1() ? 0 : -1; }
            return 0;
        }
    }

    if (!railsWanted()) return 0;

    if (!res) return -1;
    const int position = railPosition(res, level, &address);
    if (position < 0 || address == 0) {
        printf("PICASSO_WLAN qmp: no position for %s at level %u\n", res, level);
        return -1;
    }
    printf("PICASSO_WLAN qmp: %s (0x%x) <- level %u (position %d)\n", res, address,
        level, position);
    return rpmhWrite(address, static_cast<uint32_t>(position)) ? 0 : -1;
}

/* ---- icnss_power, copied from raphael xnu_files_bsd_dev_hakios.wifi.c
 *      icnss_power() :4144-4185.  Rail names/voltages are picasso's own
 *      (PicassoWlan.cpp:5826-5838): SM7250 uses ldoa14 where SM8150 uses ldoa7,
 *      and ldoc11 is 3312 mV here vs 3104 mV on raphael.  The write pattern --
 *      base=mV, base+8=drive (L10C HPM 7, others 4), base+4=enable, issued as
 *      one TCS1 batch -- is verbatim.  on=false walks the rails in reverse and
 *      drops the QDSS clock, also like raphael. */
bool icnssPower(bool on)
{
    static const char *const names[5] = { "ldoa1", "ldoa14", "ldoc2", "ldoc10", "ldoc11" };
    static const uint32_t millivolts[5] = { 752, 1800, 1304, 3312, 3312 };
    uint32_t addresses[15], values[15];
    unsigned count = 0;
    static uint32_t railAddressCache[5] = { 0, 0, 0, 0, 0 };
    static bool qdssTouched = false;
    bool clockDone = true;

    if (!on && qdssTouched) {
        clockDone = hakios_aop_mailbox_send("{class: clock, res: qdss, val: 0}");
        if (clockDone) qdssTouched = false;
    }
    for (unsigned i = 0; i < 5; i++) {
        if (on && railAddressCache[i] == 0) {
            if (!railAddress(names[i], &railAddressCache[i])) return false;
        }
    }
    for (unsigned n = 0; n < 5; n++) {
        unsigned i = on ? n : 4u - n;
        uint32_t base = railAddressCache[i];
        if (on) {
            addresses[count] = base; values[count++] = millivolts[i];
            addresses[count] = base + 8; values[count++] = (i == 3) ? 7 : 4;
        }
        addresses[count] = base + 4; values[count++] = on ? 1 : 0;
        printf("PICASSO_WLAN qmp: ICNSS %s %s, %u mV\n", names[i], on ? "on" : "off", millivolts[i]);
    }
    bool done = wifiRpmhWriteTcs1(addresses, values, count);
    if (on && done) {
        qdssTouched = true;
        done = hakios_aop_mailbox_send("{class: clock, res: qdss, val: 1}");
    }
    printf("PICASSO_WLAN qmp: ICNSS power %s: TCS1 %s, QDSS %s\n",
        on ? "on" : "off", done ? "ok" : "FAILED", clockDone ? "ok" : "FAILED");
    return done && clockDone;
}

/* MPSS proxy vote, copied from raphael mpss_proxy() :1091-1126.
 * raphael casts cx.lvl/mss.lvl at 384 (TURBO) and xo.lvl at 3, all through
 * the same TCS1 batch -- NOT through the TCS0 rpmhWrite that freezes the
 * panel.  Used when wlan_rails=2 so the whole rail/proxy path stays on TCS1. */
bool mpssProxyTcs1(void)
{
    static const char *const names[3] = { "cx.lvl", "mss.lvl", "xo.lvl" };
    uint32_t addresses[3] = { 0, 0, 0 }, values[3] = { 0, 0, 3 };
    for (unsigned i = 0; i < 2; i++) {
        int pos = railPosition(names[i], 384, &addresses[i]);
        if (pos < 0 || !addresses[i]) {
            printf("PICASSO_WLAN qmp: MPSS has no TURBO position for %s\n", names[i]);
            return false;
        }
        values[i] = static_cast<uint32_t>(pos);
    }
    if (railPosition(names[2], 0, &addresses[2]) < 0 || !addresses[2]) return false;
    printf("PICASSO_WLAN qmp: MPSS proxy TCS1: cx=%u mss=%u xo=%u\n", values[0], values[1], values[2]);
    return wifiRpmhWriteTcs1(addresses, values, 3);
}

/* PicassoWlan.cpp's sweet_rpmh_vreg_enable() stub forwards here.  Gated:
 *   wlan_rails=0 (default): no vote, report the stub's historical success
 *   wlan_rails=1: legacy cx.lvl/mss.lvl single yazoo vote (rpmhWrite, TCS0)
 *   wlan_rails=2: raphael-style 5-rail + QDSS over TCS1 (icnssPower)
 * The 5 calls made by PicassoWlan.cpp collapse to one icnssPower(true) batch. */
extern "C" int hakios_wlan_icnss_power(void)
{
    uint32_t mode = 0;
    PE_parse_boot_argn("wlan_rails", &mode, sizeof(mode));
    if (mode < 2) return mode; /* 0/1: caller keeps its legacy behaviour */
    static bool applied = false;
    if (applied) return 2;      /* PicassoWlan.cpp calls vreg_enable five times */
    applied = true;
    return icnssPower(true) ? 2 : -2;
}
