/*
 * WCN3990 host-side HIF: copy-engine rings, HTC handshake, WMI-TLV scanning.
 *
 * The modem-side work (PAS, WLFW QMI, BDF, mission mode) already happens in
 * PicassoWlan.cpp.  What was missing on the host was everything above the
 * WLFW QMI layer: programming the copy engines, speaking HTC to the WLAN
 * firmware and driving WMI-TLV.  This file is that layer.
 *
 * Every constant below comes from Linux ath10k (torvalds/linux master, read on
 * 2026-11-05); the file:macro pairs are spelled out where they are used:
 *
 *   drivers/net/wireless/ath/ath10k/hw.c
 *       wcn3990_regs (ceN_base_address), wcn3990_ce_regs, wcn3990_values
 *   drivers/net/wireless/ath/ath10k/ce.h / ce.c
 *       struct ce_desc_64, CE_DESC_ADDR_MASK, shadow_sr_wr_ind_addr(),
 *       _ath10k_ce_send_nolock_64(), __ath10k_ce_rx_post_buf_64(),
 *       ath10k_ce_init_src_ring(), ath10k_ce_init_dest_ring()
 *   drivers/net/wireless/ath/ath10k/snoc.c
 *       target_ce_config_wlan, target_service_to_ce_map_wlan,
 *       target_shadow_reg_cfg_map, WCN3990_{SRC,DST}_WR_IDX_OFFSET
 *   drivers/net/wireless/ath/ath10k/htc.h / htc.c
 *       struct ath10k_htc_hdr, enum ath10k_ath10k_htc_msg_id, service ids,
 *       ath10k_htc_connect_service(), ath10k_htc_send_setup_complete()
 *   drivers/net/wireless/ath/ath10k/wmi.h / wmi-tlv.h / wmi-tlv.c
 *       enum wmi_cmd_group, enum wmi_cmd_id, enum wmi_event_id,
 *       enum wmi_tlv_tag, struct wmi_tlv, struct wmi_tlv_mgmt_rx_ev,
 *       ath10k_wmi_tlv_op_gen_init(), ath10k_wmi_tlv_op_gen_start_scan(),
 *       ath10k_wmi_tlv_op_pull_mgmt_rx_ev()
 *
 * WCN3990 specifics that matter here: 12 copy engines, 64-bit descriptors
 * (target_64bit), CE register window 0x240000 + ce*0x1000 inside the WLAN CE
 * BAR at 0x18800000, and a shadow window at 0x32000 for the OUT rings' source
 * write index.  The WLFW WLAN_CFG request we already send installs the pipe
 * and shadow tables on the firmware side (snoc.c: ath10k_qmi_wlan_cfg_send).
 *
 * Pipe usage from target_service_to_ce_map_wlan:
 *   HTC_RSVD_CTRL (0x0001)  OUT -> CE0, IN -> CE2   (HTC control messages)
 *   WMI_CONTROL   (0x0100)  OUT -> CE3, IN -> CE2   (WMI commands/events)
 *   HTT_DATA      (0x0300)  OUT -> CE4, IN -> CE1   (not used for scanning)
 *
 * This is a polled HIF: the modem thread calls wcn3990_hif_poll() every ~1 ms.
 * No MSI, no interrupts, no DMA API: rings and buffers live in one contiguous
 * SCM-assigned block and are cache-flushed explicitly.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/debug.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>
#include <vm/pmap.h>
#include <machine/atomic.h>

extern "C" uint64_t kvtophys(vm_offset_t va);
extern "C" void flush_dcache64(addr64_t pa, unsigned int len, boolean_t phys);

#define HIF_LOG(fmt, ...)   printf("WCN3990-HIF: " fmt "\n", ##__VA_ARGS__)

static inline void
hif_mb(void)
{
	/* PicassoWlan.cpp defines its own mb(); this file must not depend on a
	 * definition in another translation unit.  Full-system DMB covers both
	 * the CE descriptor (CPU -> device) and the completion (device -> CPU). */
	__asm__ __volatile__("dmb sy" ::: "memory");
}

static inline uint16_t
hif_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t
hif_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void
hif_put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void
hif_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* ------------------------------------------------------------------ CE regs */

/* hw.c: wcn3990_regs.ceN_base_address (offsets inside the 0x18800000 BAR) */
#define CE_WINDOW(ce)            (0x00240000u + (uint32_t)(ce) * 0x1000u)
/* hw.c: wcn3990_ce_regs */
#define CE_SR_BASE_LO            0x00
#define CE_SR_BASE_HI            0x04
#define CE_SR_SIZE               0x08
#define CE_DR_BASE_LO            0x0c
#define CE_DR_BASE_HI            0x10
#define CE_DR_SIZE               0x14
#define CE_HOST_IE               0x2c
#define CE_MISC_IE               0x34
#define CE_CTRL1                 0x18   /* hw.c: wcn3990_ctrl1.addr */
#define CE_CTRL1_DMAX_MASK       0x0000ffffu
#define CE_CTRL1_SRC_BSWAP       (1u << 17)  /* wcn3990_src_ring.mask */
#define CE_CTRL1_DST_BSWAP       (1u << 18)  /* wcn3990_dst_ring.mask */
#define CE_SR_WR_INDEX           0x3c   /* WCN3990_SRC_WR_IDX_OFFSET */
#define CE_DST_WR_INDEX          0x40   /* WCN3990_DST_WR_IDX_OFFSET */
#define CE_CURRENT_SRRI          0x44
#define CE_CURRENT_DRRI          0x48
/* ce.c: shadow_sr_wr_ind_addr() */
#define CE0_SHADOW_SR_WR         0x00032000u
#define CE3_SHADOW_SR_WR         0x0003200Cu
/* ce.h: CE_DESC_ADDR_MASK = GENMASK_ULL(34, 0) */
#define CE_DESC_ADDR_MASK        0x7ffffffffull
#define CE_DESC_RING_ALIGN       8

