// SPDX-License-Identifier: BSD-2-Clause-Patent
// SMB5 charger control: stop charging at full and hand the load to the input.
//
// The register map is the one the downstream qpnp-smb5 driver uses, which is the only description of
// this block that exists publicly: CHGR at 0x1000, DCDC at 0x1100, USBIN at 0x1300, each a peripheral
// of the pm8150b at SPMI slave 2.
//
// Two separate switches matter here and they are not the same thing:
//   CHARGING_ENABLE_CMD   stops current into the pack while the input keeps powering the system
//   USBIN_CMD_IL suspend  stops drawing from the input at all
// Reaching a full pack needs the first. "Bypass" in the sense of running the system from the cable
// while the pack rests is what the first already gives; the second is only for cutting the input off.
#include "Charger.h"
#include "SpmI.h"

#include <string.h>

enum : uint16_t {
	kPeripheralChgr = 0x1000,
	kPeripheralDcdc = 0x1100,
	kPeripheralUsbin = 0x1300,
};

enum : uint16_t {
	kRegChargingEnableCmd = 0x42,   // CHGR_BASE + 0x42
	kRegPowerPathStatus = 0x0b,     // DCDC_BASE + 0x0b
	kRegIclStatus = 0x07,           // DCDC_BASE + 0x07, reports the limit actually in force
	kRegUsbinCmdIl = 0x40,          // USBIN_BASE + 0x40
	kRegUsbinCurrentLimit = 0x70,   // USBIN_BASE + 0x70
	kRegFastChargeCurrent = 0x61,   // CHGR_BASE + 0x61
	kRegFloatVoltage = 0x70,        // CHGR_BASE + 0x70
};

// One step of the input current limit register. The parameter table that would state this
// authoritatively lives in the downstream driver's initialisation, which is not reachable from here,
// so the value the block has used since PM8150 is taken instead and the caller is expected to verify
// it with chargerReadInputCurrentLimit(), which reads the limit back out of ICL_STATUS. A wrong step
// shows up there as a mismatch rather than as silently wrong current.
enum : uint32_t {
	kInputCurrentLimitStepMilliamps = 100,
	kInputCurrentLimitMinMilliamps = 100,
	kInputCurrentLimitMaxMilliamps = 3000,
};

enum : uint8_t {
	kBitChargingEnable = 1u << 0,
	kBitUsbinSuspend = 1u << 0,
};

static bool readModifyWrite(const SpmI *spmi, SpmIRegister reg, uint8_t mask, bool set)
{
	uint8_t value = 0;
	if (!spmiRead(spmi, kSpmISlavePm8150B, reg, &value)) {
		return false;
	}
	const uint8_t updated = set ? uint8_t(value | mask) : uint8_t(value & ~mask);
	if (updated == value) {
		return true;
	}
	return spmiWrite(spmi, kSpmISlavePm8150B, reg, updated);
}

bool chargerSetEnabled(const SpmI *spmi, bool enabled)
{
	if (!spmi) {
		return false;
	}
	return readModifyWrite(spmi, {kPeripheralChgr, kRegChargingEnableCmd}, kBitChargingEnable, enabled);
}

bool chargerSuspendInput(const SpmI *spmi, bool suspend)
{
	if (!spmi) {
		return false;
	}
	return readModifyWrite(spmi, {kPeripheralUsbin, kRegUsbinCmdIl}, kBitUsbinSuspend, suspend);
}

bool chargerReadState(const SpmI *spmi, struct ChargerState *state)
{
	if (!spmi || !state) {
		return false;
	}
	memset(state, 0, sizeof(*state));

	uint8_t value = 0;
	if (spmiRead(spmi, kSpmISlavePm8150B, {kPeripheralChgr, kRegChargingEnableCmd}, &value)) {
		state->chargingEnabled = (value & kBitChargingEnable) != 0;
		state->valid = true;
	}
	if (spmiRead(spmi, kSpmISlavePm8150B, {kPeripheralUsbin, kRegUsbinCmdIl}, &value)) {
		state->inputSuspended = (value & kBitUsbinSuspend) != 0;
		state->valid = true;
	}
	if (spmiRead(spmi, kSpmISlavePm8150B, {kPeripheralDcdc, kRegPowerPathStatus}, &value)) {
		state->powerPathStatus = value;
		state->valid = true;
	}
	return state->valid;
}

bool chargerSetInputCurrentLimit(const SpmI *spmi, uint32_t milliamps)
{
	if (!spmi) {
		return false;
	}
	if (milliamps < kInputCurrentLimitMinMilliamps) {
		milliamps = kInputCurrentLimitMinMilliamps;
	}
	if (milliamps > kInputCurrentLimitMaxMilliamps) {
		milliamps = kInputCurrentLimitMaxMilliamps;
	}
	const uint8_t raw = uint8_t((milliamps - kInputCurrentLimitMinMilliamps) / kInputCurrentLimitStepMilliamps);
	return spmiWrite(spmi, kSpmISlavePm8150B, {kPeripheralUsbin, kRegUsbinCurrentLimit}, raw);
}

bool chargerReadInputCurrentLimit(const SpmI *spmi, uint32_t *milliamps)
{
	if (!spmi || !milliamps) {
		return false;
	}
	uint8_t raw = 0;
	if (!spmiRead(spmi, kSpmISlavePm8150B, {kPeripheralDcdc, kRegIclStatus}, &raw)) {
		return false;
	}
	*milliamps = uint32_t(raw) * kInputCurrentLimitStepMilliamps + kInputCurrentLimitMinMilliamps;
	return true;
}

// The pack is treated as full at 4.35 V and as needing charge again below 4.15 V. The gap is what
// stops the two thresholds from fighting each other when the pack sits exactly at the boundary; a
// single threshold would toggle the switch on every reading.
bool chargerShouldStopCharging(uint32_t millivolts, bool chargingEnabled)
{
	const uint32_t fullMv = 4350, resumeMv = 4150;
	if (chargingEnabled) {
		return millivolts >= fullMv;
	}
	return millivolts >= resumeMv;
}

bool chargerApplyFullPolicy(const SpmI *spmi, const struct BatteryReading *reading, struct ChargerState *state)
{
	if (!spmi || !reading || !reading->voltageValid || !reading->usbValid) {
		return false;
	}
	struct ChargerState current;
	if (!chargerReadState(spmi, &current)) {
		return false;
	}
	const bool wantEnabled = !chargerShouldStopCharging(reading->millivolts, current.chargingEnabled);
	if (wantEnabled != current.chargingEnabled) {
		if (!chargerSetEnabled(spmi, wantEnabled)) {
			return false;
		}
		current.chargingEnabled = wantEnabled;
	}
	if (state) {
		*state = current;
	}
	return true;
}
