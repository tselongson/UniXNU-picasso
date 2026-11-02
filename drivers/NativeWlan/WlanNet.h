// SPDX-License-Identifier: BSD-2-Clause-Patent
// The network side of the WiFi module: an ifnet the stack routes through, plus the Apple80211
// requests the wireless framework issues against it.
//
// This layer does not touch the radio. It hands frames to wlanTxFrame() and takes them from
// wlanNetInput(), which is the whole contract with the WCSS driver, so the two can be developed and
// tested apart from each other.
#pragma once

#include <stdint.h>
#include <sys/types.h>
#include <sys/kernel_types.h>

struct mbuf;

// Creates the interface and announces it. Returns false if the ifnet could not be allocated or
// attached, which is the only failure this layer can have on its own.
bool wlanNetAttach(const uint8_t mac[6]);

// Received frame, ownership taken. Called from the WCSS receive path.
void wlanNetInput(mbuf_t frame);

// Link state, which also posts the stack's link event.
void wlanNetLink(bool up);

// Radio on or off as the framework sees it.
void wlanNetSetPowered(bool powered);

// Implemented by the WCSS driver. The first takes ownership of the frame; the second answers one
// Apple80211 request and returns an errno.
void wlanTxFrame(mbuf_t frame);
int wlanApple80211(bool set, void *request);