struct wcn_ce_desc_64 {
	uint64_t addr;
	uint16_t nbytes;
	uint16_t flags;
	uint32_t toeplitz;
} __attribute__((packed));

typedef char wcn_ce_desc_size_check[(sizeof(struct wcn_ce_desc_64) == 16) ? 1 : -1];

#define CE0_ENTRIES   32     /* snoc.c: target_ce_config_wlan[0] nentries */
#define CE2_ENTRIES   64     /* snoc.c: target_ce_config_wlan[2] nentries */
#define CE3_ENTRIES   32     /* snoc.c: target_ce_config_wlan[3] nentries */
#define CE_BUF_BYTES  2048    /* nbytes_max for CE0/2/3 */

/* Linux gives the CE rings to dma_alloc_coherent(), which maps them into the
 * WLAN device's IOMMU domain.  Here there is no DMA API: static .bss pages are
 * HLOS-only, and v333 proved that the moment CE2 starts writing them the SoC
 * hangs with no panic (30 s APSS watchdog bite) - the WLAN CE master is not
 * the WLAN MAC stream 0xc0 that modem_wlansmmu puts in bypass.
 *
 * Allocate one contiguous block instead and hand its pages to VMID_WLAN_CE /
 * VMID_WLAN through SCM hyp_assign (HLOS keeps RW).  The allocator lives in
 * PicassoWlan.cpp because that is where the SCM call and VMIDs already are. */
struct hif_dma_block {
	struct wcn_ce_desc_64 src0[CE0_ENTRIES];
	struct wcn_ce_desc_64 src3[CE3_ENTRIES];
	struct wcn_ce_desc_64 dst2[CE2_ENTRIES];
	uint8_t rx2[CE2_ENTRIES][CE_BUF_BYTES];
	uint8_t tx0[CE_BUF_BYTES];
	uint8_t tx3[CE_BUF_BYTES];
};

#define HIF_DMA_SIZE   0x40000u          /* 256 KiB, >sizeof(hif_dma_block) */
static struct hif_dma_block *g_dma;
#define s_src0  (g_dma->src0)
#define s_dst2  (g_dma->dst2)
#define s_src3  (g_dma->src3)
#define s_rx2   (g_dma->rx2)
#define s_tx0   (g_dma->tx0)
#define s_tx3   (g_dma->tx3)

extern "C" void *picasso_wlan_ce_dma_alloc(uint64_t size, uint64_t *pa_out,
    uint32_t assign_level);

static volatile uint8_t *g_ce_va;
static bool g_regs_live;
/* Bring-up stages: 1 = descriptors only (no CE register writes),
 * 2 = program CE base/size but leave DST_WR_INDEX at 0 (no DMA),
 * 3 = post the receive buffers and start the CE.  Default is auto: the poll
 * thread advances one stage every ~3 s, with a heartbeat before each step, so
 * a single boot shows exactly which step hangs the SoC.  wcn3990-hif=0..3
 * pins a stage (0 disables the HIF completely). */
static int g_stage = 1;
static int g_hif_mode = 0;
static bool g_bases_programmed;
/* Number of receive descriptors to hand to CE2 in stage 3. */
static uint32_t g_post_count = CE2_ENTRIES - 1;
static uint32_t g_wr0, g_wr3;         /* source ring write index */
static uint32_t g_sw2, g_post2;       /* dest ring software / posted index */
static uint64_t g_rx2_pa, g_dst2_pa, g_src0_pa, g_src3_pa;

static inline volatile uint32_t *
hif_reg(uint32_t ce, uint32_t off)
{
	return (volatile uint32_t *)(uintptr_t)(g_ce_va + CE_WINDOW(ce) + off);
}

static inline uint32_t
hif_reg_read(uint32_t ce, uint32_t off)
{
	return *hif_reg(ce, off);
}

static inline void
hif_reg_write(uint32_t ce, uint32_t off, uint32_t value)
{
	*hif_reg(ce, off) = value;
	hif_mb();
}

static inline uint32_t
hif_shadow_read(uint32_t offset)
{
	return *(volatile uint32_t *)(uintptr_t)(g_ce_va + offset);
}

/* ce.c: _ath10k_ce_send_nolock_64() + __ath10k_ce_rx_post_buf_64() */
static int
hif_ce_send(uint32_t ce, uint8_t *buf, uint32_t len)
{
	struct wcn_ce_desc_64 *ring;
	uint32_t entries, *wr;
	uint8_t *shadow = NULL;
	uint32_t idx;
	uint64_t pa;

	if (!g_regs_live || g_stage < 3 || len == 0 || len > CE_BUF_BYTES) return -1;

	switch (ce) {
	case 0:  ring = s_src0; entries = CE0_ENTRIES; wr = &g_wr0; break;
	case 3:  ring = s_src3; entries = CE3_ENTRIES; wr = &g_wr3; break;
	default: return -1;
	}

	idx = *wr;
	pa = kvtophys((vm_offset_t)buf);
	if (!pa) {
		HIF_LOG("ce%u: kvtophys(%p) failed", ce, buf);
		return -1;
	}

	ring[idx].addr = pa & CE_DESC_ADDR_MASK;
	ring[idx].nbytes = (uint16_t)len;
	ring[idx].flags = 0;
	ring[idx].toeplitz = 0;
	hif_mb();
	flush_dcache64((addr64_t)kvtophys((vm_offset_t)buf), len, TRUE);
	flush_dcache64((addr64_t)kvtophys((vm_offset_t)&ring[idx]), 64, TRUE);

	*wr = (idx + 1) & (entries - 1);

	/* Real CE register first (ath10k_ce_src_ring_write_index_set).  WCN3990
	 * also exposes a TrustZone-safe shadow copy; write that too so whichever
	 * one the firmware polls sees the same index.  snoc.c:0x3C/0x40 table. */
	hif_reg_write(ce, CE_SR_WR_INDEX, *wr);
	if (ce == 0) shadow = (uint8_t *)(uintptr_t)CE0_SHADOW_SR_WR;
	else if (ce == 3) shadow = (uint8_t *)(uintptr_t)CE3_SHADOW_SR_WR;
	if (shadow) {
		*(volatile uint32_t *)(uintptr_t)(g_ce_va + (uintptr_t)shadow) = *wr;
		hif_mb();
	}

	HIF_LOG("ce%u send: desc %u pa %llx len %u -> wr %u (reg %x shadow %x)",
	    ce, idx, (unsigned long long)pa, len, *wr,
	    hif_reg_read(ce, CE_SR_WR_INDEX),
	    shadow ? hif_shadow_read((uint32_t)(uintptr_t)shadow) : 0);
	return 0;
}

