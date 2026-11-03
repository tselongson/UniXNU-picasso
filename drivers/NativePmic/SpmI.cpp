// SPDX-License-Identifier: BSD-2-Clause-Patent
// SPMI PMIC arbiter transport for the SM7250 board, v5 layout.
//
// The block is the "qcom,spmi-pmic-arb" generation whose DT reg windows are core 0x1100 and cnfg
// 0x26000; a transfer goes through a per-APID channel for writes and through this EE's observer
// window for reads. That is exactly the path drivers/NativeButtons/ButtonProbe.cpp already
// exercises on this hardware, so the primitives (opcode, status bits, window strides) are copied
// from there rather than invented. The windows must be mapped with IODeviceMemory: casting a
// physical address and dereferencing it faults the kernel, which is what stalled the boot when
// this file used to hand the raw base addresses to spmiRead.
#include "SpmI.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>

namespace {

// Window bases and sizes, from /soc/qcom,spmi@c440000 in the SoC device tree and identical to the
// constants in ButtonProbe.cpp.
constexpr uint64_t kCore = 0x0c440000, kCnfg = 0x0c40a000, kChn = 0x0c600000, kObs = 0xe600000;
constexpr uint32_t kCoreBytes = 0x1100, kCnfgBytes = 0x26000, kChnBytes = 0x2000000, kObsBytes = 0x100000;

// Arbiter core layout: version at 0x00, features at 0x04, APID map at 0x900 (4 bytes per APID,
// PPID in bits 19:8), APID owner at 0x700.
constexpr uint32_t kFeatures = 0x04, kApidMap = 0x900, kApidOwner = 0x700;

// Per-channel register layout inside both the channel window and the observer window.
constexpr uint32_t kChanCmd = 0x00, kChanStatus = 0x08, kChanWdata = 0x10, kChanRdata = 0x18;
constexpr uint32_t kChanApidStride = 0x10000, kObsEeStride = 0x10000, kObsApidStride = 0x80;
constexpr uint32_t kOpExtReadL = 1;
constexpr uint32_t kStatusDone = 1u << 0, kStatusErrors = 0xeu;

bool mapWindow(void **mapOut, volatile uint32_t **vaOut, uint64_t base, uint64_t size)
{
	IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
	IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
	if (memory) {
		memory->release();
	}
	*mapOut = map;
	*vaOut = map ? reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress()) : nullptr;
	return *vaOut != nullptr;
}

volatile uint32_t *rwChannel(SpmI *spmi, uint32_t apid)
{
	return spmi->chn + (kChanApidStride * apid) / 4;
}

volatile uint32_t *obsChannel(SpmI *spmi, uint32_t apid)
{
	return spmi->obs + (kObsEeStride * spmi->ee + kObsApidStride * apid) / 4;
}

// The APID that serves ppid for this EE, as the mainline driver picks it: the mapping whose owner
// word names this EE, else the first mapping of that PPID.
int findApid(const SpmI *spmi, uint32_t ppid)
{
	int first = -1;
	for (uint32_t index = 0; index < spmi->apids; ++index) {
		const uint32_t map = spmi->core[(kApidMap + 4 * index) / 4];
		if (!map || ((map >> 8) & 0xfff) != ppid) {
			continue;
		}
		if ((spmi->cnfg[(kApidOwner + 4 * index) / 4] & 7u) == spmi->ee) {
			return (int)index;
		}
		if (first < 0) {
			first = (int)index;
		}
	}
	return first;
}

} // namespace

bool spmiInit(SpmI *spmi)
{
	if (!spmi) {
		return false;
	}
	if (spmi->ready) {
		return true;
	}
	if (!mapWindow(&spmi->coreMap, &spmi->core, kCore, kCoreBytes) ||
	    !mapWindow(&spmi->cnfgMap, &spmi->cnfg, kCnfg, kCnfgBytes) ||
	    !mapWindow(&spmi->chnMap, &spmi->chn, kChn, kChnBytes) ||
	    !mapWindow(&spmi->obsMap, &spmi->obs, kObs, kObsBytes)) {
		IOLog("NativePmic: SPMI windows could not be mapped\n");
		return false;
	}
	const uint32_t version = spmi->core[0];
	const uint32_t apids = spmi->core[kFeatures / 4] & 0x7ffu;
	// Only the v5 layout is described above; refuse anything else rather than poke it.
	if (version < 0x50000000u || version >= 0x60000000u || !apids || apids > 512) {
		IOLog("NativePmic: unexpected SPMI arbiter version %08x apids %u\n", version, apids);
		return false;
	}
	spmi->apids = apids;
	spmi->ee = 0;
	spmi->ready = true;
	IOLog("NativePmic: SPMI arbiter v%x, %u APIDs\n", version >> 24, apids);
	return true;
}

bool spmiRead(const SpmI *spmi, uint8_t slave, SpmIRegister reg, uint8_t *value)
{
	if (!spmi || !spmi->ready || !value) {
		return false;
	}
	SpmI *self = const_cast<SpmI *>(spmi);
	const int apid = findApid(spmi, ((uint32_t)slave << 8) | reg.peripheral);
	if (apid < 0) {
		return false;
	}
	volatile uint32_t *channel = obsChannel(self, (uint32_t)apid);
	channel[kChanCmd / 4] = (kOpExtReadL << 27) | ((uint32_t)(reg.offset & 0xff) << 4); // one byte
	for (unsigned poll = 0; poll < 100000; ++poll) {
		const uint32_t status = channel[kChanStatus / 4];
		if (!(status & kStatusDone)) {
			continue;
		}
		if (status & kStatusErrors) {
			return false;
		}
		*value = (uint8_t)(channel[kChanRdata / 4] & 0xff);
		return true;
	}
	return false;
}

bool spmiWrite(const SpmI *spmi, uint8_t slave, SpmIRegister reg, uint8_t value)
{
	if (!spmi || !spmi->ready) {
		return false;
	}
	SpmI *self = const_cast<SpmI *>(spmi);
	const int apid = findApid(spmi, ((uint32_t)slave << 8) | reg.peripheral);
	if (apid < 0) {
		return false;
	}
	volatile uint32_t *channel = rwChannel(self, (uint32_t)apid);
	channel[kChanCmd / 4] = ((uint32_t)(reg.offset & 0xff) << 4) | 3u; // EXT_WRITEL, four bytes
	channel[kChanWdata / 4] = (uint32_t)value * 0x01010101u;
	for (unsigned poll = 0; poll < 100000; ++poll) {
		const uint32_t status = channel[kChanStatus / 4];
		if (!(status & kStatusDone)) {
			continue;
		}
		return (status & kStatusErrors) ? false : true;
	}
	return false;
}

bool spmiRead16(const SpmI *spmi, uint8_t slave, SpmIRegister reg, uint16_t *value)
{
	uint8_t high = 0, low = 0;
	if (!spmiRead(spmi, slave, reg, &high) ||
	    !spmiRead(spmi, slave, {reg.peripheral, uint16_t(reg.offset + 1)}, &low)) {
		return false;
	}
	*value = (uint16_t(high) << 8) | low;
	return true;
}
