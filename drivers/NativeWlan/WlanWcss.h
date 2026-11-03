// SPDX-License-Identifier: BSD-2-Clause-Patent
// The WCSS block and the WLFW handshake that brings its firmware up.
#pragma once

#include <stdint.h>

struct wlan_mbn;

// Maps the block's register window. Returns false only when the window is not there at all.
bool wlanWcssMap(void);

// Reads the welcome register, which answers only when the block is powered and out of reset. A block
// that reads all ones is unclocked and its rail was probably never voted for.
bool wlanWcssProbe(uint32_t *welcome);

// Runs the firmware handshake against an image the caller already parsed and hash-checked.
bool wlanWcssRunHandshake(const struct wlan_mbn *image, uint64_t msaBase, uint64_t msaSize);