/* Post one more receive buffer on CE2 and advance the dest write index.
 * ce.c: ath10k_ce_rx_update_write_idx() keeps one slot free; we start with
 * CE2_ENTRIES-1 posted buffers for the same reason. */
static void
hif_ce2_repost(uint32_t idx)
{
	struct wcn_ce_desc_64 *d = &s_dst2[idx];

	d->addr = (g_rx2_pa + (uint64_t)idx * CE_BUF_BYTES) & CE_DESC_ADDR_MASK;
	d->nbytes = 0;
	d->flags = 0;
	d->toeplitz = 0;
	hif_mb();
	flush_dcache64((addr64_t)kvtophys((vm_offset_t)d), 64, TRUE);

	g_post2 = (g_post2 + 1) & (CE2_ENTRIES - 1);
	hif_reg_write(2, CE_DST_WR_INDEX, g_post2);
}

static void
hif_dump_regs(const char *why)
{
	HIF_LOG("regs (%s): stage=%d CE0 sr_base=%x sr_size=%x srri=%x wr=%x | "
	    "CE2 dr_base=%x dr_size=%x drri=%x wr=%x desc0.nbytes=%u | "
	    "CE3 sr_base=%x sr_size=%x srri=%x wr=%x",
	    why, g_stage,
	    hif_reg_read(0, CE_SR_BASE_LO), hif_reg_read(0, CE_SR_SIZE),
	    hif_reg_read(0, CE_CURRENT_SRRI), hif_reg_read(0, CE_SR_WR_INDEX),
	    hif_reg_read(2, CE_DR_BASE_LO), hif_reg_read(2, CE_DR_SIZE),
	    hif_reg_read(2, CE_CURRENT_DRRI), hif_reg_read(2, CE_DST_WR_INDEX),
	    (unsigned)s_dst2[0].nbytes,
	    hif_reg_read(3, CE_SR_BASE_LO), hif_reg_read(3, CE_SR_SIZE),
	    hif_reg_read(3, CE_CURRENT_SRRI), hif_reg_read(3, CE_SR_WR_INDEX));
}

/* ------------------------------------------------------------------- HTC/WMI */

/* htc.h: enum ath10k_ath10k_htc_msg_id */
#define HTC_MSG_READY                 1
#define HTC_MSG_CONNECT_SERVICE       2
#define HTC_MSG_CONNECT_SERVICE_RESP  3
#define HTC_MSG_SETUP_COMPLETE        4
#define HTC_MSG_SETUP_COMPLETE_EX     5
/* htc.h: SVC(group, idx) = group << 8 | idx */
#define HTC_SVC_RSVD_CTRL             0x0001
#define HTC_SVC_WMI_CONTROL           0x0100
#define HTC_EP0                       0
#define HTC_HDR_BYTES                 8

/* wmi.h: WMI_CMD_GRP(grp) = (grp << 12) | 1, WMI_GRP_START = 3 */
#define WMI_INIT_CMDID                0x0001
#define WMI_SERVICE_READY_EVENTID     0x0001
#define WMI_READY_EVENTID             0x0002
#define WMI_START_SCAN_CMDID          0x3001   /* WMI_GRP_SCAN  = 3 */
#define WMI_SCAN_EVENTID              0x3001
#define WMI_MGMT_RX_EVENTID           0x7001   /* WMI_GRP_MGMT  = 7 */

/* wmi-tlv.h: enum wmi_tlv_tag (values computed from the enum order) */
#define WMI_TLV_ARRAY_UINT32          16
#define WMI_TLV_ARRAY_BYTE            17
#define WMI_TLV_ARRAY_STRUCT          18
#define WMI_TLV_ARRAY_FIXED_STRUCT    19
#define WMI_TLV_MGMT_RX_HDR           44
#define WMI_TLV_INIT_CMD              74
#define WMI_TLV_RESOURCE_CONFIG       75
#define WMI_TLV_START_SCAN_CMD        77
/* wmi-tlv.c: WMI_TLV_ABI_VER0 / VER1 / VER_NS0 */
#define WMI_TLV_ABI_VER0              0x01000000u
#define WMI_TLV_ABI_VER1              53u
#define WMI_TLV_ABI_VER_NS0           0x5f414351u
/* wmi.h: WMI_SCAN_FILTER_PROBE_REQ */
#define WMI_SCAN_FILTER_PROBE_REQ     0x20

