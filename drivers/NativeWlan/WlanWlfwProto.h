/*
 * Ported from Trogdor-ipadOS (chiba233) platform/coachz-prelink/pending/wifi/,
 * BSD-2-Clause, wire facts from Linux smem.c/smp2p.c/ath10k qmi_wlfw_v01.h.
 * picasso/SM7250 adaptation status: the CW_* address constants below are the
 * SC7180 reference values; WlanBoard.h carries the authoritative SM7250
 * numbers (SMEM/WLAN/MSA/SID) and must be reconciled before any live use.
 */
/* SPDX-License-Identifier: BSD-2-Clause */
/* Coachz WCN3990 control-plane wire helpers. No transport or hardware access.
 * Protocol references and fixed revisions are listed in SOURCES.md.
 */
#ifndef NATIVE_WLAN_WLFW_PROTO_H
#define NATIVE_WLAN_WLFW_PROTO_H
#include <stdint.h>
#include <stddef.h>

enum { CW_OK, CW_ABSENT, CW_FORMAT, CW_BOUNDS, CW_IO, CW_REMOTE, CW_UNSUPPORTED };
#define CW_SMEM_BASE 0x80900000ull
#define CW_SMEM_SIZE 0x00200000u
#define CW_WLAN_BASE 0x18800000ull
#define CW_WLAN_SIZE 0x00800000u
#define CW_MSA_BASE 0x94100000ull
#define CW_MSA_SIZE 0x00200000u
#define CW_HOST_SID 0x00c0u
#define CW_FW_SID 0x00c2u
#define CW_WLFW_SERVICE 0x45u
#define CW_WLFW_VERSION 1u
#define CW_WLFW_IND_REGISTER 0x0020u
#define CW_WLFW_FW_READY 0x0021u
#define CW_WLFW_CAP 0x0024u
#define CW_WLFW_MSA_INFO 0x002du
#define CW_WLFW_MSA_READY 0x002eu
#define CW_WLFW_CLIENT_ID 0x4b4e454cu
#define CW_QMI_HEADER 7u
#define CW_QRTR_HEADER 32u
enum { CW_QMI_REQUEST = 0, CW_QMI_RESPONSE = 2, CW_QMI_INDICATION = 4 };

