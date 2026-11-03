// SPDX-License-Identifier: BSD-2-Clause-Patent
// Power and volume keys of the picasso board (boot argument native-buttons=1), sent as the Consumer
// Power, Volume Increment and Volume Decrement bits of the HID input device's report 2
// (drivers/NativeHidInput).
//
// The devicetree facts (work/base.dts, work/picasso.dts):
//   - the SPMI arbiter is the older generation: core 0xc440000 (0x1100), chnls 0xc600000, obsrvr
//     0xe600000, intr 0xe700000, cnfg 0xc40a000, EE 0, and its interrupt is PDC pin 1
//     (<&pdc 1 IRQ_TYPE_LEVEL_HIGH>, PDC range 0 -> SPI 480, so the pin forwards as SPI 481);
//   - power and volume down are the pm6350 PON at SID 0, peripheral 0x08: kpdpwr is interrupt 0,
//     RESIN interrupt 1 (<0 8 0> and <0 8 1> in qcom,power-on@800);
//   - volume up is pm8150l GPIO 5 (SID 4, peripheral 0xc5, active low with a pull-up).
//
// Register layout as the mainline spmi-pmic-arb driver's v5 ops use it, NOT the v7 layout the
// sheng board has. Every register that moved between the two generations is called out below:
//   APID map   core + 0x900 + 4*apid      bits 19:8 PPID            (v7: core + 0x2000)
//   APID owner cnfg + 0x700 + 4*apid      bits 2:0 the owning EE   (v7: cnfg + 4*apid)
//   RW channel chnls + 0x10000*apid       cmd 0x00 status 0x08 wdata 0x10 rdata 0x18
//   acc enable chnls + 0x100 + 0x10000*apid
//   irq status chnls + 0x104 + 0x10000*apid
//   irq clear  chnls + 0x108 + 0x10000*apid
//   obs channel obsrvr + 0x10000*ee + 0x80*apid   (v7: 0x8000*ee + 0x20*apid)
//   accumulator intr + 0x10000*ee + 4*word        (v7: intr + 0x1000*ee + 4*word)
// The command word is the v2 encoding, which v5 also uses:
//   (opcode << 27) | ((byte address & 0xff) << 4) | (byte count & 7), EXT_READL = 1.
// The peripheral interrupt registers are the usual qpnpint page: 0x10 RT_STS, 0x11 SET_TYPE,
// 0x12 POLARITY_HIGH, 0x13 POLARITY_LOW, 0x14 LATCHED_CLR, 0x15 EN_SET.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *)
    __attribute__((weak_import));
// NativeHidInput publishes this when its report-2 consumer starts; without it the probe still reads
// the keys but has nowhere to send them.
extern "C" void native_buttons_set_sink(void (*)(void *, uint32_t), void *)
    __attribute__((weak_import));

// [0] stage (1 starting, 2 windows mapped, 3 armed, 4 running, -n the step that failed),
// [1] interrupts, [2] reports sent, [3] pressed mask, [4] PON apid, [5] volume-up apid,
// [6] foreign APID interrupts cleared, [7] PDC version, [8] last error, [9] spurious interrupts.
extern "C" {
volatile int32_t native_buttons_stats[10];
}