enum hif_state {
	HIF_OFF = 0,
	HIF_CE_UP,          /* rings programmed, waiting for HTC READY */
	HIF_CONNECT_SENT,   /* CONNECT_SERVICE sent, waiting for its response */
	HIF_SETUP_SENT,     /* SETUP_COMPLETE sent, waiting for WMI service ready */
	HIF_INIT_SENT,      /* WMI_INIT sent, waiting for WMI_READY */
	HIF_READY
};

static enum hif_state g_state = HIF_OFF;
static uint8_t g_wmi_eid = 1;
static uint16_t g_credit_count, g_credit_size;
static uint32_t g_polls;
static uint32_t g_rx_packets;
static bool g_scan_active;
static uint32_t g_scan_polls;
static uint32_t g_beacons;
static void (*g_frame_sink)(const uint8_t *, size_t, int16_t, uint16_t);

extern "C" int wcn3990_hif_state(void) { return (int)g_state; }
extern "C" void wcn3990_hif_set_frame_sink(void (*sink)(const uint8_t *, size_t, int16_t, uint16_t))
{
	g_frame_sink = sink;
}

/* Build <htc_hdr><u16 message id><payload> and send it on EP0 (CE0). */
static int
hif_htc_control(uint16_t msg_id, const uint8_t *payload, uint32_t len)
{
	uint8_t *b = s_tx0;
	uint32_t total = HTC_HDR_BYTES + 2 + len;

	if (total > CE_BUF_BYTES) return -1;
	memset(b, 0, total);
	b[0] = HTC_EP0;              /* eid */
	b[1] = 0;                    /* flags: no credit update, no bundle */
	hif_put16(b + 2, (uint16_t)(2 + len));   /* payload length */
	/* b[4..7] = trailer_len/control bytes/pad = 0 */
	hif_put16(b + 8, msg_id);
	if (len && payload) memcpy(b + 10, payload, len);

	return hif_ce_send(0, b, total);
}

/* Build <htc_hdr><u32 wmi cmd id><TLVs> and send on the WMI endpoint (CE3). */
static int
hif_wmi_command(uint32_t cmd_id, const uint8_t *tlvs, uint32_t len)
{
	uint8_t *b = s_tx3;
	uint32_t total = HTC_HDR_BYTES + 4 + len;

	if (total > CE_BUF_BYTES) return -1;
	memset(b, 0, total);
	b[0] = g_wmi_eid;
	b[1] = 0;
	hif_put16(b + 2, (uint16_t)(4 + len));
	hif_put32(b + 8, cmd_id);
	if (len && tlvs) memcpy(b + 12, tlvs, len);

	return hif_ce_send(3, b, total);
}

/* wmi-tlv.h: struct wmi_tlv { __le16 len; __le16 tag; u8 value[]; }.  All
 * TLVs we emit have 4-byte-aligned lengths, matching ath10k's pointer walk. */
static uint8_t *
hif_put_tlv(uint8_t *p, uint16_t tag, uint16_t len, const void *value)
{
	hif_put16(p, len);
	hif_put16(p + 2, tag);
	if (len && value) memcpy(p + 4, value, len);
	return p + 4 + ((len + 3) & ~3u);
}

static int
hif_send_connect_service(void)
{
	struct {
		uint16_t service_id;
		uint16_t flags;
		uint8_t pad[2];
	} __attribute__((packed)) req;

	req.service_id = HTC_SVC_WMI_CONTROL;
	/* htc.c: only WMI_CONTROL keeps credit flow control enabled; the others
	 * set DISABLE_CREDIT_FLOW_CTRL.  Zero flags is exactly that case. */
	req.flags = 0;
	req.pad[0] = req.pad[1] = 0;

	HIF_LOG("HTC: send CONNECT_SERVICE svc=0x%04x", HTC_SVC_WMI_CONTROL);
	return hif_htc_control(HTC_MSG_CONNECT_SERVICE, (const uint8_t *)&req, sizeof(req));
}

static int
hif_send_setup_complete(void)
{
	/* htc.c: ath10k_htc_send_setup_complete() uses SETUP_COMPLETE_EX with an
	 * all-zero extended payload on non-SDIO buses. */
	struct {
		uint8_t pad0, pad1;
		uint32_t flags;
		uint8_t max_msgs_per_bundled_recv, pad2, pad3, pad4;
	} __attribute__((packed)) ext;

	memset(&ext, 0, sizeof(ext));
	HIF_LOG("HTC: send SETUP_COMPLETE_EX");
	return hif_htc_control(HTC_MSG_SETUP_COMPLETE_EX, (const uint8_t *)&ext, sizeof(ext));
}

/* wmi-tlv.c: ath10k_wmi_tlv_op_gen_init() */
static int
hif_send_wmi_init(void)
{
	uint8_t tlv[512];
	uint8_t *p = tlv;
	struct {
		uint32_t abi[6];
		uint32_t num_host_mem_chunks;
	} __attribute__((packed)) init;
	uint32_t cfg[44];
	uint32_t i;

	memset(&init, 0, sizeof(init));
	init.abi[0] = WMI_TLV_ABI_VER0;
	init.abi[1] = WMI_TLV_ABI_VER1;
	init.abi[2] = WMI_TLV_ABI_VER_NS0;
	init.num_host_mem_chunks = 0;

	/* wmi-tlv.h: struct wmi_tlv_resource_config (41 u32).  A deliberately
	 * small but valid configuration: two vdevs, a handful of peers, 2x2
	 * chains, native 802.11 decap, no offload. */
	for (i = 0; i < 44; i++) cfg[i] = 0;
	cfg[0] = 2;      /* num_vdevs */
	cfg[1] = 16;     /* num_peers */
	cfg[4] = 2;      /* num_peer_keys */
	cfg[5] = 16;     /* num_tids */
	cfg[7] = 3;      /* tx_chain_mask */
	cfg[8] = 3;      /* rx_chain_mask */
	cfg[13] = 0;     /* rx_decap_mode = native wifi */
	cfg[14] = 4;     /* scan_max_pending_reqs */
	cfg[22] = 0;     /* num_wds_entries */
	cfg[28] = 128;   /* num_msdu_desc */
	cfg[34] = 0;     /* num_wow_filters */
	cfg[39] = 0;     /* wmi_send_separate */

	p = hif_put_tlv(p, WMI_TLV_INIT_CMD, (uint16_t)sizeof(init), &init);
	p = hif_put_tlv(p, WMI_TLV_RESOURCE_CONFIG, (uint16_t)sizeof(cfg), cfg);
	p = hif_put_tlv(p, WMI_TLV_ARRAY_STRUCT, 0, NULL); /* no host mem chunks */

	HIF_LOG("WMI: send INIT (abi 0x%08x, %u TLV bytes)", WMI_TLV_ABI_VER0,
	    (unsigned)(p - tlv));
	return hif_wmi_command(WMI_INIT_CMDID, tlv, (uint32_t)(p - tlv));
}

