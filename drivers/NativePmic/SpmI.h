// SPDX-License-Identifier: BSD-2-Clause-Patent
// SPMI register access for the SM7250 PMICs. This is the transport both the battery and the thermal
// drivers need: every PMIC block (VADC, temp alarm, charger) is reached through the arbiter at
// 0x0c440000, which is described in the SoC device tree rather than in the board overlay, so the
// board overlay alone makes it look as if the PMIC has no bus.
#pragma once

#include <stdint.h>

// Arbiter and channel windows, from /soc/qcom,spmi@c440000 in the SoC device tree:
//   reg = <0xc440000 0x1100 0xc600000 0x2000000 ...>
#define kSpmIArbiterBase   0x0c440000u
#define kSpmIArbiterSize   0x1100u
#define kSpmIChannelBase   0x0c600000u
#define kSpmIChannelSize   0x2000000u

// Slave ids on this board: pm8150 sits at 0, pm8150l at 4. Both appear in the board overlay as
// qcom,pm8150@0 and qcom,pm8150l@4.
#define kSpmISlavePm8150   0u
#define kSpmISlavePm8150L  4u

// Register addresses are 16 bits wide, split into a peripheral page and an offset inside it.
struct SpmIRegister {
	uint16_t peripheral;
	uint16_t offset;
};

struct SpmI {
	volatile uint8_t *arbiter;   // kSpmIArbiterBase
	volatile uint8_t *channel;   // kSpmIChannelBase
	uint32_t mappedSize;
	bool ready;
};

// Maps the arbiter and channel windows. Returns false when the windows cannot be mapped, which is the
// case on a tree whose device tree does not describe the SPMI controller.
bool spmiInit(SpmI *spmi);

// Reads one register of one PMIC. Returns false on a bus error or a command that never completes;
// the caller must not treat a zero out-value as a successful read.
bool spmiRead(const SpmI *spmi, uint8_t slave, SpmIRegister reg, uint8_t *value);

// Writes one register. Read-modify-write helpers live in the callers, because the PMIC blocks have
// their own read-only and write-one-to-clear bits.
bool spmiWrite(const SpmI *spmi, uint8_t slave, SpmIRegister reg, uint8_t value);

// Reads a big-endian 16-bit pair, which is how the VADC and TSENS blocks return conversions.
bool spmiRead16(const SpmI *spmi, uint8_t slave, SpmIRegister reg, uint16_t *value);
