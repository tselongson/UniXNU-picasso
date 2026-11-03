// SPDX-License-Identifier: BSD-2-Clause-Patent
// SPMI register access for the SM7250 PMICs. This is the transport both the battery and the thermal
// drivers need: every PMIC block (VADC, temp alarm, charger) is reached through the arbiter at
// 0x0c440000, which is described in the SoC device tree rather than in the board overlay, so the
// board overlay alone makes it look as if the PMIC has no bus.
//
// The windows must be MAPPED, not merely cast: the arbiter is v5 (core 0x1100 + cnfg 0x26000 +
// chnls 0x2000000 + obsrvr, the same map ButtonProbe already exercises on hardware). Reads go
// through this EE's observer window and writes through the per-APID channel window, exactly like
// drivers/NativeButtons/ButtonProbe.cpp.
#pragma once

#include <stdint.h>

// Slave ids on this board: pm8150 at 0, pm8150b at 2, pm8150l at 4. All three appear in the board
// overlay; pm8150b is the one that carries the SMB5 charger and the pack sense channels.
#define kSpmISlavePm8150   0u
#define kSpmISlavePm8150B  2u
#define kSpmISlavePm8150L  4u

// Register addresses are 16 bits wide, split into a peripheral page and an offset inside it.
struct SpmIRegister {
	uint16_t peripheral;
	uint16_t offset;
};

struct SpmI {
	void *coreMap;               // IOMemoryMap* for the arbiter core window
	void *cnfgMap;               // IOMemoryMap* for the per-APID configuration window
	void *chnMap;                // IOMemoryMap* for the channel window
	void *obsMap;                // IOMemoryMap* for this EE's observer window
	volatile uint32_t *core;     // mapped 0x0c440000
	volatile uint32_t *cnfg;     // mapped 0x0c40a000
	volatile uint32_t *chn;      // mapped 0x0c600000
	volatile uint32_t *obs;      // mapped 0xe600000
	uint32_t apids;              // APID table entries the arbiter reports
	uint8_t ee;                  // this EE (APPS = 0)
	bool ready;
};

// Maps the four arbiter windows and validates the block generation (v5 only). Returns false when a
// window cannot be mapped or the block reports an unexpected layout; spmiRead/spmiWrite then refuse.
bool spmiInit(SpmI *spmi);

// Reads one register of one PMIC. Returns false on a bus error or a command that never completes;
// the caller must not treat a zero out-value as a successful read.
bool spmiRead(const SpmI *spmi, uint8_t slave, SpmIRegister reg, uint8_t *value);

// Writes one register. Read-modify-write helpers live in the callers, because the PMIC blocks have
// their own read-only and write-one-to-clear bits.
bool spmiWrite(const SpmI *spmi, uint8_t slave, SpmIRegister reg, uint8_t value);

// Reads a big-endian 16-bit pair, which is how the VADC and TSENS blocks return conversions.
bool spmiRead16(const SpmI *spmi, uint8_t slave, SpmIRegister reg, uint16_t *value);