static const uint32_t g_scan_freqs[] = {
	2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447,
	2452, 2457, 2462, 2467, 2472, 2484,
	5180, 5200, 5220, 5240, 5260, 5280, 5300, 5320,
	5500, 5520, 5540, 5560, 5580, 5600, 5620, 5640,
	5660, 5680, 5700, 5720, 5745, 5765, 5785, 5805, 5825
};

/* wmi-tlv.c: ath10k_wmi_tlv_op_gen_start_scan()
 * wmi.h: struct wmi_start_scan_common (16 u32) + 6 u32 + 2 mac addresses. */
static int
hif_send_wmi_start_scan(uint32_t dwell_ms)
{
	uint8_t tlv[768];
	uint8_t *p = tlv;
	struct {
		/* wmi.h: struct wmi_start_scan_common is exactly 15 __le32 fields
		 * (60 bytes).  An extra word here shifted burst_duration_ms and
		 * every following field by 4 bytes. */
		uint32_t common[15];
		uint32_t burst_duration_ms;
		uint32_t num_channels;
		uint32_t num_bssids;
		uint32_t num_ssids;
		uint32_t ie_len;
		uint32_t num_probes;
		uint8_t mac_addr[6];
		uint8_t mac_mask[6];
	} __attribute__((packed)) cmd;
	uint32_t i, nch = (uint32_t)(sizeof(g_scan_freqs) / sizeof(g_scan_freqs[0]));
	uint8_t chans[256];

	if (nch * 4 > sizeof(chans)) nch = sizeof(chans) / 4;
	if (dwell_ms == 0 || dwell_ms > 300) dwell_ms = 40;

	memset(&cmd, 0, sizeof(cmd));
	cmd.common[0] = 0x1000;                 /* scan_id */
	cmd.common[1] = 1;                       /* scan_req_id */
	cmd.common[2] = 0;                       /* vdev_id */
	cmd.common[3] = 0;                       /* scan_priority */
	cmd.common[4] = 0x3f;                    /* notify_scan_events: all */
	cmd.common[5] = dwell_ms;                /* dwell_time_active */
	cmd.common[6] = dwell_ms + 100;          /* dwell_time_passive */
	cmd.common[7] = 50;                      /* min_rest_time */
	cmd.common[8] = 200;                     /* max_rest_time */
	cmd.common[9] = 0;                       /* repeat_probe_time */
	cmd.common[10] = 0;                      /* probe_spacing_time */
	cmd.common[11] = 0;                      /* idle_time */
	cmd.common[12] = 3000;                   /* max_scan_time */
	cmd.common[13] = 0;                      /* probe_delay */
	/* ath10k's TLV path XORs WMI_SCAN_FILTER_PROBE_REQ into the flags.  We
	 * emit the post-XOR value directly: report beacons/probe responses only. */
	cmd.common[14] = WMI_SCAN_FILTER_PROBE_REQ;
	cmd.burst_duration_ms = dwell_ms;
	cmd.num_channels = nch;
	cmd.num_bssids = 0;
	cmd.num_ssids = 0;
	cmd.ie_len = 0;
	cmd.num_probes = 3;

	for (i = 0; i < nch; i++) hif_put32(chans + i * 4, g_scan_freqs[i]);

	p = hif_put_tlv(p, WMI_TLV_START_SCAN_CMD, (uint16_t)sizeof(cmd), &cmd);
	p = hif_put_tlv(p, WMI_TLV_ARRAY_UINT32, (uint16_t)(nch * 4), chans);
	p = hif_put_tlv(p, WMI_TLV_ARRAY_FIXED_STRUCT, 0, NULL);  /* ssids */
	p = hif_put_tlv(p, WMI_TLV_ARRAY_FIXED_STRUCT, 0, NULL);  /* bssids */
	p = hif_put_tlv(p, WMI_TLV_ARRAY_BYTE, 0, NULL);          /* ie */

	HIF_LOG("WMI: send START_SCAN (scan_id 0x1000, %u channels, %u ms dwell, %u TLV bytes)",
	    nch, dwell_ms, (unsigned)(p - tlv));
	return hif_wmi_command(WMI_START_SCAN_CMDID, tlv, (uint32_t)(p - tlv));
}

extern "C" int
wcn3990_hif_start_scan(uint32_t dwell_ms)
{
	if (g_state != HIF_READY) {
		HIF_LOG("scan refused: HIF state %d (need %d)", (int)g_state, (int)HIF_READY);
		return -1;
	}
	g_beacons = 0;
	g_scan_polls = 0;
	g_scan_active = true;
	return hif_send_wmi_start_scan(dwell_ms);
}

