// SPDX-License-Identifier: BSD-2-Clause-Patent
// BMI, the firmware download protocol that runs before any copy engine exists.
#pragma once

#include <stddef.h>
#include <stdint.h>

// The mailbox transport, supplied by the caller. write() sends a command word and its payload and
// returns 0 when the target acknowledged; read() sends a command word and fills the answer.
struct wlan_bmi_ops {
	void *ctx;
	int (*write)(void *ctx, uint32_t command, const void *payload, size_t length);
	int (*read)(void *ctx, uint32_t command, void *buffer, size_t length);
	int (*execute)(void *ctx);
};

// Downloads the image in 256-byte chunks and starts it. Returns false on the first chunk the target
// refuses, so a partial download is reported rather than followed by an execute that would run
// whatever happened to arrive.
bool wlanBmiLoad(const struct wlan_bmi_ops *ops, const uint8_t *image, size_t size);

// Target register access over the same mailbox, which is how the host reads the chip identity before
// the firmware is running.
bool wlanBmiReadSocRegister(const struct wlan_bmi_ops *ops, uint32_t address, uint32_t *value);
bool wlanBmiWriteSocRegister(const struct wlan_bmi_ops *ops, uint32_t address, uint32_t value);