static inline uint16_t cw_u16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static inline uint32_t cw_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint64_t cw_u64(const uint8_t *p) { return cw_u32(p) | (uint64_t)cw_u32(p + 4) << 32; }
static inline void cw_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void cw_put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static inline void cw_put64(uint8_t *p, uint64_t v) {
    for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static inline int cw_span(uint32_t limit, uint32_t offset, uint32_t bytes) {
    return offset <= limit && bytes <= limit - offset;
}
static inline int cw_disjoint(uint64_t a, uint64_t a_size, uint64_t b, uint64_t b_size) {
    if (!a_size || !b_size || a_size > UINT64_MAX - a || b_size > UINT64_MAX - b) return 0;
    return a + a_size <= b || b + b_size <= a;
}

struct cw_qmi {
    uint8_t type;
    uint16_t transaction, message, bytes;
    const uint8_t *tlvs;
};

/* A QRTR payload is one complete QMI message. Truncation is never success. */
static inline int cw_qmi_parse(const uint8_t *p, size_t n, struct cw_qmi *m) {
    if (!p || !m || n < CW_QMI_HEADER) return CW_FORMAT;
    if (p[0] != CW_QMI_REQUEST && p[0] != CW_QMI_RESPONSE && p[0] != CW_QMI_INDICATION) return CW_FORMAT;
    const uint16_t bytes = cw_u16(p + 5);
    if (n - CW_QMI_HEADER != bytes) return CW_FORMAT;
    for (uint32_t off = 0; off < bytes;) {
        if (!cw_span(bytes, off, 3)) return CW_FORMAT;
        const uint32_t size = cw_u16(p + CW_QMI_HEADER + off + 1);
        if (!cw_span(bytes, off + 3, size)) return CW_FORMAT;
        off += 3 + size;
    }
    m->type = p[0]; m->transaction = cw_u16(p + 1); m->message = cw_u16(p + 3);
    m->bytes = bytes; m->tlvs = p + CW_QMI_HEADER;
    return CW_OK;
}

static inline int cw_qmi_tlv(const struct cw_qmi *m, uint8_t tag, const uint8_t **data, uint16_t *size) {
    int found = 0;
    if (!m || !data || !size || (!m->tlvs && m->bytes)) return CW_FORMAT;
    for (uint32_t off = 0; off < m->bytes;) {
        if (!cw_span(m->bytes, off, 3)) return CW_FORMAT;
        const uint16_t n = cw_u16(m->tlvs + off + 1);
        if (!cw_span(m->bytes, off + 3, n)) return CW_FORMAT;
        if (m->tlvs[off] == tag) {
            if (found) return CW_FORMAT;
            *data = m->tlvs + off + 3; *size = n; found = 1;
        }
        off += 3 + n;
    }
    return found ? CW_OK : CW_ABSENT;
}

/* Match both transaction and message; the result TLV is mandatory. */
static inline int cw_qmi_response(const struct cw_qmi *m, uint16_t transaction, uint16_t message, uint16_t *remote_error) {
    const uint8_t *p = NULL; uint16_t size = 0;
    if (!m || !remote_error || m->type != CW_QMI_RESPONSE || m->transaction != transaction || m->message != message)
        return CW_FORMAT;
    *remote_error = 0;
    if (cw_qmi_tlv(m, 2, &p, &size) != CW_OK || size != 4) return CW_FORMAT;
    const uint16_t result = cw_u16(p);
    *remote_error = cw_u16(p + 2);
    if (result > 1 || (!result && *remote_error)) return CW_FORMAT;
    return result ? CW_REMOTE : CW_OK;
}

struct cw_builder { uint8_t *data; size_t capacity, used; int error; };
static inline void cw_qmi_begin(struct cw_builder *b, uint8_t *data, size_t capacity, uint16_t transaction, uint16_t message) {
    b->data = data; b->capacity = capacity; b->used = 0; b->error = CW_BOUNDS;
    if (!data || capacity < CW_QMI_HEADER) return;
    data[0] = CW_QMI_REQUEST; cw_put16(data + 1, transaction); cw_put16(data + 3, message); cw_put16(data + 5, 0);
    b->used = CW_QMI_HEADER; b->error = CW_OK;
}
static inline void cw_qmi_add(struct cw_builder *b, uint8_t tag, const uint8_t *p, uint16_t size) {
    if (b->error) return;
    if ((size && !p) || b->used > b->capacity || (size_t)size + 3 > b->capacity - b->used ||
        b->used - CW_QMI_HEADER + 3 + size > UINT16_MAX) { b->error = CW_BOUNDS; return; }
    b->data[b->used] = tag; cw_put16(b->data + b->used + 1, size);
    for (unsigned i = 0; i < size; i++) b->data[b->used + 3 + i] = p[i];
    b->used += 3 + size;
}
static inline size_t cw_qmi_finish(struct cw_builder *b) {
    if (b->error) return 0;
    cw_put16(b->data + 5, (uint16_t)(b->used - CW_QMI_HEADER));
    return b->used;
}

static inline size_t cw_wlfw_register(uint8_t *p, size_t room, uint16_t transaction) {
    struct cw_builder b; const uint8_t yes = 1; uint8_t client[4];
    cw_qmi_begin(&b, p, room, transaction, CW_WLFW_IND_REGISTER);
    cw_qmi_add(&b, 0x10, &yes, 1); /* fw_ready_enable */
    cw_qmi_add(&b, 0x13, &yes, 1); /* msa_ready_enable */
    cw_qmi_add(&b, 0x14, &yes, 1); /* pin_connect_result_enable */
    cw_put32(client, CW_WLFW_CLIENT_ID); cw_qmi_add(&b, 0x15, client, 4);
    return cw_qmi_finish(&b);
}
static inline size_t cw_wlfw_cap_request(uint8_t *p, size_t room, uint16_t transaction) {
    struct cw_builder b; cw_qmi_begin(&b, p, room, transaction, CW_WLFW_CAP); return cw_qmi_finish(&b);
}
/* Caller must first own/protect the real target MSA. Building is not permission to send. */
static inline size_t cw_wlfw_msa_request(uint8_t *p, size_t room, uint16_t transaction, uint64_t base, uint32_t size) {
    struct cw_builder b; uint8_t address[8], bytes[4];
    if (base != CW_MSA_BASE || size != CW_MSA_SIZE) return 0;
    cw_qmi_begin(&b, p, room, transaction, CW_WLFW_MSA_INFO);
    cw_put64(address, base); cw_put32(bytes, size);
    cw_qmi_add(&b, 1, address, sizeof(address)); cw_qmi_add(&b, 2, bytes, sizeof(bytes));
    return cw_qmi_finish(&b);
}

struct cw_capability { uint32_t present, chip, family, board, soc, firmware; };
/* Optional fields remain absent, not invented IDs. Bit 0=chip/family, 1=board, 2=soc, 3=firmware. */
static inline int cw_wlfw_capability(const struct cw_qmi *m, struct cw_capability *c) {
    if (!m || !c || m->type != CW_QMI_RESPONSE || m->message != CW_WLFW_CAP) return CW_FORMAT;
    c->present = c->chip = c->family = c->board = c->soc = c->firmware = 0;
    uint16_t remote_error = 0;
    const int response = cw_qmi_response(m, m->transaction, CW_WLFW_CAP, &remote_error);
    if (response) return response;
    for (uint8_t i = 0; i < 4; i++) {
        const uint8_t *p = NULL; uint16_t size = 0;
        const int status = cw_qmi_tlv(m, (uint8_t)(0x10 + i), &p, &size);
        if (status == CW_ABSENT) continue;
        if (status || size < (i == 0 ? 8 : 4)) return CW_FORMAT;
        c->present |= 1u << i;
        if (i == 0) { c->chip = cw_u32(p); c->family = cw_u32(p + 4); }
        else if (i == 1) c->board = cw_u32(p);
        else if (i == 2) c->soc = cw_u32(p);
        else c->firmware = cw_u32(p);
    }
    return CW_OK;
}

struct cw_service { uint32_t node, port, instance; };
/* QRTR v1 NEW_SERVER/DEL_SERVER only. No GLINK FIFO pointers are changed here. */
static inline int cw_qrtr_wlfw_service(const uint8_t *p, size_t n, struct cw_service *s, int *up) {
    if (!p || !s || !up || n < CW_QRTR_HEADER + 20 || cw_u32(p) != 1) return CW_FORMAT;
    const uint32_t type = cw_u32(p + 4), size = cw_u32(p + 20);
    if ((type != 4 && type != 5) || size != 20 || n != CW_QRTR_HEADER + size || cw_u32(p + 32) != type)
        return CW_FORMAT;
    if (cw_u32(p + 36) != CW_WLFW_SERVICE || (cw_u32(p + 40) & 0xffu) != CW_WLFW_VERSION) return CW_ABSENT;
    s->instance = cw_u32(p + 40) >> 8; s->node = cw_u32(p + 44); s->port = cw_u32(p + 48);
    *up = type == 4;
    return CW_OK;
}
#endif