/* --------------------------------------------------------------- RX handling */

static void
hif_handle_mgmt_rx(const uint8_t *tlvs, uint32_t len)
{
	uint32_t off = 0;
	uint32_t channel = 0, buf_len = 0;
	int32_t rssi = -60;
	const uint8_t *frame = NULL;
	uint32_t frame_len = 0;

	/* wmi-tlv.c: ath10k_wmi_tlv_op_pull_mgmt_rx_ev() looks for
	 * WMI_TLV_TAG_STRUCT_MGMT_RX_HDR (44) and WMI_TLV_TAG_ARRAY_BYTE (17). */
	while (off + 4 <= len) {
		uint32_t tlv_len = hif_le16(tlvs + off);
		uint16_t tag = hif_le16(tlvs + off + 2);
		const uint8_t *value = tlvs + off + 4;
		if (off + 4 + tlv_len > len) break;
		if (tag == WMI_TLV_MGMT_RX_HDR && tlv_len >= 40) {
			channel = hif_le32(value);          /* channel (MHz) */
			buf_len = hif_le32(value + 16);     /* buf_len */
			rssi = (int32_t)hif_le32(value + 24); /* rssi[0] */
		} else if (tag == WMI_TLV_ARRAY_BYTE) {
			frame = value;
			frame_len = tlv_len;
		}
		off += 4 + ((tlv_len + 3) & ~3u);
	}

	if (!frame || !buf_len || buf_len > frame_len) return;
	if (rssi > 0 || rssi < -120) rssi = -60;
	if (g_frame_sink) {
		uint16_t freq = (channel > 1000 && channel < 6000) ? (uint16_t)channel : 2412;
		g_frame_sink(frame, (size_t)buf_len, (int16_t)rssi, freq);
		g_beacons++;
	}
}

static void
hif_handle_wmi(const uint8_t *payload, uint32_t len)
{
	uint32_t event_id;

	if (len < 4) return;
	event_id = hif_le32(payload);
	switch (event_id) {
	case WMI_SERVICE_READY_EVENTID:
		HIF_LOG("WMI: SERVICE_READY (0x%x, %u bytes)", event_id, len);
		if (g_state == HIF_SETUP_SENT) {
			if (hif_send_wmi_init() == 0) g_state = HIF_INIT_SENT;
		}
		break;
	case WMI_READY_EVENTID:
		HIF_LOG("WMI: READY (0x%x) - WMI is up, scanning is available", event_id);
		g_state = HIF_READY;
		break;
	case WMI_MGMT_RX_EVENTID:
		hif_handle_mgmt_rx(payload + 4, len - 4);
		break;
	case WMI_SCAN_EVENTID:
		if (len >= 16) {
			HIF_LOG("WMI: SCAN event type=%u reason=%u freq=%u beacons=%u",
			    hif_le32(payload + 4), hif_le32(payload + 8),
			    hif_le32(payload + 12), g_beacons);
		}
		g_scan_active = false;
		break;
	default:
		HIF_LOG("WMI: event 0x%x (%u bytes)", event_id, len);
		break;
	}
}

static void
hif_handle_htc_control(const uint8_t *payload, uint32_t len)
{
	uint16_t msg_id;

	if (len < 2) return;
	msg_id = hif_le16(payload);
	switch (msg_id) {
	case HTC_MSG_READY:
		if (len >= 6) {
			g_credit_count = hif_le16(payload + 2);
			g_credit_size = hif_le16(payload + 4);
		}
		HIF_LOG("HTC: READY credits=%u size=%u max_ep=%u",
		    g_credit_count, g_credit_size, len >= 7 ? payload[6] : 0);
		if (hif_send_connect_service() == 0) g_state = HIF_CONNECT_SENT;
		break;
	case HTC_MSG_CONNECT_SERVICE_RESP:
		if (len < 8) break;
		HIF_LOG("HTC: CONNECT_SERVICE_RESP svc=0x%04x status=%u eid=%u max_msg=%u",
		    hif_le16(payload + 2), payload[4], payload[5], hif_le16(payload + 6));
		if (payload[4] != 0) break;      /* ATH10K_HTC_CONN_SVC_STATUS_SUCCESS */
		g_wmi_eid = payload[5];
		if (hif_send_setup_complete() == 0) g_state = HIF_SETUP_SENT;
		break;
	default:
		HIF_LOG("HTC: control message %u (%u bytes)", msg_id, len);
		break;
	}
}

/* One CE2 buffer.  ath10k_htc_process_rx(): <htc_hdr><payload><trailer>,
 * hdr.len counts payload+trailer, byte0 is the trailer length on RX. */
static void
hif_handle_rx(const uint8_t *buf, uint32_t nbytes)
{
	uint8_t eid, flags, trailer_len;
	uint16_t htc_len;
	uint32_t payload_len;

	if (nbytes < HTC_HDR_BYTES) return;
	eid = buf[0];
	flags = buf[1];
	htc_len = hif_le16(buf + 2);
	trailer_len = buf[4];
	if (htc_len > nbytes - HTC_HDR_BYTES) htc_len = (uint16_t)(nbytes - HTC_HDR_BYTES);
	payload_len = htc_len;
	if ((flags & 0x02) && trailer_len <= payload_len) payload_len -= trailer_len;

	g_rx_packets++;
	if (g_rx_packets <= 8) {
		HIF_LOG("rx #%u eid=%u flags=%x htc_len=%u trailer=%u payload=%u first=%02x %02x %02x %02x",
		    g_rx_packets, eid, flags, htc_len, trailer_len, payload_len,
		    nbytes > 8 ? buf[8] : 0, nbytes > 9 ? buf[9] : 0,
		    nbytes > 10 ? buf[10] : 0, nbytes > 11 ? buf[11] : 0);
	}

	if (eid == HTC_EP0) {
		hif_handle_htc_control(buf + HTC_HDR_BYTES, payload_len);
	} else if (eid == g_wmi_eid) {
		hif_handle_wmi(buf + HTC_HDR_BYTES, payload_len);
	} else {
		HIF_LOG("rx: unhandled eid %u (%u bytes)", eid, nbytes);
	}
}

