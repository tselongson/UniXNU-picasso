// SPDX-License-Identifier: BSD-2-Clause-Patent
// Battery and charger readings for the board, filled from the PMIC ADCs.
#pragma once

#include <stdint.h>

struct SpmI;

struct BatteryReading {
	bool present;

	// Pack, from vbat_sns on pm8150b (vph_pwr on pm8150 as the fallback).
	bool voltageValid;
	uint32_t millivolts;

	// USB input, from usb_in_v_div_16 and usb_in_i_uv on pm8150b. Both are only meaningful with
	// something plugged in; the current channel answers even with no cable, so usbValid is what
	// says whether the pair describes a real input.
	bool usbValid;
	bool usbCurrentValid;
	bool powerValid;
	uint32_t usbMillivolts;
	uint32_t usbMilliamps;
	uint32_t chargerMilliwatts;

	// Die temperatures, tenths of a kelvin.
	bool temperatureValid;
	uint32_t pmicDeciKelvin;
	bool chargerTemperatureValid;
	uint32_t chargerDeciKelvin;
};

// Fills the reading from the PMICs. Returns false only when nothing at all could be read; the
// individual valid flags say which parts succeeded, because the charger PMIC can be absent while
// the pack is still readable.
bool batteryRead(const struct SpmI *spmi, struct BatteryReading *reading);

// Linear estimate between an empty and a full single cell. Without a Coulomb counter this is the
// honest resolution of the measurement, so it is exposed as its own function rather than folded in.
uint32_t batteryPercentFromMillivolts(uint32_t millivolts);
