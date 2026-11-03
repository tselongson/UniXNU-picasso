// SPDX-License-Identifier: BSD-2-Clause-Patent
// Temperature sensors from the SM7250 TSENS block.
//
// tsens@c222000 in the SoC device tree: srot at 0x0c222000 (8 bytes) and the temperature monitor at
// 0x0c263000 (0x1ff bytes), compatible "qcom,tsens24xx". That is the v2 monitor: each sensor owns a
// 0x10-byte window holding a status word and the last conversion, and a valid conversion is flagged
// in the status word rather than by a separate interrupt.
//
// The window offsets below are the part that has to be confirmed on hardware. tsensProbe() returns
// the raw status of sensor 0 so a wrong layout shows up immediately as "no valid bit anywhere"
// instead of as plausible-looking but wrong temperatures.
#include "Thermal.h"

#include <IOKit/IOLib.h>

// Temperature monitor window.
#define kTsensTmBase    0x0c263000u
#define kTsensSrotBase  0x0c222000u

// Per-sensor stride and the two fields inside it.
enum : uint32_t {
	kTsensSensorStride = 0x10,
	kTsensStatusOffset = 0x0,
	kTsensLastTempOffset = 0x4,
	kTsensMaxSensors = 16,
};

// Status word: bit 0 says the last conversion is ready, and the temperature occupies the low bits of
// the second word as a signed value in quarter-kelvin steps with a -40 C origin, which is the same
// encoding the PMIC's on-die channel uses.
enum : uint32_t {
	kTsensStatusValid = 1u << 0,
};

static uint32_t tsensRawStatus(unsigned sensor)
{
	volatile uint32_t *window = (volatile uint32_t *)(kTsensTmBase + sensor * kTsensSensorStride);
	return window[kTsensStatusOffset / 4];
}

static uint32_t tsensRawTemp(unsigned sensor)
{
	volatile uint32_t *window = (volatile uint32_t *)(kTsensTmBase + sensor * kTsensSensorStride);
	return window[kTsensLastTempOffset / 4];
}

bool tsensProbe(unsigned *sensorsReporting)
{
	if (sensorsReporting) {
		*sensorsReporting = 0;
	}
	// The monitor must answer at all before any of the per-sensor layout matters.
	const uint32_t first = tsensRawStatus(0);
	if (first == 0xffffffffu || first == 0) {
		return false;
	}
	unsigned reporting = 0;
	for (unsigned sensor = 0; sensor < kTsensMaxSensors; ++sensor) {
		if (tsensRawStatus(sensor) & kTsensStatusValid) {
			++reporting;
		}
	}
	if (sensorsReporting) {
		*sensorsReporting = reporting;
	}
	return reporting > 0;
}

bool tsensRead(unsigned sensor, struct ThermalReading *reading)
{
	if (!reading || sensor >= kTsensMaxSensors) {
		return false;
	}
	reading->sensor = sensor;
	reading->valid = false;

	const uint32_t status = tsensRawStatus(sensor);
	if ((status & kTsensStatusValid) == 0) {
		return false;
	}
	const int32_t raw = int32_t(tsensRawTemp(sensor) & 0xffffu);
	// Quarter-kelvin from -40 C, so deci-kelvin is 400 + raw * 2.5, kept in integers.
	reading->deciKelvin = 400 + (raw * 25) / 10;
	reading->valid = true;
	return true;
}

// The board has no thermal zone description of its own, so the callers map sensors to zones. These
// are the ones the downstream "lito" devicetree names, in monitor order.
const char *tsensSensorName(unsigned sensor)
{
	static const char *const names[] = {
		"cpu0-0", "cpu0-1", "cpu1-0", "cpu1-1",
		"gpu-0", "gpu-1", "cpuss-0", "cpuss-1",
		"nsp-0", "nsp-1", "video", "camera",
		"modem-0", "modem-1", "disp", "q6-hvx",
	};
	return sensor < (sizeof(names) / sizeof(names[0])) ? names[sensor] : "unknown";
}