/* Defined in the init section below, but the poll ladder must see them. */
static void hif_program_bases(void);
static void hif_post_buffers(void);

extern "C" void
wcn3990_hif_poll(void)
{
	uint32_t budget = CE2_ENTRIES;

	if (!g_regs_live) return;
	g_polls++;

	/* Heartbeat every ~1000 polls, and advance the staged ladder.  This is
	 * deliberately cheap: no cache flushes and no CE register access until
	 * the corresponding stage is entered. */
	if ((g_polls % 1000) == 0) {
		HIF_LOG("heartbeat polls=%u stage=%d state=%d rx=%u beacons=%u desc0.nbytes=%u drri=%x",
		    g_polls, g_stage, (int)g_state, g_rx_packets, g_beacons,
		    (unsigned)s_dst2[0].nbytes,
		    g_bases_programmed ? hif_reg_read(2, CE_CURRENT_DRRI) : 0);
		if (g_hif_mode >= 4) {
			if (g_stage == 1 && g_polls >= 3000) {
				g_stage = 2;
				hif_program_bases();
			} else if (g_stage == 2 && g_polls >= 6000) {
				g_stage = 3;
				hif_post_buffers();
			}
		}
	}

	if (g_stage < 3) return;
	if (g_scan_active) g_scan_polls++;

	while (budget-- > 0) {
		struct wcn_ce_desc_64 *d = &s_dst2[g_sw2];
		uint32_t nbytes;
		uint32_t idx = g_sw2;

		/* Only touch the cache on a completed descriptor; doing it on
		 * every idle poll is what the earlier build did 1000x/s. */
		nbytes = d->nbytes;
		if (nbytes == 0) break;
		if (nbytes > CE_BUF_BYTES) nbytes = CE_BUF_BYTES;

		flush_dcache64((addr64_t)kvtophys((vm_offset_t)d), 64, TRUE);
		flush_dcache64((addr64_t)(g_rx2_pa + (uint64_t)idx * CE_BUF_BYTES),
		    nbytes, TRUE);
		hif_handle_rx(s_rx2[idx], nbytes);

		d->nbytes = 0;
		hif_mb();
		g_sw2 = (g_sw2 + 1) & (CE2_ENTRIES - 1);
		hif_ce2_repost(idx);
	}

	if (g_state == HIF_CE_UP && (g_polls % 2000) == 0) {
		hif_reg_write(2, CE_DST_WR_INDEX, g_post2);
	}
}

/* --------------------------------------------------------------------- init */

/* ce.c: ath10k_ce_init_src_ring() / ath10k_ce_init_dest_ring() both do a
 * read-modify-write of CE_CTRL1: DMAX = src_sz_max for the OUT rings, and the
 * src/dst byte-swap bits cleared.  Leaving the reset defaults in place lets
 * the CE reinterpret our little-endian 64-bit descriptors. */
static void
hif_ce_ctrl1(uint32_t ce, bool is_src)
{
	uint32_t old = hif_reg_read(ce, CE_CTRL1);
	uint32_t v = old;

	if (is_src) {
		v = (v & ~CE_CTRL1_DMAX_MASK) | CE_BUF_BYTES;
		v &= ~CE_CTRL1_SRC_BSWAP;
	} else {
		v &= ~CE_CTRL1_DST_BSWAP;
	}
	hif_reg_write(ce, CE_CTRL1, v);
	HIF_LOG("CE%u ctrl1 %08x -> %08x", ce, old, v);
}

/* Stage 2: program the CE ring registers but do not post any receive buffer.
 * ce.c: ath10k_ce_set_dest_ring_base_addr_hi() preserves the non-address bits
 * of dr_base_addr_hi, so use a read-modify-write with CE_DESC_ADDR_HI_MASK. */
static void
hif_program_bases(void)
{
	uint32_t hi;

	hif_reg_write(0, CE_SR_BASE_LO, (uint32_t)g_src0_pa);
	hif_reg_write(0, CE_SR_BASE_HI, (uint32_t)(g_src0_pa >> 32) & 0x1f);
	hif_reg_write(0, CE_SR_SIZE, CE0_ENTRIES);
	hif_reg_write(0, CE_HOST_IE, 0);
	hif_reg_write(0, CE_MISC_IE, 0);

	hif_reg_write(3, CE_SR_BASE_LO, (uint32_t)g_src3_pa);
	hif_reg_write(3, CE_SR_BASE_HI, (uint32_t)(g_src3_pa >> 32) & 0x1f);
	hif_reg_write(3, CE_SR_SIZE, CE3_ENTRIES);
	hif_reg_write(3, CE_HOST_IE, 0);
	hif_reg_write(3, CE_MISC_IE, 0);

	hif_reg_write(2, CE_DR_BASE_LO, (uint32_t)g_dst2_pa);
	hi = hif_reg_read(2, CE_DR_BASE_HI);
	hi = (hi & ~0x1fu) | ((uint32_t)(g_dst2_pa >> 32) & 0x1fu);
	hif_reg_write(2, CE_DR_BASE_HI, hi);
	hif_reg_write(2, CE_DR_SIZE, CE2_ENTRIES);
	hif_reg_write(2, CE_HOST_IE, 0);
	hif_reg_write(2, CE_MISC_IE, 0);

	hif_ce_ctrl1(0, true);
	hif_ce_ctrl1(3, true);
	hif_ce_ctrl1(2, false);

	g_bases_programmed = true;
	hif_dump_regs("stage 2: CE bases programmed, no buffers posted");
}

