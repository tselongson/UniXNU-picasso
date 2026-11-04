// SPDX-License-Identifier: BSD-2-Clause
//
// Minimal GLINK SMEM transport for picasso (APPS <-> MODEM).
//
// Wire facts from Linux drivers/rpmsg/qcom_glink_smem.c and qcom_glink_native.c:
//   - SMEM item 478 holds the 32-byte descriptor: four little-endian FIFO indices in the
//     order tx.tail, tx.head, rx.tail, rx.head. APPS owns the descriptor words; the
//     remote endpoint advances the words it is responsible for.
//   - Item 479 is the APPS-to-remote FIFO, item 480 the remote-to-APPS FIFO. The whole
//     item is ring payload; the indices live in the descriptor, not in the FIFO.
//   - Every packet starts with { le16 cmd; le16 param1; le32 param2 } and a payload whose
//     length is implied by the command. head always advances 8-byte aligned, and writers
//     must publish the FIFO bytes before the head word.
//
// This file only brings the link up far enough to prove the remote endpoint is alive: it
// reports the descriptor, sends a VERSION request, and prints whatever comes back. Nothing
// here dereferences another host's SMEM partition, and every access is bounded by the item
// sizes read from the SMEM table.

#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <pexpert/pexpert.h>
#include <string.h>

extern "C" {
#include "WlanSmem.h"
}

