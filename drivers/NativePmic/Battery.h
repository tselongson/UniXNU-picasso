// SPDX-License-Identifier: BSD-2-Clause-Patent
// Battery readings for the board, filled from the PM8150 VADC.
#pragma once

#include <stdint.h>

struct SpmI;

struct BatteryReading {
	bool present;
	bool voltageValid;
	bool temperatureValid;
	uint32_t millivolts;      // vph_pwr, the rail the cell feeds
	uint32_t pmicDeciKelvin;  // PM8150 die temperature, tenths of a kelvin
};

// Fills the reading from the PMIC. Returns false only when nothing at all could be read; the
// individual valid flags say which halves succeeded.
bool batteryRead(const struct SpmI *spmi, struct BatteryReading *reading);

// Linear estimate between an empty and a full single cell. Without a Coulomb counter this is the
// honest resolution of the measurement, so it is exposed as its own function rather than folded in.
uint32_t batteryPercentFromMillivolts(uint32_t millivolts);