namespace {

// Windows from /soc/qcom,spmi@c440000 and /soc/interrupt-controller@b220000. The sizes are the
// devicetree's own: core 0x1100 and cnfg 0x26000 are what makes this the v5 map.
constexpr uint64_t kCore = 0xc440000, kCnfg = 0xc40a000;
constexpr uint64_t kObs = 0xe600000, kChn = 0xc600000, kIntr = 0xe700000, kPdc = 0xb220000;
constexpr uint32_t kCoreBytes = 0x1100, kCnfgBytes = 0x26000;
constexpr uint32_t kObsBytes = 0x100000, kChnBytes = 0x2000000, kIntrBytes = 0xa0000, kPdcBytes = 0x40000;

constexpr uint32_t kFeatures = 0x04, kApidMap = 0x900, kApidOwner = 0x700;
constexpr uint32_t kAccEnable = 0x100, kIrqStatus = 0x104, kIrqClear = 0x108;
constexpr uint32_t kChanCmd = 0x00, kChanStatus = 0x08, kChanRdata = 0x18;
constexpr uint32_t kObsEeStride = 0x10000, kObsApidStride = 0x80, kChanApidStride = 0x10000;
constexpr uint32_t kOpExtReadL = 1, kStatusDone = 1u << 0, kStatusErrors = 0xeu;

// qpnpint page of a peripheral.
constexpr uint8_t kRtSts = 0x10, kSetType = 0x11, kPolHigh = 0x12, kPolLow = 0x13;
constexpr uint8_t kLatchedClr = 0x14, kEnSet = 0x15;

// The board's keys. PON interrupt 0 is kpdpwr, 1 is RESIN on this PMIC (the sheng board's PMIC
// numbers them 7 and 6, which is why those constants do not carry over).
constexpr uint32_t kPonPpid = 0x08, kVolUpPpid = 0xc5;
constexpr uint8_t kPowerBit = 1u << 0, kResinBit = 1u << 1;

constexpr uint32_t kPdcPin = 1, kSpi = 481, kPdcVersion = 0x1000, kPdcCfg = 0x110;
constexpr uint32_t kPdcEnableBank = 0x10;

// Report 2 bits (NativeHidInput): 1 Power, 2 Volume Increment, 3 Volume Decrement.
constexpr uint32_t kReportPower = 1u << 1, kReportVolumeUp = 1u << 2, kReportVolumeDown = 1u << 3;

struct Window { IOMemoryMap *map; volatile uint32_t *va; };
Window gCore, gCnfg, gObs, gChn, gIntr, gPdc;
uint32_t gApids;
uint8_t gEe;
int gPon = -1, gVolUp = -1;
volatile uint32_t gEvents;
void (*volatile gSink)(void *, uint32_t);
void *volatile gSinkContext;
// Volume up has no interrupt routed to us (its GPIO block would need a second PDC pin), so the
// thread samples the keys while any of them is down and does one slow poll otherwise. A key press
// therefore reaches the system within one PON interrupt or one poll, whichever comes first.
constexpr uint32_t kIdlePollMs = 100;

bool mapWindow(Window &window, uint64_t base, uint64_t size) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    window.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    window.va = window.map ? reinterpret_cast<volatile uint32_t *>(window.map->getVirtualAddress()) : nullptr;
    return window.va != nullptr;
}

void fail(int step, int error) {
    native_buttons_stats[8] = error;
    native_buttons_stats[0] = -step;
}

volatile uint32_t *obsChannel(int apid) {
    return gObs.va + (kObsEeStride * gEe + kObsApidStride * (uint32_t)apid) / 4;
}
volatile uint32_t *rwChannel(int apid) {
    return gChn.va + (kChanApidStride * (uint32_t)apid) / 4;
}

// One byte of a peripheral through this EE's observer channel. Returns 0, 1 (never completed) or
// 2 (the arbiter reported a failed, denied or dropped transaction).
int read8(int apid, uint8_t address, uint8_t *value) {
    if (apid < 0) return 2;
    volatile uint32_t *channel = obsChannel(apid);
    channel[kChanCmd / 4] = (kOpExtReadL << 27) | ((uint32_t)address << 4);   // one byte: count - 1 = 0
    for (unsigned poll = 0; poll < 100000; ++poll) {
        const uint32_t status = channel[kChanStatus / 4];
        if (!(status & kStatusDone)) continue;
        if (status & kStatusErrors) return 2;
        *value = (uint8_t)(channel[kChanRdata / 4] & 0xff);
        return 0;
    }
    return 1;
}

int write8(int apid, uint8_t address, uint8_t value) {
    if (apid < 0) return 2;
    volatile uint32_t *channel = rwChannel(apid);
    channel[kChanCmd / 4] = ((uint32_t)address << 4) | 3u;   // EXT_WRITEL, four bytes: count - 1 = 3
    channel[0x10 / 4] = (uint32_t)value * 0x01010101u;
    for (unsigned poll = 0; poll < 100000; ++poll) {
        const uint32_t status = channel[kChanStatus / 4];
        if (!(status & kStatusDone)) continue;
        return (status & kStatusErrors) ? 2 : 0;
    }
    return 1;
}

// The APID that serves ppid for this EE, as the mainline driver picks it: the mapping whose owner
// word names this EE, else the first mapping of that PPID.
int findApid(uint32_t ppid) {
    int first = -1;
    for (uint32_t index = 0; index < gApids; ++index) {
        const uint32_t map = gCore.va[(kApidMap + 4 * index) / 4];
        if (!map || ((map >> 8) & 0xfff) != ppid) continue;
        if ((gCnfg.va[(kApidOwner + 4 * index) / 4] & 7u) == gEe) return (int)index;
        if (first < 0) first = (int)index;
    }
    return first;
}