namespace {

constexpr uint32_t kSmemBase = 0x80900000;
constexpr uint32_t kSmemSize = 0x200000;

constexpr uint16_t kCmdVersion = 0;
constexpr uint16_t kCmdVersionAck = 1;
constexpr uint16_t kCmdOpen = 2;
constexpr uint16_t kCmdClose = 3;
constexpr uint16_t kCmdOpenAck = 4;
constexpr uint16_t kCmdIntent = 5;
constexpr uint16_t kCmdRxIntent = 6;
constexpr uint16_t kCmdTxData = 7;
constexpr uint16_t kCmdTxDataCont = 8;
constexpr uint16_t kCmdReadNotif = 9;
constexpr uint16_t kCmdRxDone = 10;
constexpr uint16_t kCmdRxDoneWithReason = 11;

constexpr uint32_t kFifoFullReserve = 8;
constexpr uint32_t kTxBlockedReserve = 8;
constexpr uint32_t kFifoAlignment = 8;

volatile uint8_t *gSmem = nullptr;
size_t gSmemBytes = 0;
volatile uint8_t *gTxFifo = nullptr;
uint32_t gTxLength = 0;
volatile uint8_t *gRxFifo = nullptr;
uint32_t gRxLength = 0;
volatile uint32_t *gTxTail = nullptr;
volatile uint32_t *gTxHead = nullptr;
volatile uint32_t *gRxTail = nullptr;
volatile uint32_t *gRxHead = nullptr;
bool gAttached = false;
uint16_t gVersion = 0;

uint32_t readLe32(volatile const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t readLe16(volatile const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

int smemReadCallback(void *context, uint32_t offset, void *dst, uint32_t bytes)
{
    volatile const uint8_t *base = static_cast<volatile const uint8_t *>(context);
    if (!base || !dst || offset + bytes > kSmemSize) return CW_BOUNDS;
    uint8_t *out = static_cast<uint8_t *>(dst);
    for (uint32_t index = 0; index < bytes; ++index) out[index] = base[offset + index];
    return CW_OK;
}

void fifoWrite(uint32_t head, const void *data, uint32_t bytes)
{
    const uint8_t *src = static_cast<const uint8_t *>(data);
    uint32_t written = 0;
    while (written < bytes) {
        uint32_t at = head + written;
        if (at >= gTxLength) at -= gTxLength;
        uint32_t span = gTxLength - at;
        if (span > bytes - written) span = bytes - written;
        for (uint32_t index = 0; index < span; ++index) gTxFifo[at + index] = src[written + index];
        written += span;
    }
}

uint32_t txAvailable()
{
    const uint32_t head = *gTxHead, tail = *gTxTail;
    const uint32_t used = (tail <= head) ? (gTxLength - head + tail) : (tail - head);
    const uint32_t free = gTxLength - used;
    if (free < kFifoFullReserve + kTxBlockedReserve) return 0;
    return free - kFifoFullReserve - kTxBlockedReserve;
}

bool sendPacket(uint16_t command, uint16_t param1, uint32_t param2, const void *payload, uint32_t payloadBytes)
{
    if (!gAttached) return false;
    const uint32_t total = 8 + payloadBytes;
    if (txAvailable() < total) {
        printf("PICASSO_WLAN glink: tx full need=%u avail=%u\n", total, txAvailable());
        return false;
    }
    uint32_t head = *gTxHead;
    uint8_t header[8];
    header[0] = static_cast<uint8_t>(command & 0xff);
    header[1] = static_cast<uint8_t>(command >> 8);
    header[2] = static_cast<uint8_t>(param1 & 0xff);
    header[3] = static_cast<uint8_t>(param1 >> 8);
    header[4] = static_cast<uint8_t>(param2 & 0xff);
    header[5] = static_cast<uint8_t>((param2 >> 8) & 0xff);
    header[6] = static_cast<uint8_t>((param2 >> 16) & 0xff);
    header[7] = static_cast<uint8_t>((param2 >> 24) & 0xff);
    fifoWrite(head, header, sizeof header);
    head += sizeof header;
    if (head >= gTxLength) head -= gTxLength;
    if (payload && payloadBytes) {
        fifoWrite(head, payload, payloadBytes);
        head += payloadBytes;
        if (head >= gTxLength) head -= gTxLength;
    }
    // head must stay 8-byte aligned, exactly as glink_smem_tx_write() does.
    const uint32_t aligned = (head + kFifoAlignment - 1) & ~(kFifoAlignment - 1);
    head = aligned >= gTxLength ? aligned - gTxLength : aligned;
    __asm__ volatile("dsb sy" ::: "memory");
    *gTxHead = head;
    __asm__ volatile("dsb sy" ::: "memory");
    printf("PICASSO_WLAN glink: tx cmd=%u p1=%u p2=%u len=%u head=%u\n", command, param1, param2,
           payloadBytes, head);
    return true;
}

// Bounded by the item size the remote published, never by a guess.
void dumpPacket(uint32_t offset, uint32_t available)
{
    if (available < 8) return;
    uint8_t header[8];
    for (unsigned index = 0; index < 8; ++index) header[index] = gRxFifo[(offset + index) % gRxLength];
    const uint16_t command = readLe16(header);
    const uint16_t param1 = static_cast<uint16_t>(header[2] | (header[3] << 8));
    const uint32_t param2 = readLe32(header + 4);
    printf("PICASSO_WLAN glink: rx cmd=%u p1=%u p2=%u avail=%u\n", command, param1, param2, available);
}

}  // namespace

// Maps SMEM, resolves the GLINK items and reports what the remote has published so far.
// Returns true when the descriptor exists, i.e. when the modem endpoint has come up.
extern "C" bool native_wlan_glink_attach(void)
{
    if (gAttached) return true;
    IODeviceMemory *memory = IODeviceMemory::withRange(kSmemBase, kSmemSize);
    IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    if (!map) {
        printf("PICASSO_WLAN glink: smem map failed\n");
        return false;
    }
    gSmem = reinterpret_cast<volatile uint8_t *>(static_cast<uintptr_t>(map->getAddress()));
    gSmemBytes = kSmemSize;

    struct cw_reader reader = {const_cast<uint8_t *>(gSmem), kSmemSize, smemReadCallback};
    struct cw_smem smem = {};
    if (cw_smem_layout(&reader, &smem) != CW_OK || !smem.has_modem) {
        printf("PICASSO_WLAN glink: smem layout unavailable\n");
        return false;
    }
    struct cw_item descriptor = {}, txItem = {}, rxItem = {};
    const int descRc = cw_smem_find(&reader, &smem.modem, CW_GLINK_DESC, &descriptor);
    const int txRc = cw_smem_find(&reader, &smem.modem, CW_GLINK_TX, &txItem);
    const int rxRc = cw_smem_find(&reader, &smem.modem, CW_GLINK_RX, &rxItem);
    printf("PICASSO_WLAN glink: items desc=%d/%u tx=%d/%u rx=%d/%u\n", descRc, descriptor.bytes,
           txRc, txItem.bytes, rxRc, rxItem.bytes);
    if (descRc != CW_OK || txRc != CW_OK || rxRc != CW_OK) return false;
    if (descriptor.bytes < 32 || !txItem.bytes || !rxItem.bytes) return false;

    gTxTail = reinterpret_cast<volatile uint32_t *>(const_cast<uint8_t *>(gSmem + descriptor.offset));
    gTxHead = gTxTail + 1;
    gRxTail = gTxTail + 2;
    gRxHead = gTxTail + 3;
    gTxFifo = gSmem + txItem.offset;
    gTxLength = txItem.bytes;
    gRxFifo = gSmem + rxItem.offset;
    gRxLength = rxItem.bytes;
    printf("PICASSO_WLAN glink: desc tx.tail=%u tx.head=%u rx.tail=%u rx.head=%u txlen=%u rxlen=%u\n",
           *gTxTail, *gTxHead, *gRxTail, *gRxHead, gTxLength, gRxLength);
    gAttached = true;
    return true;
}

// VERSION handshake and one bounded RX sweep. This is the cheapest possible proof that the
// remote endpoint is running and that our FIFO writes reach it.
extern "C" void native_wlan_glink_probe(void)
{
    if (!native_wlan_glink_attach()) {
        printf("PICASSO_WLAN glink: no endpoint (modem not up)\n");
        return;
    }
    // Start from an empty ring: the consumer owns tail; we may reset head only when both agree.
    if (*gTxHead != *gTxTail) {
        printf("PICASSO_WLAN glink: stale ring head=%u tail=%u, aligning\n", *gTxHead, *gTxTail);
        *gTxHead = *gTxTail;
    }
    sendPacket(kCmdVersion, 1, 0, nullptr, 0);

    const uint32_t rxTail = *gRxTail, rxHead = *gRxHead;
    const uint32_t available = (rxHead >= rxTail) ? (rxHead - rxTail) : (gRxLength - rxTail + rxHead);
    printf("PICASSO_WLAN glink: rx pending=%u\n", available);
    uint32_t offset = rxTail;
    uint32_t remaining = available;
    while (remaining >= 8) {
        dumpPacket(offset, remaining);
        // Without command-specific lengths the walk advances one 8-byte unit at a time; the
        // point of this pass is the command ids, not payload reassembly.
        offset += 8;
        if (offset >= gRxLength) offset -= gRxLength;
        remaining -= 8;
    }
}

// Polls the modem's SMP2P state entry. Item 435 is the inbound entry the modem writes (see
// WlanSmem.h); a state that changes from 0 to nonzero after our PAS load is the first sign that
// MPSS actually started. Read-only, bounded, and printing only on change.
extern "C" void native_wlan_smp2p_poll(unsigned seconds)
{
    if (!gSmem) return;
    struct cw_reader reader = {const_cast<uint8_t *>(gSmem), kSmemSize, smemReadCallback};
    struct cw_smem smem = {};
    if (cw_smem_layout(&reader, &smem) != CW_OK || !smem.has_modem) return;
    struct cw_item item = {};
    if (cw_smem_find(&reader, &smem.modem, CW_SMP2P_IN, &item) != CW_OK) {
        printf("PICASSO_WLAN smp2p: item %u absent\n", CW_SMP2P_IN);
        return;
    }
    struct cw_smp2p state = {};
    int last = -1;
    for (unsigned step = 0; step < seconds; ++step) {
        if (cw_smp2p_read(&reader, &item, &state) == CW_OK) {
            const int current = static_cast<int>(state.state);
            if (current != last) {
                printf("PICASSO_WLAN smp2p: t=%us state=%u valid=%u version=%u\n", step, state.state,
                       state.valid, state.version);
                last = current;
            }
            if (current) return;
        }
        IOSleep(1000);
    }
}
