// SPDX-License-Identifier: BSD-2-Clause-Patent
// Battery and charger readings from the PMIC ADCs.
//
// Two PMICs matter here. pm8150 (slave 0) carries the system rails and the die temperature; pm8150b
// (slave 2) carries the SMB5 charger and the channels that describe the pack and the USB input:
//
//   vbat_sns         0x84   the pack, through a 1/3 divider
//   usb_in_v_div_16  0x08   the USB input, through a 1/16 divider
//   usb_in_i_uv      0x07   the drop across the input sense resistor, in microvolts
//   chg_temp         0x09   charger die temperature
//   vph_pwr          0x83   the system rail
//   bat_therm        0x4a   the pack thermistor
//
// The ADC is the 1.875 V reference converter, so a conversion is raw * 1875 / 32767 millivolts and
// the channel's prescale multiplies that back up to the pin voltage.
#include "Battery.h"
#include "SpmI.h"

#include <IOKit/IOLib.h>
#include <string.h>

// VADC block, identical layout in every PMIC on this board: peripheral 0x3100, 0x100 bytes, channel
// n at 0x2600 + 0x100 * n, result at +0x42.
static const SpmIRegister kAdcStatus1 = {0x3100, 0x08};
static const SpmIRegister kAdcStatus2 = {0x3100, 0x0a};

static SpmIRegister adcChannelConfig(unsigned channel)
{
	return {0x3100, uint16_t(0x2600 + 0x100 * channel)};
}

static SpmIRegister adcChannelResult(unsigned channel)
{
	return {0x3100, uint16_t(0x2600 + 0x100 * channel + 0x42)};
}

enum : unsigned {
	kChannelVbatSns = 0x84,
	kChannelVphPwr = 0x83,
	kChannelUsbInCurrent = 0x07,
	kChannelUsbInVoltage = 0x08,
	kChannelChgTemp = 0x09,
	kChannelDieTemp = 0x06,
};

// Prescale of each channel, from the channel nodes in the board overlay.
enum : uint32_t {
	kPrescaleVbat = 3,
	kPrescaleUsbVoltage = 16,
	kPrescaleDirect = 1,
};

// The SMB5 input sense resistor is 10 milliohms, so microvolts / 10 gives milliamps.
static const uint32_t kSenseMilliohms = 10;

static bool adcReadRaw(const SpmI *spmi, uint8_t slave, unsigned channel, uint16_t *raw)
{
	uint8_t status1 = 0, status2 = 0;
	if (!spmiRead(spmi, slave, kAdcStatus1, &status1) ||
	    !spmiRead(spmi, slave, kAdcStatus2, &status2)) {
		return false;
	}
	if (!spmiWrite(spmi, slave, kAdcStatus1, status1) ||
	    !spmiWrite(spmi, slave, kAdcStatus2, status2)) {
		return false;
	}
	for (unsigned attempt = 0; attempt < 200; ++attempt) {
		uint8_t ready = 0;
		if (!spmiRead(spmi, slave, adcChannelConfig(channel), &ready)) {
			return false;
		}
		if (ready & 0x80) {
			return spmiRead16(spmi, slave, adcChannelResult(channel), raw);
		}
		IODelay(10);
	}
	return false;
}

// raw * 1875 / 32767 * prescale, kept in integers and rounded.
static uint32_t adcMillivolts(uint16_t raw, uint32_t prescale)
{
	const uint32_t microvolts = uint32_t(raw) * 1875000u * prescale / 32767u;
	return (microvolts + 500) / 1000;
}

bool batteryRead(const SpmI *spmi, struct BatteryReading *reading)
{
	if (!spmi || !reading) {
		return false;
	}
	memset(reading, 0, sizeof(*reading));

	uint16_t raw = 0;

	// The pack sense is the real cell voltage; vph_pwr is the fallback because it sits a little
	// below it under load and is present even when the charger PMIC does not answer.
	if (adcReadRaw(spmi, kSpmISlavePm8150B, kChannelVbatSns, &raw)) {
		reading->millivolts = adcMillivolts(raw, kPrescaleVbat);
		reading->voltageValid = true;
	} else if (adcReadRaw(spmi, kSpmISlavePm8150, kChannelVphPwr, &raw)) {
		reading->millivolts = adcMillivolts(raw, kPrescaleVbat);
		reading->voltageValid = true;
	}

	if (adcReadRaw(spmi, kSpmISlavePm8150, kChannelDieTemp, &raw)) {
		reading->pmicDeciKelvin = 250 + uint32_t(raw) * 25 / 100;
		reading->temperatureValid = true;
	}

	// Charger side. usb_in_v_div_16 is only meaningful when something is plugged in, which shows up
	// as a non-zero input voltage rather than as a separate presence bit.
	if (adcReadRaw(spmi, kSpmISlavePm8150B, kChannelUsbInVoltage, &raw)) {
		const uint32_t millivolts = adcMillivolts(raw, kPrescaleUsbVoltage);
		if (millivolts >= 3000) {
			reading->usbMillivolts = millivolts;
			reading->usbValid = true;
		}
	}
	if (adcReadRaw(spmi, kSpmISlavePm8150B, kChannelUsbInCurrent, &raw)) {
		// The channel reports microvolts across the sense resistor.
		const uint32_t microvolts = uint32_t(raw) * 1875000u / 32767u;
		reading->usbMilliamps = microvolts / kSenseMilliohms / 1000u;
		reading->usbCurrentValid = true;
	}
	if (reading->usbValid && reading->usbCurrentValid) {
		// Milliwatts; the product of two 32-bit millivolt/milliamp values stays well inside range
		// for any input this board can see.
		reading->chargerMilliwatts = reading->usbMillivolts * reading->usbMilliamps / 1000u;
		reading->powerValid = true;
	}
	if (adcReadRaw(spmi, kSpmISlavePm8150B, kChannelChgTemp, &raw)) {
		reading->chargerDeciKelvin = 250 + uint32_t(raw) * 25 / 100;
		reading->chargerTemperatureValid = true;
	}

	reading->present = reading->voltageValid;
	return reading->voltageValid || reading->usbValid || reading->temperatureValid;
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
