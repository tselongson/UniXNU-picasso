// SPDX-License-Identifier: BSD-2-Clause-Patent
// The service that runs the WLAN bring-up in order.
#pragma once

#include <IOKit/IOService.h>

class NativeWlanService : public IOService {
	OSDeclareDefaultStructors(NativeWlanService)
public:
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;

private:
	static void bringUpThread(void *context, wait_result_t);
};
