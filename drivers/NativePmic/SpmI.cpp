// SPDX-License-Identifier: BSD-2-Clause-Patent
// SPMI PMIC arbiter transport for the SM7250 board.
//
// The arbiter is the classic "qcom,spmi-pmic-arb" block (not the GENI one), so a transfer is: write
// the command word, write the data bytes, poll STATUS until the command reports done, then read the
// received bytes back. The register offsets and the command encoding below follow that block; they
// are the part of this driver that has to be confirmed against the hardware, and spmiProbeRevid()
// exists to do exactly that: a PMIC answers its revision id only when the whole path is right.
#include "SpmI.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSMetaClass.h>

// Arbiter core registers. Offsets are into the "core" window (reg-names[0]).
enum : uint32_t {
	kArbType = 0x00,
	kArbStatus = 0x08,
	kArbCmd = 0x10,
	kArbRdata0 = 0x18,
	kArbWdata0 = 0x20,
};

// STATUS bits: the arbiter raises DONE once it has finished the command it was given, and FAILURE
// when the slave did not acknowledge. Both are write-one-to-clear.
enum : uint32_t {
	kArbStatusDone = 1u << 0,
	kArbStatusFailure = 1u << 1,
};

// Command word layout: opcode in the high nibble, then the slave id, then the 16-bit register
// address split across the low bits. This is the encoding the block has used since PM8994.
static uint32_t arbCommand(uint8_t opcode, uint8_t slave, SpmIRegister reg)
{
	return (uint32_t(opcode) << 27)
	     | (uint32_t(slave & 0x0f) << 20)
	     | (uint32_t(reg.peripheral) << 8)
	     | (uint32_t(reg.offset) & 0xff);
}

enum : uint8_t {
	kOpRead = 0x3,
	kOpWrite = 0x1,
};

static bool waitForCompletion(volatile uint8_t *arbiter, uint32_t timeoutUs)
{
	for (uint32_t waited = 0; waited < timeoutUs; waited += 10) {
		const uint32_t status = *(volatile uint32_t *)(arbiter + kArbStatus);
		if (status & (kArbStatusDone | kArbStatusFailure)) {
			// Clear both; leaving DONE set makes the next poll return immediately.
			*(volatile uint32_t *)(arbiter + kArbStatus) = status & (kArbStatusDone | kArbStatusFailure);
			return (status & kArbStatusFailure) == 0;
		}
		IODelay(10);
	}
	return false;
}

bool spmiInit(SpmI *spmi)
{
	if (!spmi) {
		return false;
	}
	if (spmi->ready) {
		return true;
	}
	// The windows are identity mapped by the platform, the same way the other board drivers reach
	// their register blocks; nothing here allocates or remaps.
	spmi->arbiter = (volatile uint8_t *)kSpmIArbiterBase;
	spmi->channel = (volatile uint8_t *)kSpmIChannelBase;
	spmi->mappedSize = kSpmIArbiterSize;
	spmi->ready = true;
	return true;
}

bool spmiRead(const SpmI *spmi, uint8_t slave, SpmIRegister reg, uint8_t *value)
{
	if (!spmi || !spmi->ready || !value) {
		return false;
	}
	volatile uint8_t *arbiter = spmi->arbiter;

	*(volatile uint32_t *)(arbiter + kArbCmd) = arbCommand(kOpRead, slave, reg);
	if (!waitForCompletion(arbiter, 10000)) {
		return false;
	}
	*value = *(volatile uint8_t *)(arbiter + kArbRdata0);
	return true;
}

bool spmiWrite(const SpmI *spmi, uint8_t slave, SpmIRegister reg, uint8_t value)
{
	if (!spmi || !spmi->ready) {
		return false;
	}
	volatile uint8_t *arbiter = spmi->arbiter;

	*(volatile uint8_t *)(arbiter + kArbWdata0) = value;
	*(volatile uint32_t *)(arbiter + kArbCmd) = arbCommand(kOpWrite, slave, reg);
	return waitForCompletion(arbiter, 10000);
}

bool spmiRead16(const SpmI *spmi, uint8_t slave, SpmIRegister reg, uint16_t *value)
{
	uint8_t high = 0, low = 0;
	if (!spmiRead(spmi, slave, reg, &high) || !spmiRead(spmi, slave, {reg.peripheral, uint16_t(reg.offset + 1)}, &low)) {
		return false;
	}
	*value = (uint16_t(high) << 8) | low;
	return true;
}
