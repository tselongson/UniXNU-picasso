// SPDX-License-Identifier: BSD-2-Clause-Patent
// No-op stand-ins for the display reporting entry points the USB driver calls.
//
// The USB console polls the display layer for its state and hands it its scanout. Both are defined in
// the display layer's archive, which is only linked when the layer is built -- and the layer cannot be
// built on a machine without a prelink candidate from an earlier run, because its build qualifies
// itself against the kernel it was linked with last time.
//
// So when the display layer is off, these definitions take its place. They are no-ops that return
// success rather than error: the USB driver treats an error as "display is there but unhappy" and
// retries, and what it should hear here is "there is no display to poll" -- which for the link is the
// same as a quiet success.
#include <IOKit/IOLib.h>

#include <mach/kern_return.h>
#include <libkern/OSTypes.h>

extern "C" {

// The USB console asks to register the display and its reporting; without a display there is nothing
// to register, and the console carries on without one.
kern_return_t native_register_display_and_report(void)
{
	printf("NATIVE DISPLAY: absent, USB console continues without a display to poll\n");
	return KERN_SUCCESS;
}

// Per-poll publication. Without a display there is no level to publish; returning success says the
// absence is expected rather than a fault the caller should retry.
kern_return_t native_publish_display_and_report(int level)
{
	(void)level;
	return KERN_SUCCESS;
}

// A state poll with no display reports the state that says so.
void native_legacy_display_state_report(void)
{
	// A null report: the display layer's own implementation sends a short state word the console
	// displays, and "none" is the honest one here.
	printf("NATIVE DISPLAY: state none\n");
}

} // extern "C"
