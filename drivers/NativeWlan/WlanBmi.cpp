// SPDX-License-Identifier: BSD-2-Clause-Patent
// BMI: the boot manager protocol that loads the firmware before any copy engine exists.
//
// BMI is the reason the download does not have to wait for the rings. It is a request/response
// protocol over the control mailbox: the host writes a command word and its arguments, the target
// answers in the same mailbox, and the commands are a small closed set. That makes it implementable
// and host-testable on its own, with the mailbox transport supplied as callbacks the way the WLFW
// handshake already takes its transport.
//
// The command numbers below are the ones the vendor downloader uses; the vendor module on this board
// is qca_cld3_wlan.ko with the athwlan.bin image, so the numbering is that downloader's.
#include <IOKit/IOLib.h>
#include <string.h>

extern "C" {
#include "WlanBmi.h"
}

// Command words. Only the low byte is the command; the rest is the length or the address, which is
// why each helper below packs its own argument rather than the caller doing it.
enum : uint32_t {
	kBmiDone = 0,
	kBmiWrite = 1,
	kBmiRead = 2,
	kBmiExecute = 3,
	kBmiSetAppStart = 4,
	kBmiReadSocRegister = 5,
	kBmiWriteSocRegister = 6,
	kBmiDoneResponse = 0,
	kBmiError = 0xff,
};

// A write carries the target address in the word above the command and the byte count in the word
// below it; the payload follows. A read carries the same address and count and expects the bytes back.
static uint32_t writeCommand(uint32_t address, uint32_t length)
{
	return (kBmiWrite & 0xff) | ((address & 0xffff) << 16) | ((length & 0xff) << 8);
}

static uint32_t readCommand(uint32_t address, uint32_t length)
{
	return (kBmiRead & 0xff) | ((address & 0xffff) << 16) | ((length & 0xff) << 8);
}

bool wlanBmiLoad(const struct wlan_bmi_ops *ops, const uint8_t *image, size_t size)
{
	if (!ops || !ops->write || !ops->read || !ops->execute || !image || size == 0) {
		return false;
	}

	// Chunk size is the mailbox width: the protocol moves at most 256 bytes per write, and a larger
	// chunk would silently wrap the count field rather than fail.
	const size_t chunk = 256;
	for (size_t offset = 0; offset < size; offset += chunk) {
		const size_t remaining = size - offset;
		const size_t length = remaining < chunk ? remaining : chunk;
		if (ops->write(ops->ctx, writeCommand(uint32_t(offset), uint32_t(length)),
		               image + offset, length) != 0) {
			IOLog("PICASSO_WLAN bmi: write failed at %zu of %zu\n", offset, size);
			return false;
		}
	}

	// The entry point is where the image asked to be started, which the target knows from its own
	// header; the host only says "run".
	if (ops->execute(ops->ctx) != 0) {
		IOLog("PICASSO_WLAN bmi: execute failed\n");
		return false;
	}
	IOLog("PICASSO_WLAN bmi: %zu bytes downloaded and started\n", size);
	return true;
}

bool wlanBmiReadSocRegister(const struct wlan_bmi_ops *ops, uint32_t address, uint32_t *value)
{
	if (!ops || !ops->read || !value) {
		return false;
	}
	uint8_t bytes[4] = {};
	if (ops->read(ops->ctx, readCommand(address, 4), bytes, sizeof(bytes)) != 0) {
		return false;
	}
	// The target answers little-endian, the same order it reads the command in.
	*value = uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
	return true;
}

bool wlanBmiWriteSocRegister(const struct wlan_bmi_ops *ops, uint32_t address, uint32_t value)
{
	if (!ops || !ops->write) {
		return false;
	}
	uint8_t bytes[4] = {
		uint8_t(value & 0xff), uint8_t((value >> 8) & 0xff),
		uint8_t((value >> 16) & 0xff), uint8_t((value >> 24) & 0xff),
	};
	return ops->write(ops->ctx, writeCommand(address, 4), bytes, sizeof(bytes)) == 0;
}
