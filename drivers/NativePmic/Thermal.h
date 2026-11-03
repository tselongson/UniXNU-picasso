// SPDX-License-Identifier: BSD-2-Clause-Patent
// Temperature readings from the SM7250 TSENS block.
#pragma once

#include <stdint.h>

struct ThermalReading {
	unsigned sensor;
	bool valid;
	uint32_t deciKelvin;
};

// Checks that the monitor answers and counts the sensors whose last conversion is valid. Use this
// first: it separates "the block is not where the device tree says" from "the per-sensor layout is
// wrong", which look identical once temperatures are being read.
bool tsensProbe(unsigned *sensorsReporting);

// Reads one sensor. Returns false when that sensor has no valid conversion.
bool tsensRead(unsigned sensor, struct ThermalReading *reading);

// Name of a monitor in the order the downstream devicetree lists them.
const char *tsensSensorName(unsigned sensor);