// Both edges of the given interrupt bits, latched state cleared, then enabled in the peripheral.
int armPeripheral(int apid, uint8_t bits) {
    uint8_t type = 0, high = 0, low = 0;
    int rc = read8(apid, kSetType, &type);
    if (!rc) rc = read8(apid, kPolHigh, &high);
    if (!rc) rc = read8(apid, kPolLow, &low);
    if (!rc) rc = write8(apid, kSetType, (uint8_t)(type | bits));
    if (!rc) rc = write8(apid, kPolHigh, (uint8_t)(high | bits));
    if (!rc) rc = write8(apid, kPolLow, (uint8_t)(low | bits));
    if (!rc) rc = write8(apid, kLatchedClr, bits);
    if (!rc) rc = write8(apid, kEnSet, bits);
    if (!rc) rwChannel(apid)[kAccEnable / 4] = 1;
    return rc;
}

// Primary interrupt context: acknowledge in the arbiter and in the peripheral, then wake the
// thread. An APID that is not ours but raised the line only gets its arbiter status cleared, since
// reading another EE's peripheral can end in a synchronous external abort.
void buttonsIrq(void *) {
    bool ours = false;
    for (uint32_t word = 0; word * 32 < gApids; ++word) {
        uint32_t pending = gIntr.va[(kObsEeStride * gEe) / 4 + word];
        while (pending) {
            const int apid = (int)(word * 32 + __builtin_ctz(pending));
            pending &= pending - 1;
            if (apid != gPon && apid != gVolUp) {
                rwChannel(apid)[kIrqClear / 4] = 0xff;
                native_buttons_stats[6] = native_buttons_stats[6] + 1;
                continue;
            }
            const uint32_t status = rwChannel(apid)[kIrqStatus / 4];
            rwChannel(apid)[kIrqClear / 4] = status;
            if (status) write8(apid, kLatchedClr, (uint8_t)status);
            ours = true;
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    // A line that keeps firing with none of our peripherals behind it would hold this core in the
    // handler: after 1000 such interrupts in a row the PDC pin is masked and stage -9 records it.
    static uint32_t idle;
    if (!ours) {
        native_buttons_stats[9] = native_buttons_stats[9] + 1;
        if (++idle >= 1000) {
            gPdc.va[(kPdcCfg + 4 * kPdcPin) / 4] = 4u;
            __asm__ volatile("dsb sy" ::: "memory");
            native_buttons_stats[0] = -9;
        }
        return;
    }
    idle = 0;
    native_buttons_stats[1] = native_buttons_stats[1] + 1;
    __atomic_fetch_add(&gEvents, 1, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gEvents);
}

uint32_t pressed() {
    uint8_t pon = 0, gpio = 0xff;
    uint32_t mask = 0;
    if (!read8(gPon, kRtSts, &pon)) {
        if (pon & kPowerBit) mask |= kReportPower;
        if (pon & kResinBit) mask |= kReportVolumeDown;
    }
    // pm8150l GPIO 5 is active low with a pull-up, so a low level is the press.
    if (gVolUp >= 0 && !read8(gVolUp, 0x08, &gpio) && !(gpio & 1u)) mask |= kReportVolumeUp;
    return mask;
}

bool setup() {
    gEe = 0;
    native_buttons_stats[0] = 1;
    if (!mapWindow(gCore, kCore, kCoreBytes) || !mapWindow(gCnfg, kCnfg, kCnfgBytes) ||
        !mapWindow(gObs, kObs, kObsBytes) || !mapWindow(gChn, kChn, kChnBytes) ||
        !mapWindow(gIntr, kIntr, kIntrBytes) || !mapWindow(gPdc, kPdc, kPdcBytes)) {
        fail(1, -12);
        return false;
    }
    const uint32_t version = gCore.va[0];
    gApids = gCore.va[kFeatures / 4] & 0x7ffu;
    // Only the v5 layout is described above. Anything else is refused rather than poked: the
    // board's devicetree says 0x1100/0x26000 windows, which is v5.
    if (version < 0x50000000u || version >= 0x60000000u || !gApids || gApids > 512) {
        fail(2, (int)version);
        return false;
    }
    native_buttons_stats[0] = 2;
    gPon = findApid(kPonPpid);
    gVolUp = findApid(kVolUpPpid);
    native_buttons_stats[4] = gPon;
    native_buttons_stats[5] = gVolUp;
    if (gPon < 0) {
        fail(3, -2);
        return false;
    }
    int rc = armPeripheral(gPon, (uint8_t)(kPowerBit | kResinBit));
    if (rc) {
        fail(4, rc);
        return false;
    }
    // PDC pin 1: level high, enabled. The enable bit lives in the configuration word on 3.2 and
    // later, and in a separate bank on the older revisions; the version register decides.
    const uint32_t pdcVersion = gPdc.va[kPdcVersion / 4];
    native_buttons_stats[7] = (int32_t)pdcVersion;
    if (pdcVersion >= 0x30200) {
        gPdc.va[(kPdcCfg + 4 * kPdcPin) / 4] = (1u << 3) | 4u;
    } else {
        gPdc.va[(kPdcCfg + 4 * kPdcPin) / 4] = 4u;
        gPdc.va[(kPdcEnableBank + 4 * (kPdcPin / 32)) / 4] |= 1u << (kPdcPin % 32);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    native_buttons_stats[0] = 3;
    if (!armcpu_spi_attach) {
        fail(5, -78);
        return false;
    }
    // The probe can run before the interrupt controller driver has started, so wait for it.
    for (unsigned tries = 0;; ++tries) {
        rc = armcpu_spi_attach(32 + kSpi, false, buttonsIrq, nullptr, nullptr, nullptr);
        if (rc != kIOReturnNotReady || tries == 240) break;
        IOSleep(250);
    }
    if (rc) {
        fail(5, rc);
        return false;
    }
    native_buttons_stats[0] = 4;
    return true;
}

void buttonsThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "picasso-buttons");
    if (!setup()) return;
    uint32_t seen = gEvents, last = 0;
    for (;;) {
        const uint32_t mask = pressed();
        if (mask != last) {
            last = mask;
            native_buttons_stats[3] = (int32_t)mask;
            void (*sink)(void *, uint32_t) = __atomic_load_n(&gSink, __ATOMIC_ACQUIRE);
            if (sink) {
                sink(gSinkContext, mask);
                native_buttons_stats[2] = native_buttons_stats[2] + 1;
            }
        }
        assert_wait((event_t)&gEvents, THREAD_UNINT);
        if (gEvents != seen) clear_wait(current_thread(), THREAD_AWAKENED);
        else {
            // Volume up is level-sampled, so bound the wait: one poll per idle tick, and the
            // interrupt still cuts it short the moment power or volume down moves.
            assert_wait_timeout((event_t)&gEvents, THREAD_UNINT, kIdlePollMs, NSEC_PER_MSEC);
            thread_block(THREAD_CONTINUE_NULL);
        }
        seen = gEvents;
    }
}
}  // namespace

