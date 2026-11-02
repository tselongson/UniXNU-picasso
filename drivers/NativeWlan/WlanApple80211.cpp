// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Apple80211 requests the wireless framework issues against the interface.
//
// Two rules run through this file. A request this driver cannot answer is refused, never answered
// with zeroes: a zeroed answer is indistinguishable from a real one to the framework, so a zero RSSI
// reads as a genuine zero and an empty channel list reads as a genuine empty scan. And a request that
// only makes sense once associated is answered from the station state rather than from a guess, so
// the framework's picture of the link cannot drift ahead of the radio.
//
// The structures below are the ones the framework reads and writes. Fields whose meaning is not known
// are left zero, which is safe in a reply the framework only reads specific members of, and the
// request is logged with its type and length so an unhandled one can be identified from a boot log
// instead of guessed at.
#include <IOKit/IOLib.h>
#include <sys/types.h>
#include <sys/errno.h>
#include <sys/uio.h>
#include <string.h>

extern "C" {
#include "WlanBoard.h"
#include "WlanNet.h"
}

namespace {

struct apple80211req {
	char req_if_name[16];
	int32_t req_type;
	int32_t req_val;
	uint32_t req_len;
	user_addr_t req_data __attribute__((aligned(8)));
};

enum {
	kSsid = 1, kAuthType = 2, kChannel = 4, kTxPower = 7, kBssid = 9,
	kCardCaps = 12, kState = 13, kPhyMode = 14, kOpMode = 15,
	kRssi = 16, kNoise = 17, kPower = 19, kAssociate = 20,
	kDisassociate = 22, kSupportedChannels = 27, kDriverVersion = 43,
	kHardwareVersion = 44, kCurrentNetwork = 103,
};

// Interface states, as the framework numbers them.
enum {
	kStateDisconnected = 0,
	kStateAssociated = 2,
};

// The station state the WCSS driver maintains. Until that driver reports association this stays
// disconnected, which is the truthful answer rather than a placeholder.
struct StationState {
	bool powered;
	bool associated;
	uint8_t bssid[6];
	uint8_t ssid[32];
	uint32_t ssidLength;
	int32_t rssi;
};

StationState gStation;

int copyOut(apple80211req &request, const void *source, size_t size)
{
	if (!request.req_data || request.req_len < size) {
		return EINVAL;
	}
	if (copyout(source, request.req_data, size)) {
		return EFAULT;
	}
	request.req_len = static_cast<uint32_t>(size);
	return 0;
}

int answerValue(apple80211req &request, uint32_t value)
{
	return copyOut(request, &value, sizeof(value));
}

int answerBytes(apple80211req &request, const void *source, uint32_t size)
{
	return copyOut(request, source, size);
}

int copyIn(const apple80211req &request, void *destination, size_t size)
{
	if (!request.req_data || request.req_len < size) {
		return EINVAL;
	}
	return copyin(request.req_data, destination, size) ? EFAULT : 0;
}

} // namespace

int wlanApple80211(bool set, void *opaque)
{
	apple80211req &request = *static_cast<apple80211req *>(opaque);
	const int type = request.req_type;

	if (set) {
		switch (type) {
		case kPower: {
			// The framework sends a six-word structure and the power state is the third word.
			uint32_t words[6];
			if (copyIn(request, words, sizeof(words))) {
				return EFAULT;
			}
			gStation.powered = words[2] != 0;
			wlanNetSetPowered(gStation.powered);
			return 0;
		}
		case kAssociate: {
			// Association is the WCSS driver's to perform. Refusing here keeps the framework from
			// believing it succeeded and then waiting for a link that will not come up.
			IOLog("PICASSO_WLAN a80211: associate refused, no station driver yet\n");
			return ENOTSUP;
		}
		case kDisassociate:
			gStation.associated = false;
			memset(gStation.bssid, 0, sizeof(gStation.bssid));
			gStation.ssidLength = 0;
			return 0;
		default:
			IOLog("PICASSO_WLAN a80211: unhandled set type %d len %u\n", type, request.req_len);
			return ENOTSUP;
		}
	}

	switch (type) {
	case kPower:
		return answerValue(request, gStation.powered ? 1u : 0u);
	case kState:
		return answerValue(request, gStation.associated ? uint32_t(kStateAssociated) : uint32_t(kStateDisconnected));
	case kSsid:
		return answerBytes(request, gStation.ssid, gStation.ssidLength);
	case kBssid:
		if (!gStation.associated) {
			return ENOTSUP;
		}
		return answerBytes(request, gStation.bssid, sizeof(gStation.bssid));
	case kRssi:
		if (!gStation.associated) {
			return ENOTSUP;
		}
		return answerValue(request, uint32_t(gStation.rssi));
	case kNoise:
		// A noise floor the driver has not measured is refused rather than reported as zero, which
		// would claim a perfectly quiet channel.
		return ENOTSUP;
	case kChannel:
	case kTxPower:
	case kPhyMode:
	case kOpMode:
	case kSupportedChannels:
	case kCurrentNetwork:
		// All of these describe a link or a radio the WCSS driver has not brought up yet.
		return ENOTSUP;
	case kCardCaps:
		// One band, no hardware crypto offload claimed, no monitor mode.
		return answerValue(request, 0);
	case kDriverVersion:
		return answerValue(request, (1u << 16) | 0u);
	case kHardwareVersion:
		return answerValue(request, 0x00010000u);
	default:
		IOLog("PICASSO_WLAN a80211: unhandled get type %d len %u\n", type, request.req_len);
		return ENOTSUP;
	}
}

// Called by the WCSS driver when the station joins or leaves. Kept here so the framework-facing
// bookkeeping lives in one place rather than being spread across the radio code.
void wlanApple80211StationJoined(const uint8_t bssid[6], const uint8_t *ssid, uint32_t ssidLength, int32_t rssi)
{
	memcpy(gStation.bssid, bssid, 6);
	gStation.ssidLength = ssidLength > sizeof(gStation.ssid) ? sizeof(gStation.ssid) : ssidLength;
	memcpy(gStation.ssid, ssid, gStation.ssidLength);
	gStation.rssi = rssi;
	gStation.associated = true;
	wlanNetLink(true);
}

void wlanApple80211StationLeft(void)
{
	gStation.associated = false;
	gStation.ssidLength = 0;
	memset(gStation.bssid, 0, sizeof(gStation.bssid));
	wlanNetLink(false);
}
