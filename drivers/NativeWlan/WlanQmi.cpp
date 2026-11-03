// SPDX-License-Identifier: BSD-2-Clause-Patent
// QMI framing: the messages the WLFW handshake exchanges once the firmware is running.
//
// A QMI message is a seven-byte header and a run of type-length-value records. The header carries the
// kind (request, response, indication), a transaction id the response echoes, the message id and the
// payload length. The framing is small enough to be worth getting exactly right here rather than in
// the transport, because a header written with the wrong byte order produces a message the target
// accepts and then ignores, which looks like a dead radio rather than a bug.
//
// Everything is little-endian on the wire.
#include "WlanQmi.h"

#include <string.h>

enum : uint8_t {
	kQmiRequest = 0x00,
	kQmiResponse = 0x02,
	kQmiIndication = 0x04,
};

enum : size_t {
	kQmiHeaderSize = 7,
	kQmiTlvHeaderSize = 3,
};

static void put16(uint8_t *at, uint16_t value)
{
	at[0] = uint8_t(value & 0xff);
	at[1] = uint8_t(value >> 8);
}

static uint16_t get16(const uint8_t *at)
{
	return uint16_t(at[0]) | (uint16_t(at[1]) << 8);
}

size_t wlanQmiWriteHeader(uint8_t *buffer, size_t capacity, uint8_t kind, uint16_t transaction,
                          uint16_t messageId, size_t payloadLength)
{
	if (!buffer || capacity < kQmiHeaderSize || payloadLength > 0xffff) {
		return 0;
	}
	buffer[0] = kind;
	put16(buffer + 1, transaction);
	put16(buffer + 3, messageId);
	put16(buffer + 5, uint16_t(payloadLength));
	return kQmiHeaderSize;
}

bool wlanQmiParseHeader(const uint8_t *buffer, size_t length, struct wlan_qmi_header *header)
{
	if (!buffer || !header || length < kQmiHeaderSize) {
		return false;
	}
	header->kind = buffer[0];
	header->transaction = get16(buffer + 1);
	header->messageId = get16(buffer + 3);
	header->payloadLength = get16(buffer + 5);
	// A header that claims more payload than arrived is the one case worth rejecting outright: the
	// TLV walk below would otherwise read past the buffer.
	return size_t(header->payloadLength) + kQmiHeaderSize <= length;
}

size_t wlanQmiWriteTlv(uint8_t *buffer, size_t capacity, uint8_t type, const void *value, size_t length)
{
	const size_t needed = kQmiTlvHeaderSize + length;
	if (!buffer || capacity < needed || length > 0xffff) {
		return 0;
	}
	buffer[0] = type;
	put16(buffer + 1, uint16_t(length));
	if (length) {
		memcpy(buffer + kQmiTlvHeaderSize, value, length);
	}
	return needed;
}

bool wlanQmiNextTlv(const uint8_t *payload, size_t payloadLength, size_t *cursor,
                    struct wlan_qmi_tlv *tlv)
{
	if (!payload || !cursor || !tlv) {
		return false;
	}
	if (*cursor + kQmiTlvHeaderSize > payloadLength) {
		return false;
	}
	const uint8_t type = payload[*cursor];
	const uint16_t length = get16(payload + *cursor + 1);
	if (*cursor + kQmiTlvHeaderSize + length > payloadLength) {
		return false;
	}
	tlv->type = type;
	tlv->length = length;
	tlv->value = payload + *cursor + kQmiTlvHeaderSize;
	*cursor += kQmiTlvHeaderSize + length;
	return true;
}

// Builds a request whose payload is a single TLV, which is the shape every WLFW request has: a
// capability request carries its mask, a mode request its mode, and so on.
size_t wlanQmiBuildRequest(uint8_t *buffer, size_t capacity, uint16_t transaction, uint16_t messageId,
                           uint8_t tlvType, const void *tlvValue, size_t tlvLength)
{
	// The payload is written first at its final offset so the header can carry its length.
	if (!buffer || capacity < kQmiHeaderSize + kQmiTlvHeaderSize + tlvLength) {
		return 0;
	}
	const size_t payload = wlanQmiWriteTlv(buffer + kQmiHeaderSize, capacity - kQmiHeaderSize,
	                                       tlvType, tlvValue, tlvLength);
	if (payload == 0 && tlvLength != 0) {
		return 0;
	}
	if (wlanQmiWriteHeader(buffer, capacity, kQmiRequest, transaction, messageId, payload) == 0) {
		return 0;
	}
	return kQmiHeaderSize + payload;
}
