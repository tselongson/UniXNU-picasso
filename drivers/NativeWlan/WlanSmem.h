/*
 * Ported from Trogdor-ipadOS (chiba233) platform/coachz-prelink/pending/wifi/,
 * BSD-2-Clause, wire facts from Linux smem.c/smp2p.c/ath10k qmi_wlfw_v01.h.
 * picasso/SM7250 adaptation status: the CW_* address constants below are the
 * SC7180 reference values; WlanBoard.h carries the authoritative SM7250
 * numbers (SMEM/WLAN/MSA/SID) and must be reconciled before any live use.
 */
/* SPDX-License-Identifier: BSD-2-Clause */
/* Read-only SMEM v12 inventory for APPS<->MODEM and the global partition.
 * Wire layout: Linux smem.c and smp2p.c; see SOURCES.md. No allocation,
 * hwspinlock, FIFO acknowledgement or write callback is provided.
 */
#ifndef NATIVE_WLAN_SMEM_H
#define NATIVE_WLAN_SMEM_H
#include "coachz_wifi_proto.h"

#define CW_SMEM_APPS 0u
#define CW_SMEM_MODEM 1u
#define CW_SMEM_GLOBAL 0xfffeu
/* smp2p.c reads qcom,smem = <435>, <428> as inbound, outbound: the modem writes 435, the AP 428. */
#define CW_SMP2P_IN 435u
#define CW_SMP2P_OUT 428u
#define CW_GLINK_DESC 478u
#define CW_GLINK_TX 479u
#define CW_GLINK_RX 480u

struct cw_reader {
    void *context;
    uint32_t bytes;
    int (*read)(void *, uint32_t offset, void *, uint32_t bytes);
};
struct cw_partition { uint32_t offset, bytes, uncached, cached, cacheline; uint16_t host0, host1; };
struct cw_smem { struct cw_partition modem, global; uint32_t count; int has_modem, has_global; };
struct cw_item { uint32_t offset, bytes; };
struct cw_smp2p { uint32_t version, features, flags, state; uint16_t valid; int has_slave; };

static inline int cw_read(const struct cw_reader *r, uint32_t off, void *p, uint32_t n) {
    if (!r || !r->read || !p || !cw_span(r->bytes, off, n)) return CW_BOUNDS;
    return r->read(r->context, off, p, n) ? CW_IO : CW_OK;
}

/* The public TOC is read first; other processors' protected partitions are never dereferenced. */
static inline int cw_smem_layout(const struct cw_reader *r, struct cw_smem *s) {
    uint8_t header[12], entry[20], part[20];
    if (!r || !s || r->bytes < 4096) return CW_BOUNDS;
    *s = (struct cw_smem){0};
    const uint32_t toc = r->bytes - 4096;
    int status = cw_read(r, toc, header, sizeof(header));
    if (status) return status;
    if (cw_u32(header) != 0x434f5424u || cw_u32(header + 4) != 1) return CW_UNSUPPORTED;
    s->count = cw_u32(header + 8);
    if (s->count > (4096 - 32) / 48) return CW_FORMAT;
    for (uint32_t i = 0; i < s->count; i++) {
        status = cw_read(r, toc + 32 + 48 * i, entry, sizeof(entry));
        if (status) return status;
        const uint16_t host0 = cw_u16(entry + 12), host1 = cw_u16(entry + 14);
        const int global = host0 == CW_SMEM_GLOBAL && host1 == CW_SMEM_GLOBAL;
        const int modem = (host0 == CW_SMEM_APPS && host1 == CW_SMEM_MODEM) ||
            (host1 == CW_SMEM_APPS && host0 == CW_SMEM_MODEM);
        if (!global && !modem) continue;
        const uint32_t off = cw_u32(entry), bytes = cw_u32(entry + 4), cacheline = cw_u32(entry + 16);
        if (!off && !bytes) continue;
        if (!off || bytes < 32 || off % 4 || bytes % 4 || !cw_span(toc, off, bytes)) return CW_FORMAT;
        if (cacheline < 4 || cacheline > 4096 || (cacheline & (cacheline - 1))) return CW_FORMAT;
        status = cw_read(r, off, part, sizeof(part));
        if (status) return status;
        if (cw_u32(part) != 0x54525024u || cw_u16(part + 4) != host0 || cw_u16(part + 6) != host1 ||
            cw_u32(part + 8) != bytes) return CW_FORMAT;
        struct cw_partition p = {off, bytes, cw_u32(part + 12), cw_u32(part + 16), cacheline, host0, host1};
        if (p.uncached < 32 || p.uncached > p.cached || p.cached > bytes) return CW_FORMAT;
        if (global) { if (s->has_global) return CW_FORMAT; s->global = p; s->has_global = 1; }
        else { if (s->has_modem) return CW_FORMAT; s->modem = p; s->has_modem = 1; }
    }
    if (s->has_global && s->has_modem && s->global.offset < s->modem.offset + s->modem.bytes &&
        s->modem.offset < s->global.offset + s->global.bytes) return CW_FORMAT;
    return CW_OK;
}

