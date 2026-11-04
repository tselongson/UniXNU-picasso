// SPDX-License-Identifier: BSD-2-Clause-Patent
// QMI framing: the messages the WLFW handshake exchanges once the firmware is running.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif


#include <stddef.h>
#include <stdint.h>

struct wlan_qmi_header {
	uint8_t kind;          // request 0, response 2, indication 4
	uint16_t transaction;
	uint16_t messageId;
	uint16_t payloadLength;
};

struct wlan_qmi_tlv {
	uint8_t type;
	uint16_t length;
	const uint8_t *value;
};

// Writes the seven-byte header and returns its size, or 0 when the buffer is too small.
size_t wlanQmiWriteHeader(uint8_t *buffer, size_t capacity, uint8_t kind, uint16_t transaction,
                          uint16_t messageId, size_t payloadLength);

// Reads a header and checks that the payload it claims actually arrived. A header promising more than
// the buffer holds is rejected, because the TLV walk would otherwise read past the end.
bool wlanQmiParseHeader(const uint8_t *buffer, size_t length, struct wlan_qmi_header *header);

// Appends one TLV and returns its size, or 0 when it does not fit.
size_t wlanQmiWriteTlv(uint8_t *buffer, size_t capacity, uint8_t type, const void *value, size_t length);

// Walks the records of a payload. The cursor starts at 0 and is advanced by each successful call.
bool wlanQmiNextTlv(const uint8_t *payload, size_t payloadLength, size_t *cursor,
                    struct wlan_qmi_tlv *tlv);

// Builds a request whose payload is one TLV, which is the shape every WLFW request has.
size_t wlanQmiBuildRequest(uint8_t *buffer, size_t capacity, uint16_t transaction, uint16_t messageId,
                           uint8_t tlvType, const void *tlvValue, size_t tlvLength);

#ifdef __cplusplus
}
#endif