extern "C" void native_buttons_set_sink(void (*sink)(void *, uint32_t), void *context) {
    gSinkContext = context;
    __atomic_store_n(&gSink, sink, __ATOMIC_RELEASE);
}

// Read the three keys once and report them: the board file can then be checked against the
// hardware without a HID consumer, and a press can be confirmed from the console.
extern "C" void native_buttons_report(void) {
    uint8_t pon = 0, gpio = 0xff, type = 0;
    const int ponRc = read8(gPon, kRtSts, &pon);
    const int typeRc = read8(gPon, kSetType, &type);
    const int gpioRc = read8(gVolUp, 0x08, &gpio);
    printf("PICASSO_BUTTONS stage=%d err=%d irq=%d reports=%d mask=%x pon=%d volup=%d word=%x "
           "pdc=%x read=%d/%d/%d pon_rt=%02x pon_type=%02x gpio=%02x\n",
        native_buttons_stats[0], native_buttons_stats[8], native_buttons_stats[1],
        native_buttons_stats[2], native_buttons_stats[3], gPon, gVolUp, (unsigned)gApids,
        native_buttons_stats[7], ponRc, typeRc, gpioRc, pon, type, gpio);
}

extern "C" void native_buttons_probe(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("native-buttons", &enabled, sizeof enabled) || enabled != 1) return;
    if (native_buttons_stats[0]) return;
    native_buttons_stats[0] = 1;
    thread_t thread = nullptr;
    if (kernel_thread_start(buttonsThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else fail(6, -12);
}
