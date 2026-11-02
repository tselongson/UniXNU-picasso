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

static int wcssLoadImage(void *, const struct wlan_mbn *image)
{
	// The image has already been parsed and hash-checked by the caller. Handing it to the block means
	// writing it into the MSA region and pointing the firmware entry at it, which needs the copy
	// engine ring; without the ring the load is reported as not done rather than pretended.
	if (!image || !gWcss.ready) {
		return -1;
	}
	IOLog("PICASSO_WLAN wcss: image load needs the copy engine ring, not implemented\n");
	return -1;
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
