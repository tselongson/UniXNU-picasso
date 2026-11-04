// SPDX-License-Identifier: BSD-2-Clause-Patent
// The service that polls the PMIC and publishes the readings.
#pragma once

#include <IOKit/IOService.h>

#include "SpmI.h"

class IOWorkLoop;
class IOTimerEventSource;

class NativePmicService : public IOService {
	OSDeclareDefaultStructors(NativePmicService)
public:
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;

private:
	void tick();

	SpmI spmi {};
	IOWorkLoop *workLoop = nullptr;
	IOTimerEventSource *timer = nullptr;
};
