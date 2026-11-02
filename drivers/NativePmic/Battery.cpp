// SPDX-License-Identifier: BSD-2-Clause-Patent
// Battery readings from the PM8150 VADC.
//
// This board has no charger PMIC node in its device tree (only pm8150 at slave 0 and pm8150l at
// slaves 4 and 5), so there is no Coulomb counter and no charge-status register to read. What the
// PMIC does expose is vph_pwr, the system rail, which tracks the cell voltage within a few tens of
// millivolts under light load. That is enough to stop reporting a fixed 5020 mAh pack at 100% and to
// let the system see the pack actually discharge; it is not enough to report charge current or a
// coulomb-counted percentage, and this file does not pretend otherwise.
#include "Battery.h"
#include "SpmI.h"

#include <string.h>

// VADC block of pm8150, from vadc@3100 in the board overlay: peripheral 0x3100, 0x100 bytes.
static const SpmIRegister kAdcStatus1 = {0x3100, 0x08};
static const SpmIRegister kAdcStatus2 = {0x3100, 0x0a};

// ADC5 channel registers: a channel's configuration starts at 0x2600 + 0x100 * channel, and the
// conversion result sits at 0x2600 + 0x100 * channel + 0x42.
static SpmIRegister adcChannelConfig(unsigned channel)
{
	return {0x3100, uint16_t(0x2600 + 0x100 * channel)};
}

static SpmIRegister adcChannelResult(unsigned channel)
{
	return {0x3100, uint16_t(0x2600 + 0x100 * channel + 0x42)};
}

// Channels this board's overlay declares on pm8150 (label, reg):
//   ref_gnd 0x00, vref_1p25 0x01, die_temp 0x06, vph_pwr 0x83.
static const unsigned kChannelDieTemp = 0x06;
static const unsigned kChannelVphPwr = 0x83;

// 1/3 divider on vph_pwr and a 1.875 V reference give 0.375 mV per count; the on-die temperature
// channel is ratiometric against the reference and reads 0.25 K per count from -40 C.
static const uint32_t kMillivoltsNumerator = 375;   // mV * 1000 per count
static const uint32_t kMillivoltsDenominator = 1000;

static bool adcReadRaw(const SpmI *spmi, unsigned channel, uint16_t *raw)
{
	// The status pair reports the last conversion's done bit; clearing it before starting keeps a
	// stale done from being mistaken for this conversion.
	uint8_t status1 = 0, status2 = 0;
	if (!spmiRead(spmi, kSpmISlavePm8150, kAdcStatus1, &status1) ||
	    !spmiRead(spmi, kSpmISlavePm8150, kAdcStatus2, &status2)) {
		return false;
	}
	if (!spmiWrite(spmi, kSpmISlavePm8150, kAdcStatus1, status1) ||
	    !spmiWrite(spmi, kSpmISlavePm8150, kAdcStatus2, status2)) {
		return false;
	}
	// A conversion takes a few hundred microseconds; poll the data-ready bit of the channel itself
	// rather than sleeping a fixed time, so a slow reference still returns a correct value.
	for (unsigned attempt = 0; attempt < 200; ++attempt) {
		uint8_t ready = 0;
		if (!spmiRead(spmi, kSpmISlavePm8150, adcChannelConfig(channel), &ready)) {
			return false;
		}
		if (ready & 0x80) {
			return spmiRead16(spmi, kSpmISlavePm8150, adcChannelResult(channel), raw);
		}
		IODelay(10);
	}
	return false;
}

bool batteryRead(const SpmI *spmi, struct BatteryReading *reading)
{
	if (!spmi || !reading) {
		return false;
	}
	memset(reading, 0, sizeof(*reading));

	uint16_t raw = 0;
	if (adcReadRaw(spmi, kChannelVphPwr, &raw)) {
		reading->millivolts = uint32_t(raw) * kMillivoltsNumerator / kMillivoltsDenominator;
		reading->voltageValid = true;
	}
	if (adcReadRaw(spmi, kChannelDieTemp, &raw)) {
		// 0.25 K per count, offset so that raw 0 is -40 C.
		reading->pmicDeciKelvin = 250 + uint32_t(raw) * 25 / 100;
		reading->temperatureValid = true;
	}
	reading->present = reading->voltageValid;
	return reading->voltageValid || reading->temperatureValid;
}

// A single-cell pack sits between about 3.0 V empty and 4.4 V full. The curve is deliberately linear
// between those: without a Coulomb counter a piecewise curve would only look more precise than the
// measurement is.
uint32_t batteryPercentFromMillivolts(uint32_t millivolts)
{
	const uint32_t emptyMv = 3000, fullMv = 4400;
	if (millivolts <= emptyMv) {
		return 0;
	}
	if (millivolts >= fullMv) {
		return 100;
	}
	return (millivolts - emptyMv) * 100 / (fullMv - emptyMv);
}