static inline int cw_smem_find(const struct cw_reader *r, const struct cw_partition *p, uint16_t item, struct cw_item *found) {
    uint8_t h[16];
    if (!p || !found || !p->cacheline || p->uncached < 32 || p->uncached > p->cached || p->cached > p->bytes ||
        !r || !cw_span(r->bytes, p->offset, p->bytes)) return CW_FORMAT;
    *found = (struct cw_item){0};
    for (uint32_t off = 32; off < p->uncached;) {
        if (!cw_span(p->uncached, off, sizeof(h))) return CW_FORMAT;
        int status = cw_read(r, p->offset + off, h, sizeof(h));
        if (status) return status;
        const uint32_t bytes = cw_u32(h + 4), pad = cw_u16(h + 8), gap = cw_u16(h + 10);
        if (cw_u16(h) != 0xa5a5 || pad > bytes || !cw_span(p->uncached, off + 16, gap) ||
            !cw_span(p->uncached, off + 16 + gap, bytes)) return CW_FORMAT;
        if (cw_u16(h + 2) == item) {
            found->offset = p->offset + off + 16 + gap; found->bytes = bytes - pad; return CW_OK;
        }
        off += 16 + gap + bytes;
    }
    const uint32_t cl = p->cacheline;
    if (cl < 4 || cl > 4096 || (cl & (cl - 1))) return CW_FORMAT;
    const uint32_t stride = (16 + cl - 1) & ~(cl - 1);
    if (stride > p->bytes) return CW_FORMAT;
    uint32_t off = p->bytes - stride;
    while (off > p->cached) {
        if (!cw_span(p->bytes, off, stride)) return CW_FORMAT;
        int status = cw_read(r, p->offset + off, h, sizeof(h));
        if (status) return status;
        const uint32_t bytes = cw_u32(h + 4), pad = cw_u16(h + 8);
        if (cw_u16(h) != 0xa5a5 || pad > bytes || bytes > off || off - bytes < p->cached) return CW_FORMAT;
        if (cw_u16(h + 2) == item) {
            found->offset = p->offset + off - bytes; found->bytes = bytes - pad; return CW_OK;
        }
        if (bytes + stride > off) break;
        off -= bytes + stride;
    }
    return CW_ABSENT;
}

/* Incoming MPSS item only; a ready bit here is a snapshot, not proof of a live WLFW endpoint. */
static inline int cw_smp2p_read(const struct cw_reader *r, const struct cw_item *item, struct cw_smp2p *s) {
    uint8_t h[20], e[20];
    if (!item || !s || item->bytes < sizeof(h)) return CW_FORMAT;
    *s = (struct cw_smp2p){0};
    int status = cw_read(r, item->offset, h, sizeof(h));
    if (status) return status;
    if (cw_u32(h) != 0x504d5324u || h[4] < 1 || h[4] > 2 || cw_u16(h + 8) != CW_SMEM_MODEM ||
        cw_u16(h + 10) != CW_SMEM_APPS) return CW_FORMAT;
    const uint16_t total = cw_u16(h + 12), valid = cw_u16(h + 14);
    if (total > 16 || valid > total || item->bytes < 20u + 20u * valid) return CW_FORMAT;
    s->version = h[4]; s->features = cw_u32(h + 4) >> 8; s->valid = valid; s->flags = cw_u32(h + 16);
    const char slave[] = "slave-kernel";
    for (uint16_t i = 0; i < valid; i++) {
        status = cw_read(r, item->offset + 20 + 20 * i, e, sizeof(e));
        if (status) return status;
        unsigned j = 0; for (; j < sizeof(slave) && e[j] == (uint8_t)slave[j]; j++) {}
        if (j == sizeof(slave)) { s->has_slave = 1; s->state = cw_u32(e + 16); }
    }
    return CW_OK;
}
#endif
