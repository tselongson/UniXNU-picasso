// SPDX-License-Identifier: BSD-2-Clause-Patent
// SMB5 charger control, on pm8150b at SPMI slave 2.
#pragma once

#include <stdint.h>

#include "Battery.h"

struct SpmI;

// The block runs these phases by itself, from the pack voltage and the JEITA window. Trickle is the
// slow phase for a deeply discharged pack, pre-charge the one just above it, full-on the bulk phase
// and taper the constant-voltage tail; the driver reports them rather than emulating them.
enum ChargerPhase {
	kChargerPhaseInhibit = 0,
	kChargerPhaseTrickle,
	kChargerPhasePreCharge,
	kChargerPhaseFullOn,
	kChargerPhaseTaper,
	kChargerPhaseTerminate,
	kChargerPhasePause,
	kChargerPhaseDisable,
};

struct ChargerPhaseEnables {
	bool trickle;
	bool preCharge;
	bool fullOn;
};

struct ChargerState {
	bool valid;
	bool chargingEnabled;    // CHARGING_ENABLE_CMD bit 0
	bool inputSuspended;     // USBIN_CMD_IL suspend bit
	uint8_t powerPathStatus; // DCDC POWER_PATH_STATUS
};

// Stops or allows current into the pack. The input keeps powering the system either way, which is the
// bypass the caller usually means: the pack rests while the cable carries the load.
bool chargerSetEnabled(const struct SpmI *spmi, bool enabled);

// Cuts the input off entirely. This is not the same as stopping charging and is only for a caller
// that wants the cable to stop feeding the board.
bool chargerSuspendInput(const struct SpmI *spmi, bool suspend);

bool chargerReadState(const struct SpmI *spmi, struct ChargerState *state);

// Sets the USB input current limit, clamped to the range the block accepts. The register step is the
// one this block has used since PM8150; read it back with chargerReadInputCurrentLimit() to confirm,
// because a wrong step shows up as a mismatch there rather than as silently wrong current.
bool chargerSetInputCurrentLimit(const struct SpmI *spmi, uint32_t milliamps);
bool chargerReadInputCurrentLimit(const struct SpmI *spmi, uint32_t *milliamps);

// Which phase the block is in, and which of the slow phases it has enabled.
bool chargerReadPhase(const struct SpmI *spmi, enum ChargerPhase *phase);
const char *chargerPhaseName(enum ChargerPhase phase);
bool chargerReadPhaseEnables(const struct SpmI *spmi, struct ChargerPhaseEnables *enables);

// Bulk-phase current and the constant-voltage setpoint. The steps are the ones this block has used
// since PM8150; the addresses come from the register map.
bool chargerSetFastChargeCurrent(const struct SpmI *spmi, uint32_t milliamps);
bool chargerSetFloatVoltage(const struct SpmI *spmi, uint32_t millivolts);

// True when the pack is at or past the full threshold, with hysteresis so the switch does not
// chatter at the boundary.
bool chargerShouldStopCharging(uint32_t millivolts, bool chargingEnabled);

// Reads the pack, decides, and switches. Returns false when the pack or the input could not be read,
// so a caller can tell "nothing to do" from "could not tell".
bool chargerApplyFullPolicy(const struct SpmI *spmi, const struct BatteryReading *reading,
                            struct ChargerState *state);
