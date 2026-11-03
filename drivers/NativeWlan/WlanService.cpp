// SPDX-License-Identifier: BSD-2-Clause-Patent
// The service that runs the WLAN bring-up in order and keeps the interface's state honest.
//
// The sequence is fixed by the hardware: the block has to answer at all, then the firmware goes down
// over BMI, then the handshake brings the firmware up, and only then is there anything for the
// interface to carry. Each step reports what it is waiting for, so a boot log says where it stopped
// rather than only that it did.
#include "WlanService.h"

#include "WlanCe.h"
#include "WlanNet.h"
#include "WlanWcss.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>

#define super IOService
OSDefineMetaClassAndStructors(NativeWlanService, IOService)

// The address the firmware is started at, and the memory it is given. Both come from the board's
// fixed MSA region in the device tree; a wrong value here is a firmware that starts and faults
// immediately, which is why they are named rather than inlined.
static const uint64_t kMsaBase = 0x0b000000;   // smmu_iova_ipa window of qcom,icnss
static const uint64_t kMsaSize = 0x00100000;

// A locally administered address derived from the board name, used until the firmware reports the
// one it was calibrated with. The second byte's bit 1 marks it local so it cannot collide with a real
// vendor prefix.
static const uint8_t kBootstrapMac[6] = {0x02, 0x50, 0x49, 0x43, 0x41, 0x00};

bool NativeWlanService::start(IOService *provider)
{
	if (!super::start(provider)) {
		return false;
	}
	setName("Picasso WLAN");
	setProperty("NativeWlan", true);

	if (!wlanWcssMap()) {
		IOLog("PICASSO_WLAN: WCSS window could not be mapped\n");
		setProperty("WcssMapped", false);
		registerService();
		return true;   // the node still exists, so a client can see why
	}
	setProperty("WcssMapped", true);

	uint32_t welcome = 0;
	if (!wlanWcssProbe(&welcome)) {
		// A block that reads all ones is unclocked, which on this board means its power rail was
		// never voted for. Saying so is more useful than reporting a probe failure with no cause.
		IOLog("PICASSO_WLAN: WCSS did not answer, its rail is probably unpowered\n");
		setProperty("WcssAnswered", false);
		registerService();
		return true;
	}
	setProperty("WcssAnswered", true);
	setProperty("WcssWelcome", uint64_t(welcome), 32);

	if (!wlanCeInit(kWcssRegBase)) {
		IOLog("PICASSO_WLAN: copy engines could not be set up\n");
		registerService();
		return true;
	}
	uint32_t chipId = 0;
	if (wlanCeProbeChipId(kWcssRegBase, &chipId)) {
		setProperty("WcssChipId", uint64_t(chipId), 32);
	}

	// The interface exists before the radio does, so the stack has somewhere to route once the link
	// comes up and a client can see the interface while it is still down.
	if (!wlanNetAttach(kBootstrapMac)) {
		IOLog("PICASSO_WLAN: interface could not be attached\n");
		setProperty("InterfaceAttached", false);
		registerService();
		return true;
	}
	setProperty("InterfaceAttached", true);

	// Bring-up runs on its own thread: the handshake waits on the target, and doing that on the
	// start() path would hold the service's registration behind it.
	thread_t thread = nullptr;
	if (kernel_thread_start(&NativeWlanService::bringUpThread, this, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		IOLog("PICASSO_WLAN: bring-up thread could not start\n");
	}

	registerService();
	return true;
}

void NativeWlanService::stop(IOService *provider)
{
	super::stop(provider);
}

void NativeWlanService::bringUpThread(void *context, wait_result_t)
{
	NativeWlanService *self = static_cast<NativeWlanService *>(context);
	if (!self) {
		return;
	}
	// Every step below reports through a property as well as the log, so the state survives the boot
	// log being scrolled away.
	const bool handshake = wlanWcssRunHandshake(nullptr, kMsaBase, kMsaSize);
	self->setProperty("FirmwareHandshake", handshake);
	if (handshake) {
		// Only now is there a radio behind the interface; the link event is what makes the stack and
		// the framework treat it as usable.
		wlanNetLink(true);
		self->setProperty("LinkUp", true);
	} else {
		IOLog("PICASSO_WLAN: firmware handshake did not complete, interface stays down\n");
		self->setProperty("LinkUp", false);
	}
	thread_terminate(current_thread());
}
