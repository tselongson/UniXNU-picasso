// SPDX-License-Identifier: BSD-2-Clause-Patent
// The service that actually runs the PMIC work: one timer, one reading, one decision, published as
// properties on the service node.
//
// Everything below it is pure register work with no IOKit dependency, which is what makes the two
// halves separable: the readings and the charger switches can be exercised on a host build, and this
// file only owns the scheduling and the property publication.
//
// The period is 10 seconds because the pack moves by a few millivolts in that time even under heavy
// load, and because the full threshold carries 200 mV of hysteresis -- polling faster would only
// re-read the same value, and polling slower would let the pack sit above the threshold longer than
// it needs to.
#include "PmicService.h"

#include "Battery.h"
#include "Charger.h"
#include "SpmI.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOWorkLoop.h>
#include <pexpert/pexpert.h>

#define super IOService
OSDefineMetaClassAndStructors(NativePmicService, IOService)

static const uint32_t kPollMilliseconds = 10000;

bool NativePmicService::start(IOService *provider)
{
	if (!super::start(provider)) {
		return false;
	}
	if (!spmiInit(&spmi)) {
		IOLog("NativePmic: SPMI windows could not be mapped\n");
		return false;
	}

	workLoop = IOWorkLoop::workLoop();
	if (!workLoop) {
		IOLog("NativePmic: no work loop\n");
		spmi.ready = false;
		super::stop(provider);
		return false;
	}
	timer = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &NativePmicService::tick));
	if (!timer || workLoop->addEventSource(timer) != kIOReturnSuccess) {
		IOLog("NativePmic: no timer\n");
		if (timer) {
			timer->release();
			timer = nullptr;
		}
		workLoop->release();
		workLoop = nullptr;
		spmi.ready = false;
		super::stop(provider);
		return false;
	}

	setName("Picasso PMIC");
	setProperty("NativePmic", true);

	// Publish the first reading before registering, so a client that looks at the node right after it
	// appears already sees values instead of zeroes.
	tick();
	registerService();
	timer->setTimeoutMS(kPollMilliseconds);
	return true;
}

void NativePmicService::stop(IOService *provider)
{
	if (timer) {
		timer->cancelTimeout();
		if (workLoop) {
			workLoop->removeEventSource(timer);
		}
		timer->release();
		timer = nullptr;
	}
	if (workLoop) {
		workLoop->release();
		workLoop = nullptr;
	}
	spmi.ready = false;
	super::stop(provider);
}

void NativePmicService::tick()
{
	struct BatteryReading reading;
	const bool read = batteryRead(&spmi, &reading);

	if (read && reading.voltageValid) {
		setProperty("BatteryVoltageMillivolts", uint64_t(reading.millivolts), 32);
		setProperty("BatteryPercent", uint64_t(batteryPercentFromMillivolts(reading.millivolts)), 32);
		setProperty("BatteryPresent", true);
	}
	if (read && reading.usbValid) {
		setProperty("UsbInputMillivolts", uint64_t(reading.usbMillivolts), 32);
		setProperty("UsbInputMilliamps", uint64_t(reading.usbMilliamps), 32);
		setProperty("ChargerMilliwatts", uint64_t(reading.chargerMilliwatts), 32);
		setProperty("ExternalPowerConnected", true);
	} else {
		setProperty("ExternalPowerConnected", false);
	}
	if (read && reading.temperatureValid) {
		setProperty("PmicDeciKelvin", uint64_t(reading.pmicDeciKelvin), 32);
	}
	if (read && reading.chargerTemperatureValid) {
		setProperty("ChargerDeciKelvin", uint64_t(reading.chargerDeciKelvin), 32);
	}

	// Which phase the block is running. Trickle and taper are normal, not a fault, so they are
	// published as they are rather than being folded into a boolean.
	enum ChargerPhase phase = kChargerPhaseInhibit;
	if (chargerReadPhase(&spmi, &phase)) {
		setProperty("ChargePhase", chargerPhaseName(phase));
	}

	// The full policy only does anything with a pack and an input; with no cable the charger block
	// keeps its last state and there is nothing to decide.
	if (read && reading.voltageValid && reading.usbValid) {
		struct ChargerState state;
		if (chargerApplyFullPolicy(&spmi, &reading, &state)) {
			setProperty("ChargingEnabled", state.chargingEnabled);
			setProperty("InputSuspended", state.inputSuspended);
			setProperty("PowerPathStatus", uint64_t(state.powerPathStatus), 32);
		}
	}

	timer->setTimeoutMS(kPollMilliseconds);
}

extern "C" IOService *native_pmic_allocate(void)
{
	return new NativePmicService;
}

// Boot-monitor entry point (weak-imported by BootProgress.c): start the service once, gated on
// native-pmic=<n>. The service attaches to the resource service, the same provider the display
// publish path uses for a top-level IOService. Every failure path releases and returns, so a
// missing SPMI window can never stall the boot monitor or the panel.
extern "C" void native_pmic_probe(void)
{
	static int done;
	if (done) {
		return;
	}
	done = 1;
	unsigned enable = 0;
	if (!PE_parse_boot_argn("native-pmic", &enable, sizeof enable) || !enable) {
		return;
	}
	IOService *pmic = native_pmic_allocate();
	if (!pmic) {
		printf("PICASSO_PMIC allocate failed\n");
		return;
	}
	if (!pmic->init()) {
		pmic->release();
		printf("PICASSO_PMIC init failed\n");
		return;
	}
	IOService *parent = IOService::getResourceService();
	if (!parent) {
		pmic->release();
		printf("PICASSO_PMIC start failed\n");
		return;
	}
	if (!pmic->attach(parent)) {
		pmic->release();
		printf("PICASSO_PMIC start failed\n");
		return;
	}
	if (!pmic->start(parent)) {
		pmic->detach(parent);
		pmic->release();
		printf("PICASSO_PMIC start failed\n");
		return;
	}
	pmic->release();
	printf("PICASSO_PMIC started\n");
}