/* Stage 3: hand CE2_ENTRIES-1 buffers to the copy engine. */
static void
hif_post_buffers(void)
{
	g_post2 = g_post_count;
	hif_reg_write(2, CE_DST_WR_INDEX, g_post2);
	HIF_LOG("stage 3: posted %u receive buffers (desc.addr=%llx)", g_post2,
	    (unsigned long long)s_dst2[0].addr);
}

extern "C" void
wcn3990_hif_poll(void);

extern "C" void
wcn3990_hif_init(volatile uint8_t *ce_va, uint64_t ce_pa)
{
	uint32_t v = 0;
	uint32_t i;

	if (!ce_va || g_regs_live) return;

	/* Default 3 = program the CE and post the buffers immediately, because
	 * HTC READY arrives within milliseconds of WLFW mission mode and a late
	 * receive ring loses it forever.  wcn3990-hif=1/2/3 pins a stage and
	 * wcn3990-hif=4 re-enables the 3 s / 6 s diagnostic ladder. */
	g_hif_mode = 3;
	if (PE_parse_boot_argn("wcn3990-hif", &v, sizeof(v)) && v <= 4) {
		g_hif_mode = (int)v;
	}
	if (g_hif_mode == 0) {
		HIF_LOG("disabled by wcn3990-hif=0");
		return;
	}

	/* wcn3990-hif-post=1 narrows stage 3 to a single descriptor; useful to
	 * tell a permission fault from a ring-wrap fault. */
	v = 0;
	if (PE_parse_boot_argn("wcn3990-hif-post", &v, sizeof(v)) && v >= 1 &&
	    v < CE2_ENTRIES) {
		g_post_count = v;
	}

	/* wcn3990-hif-assign: 0 (default) = plain HLOS pages; the real gate for
	 * the WLAN CE master is the apps-SMMU stream, not a VMID assignment.
	 * 1 = VMID_WLAN_CE, 2 = VMID_WLAN_CE + VMID_WLAN, 3 = VMID_MSS_MSA.
	 * TZ rejects 24/25 on this platform (ret=-2 res=86), so those levels
	 * leave the HIF inert on purpose rather than starting a doomed DMA. */
	uint32_t assign_level = 0;
	v = 0;
	if (PE_parse_boot_argn("wcn3990-hif-assign", &v, sizeof(v)) && v <= 3) {
		assign_level = v;
	}
	{
		uint64_t dma_pa = 0;
		g_dma = (struct hif_dma_block *)picasso_wlan_ce_dma_alloc(
		    HIF_DMA_SIZE, &dma_pa, assign_level);
		if (!g_dma || dma_pa == 0) {
			HIF_LOG("CE DMA allocation failed: HIF disabled, no CE registers touched");
			return;
		}
		HIF_LOG("CE DMA block va=%p pa=%llx size=%x assign_level=%u",
		    g_dma, (unsigned long long)dma_pa, (unsigned)HIF_DMA_SIZE, assign_level);
		/* If the SCM assign failed we must NOT start DMA: that is exactly the
		 * ungranted-memory hang from v333.  Stay at stage 1 and keep polling. */
		if (assign_level == 0) {
			HIF_LOG("assignment disabled by boot-arg; stage 3 will still post buffers");
		}
	}

	g_ce_va = ce_va;
	(void)ce_pa;

	memset(s_src0, 0, sizeof(s_src0));
	memset(s_dst2, 0, sizeof(s_dst2));
	memset(s_src3, 0, sizeof(s_src3));
	memset(s_rx2, 0, sizeof(s_rx2));

	g_dst2_pa = kvtophys((vm_offset_t)s_dst2);
	g_rx2_pa = kvtophys((vm_offset_t)s_rx2);
	g_src0_pa = kvtophys((vm_offset_t)s_src0);
	g_src3_pa = kvtophys((vm_offset_t)s_src3);
	if (!g_dst2_pa || !g_rx2_pa || !g_src0_pa || !g_src3_pa) {
		HIF_LOG("cannot resolve ring physical addresses");
		return;
	}
	HIF_LOG("rings: src0=%llx dst2=%llx rx2=%llx src3=%llx mode=%d",
	    (unsigned long long)g_src0_pa, (unsigned long long)g_dst2_pa,
	    (unsigned long long)g_rx2_pa, (unsigned long long)g_src3_pa, g_hif_mode);

	for (i = 0; i < CE2_ENTRIES; i++) {
		s_dst2[i].addr = (g_rx2_pa + (uint64_t)i * CE_BUF_BYTES) & CE_DESC_ADDR_MASK;
		s_dst2[i].nbytes = 0;
		s_dst2[i].flags = 0;
		s_dst2[i].toeplitz = 0;
	}
	hif_mb();
	flush_dcache64((addr64_t)g_dst2_pa, (unsigned int)sizeof(s_dst2), TRUE);

	g_sw2 = 0;
	g_post2 = 0;
	g_wr0 = g_wr3 = 0;
	g_polls = 0;
	g_rx_packets = 0;
	g_beacons = 0;
	g_bases_programmed = false;
	g_stage = (g_hif_mode >= 1 && g_hif_mode <= 3) ? g_hif_mode : 1;
	g_state = HIF_CE_UP;
	g_regs_live = true;

	HIF_LOG("descriptors ready, mode %d stage %d", g_hif_mode, g_stage);
	if (g_hif_mode == 2) hif_program_bases();
	if (g_hif_mode == 3) {
		hif_program_bases();
		hif_post_buffers();
	}
}

