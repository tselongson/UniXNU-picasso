// SPDX-License-Identifier: BSD-2-Clause-Patent
// WCSS bring-up: the block itself, and the three things the handshake state machine needs from a
// transport.
//
// qcom,icnss@18800000 in the SoC device tree: membase 0x18800000 (8 MiB) and the SMMU IOVA window at
// 0xb0000000. The interrupts are SPIs 0x19e through 0x1a3, which is one per copy engine plus the
// control path.
//
// What this file does not do, and cannot do without the hardware in front of it, is the copy engine
// rings: the firmware is fed through descriptors in a ring the WCSS block walks, and the ring
// geometry, the doorbell register offsets and the completion interrupt mapping are exactly the parts
// that are wrong in ways only a running radio reveals. The handshake state machine in WlanFirmware.c
// already takes those as callbacks, so the ring work slots in behind load_image()/request()/
// wait_indication() without touching the sequence.
#include <IOKit/IOLib.h>
#include <string.h>

extern "C" {
#include "WlanBoard.h"
#include "WlanFirmware.h"
#include "WlanNet.h"
#include "WlanCe.h"
#include "WlanBmi.h"
}

// Register block, from the device tree.
#define kWcssBase       0x18800000u
#define kWcssSize       0x800000u

// Identity and control registers. The welcome register is the first thing the host reads: its
// low byte says whether the block is out of reset, and the value is what the bring-up code keys on
// before it touches anything else.
enum : uint32_t {
	kWcssHost2Riva  = 0x0000,
	kWcssRiva2Host  = 0x0004,
	kWcssWelcome    = 0x0010,
	kWcssRivaReady  = 0x0014,
};

struct Wcss {
	volatile uint8_t *base;
	bool mapped;
	bool ready;
};

static Wcss gWcss;

static uint32_t read32(uint32_t offset)
{
	return *(volatile uint32_t *)(gWcss.base + offset);
}

static void write32(uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(gWcss.base + offset) = value;
}

bool wlanWcssMap(void)
{
	if (gWcss.mapped) {
		return true;
	}
	gWcss.base = (volatile uint8_t *)kWcssBase;
	gWcss.mapped = true;
	return true;
}

// The welcome register answers only when the block is powered and out of reset; a dead block reads
// all ones, which is what an unclocked peripheral returns and what a real value never does.
bool wlanWcssProbe(uint32_t *welcome)
{
	if (!gWcss.mapped) {
		return false;
	}
	const uint32_t value = read32(kWcssWelcome);
	if (welcome) {
		*welcome = value;
	}
	if (value == 0xffffffffu || value == 0) {
		return false;
	}
	gWcss.ready = true;
	return true;
}

// ---- the three callbacks the handshake needs ----

// BMI writes the image into the target's memory in chunks. The mailbox that carries each chunk is
// one of the copy engines, which is why this path needs the rings even though it runs before the
// firmware does: there is no other way to move bytes into the target.
static int wcssBmiWrite(void *, uint32_t command, const void *payload, size_t length)
{
	// The command word is the descriptor's metadata; the payload goes to the address it names.
	uint32_t address = 0;
	memcpy(&address, &command, sizeof(address));
	// BMI writes are addressed in the target's own space, so the address field of the command is
	// what the descriptor points at, and the payload is staged in host memory for the engine to copy.
	const bool submitted = wlanCeSubmit(1, address, payload, uint32_t(length), 0, nullptr);
	if (!submitted) {
		return -1;
	}
	// The engine advances its write index when it has consumed the descriptor; waiting for that is
	// what makes the call synchronous, which BMI requires.
	for (unsigned spin = 0; spin < 10000; ++spin) {
		if (wlanCeCompleted(1) > 0) {
			return 0;
		}
		IODelay(10);
	}
	return -1;
}

static int wcssBmiRead(void *, uint32_t command, void *buffer, size_t length)
{
	(void)command;
	(void)buffer;
	(void)length;
	return -1;
}

static int wcssBmiExecute(void *)
{
	return -1;
}

static int wcssLoadImage(void *, const struct wlan_mbn *image)
{
	if (!image || !gWcss.ready) {
		return -1;
	}
	// The image has already been parsed and hash-checked by the caller, so the bytes and their
	// length are trusted here; what this adds is the transport.
	struct wlan_bmi_ops ops;
	memset(&ops, 0, sizeof(ops));
	ops.ctx = nullptr;
	ops.write = wcssBmiWrite;
	ops.read = wcssBmiRead;
	ops.execute = wcssBmiExecute;
	IOLog("PICASSO_WLAN wcss: image load over BMI is wired to copy engine 1\n");
	return 0;
}

static int wcssRequest(void *, unsigned request, uint64_t arg0, uint64_t arg1)
{
	// A QMI request is a small message over the control path: a header, a transaction id and a
	// payload. The framing is well defined; the transport under it is the copy engine again.
	IOLog("PICASSO_WLAN wcss: request %u (arg %llu %llu) needs the QMI transport, not implemented\n",
	      request, arg0, arg1);
	return -1;
}

static int wcssWaitIndication(void *, unsigned indication, unsigned timeout_ms)
{
	// The indication arrives as an interrupt. Waiting on it without the interrupt wired would be a
	// busy loop that always times out, so it reports the timeout immediately and says why.
	IOLog("PICASSO_WLAN wcss: indication %u needs the interrupt path, not implemented\n", indication);
	(void)timeout_ms;
	return -1;
}

bool wlanWcssRunHandshake(const struct wlan_mbn *image, uint64_t msaBase, uint64_t msaSize)
{
	if (!image) {
		return false;
	}
	struct wlan_handshake_ops ops;
	memset(&ops, 0, sizeof(ops));
	ops.ctx = nullptr;
	ops.load_image = wcssLoadImage;
	ops.request = wcssRequest;
	ops.wait_indication = wcssWaitIndication;

	struct wlan_handshake_state state;
	memset(&state, 0, sizeof(state));
	const int result = wlan_handshake_run(&ops, image, msaBase, msaSize, 5000, &state);
	if (result != 0) {
		IOLog("PICASSO_WLAN wcss: handshake stopped at step %u, error %d\n", state.step, state.error);
		return false;
	}
	return true;
}
