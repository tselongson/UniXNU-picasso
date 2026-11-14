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

extern "C" void flush_dcache64(addr64_t pa, unsigned int len, boolean_t phys);  /* only the one-time clean in PicassoWlan.cpp */
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
/* Darwin's network stack receives fully formed Ethernet frames. */
extern "C" void hakios_net_input(const uint8_t *frame, size_t length);
/* hakios.dhcp.c: in-kernel DHCPv4 client (dhcp boot-arg gate). It installs
 * the address without waiting for configd's InterfaceNamer/IPConfiguration. */
extern "C" void hakios_dhcp_tick(uint32_t polls, int link_up, const uint8_t mac[6]);
extern "C" void hakios_dhcp_input(const uint8_t *eth, size_t len);
/* hakios.net.c: the only KEV_DL_LINK_ON switch.  configd builds the IPv4
 * service (and runs DHCP) only after this event. */
extern "C" void hakios_net_link_state(bool up);
/* hakios.apple80211.c: HAKIOS_STA_LINK = 3; status 1 joined, 0 left. */
extern "C" void hakios_apple80211_event(int event, int status);

#define HIF_LOG(fmt, ...)   printf("WCN3990-HIF: " fmt "\n", ##__VA_ARGS__)

/* TX_FRM slots shared by EAPOL and the real data plane.  Four was enough for
 * a four-way handshake but not for DHCP/ARP traffic; 8 x 2 KiB still fits in
 * the 6 MiB DMA window and in the first frag descriptors of htt_frag[]. */
#define WPA_TX_SLOTS 8u
/* netpath=1: hand received Ethernet frames to the stack and send its
 * outgoing frames over HTT TX_FRM instead of faking DHCP/ARP.  0 keeps the
 * old behaviour as an A/B control. */
static int g_net_path;
/* txguard=1 (default): keep the WCN3990 CE4 source ring from being
 * overwritten while the copy engine is still reading it.  WCN3990 raises
 * copy-complete / advances SRRI *before* the last DMA has finished
 * (Linux mainline "ath10k: Set tx credit to one for wcn3990 snoc based
 * devices" + "ath10k: Delay the unmapping of the buffer", CVE-2022-50700).
 * Trogdor-ipadOS WlanRadio.c wc_send_gather() refuses the send unless two
 * descriptors are free, gives every descriptor its own header buffer and
 * drops a frame when no TX slot is free.  Our old code did none of the
 * three, so the next gather overwrote a live descriptor/header: SMMU
 * context fault -> NATIVE_UFS_FAILURE -> disk5 I/O error -> SError panic.
 * A/B on picasso: netpath=1 panics, netpath=0 never does.
 * txguard=0 restores the old behaviour as the exact control. */
static int g_tx_guard = 1;
static uint32_t g_ce4_busy;
static uint32_t g_ce4_busy_logged;
/* dhcp=1 (wifiauto default): run the in-kernel DHCPv4 client on the real
 * data plane. 0 keeps configd solely in charge, for an exact A/B. */
static int g_dhcp;
static int g_dp_diag;
static uint32_t g_dp_bad_logged;
/* Set once message 4 has gone out and both keys are installed.  Declared
 * here because the RX path runs long before the WPA state block below. */
static int g_link_up;

/* ----------------------------------------------------------- IMEM black box */

/* Qualcomm's always-on IMEM survives a warm reset, so it is the only place a
 * record of the last moments can outlive the very reset it is documenting.
 * The msm-imem page is 0x146AA000: the DT's pil@0x94c crash record ends at
 * 0xA14 and nothing claims 0xA20..0xFFF, so the box starts there.  Each
 * record is 20 bytes (magic, sequence, tag, two args); the sequence is
 * seeded from the highest one already in IMEM so consecutive boots form one
 * uninterrupted stream.  wcn3990_hif_init() prints the previous boot's tail
 * and the reset-reason words before touching the CE, so even a machine that
 * dies inside stage 3 tells the next boot exactly where it got to. */
#define WLAN_IMEM_PA      0x146AA000ull
#define WLAN_COOKIE_PA    0x146AB000ull
#define WLAN_BB_PA        (WLAN_IMEM_PA + 0xA20)
#define WLAN_BB_MAGIC     0x574C414Eu              /* "WLAN" */
#define WLAN_BB_MAX       72u
#define WLAN_RR_COOKIE    ((WLAN_COOKIE_PA + 0x65C) / 4)  /* XNU panic arm */
#define WLAN_RR_DT        ((WLAN_IMEM_PA + 0x65C) / 4)    /* msm-imem restart_reason */
#define WLAN_PIL_OFF      (0x94C / 4)                     /* msm-imem pil@94c */
#define WLAN_PIL_WORDS    (0xC8 / 4)

/* tags */
#define BB_INIT_ENTRY     1
#define BB_BASES_DONE     2
#define BB_DOOR_NOOP      3
#define BB_DOOR_REAL      4
#define BB_WATCH          5
#define BB_WATCH_DONE     6
#define BB_RX             7
#define BB_REPOST         8

static volatile uint32_t *g_bb;       /* first record slot */
static volatile uint32_t *g_imem;     /* page base: restart reason + pil */
static volatile uint32_t *g_cookie;   /* shared-IMEM page: panic reason */
static uint32_t g_bb_seq;

static void
hif_bb_init(void)
{
	g_imem = (volatile uint32_t *)ml_io_map(WLAN_IMEM_PA, 0x1000);
	g_cookie = (volatile uint32_t *)ml_io_map(WLAN_COOKIE_PA, 0x1000);
	if (!g_imem) return;
	g_bb = g_imem + ((WLAN_BB_PA - WLAN_IMEM_PA) / 4);
	/* Continue the previous boots' sequence; a fresh IMEM starts at 1. */
	for (uint32_t i = 0; i < WLAN_BB_MAX; i++) {
		uint32_t s = g_bb[i * 5 + 1];

		if (g_bb[i * 5 + 0] == WLAN_BB_MAGIC && s > g_bb_seq && s < 0xffffff00u)
			g_bb_seq = s;
	}
}

static void
hif_bb(uint32_t tag, uint32_t a1, uint32_t a2)
{
	uint32_t idx;

	if (!g_bb) return;
	idx = g_bb_seq % WLAN_BB_MAX;
	g_bb[idx * 5 + 0] = WLAN_BB_MAGIC;
	g_bb[idx * 5 + 1] = ++g_bb_seq;
	g_bb[idx * 5 + 2] = tag;
	g_bb[idx * 5 + 3] = a1;
	g_bb[idx * 5 + 4] = a2;
	__asm__ __volatile__("dmb sy" ::: "memory");
}

/* Print the previous boot's last records, the reset reasons and the PIL
 * subsystem-crash record.  Called once, at the very top of hif init, before
 * any CE register is touched. */
static void
hif_bb_dump(void)
{
	uint32_t hi = g_bb_seq, tail;
	uint32_t i, k;

	if (!g_bb) {
		HIF_LOG("blackbox: IMEM map failed");
		return;
	}
	HIF_LOG("blackbox: reset reason cookie=%08x dt=%08x",
	    g_cookie ? g_cookie[0x65C / 4] : 0xdeadbeef,
	    g_imem[0x65C / 4]);

	{
		/* The PIL record holds one entry per subsystem that crashed:
		 * short name plus reason bytes.  Modem/WLAN assert text starts
		 * here, if the subsystem ever got to write it. */
		char pil[0xC9];

		for (i = 0; i < WLAN_PIL_WORDS; i++) {
			uint32_t w = g_imem[WLAN_PIL_OFF + i];

			pil[i * 4 + 0] = (char)(w);
			pil[i * 4 + 1] = (char)(w >> 8);
			pil[i * 4 + 2] = (char)(w >> 16);
			pil[i * 4 + 3] = (char)(w >> 24);
		}
		pil[0xC8] = 0;
		HIF_LOG("blackbox: pil imem: %s", pil);
	}

	/* The previous boot's records are the 20 below the highest sequence
	 * found before this boot started writing. */
	tail = (hi >= 20) ? hi - 20 : 0;
	for (k = 0; k < 20 && tail + k < hi; k++) {
		uint32_t s = tail + k;
		uint32_t idx = (s - 1) % WLAN_BB_MAX;
		uint32_t *r = (uint32_t *)(uintptr_t)(g_bb + idx * 5);

		if (r[0] != WLAN_BB_MAGIC) continue;
		HIF_LOG("blackbox: seq=%u tag=%u a1=%08x a2=%08x",
		    s, r[2], r[3], r[4]);
	}
}

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

/* Hex dump, 16 bytes per line.  The firmware is the only place that knows
 * how it laid a TLV event out, and the serial console is the only channel
 * it speaks through, so anything we cannot name yet is printed raw before
 * it is thrown away. */
static void
hif_dump_hex(const char *what, const uint8_t *p, uint32_t n)
{
	char line[3 * 16 + 1];
	uint32_t i, j;

	for (i = 0; i < n; i += 16) {
		uint32_t chunk = (n - i > 16) ? 16 : (n - i);
		for (j = 0; j < chunk; j++) {
			static const char hex[] = "0123456789abcdef";
			line[j * 3 + 0] = hex[p[i + j] >> 4];
			line[j * 3 + 1] = hex[p[i + j] & 15];
			line[j * 3 + 2] = (j + 1 == chunk) ? '\0' : ' ';
		}
		line[chunk * 3] = '\0';
		HIF_LOG("%s +%u: %s", what, i, line);
	}
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

static inline void
hif_put64(uint8_t *p, uint64_t v)
{
	hif_put32(p, (uint32_t)v);
	hif_put32(p + 4, (uint32_t)(v >> 32));
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
#define CE_STATUS                0x30   /* copy-complete/error/watermark, W1C */
#define CE_MISC_IE               0x34
#define CE_CTRL1                 0x18   /* hw.c: wcn3990_ctrl1.addr */
#define CE_CTRL1_DMAX_MASK       0x0000ffffu
#define CE_CTRL1_SRC_BSWAP       (1u << 17)  /* wcn3990_src_ring.mask */
#define CE_CTRL1_DST_BSWAP       (1u << 18)  /* wcn3990_dst_ring.mask */
#define CE_SR_WR_INDEX           0x3c   /* WCN3990_SRC_WR_IDX_OFFSET */
#define CE_DST_WR_INDEX          0x40   /* WCN3990_DST_WR_IDX_OFFSET */
#define CE_CURRENT_SRRI          0x44
#define CE_CURRENT_DRRI          0x48
/* hw.c: wcn3990_wm_dst_ring - high watermark in [15:0], low in [31:16].
 * ce.c: ath10k_ce_init_dest_ring() sets low 0 and high nentries; both reset
 * to 0, so without this the ring claims zero usable buffers. */
#define CE_DST_WM                0x50
#define CE_DST_WM_HIGH_MASK      0x0000ffffu
#define CE_DST_WM_LOW_MASK       0xffff0000u
/* hw.c: wcn3990_wm_src_ring - the src ring's own watermark register, at a
 * different offset from the dst one.  ath10k_ce_init_src_ring() also writes
 * low 0 / high nentries here; leaving it at reset 0 was a divergence. */
#define CE_SRC_WM                0x4c
/* hw.c: wcn3990_ce_regs.ce_rri_low/high live in the CE wrapper at 0x24C000,
 * not inside a per-CE window.  ce.c: ath10k_ce_alloc_rri() stores the DDR
 * address of the read-index array there and then sets bit 19 in every CE's
 * CTRL1 so the CE updates that array instead of the 0x44 register. */
#define CE_WRAP_RRI_LOW          0x0024C004u
#define CE_WRAP_RRI_HIGH         0x0024C008u
#define CE_CTRL1_RRI_UPD         (1u << 19)  /* wcn3990_ctrl1_upd.mask */
/* ce.c: shadow_sr_wr_ind_addr() */
#define CE0_SHADOW_SR_WR         0x00032000u
#define CE3_SHADOW_SR_WR         0x0003200Cu
/* snoc.c target_shadow_reg_cfg_map[] also covers CE4/CE5/CE7, one 4-byte
 * slot each in the wrapper's shadow array (src_wr_idx offset 0x3C). */
#define CE4_SHADOW_SR_WR         0x00032010u
/* ce.c shadow_dst_wr_ind_addr(): WCN3990's always-on shadow window is
 * the only safe destination doorbell once the target may be idle.
 * The CE-body 0x40 register is un-clocked in that state (IMPS 2/4). */
#define CE1_SHADOW_DST_WR        0x00032034u
#define CE2_SHADOW_DST_WR        0x00032038u
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

/* ---- sizes shared by the DMA block and the HTT code below ----------------
 * htc.h: every CE buffer starts with an 8-byte HTC header.
 * htt.h: struct htt_cmd_hdr is { u8 msg_type; } and struct htt_cmd puts it in
 * front of a union, so every H2T message starts with 4 bytes: the type and
 * three padding bytes.  htt_tx.c ath10k_htt_h2t_ver_req_msg() sends exactly
 * hdr + htt_ver_req (3 pad bytes) = 4 bytes for VERSION_REQ.
 * HTT_RX_RING_SIZE_MIN is 128; the real kernel uses 2048 entries (4 MiB of
 * buffers), MIN keeps the target's RX path alive and fits the 1 MiB window. */
#define HTC_HDR_BYTES                 8
/* htt.h: struct htt_cmd_hdr is { u8 msg_type; } __packed and struct htt_cmd
 * is __packed too, so the command payload starts at byte 1, not 4.  Every
 * builder in htt_tx.c uses sizeof(cmd->hdr) == 1; VERSION_REQ only looks
 * 4-byte aligned because struct htt_ver_req is itself three pad bytes. */
#define HTT_HDR_BYTES                 1
#define HTT_H2T_MSG_TYPE_VERSION_REQ  0
#define HTT_H2T_MSG_TYPE_RX_RING_CFG  2
#define HTT_H2T_MSG_TYPE_AGGR_CFG     5
#define HTT_H2T_MSG_TYPE_FRAG_DESC_BANK_CFG 6
#define HTT_T2H_MSG_TYPE_VERSION_CONF 0
/* wmi-tlv.h/hw.c: WCN3990 advertises rx_ring_fill_level =
 * HTT_RX_RING_FILL_LEVEL_DUAL_MAC = HTT_RX_RING_SIZE - 1, and
 * ath10k_htt_rx_ring_size() is fill_level + 1 = 2048.  The target is told
 * that number and the host posts one buffer per entry minus the one slot
 * that separates "full" from "empty".  128 is the driver-side minimum, but
 * the firmware sizes its own bookkeeping from the value it is given, so
 * follow Linux and hand it the real thing: 2048 entries, 4 MiB of buffers. */
#define HTT_RX_RING_ENTRIES           2048
#define HTT_RX_BUF_SIZE               2048

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
	/* src rings: every OUT pipe gets a programmed ring even if nothing
	 * ever sends on it - snoc.c: ath10k_snoc_init_pipes() walks all 12. */
	struct wcn_ce_desc_64 src0[CE0_ENTRIES];
	struct wcn_ce_desc_64 src3[CE3_ENTRIES];
	struct wcn_ce_desc_64 src4[32];       /* HTT out (unused) */
	struct wcn_ce_desc_64 src7[2];        /* diag */
	/* dst rings: snoc.c posts receive buffers on every IN pipe
	 * (ath10k_snoc_rx_post loops CE_COUNT), because the target starts
	 * using whichever pipe it wants the moment it is up.  A reset ring
	 * base of 0 made the CE dma firmware data to garbage addresses. */
	struct wcn_ce_desc_64 dst2[CE2_ENTRIES];
	struct wcn_ce_desc_64 dst1[32];       /* HTT + HTC control in */
	struct wcn_ce_desc_64 dst5[32];       /* HTT in (IPA, unused) */
	struct wcn_ce_desc_64 dst7[2];        /* diag in */
	struct wcn_ce_desc_64 dst8[32];       /* target->uMC */
	struct wcn_ce_desc_64 dst9[32];       /* HTT in */
	struct wcn_ce_desc_64 dst10[32];      /* HTT in */
	struct wcn_ce_desc_64 dst11[32];      /* pktlog in */
	/* receive buffers */
	uint8_t rx2[CE2_ENTRIES][CE_BUF_BYTES];
	uint8_t rx1[32][CE_BUF_BYTES];
	uint8_t rx5[32][512];
	uint8_t rx7[2][CE_BUF_BYTES];
	uint8_t rx8[32][CE_BUF_BYTES];
	uint8_t rx9[32][CE_BUF_BYTES];
	uint8_t rx10[32][CE_BUF_BYTES];
	uint8_t rx11[32][CE_BUF_BYTES];
	/* transmit scratch */
	uint8_t tx0[CE_BUF_BYTES];
	/* CVE-2022-50700 / Linux "wifi: ath10k: Delay the unmapping of the
	 * buffer": on WCN3990 the CE3 copy-complete interrupt can arrive while
	 * the engine is still reading the source buffer.  A single shared tx3
	 * scratch buffer meant every WMI command reused the SAME physical
	 * address (all three "ce3 send" lines logged pa cdd9ae40).  WMI READY
	 * then issues PDEV_SET_REGDOMAIN and VDEV_CREATE back-to-back, so the
	 * second memset/memcpy overwrites the first while CE3 is still fetching
	 * it -> SMMU external fault -> ARM64 SError "Unhandled implementation
	 * specific error" from sleh_serror.  Rotate over 8 buffers instead. */
	uint8_t tx3[8][CE_BUF_BYTES];
	/* HTT H2T scratch.  snoc.c gives CE4 src_sz_max = 256, so every
	 * htt_cmd (4-byte header + payload) must fit in 256 bytes.  Four
	 * rotating buffers for the same reason as tx3. */
	uint8_t tx4[4][256];
	/* join=<n>: WMI_MGMT_TX_SEND is by-reference - the 802.11 frame body
	 * lives in a slot the firmware DMAs itself.  The wlan-wip reference
	 * (drivers/NativeWlan/WlanRadio.c wj_send_mgmt) uses 8 x 1024-byte
	 * slots and only reuses a slot when it comes round again, so a frame
	 * stays intact until the target has fetched it.  8 KiB, far inside
	 * the HIF_DMA_SIZE slack (block is ~4.5 MiB of 6 MiB). */
	uint8_t mgmt_tx[8][1024];
	/* WPA2 TX frames and, with netpath=1, real data frames go out through
	 * HTT TX_FRM with a frag descriptor.  One rotating 2 KiB slot per frag
	 * descriptor at the head of htt_frag[]. */
	uint8_t tx_frame[WPA_TX_SLOTS][2048];
	/* TX_FRM headers for the gather path.  Trogdor wc_send_gather() puts
	 * the header in a per-descriptor buffer (s->buf + write * src_max), so
	 * it cannot be reused before the CE has consumed that descriptor.
	 * The old tx_head[idx & 3] shared just four 64-byte slots (in practice
	 * only two, because a gather advances the write index by two), and
	 * head_len can be up to 256 bytes.  One 256-byte slot per CE4 source
	 * descriptor = 8 KiB, well inside HIF_DMA_SIZE. */
	uint8_t tx_head[32][256];
	/* wcn3990 keeps the per-CE read indices in DDR (hw_params.rri_on_ddr);
	 * ce.c: ath10k_ce_alloc_rri() points the wrapper at this array. */
	uint32_t rri[16];
	/* HTT RX ring handed to the target by RX_RING_CFG: an array of buffer
	 * physical addresses plus the 4-byte alloc index the firmware bumps.
	 * Without this the target has nowhere to put anything it receives. */
	uint64_t htt_ring[HTT_RX_RING_ENTRIES];
	uint32_t htt_alloc_idx;
	uint8_t htt_buf[HTT_RX_RING_ENTRIES][HTT_RX_BUF_SIZE];
	/* HTT TX frag descriptor bank (FRAG_DESC_BANK_CFG): 1056 x 72 B, the
	 * same geometry trogdor registers (WS_FRAG_DESCS / WS_FRAG_DESC).
	 * chain-543 proved VDEV_UP is the only command that kills the target
	 * (assoc without it survives 170 s).  VDEV_UP enables the TX data
	 * path; ath10k/trogdor always register this bank during htt_setup,
	 * ours never did. */
	uint8_t htt_frag[1056 * 72];
};

#define HIF_DMA_SIZE   0x600000u         /* 6 MiB: rings + CE bufs + 4 MiB HTT RX ring */
/* The rotating CE3 buffers and the HTT RX ring (2048 x 2048 bytes of
 * buffers plus the 2048-entry paddr ring) must fit; make an overflow a
 * build error. */
typedef char hif_dma_window_check[(sizeof(struct hif_dma_block) <= HIF_DMA_SIZE) ? 1 : -1];
/* CE4 gather headers: one 256-byte slot per source descriptor (32), so a
 * header cannot be reused before the CE has consumed that descriptor. */
typedef char hif_ce4_head_check[
    (sizeof(((struct hif_dma_block *)0)->tx_head) /
         sizeof(((struct hif_dma_block *)0)->tx_head[0]) >= 32u &&
     sizeof(((struct hif_dma_block *)0)->tx_head[0]) >= 256u) ? 1 : -1];
static struct hif_dma_block *g_dma;
static uint64_t g_dma_pa;         /* physical base of the uncached alias */
/* The CE DMA block is a Normal non-cacheable / write-combining mapping, so
 * kvtophys() is no longer meaningful for it.  Linux's dma_alloc_coherent()
 * and CoachZ's wc_alloc() both hand out (va, pa) pairs and compute every
 * descriptor address as an offset from the block's physical base. */
static inline uint64_t
hif_pa_of(const void *p)
{
	return g_dma_pa + (uint64_t)((const uint8_t *)p - (const uint8_t *)g_dma);
}
#define s_src0  (g_dma->src0)
#define s_dst2  (g_dma->dst2)
#define s_src3  (g_dma->src3)
#define s_rx2   (g_dma->rx2)
#define s_tx0   (g_dma->tx0)
#define s_tx3   (g_dma->tx3[0])
#define s_tx3_slot(i) (g_dma->tx3[(i) & 7u])
#define s_rri   (g_dma->rri)

extern "C" void *picasso_wlan_ce_dma_alloc(uint64_t size, uint64_t *pa_out,
    uint32_t assign_level);
extern "C" void picasso_wlan_smmu_fault_read(uint32_t * out);
/* smp2p_wlan_1_in entry "wlan": bit 0 force-fatal-error, bit 1 early-crash.
 * 0xfffffff* when the mailbox cannot be parsed. */
extern "C" uint32_t picasso_wlan_smp2p_bits(void);
extern "C" int picasso_wlan_smmu_s2cr_set(uint32_t index, uint32_t value,
    uint32_t * readback);

/* The passthrough context bank value, mirroring PicassoWlan.h's constant
 * without pulling the whole header in. */
#define WLAN_SMMU_PASSTHRU_BANK 61u

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
/* wcn3990-hif-quiet=1: after the doorbell, stop touching the copy engines
 * entirely and heartbeat from the poller instead.  The console dies within
 * a millisecond of "posted 63 receive buffers" while the machine itself
 * stays up, and this separates the two candidate causes: the doorbell
 * wedging the interconnect, or the poller's very next CE access.  One
 * single probe read is made five seconds after the doorbell, and whether
 * the heartbeat stops there says which access it was. */
static int g_ce_quiet;
static int g_tx3_rotate;      /* 0 = legacy single buffer, 1 = 8 rotating */
static int g_autoscan;        /* issue START_SCAN once WMI is ready */
static int g_scan_wait;       /* scanwait=N: delay the scan N seconds */
static uint32_t g_scan_at;    /* poll count at which autoscan may fire */
static int g_autoscan_sent;
/* hifstop: bisect the two WMI commands sent right after WMI READY.
 * 0 = both, 1 = skip PDEV_SET_REGDOMAIN, 2 = skip VDEV_CREATE,
 * 3 = skip both.  A WCN3990 firmware crash ~17 ms after vdev_create is
 * a documented failure mode, so this separates "our command kills the
 * firmware" from "the firmware kills the host on its own". */
static int g_ready_stop;
/* 485 bisect proved VDEV_CREATE is the sole trigger of the post-WMI
 * ARM64 SError, with clean SMMU fault registers.  Linux sends
 * WMI_PDEV_SET_BASE_MACADDR right after WMI READY and only creates the
 * vdev much later, when the interface is brought up.  vdevpre=1 adds
 * that missing command; hifstop=5/6 delays VDEV_CREATE by 1k/5k polls;
 * hifstop=4 skips it and forces an autoscan to see whether the firmware
 * will scan without a vdev at all. */
static int g_vdev_pre;
static int g_wmi_init_full = 1; /* Linux WCN3990 resource config (the 44-field one) */
/* initfix=<n>: per-field gates for the resource-config values the working
 * same-chip wlan-wip port (work/trogdor/port/WlanRadio.c wm_send_init) uses
 * and the 44-field Linux table here leaves at zero.  WCN3990's full-RX-
 * reorder firmware needs offload reorder buffers and an AST table to build a
 * peer data path; chain-571 saw PEER_CREATE consumed but no peer map, no
 * HTT frame and no MSDU, which is what a zero-reorder peer looks like.
 *   bit0 (1): cfg[2]=4 num_offload_peers, cfg[3]=4 num_offload_reorder_bufs
 *   bit1 (2): cfg[6]=16 ast_skid_limit
 *   bit2 (4): cfg[22]=2 num_wds_entries
 * 0 = today's behaviour (control), 7 = all three (reference behaviour).
 *
 * chain-572 four-way single-variable (0/1/3/7) proved bit1 ast_skid_limit
 * is what makes the target build a peer data path: 0/1 have no peer map,
 * no HTT RX_IN_ORD and no EAPOL; 3/7 deliver all of them.  bit2 (wds) is
 * not required.  Default is now 3; initfix=0 still reproduces the old
 * control for A/B. */
static int g_init_fix = 3;
static int g_init_arr;        /* initarr: 0 omit, 1 tag 19, 2 tag 18 */
static int g_scan_shape;      /* scanshp: 0 full, 1 struct only, 2 +empty array */
static int g_vdev_start;      /* vdstart=1: WMI_VDEV_START before the scan */
static int g_msdu_desc;       /* msdudesc=N: override resource_config[28] */
static int g_scan_chans = -1; /* scanchans=N: cap the START_SCAN channel list */
/* wmi-tlv.c ath10k_wmi_tlv_iter() advances by exactly tlv_len, with no
 * roundup.  This driver used to round every TLV up to 4 bytes.  Three of
 * the TLVs we emit are not 4-aligned (VDEV_CREATE 18, scan bssid 6, base
 * MAC 6), so two zero bytes were injected after each: VDEV_CREATE ended
 * with a 2-byte tail that cannot even hold a TLV header.  tlvpad=1 restores
 * the old behaviour as an in-chain negative control. */
static int g_tlv_pad;         /* 0 = Linux-exact TLV lengths */
/* 526+: byte-for-byte alignment with the other WCN3990 port
 * (work/trogdor/coachz_wifi.c, SC7180) and Linux ath10k.  Each switch is
 * independent so a build can add one difference at a time. */
/* One bitmask boot-arg (scanfix=N) carries all four independent switches:
 * the 995-byte cmdline cannot fit four extra tokens, and a single token
 * also lets one image express all 15 A/B combinations. */
#define SCANFIX_CMD       0x1
#define SCANFIX_REGDOM    0x2
#define SCANFIX_CHANV2    0x4
#define SCANFIX_PDEVPARAM 0x8
#define SCANFIX_PDEV7     0x40  /* trogdor ws_next(): 7 PDEV_SET_PARAM */
/* 0x10: struct wmi_mac_addr is a 2-word (8-byte) union, not 6 bytes.
 * abi-check.py found three more TLVs that still used a 6-byte MAC:
 * VDEV_CREATE (18 vs 20), VDEV_UP (14 vs 16) and PDEV_SET_BASE_MACADDR
 * (6 vs 8).  Linux wmi.h:851 and trogdor's WS_TLV(0x56, 20) agree.  The
 * vdev MAC is what the firmware matches scan frames against, so this is a
 * candidate for the "firmware scans but never emits WMI_MGMT_RX" symptom. */
#define SCANFIX_MAC       0x10
/* 0x20: (historical) post HTT RX buffers.  Chain-531 proved on device that
 * fw_idx_init_val = 0 means "host posted no buffers" and the firmware then
 * never delivers a frame; 1023 posted delivers beacons within seconds.
 * This is now the unconditional default in hif_htt_rx_ring_cfg(), so the
 * bit no longer changes anything and is kept only for boot-arg compat. */
#define SCANFIX_HTTFILL   0x20
#define HTT_RX_FILL_LEVEL 1023u   /* half of a 2048-entry ring, as trogdor */
static int g_scan_fix;
static int g_chan_list;       /* chanlist=1: WMI_SCAN_CHAN_LIST before regdomain */
static int g_vdev_up;         /* vdevup=1: WMI_VDEV_UP after VDEV_CREATE */
static int g_scan_linux;      /* scanlinux=1: Linux-exact START_SCAN arguments */
static int g_htt_connect;     /* httconn=1: connect HTT before SETUP_COMPLETE */
static int g_htt_setup;       /* httsetup=1: send HTT VERSION_REQ (htt_setup) */
static int g_htt_ring;        /* httring=1: send RX_RING_CFG + AGGR_CFG */
static uint8_t g_htt_eid;
static uint32_t g_wr4;        /* CE4 (HTT H2T) source ring write index */
static uint32_t g_sw1, g_post1; /* CE1 (HTT T2H) dst ring software/posted */
static uint32_t g_htt_rx;     /* HTT messages seen on CE1 */
static uint8_t g_htt_seq;     /* HTC seq_no for the HTT endpoint (htc.h @5) */
static int g_htt_ring_done;   /* RX_RING_CFG + AGGR_CFG already sent */
static void hif_htt_setup_finish(void);
static void hif_join_peer_event(const uint8_t *, uint32_t);
/* Defined up here because hif_service_ce1() (the HTT pipe) needs them well
 * before the join state block further down the file. */
static int g_join;               /* join=<n>: 0 = off, 1 = legacy, >=2 = full pre-req path */
static int g_join_skip;          /* joinskip: bit1 skip PEER_ASSOC, bit2 skip VDEV_UP */
static int g_join_sel;           /* joinsel=1: prefer SSID ChinaUnicom; =2: 1902_WIFI_CLOCK */
static int g_vdev_wait_ms;       /* vdwait=N: delay VDEV_UP N ms after peer_assoc */
static int g_vdev_up_pending;    /* a delayed VDEV_UP is armed */
static uint32_t g_join_backoff_at; /* do not rejoin before this poll count */
static int g_join_once;          /* the wj_begin() once-block (vdev/pdev params) */
static int g_join_once_mask = 0x1ff; /* joinonce=<n>: per-command gates, 0x1ff = all */
#define HTT_T2H_MSG_TYPE_PEER_MAP    3
#define HTT_T2H_MSG_TYPE_PEER_UNMAP  4
#define HTT_T2H_MSG_TYPE_TX_COMPL    7
/* HTT TX_COMPL_IND (0x7) layout, from wlan-wip wj_tx_compl() and the 10.4
 * enum in work/refs/ath10k/htt.h: {type, status&7, count, flags} followed
 * by count little-endian 16-bit msdu ids.  status 0 = ACKed, 2 = no ACK,
 * anything else = discarded.  chain-573 w1 emitted 07 00 01 00 00 00 ff ff
 * after every M2, i.e. status 0: all four EAPOL replies really went out. */
static void hif_wpa_tx_compl(const uint8_t *, uint32_t);
static int g_wpa_diag;                 /* wpadiag=1: log key/MIC fingerprints */
static uint32_t g_wpa_tx_ack, g_wpa_tx_noack, g_wpa_tx_discard;
static uint8_t g_wpa_tx_busy[WPA_TX_SLOTS]; /* TX slots not yet completed */
/* wlan-wip wj_req / nw_wifi_connect(): the 802.11 stack hands down an
 * SSID + passphrase pair and wj_begin() matches that SSID exactly.  Our
 * boot-arg joinsel was only ever a lab shortcut; wifiauto and the real
 * join callback both go through this request now. */
static struct {
	uint8_t ssid[32];
	uint8_t ssid_len;
	uint8_t pending;
} g_join_req;
static int g_wifi_auto;
static uint32_t g_last_rescan_at;   /* wifiauto: re-scan while waiting */
static int g_vdev_pending;
static uint32_t g_vdev_at;
static uint32_t g_quiet_probe_polls;
static int g_quiet_probed;
static bool g_bases_programmed;
/* Number of receive descriptors to hand to CE2 in stage 3. */
static uint32_t g_post_count = CE2_ENTRIES - 1;
static uint32_t g_wr0, g_wr3;         /* source ring write index */
static uint32_t g_sw2, g_post2;       /* dest ring software / posted index */
static uint64_t g_rx2_pa, g_dst2_pa, g_src0_pa, g_src3_pa;

/* Per-CE host-side layout, from snoc.c: host_ce_config_wlan (buf sizes) and
 * target_ce_config_wlan (ring depth the target pairs with).  Rings exist for
 * every pipe so a target-side pipe waking up always finds a programmed ring;
 * entry counts beyond 64/32/2 were trimmed to what the bring-up needs. */
struct hif_ce_shape {
	uint8_t src_n;      /* host-side src ring entries, 0 = none */
	uint8_t dst_n;      /* host-side dst ring entries, 0 = none */
	uint16_t buf_sz;    /* rx buffer bytes for a dst pipe */
	uint16_t dmax;      /* src_sz_max for a src pipe (CTRL1.DMAX) */
};
static const struct hif_ce_shape hif_shape[12] = {
	/*  0 */ {16, 0, 0, CE_BUF_BYTES},  /* host->target HTC ctrl */
	/*  1 */ {0, 32, CE_BUF_BYTES, 0},  /* target->host HTT+ctrl */
	/*  2 */ {0, 64, CE_BUF_BYTES, 0},  /* target->host WMI */
	/*  3 */ {32, 0, 0, CE_BUF_BYTES},  /* host->target WMI */
	/*  4 */ {32, 0, 0, 256},           /* host->target HTT (unused) */
	/*  5 */ {0, 32, 512, 0},           /* target->host HTT (IPA, unused) */
	/*  6 */ {0, 0, 0, 0},
	/*  7 */ {2, 2, CE_BUF_BYTES, CE_BUF_BYTES}, /* diag */
	/*  8 */ {0, 32, CE_BUF_BYTES, 0},
	/*  9 */ {0, 32, CE_BUF_BYTES, 0},
	/* 10 */ {0, 32, CE_BUF_BYTES, 0},
	/* 11 */ {0, 32, CE_BUF_BYTES, 0},  /* pktlog */
};
/* ring physical addresses: index by CE id */
static uint64_t g_sr_pa[12], g_dr_pa[12], g_buf_pa[12];
static struct wcn_ce_desc_64 *g_sr_desc[12], *g_dr_desc[12];
static uint8_t *g_buf_va[12];
static uint32_t g_rri_last[12];       /* heartbeat: last seen DDR read index */

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

/* WCN3990's always-on shadow window is WRITE-ONLY (IMPS patch 2/4):
 * never read it back, and never read the CE-body 0x3c/0x44/0x48 either. */

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
	case 0:  ring = s_src0; entries = hif_shape[0].src_n; wr = &g_wr0; break;
	case 3:  ring = s_src3; entries = hif_shape[3].src_n; wr = &g_wr3; break;
	case 4:  ring = g_dma->src4; entries = hif_shape[4].src_n; wr = &g_wr4; break;
	default: return -1;
	}

	if (len > hif_shape[ce].dmax && hif_shape[ce].dmax != 0) return -1;

	idx = *wr;
	pa = hif_pa_of(buf);
	if (!pa) {
		HIF_LOG("ce%u: no physical address for %p", ce, buf);
		return -1;
	}

	ring[idx].addr = pa & CE_DESC_ADDR_MASK;
	ring[idx].nbytes = (uint16_t)len;
	ring[idx].flags = 0;
	ring[idx].toeplitz = 0;
	hif_mb();
	/* The ring and the buffer both live in the non-cacheable window: the
	 * CE sees this descriptor as soon as the doorbell below is written. */

	*wr = (idx + 1) & (entries - 1);

	/* wcn3990 sets hw_params.shadow_reg_support, and ce.c only takes the
	 * shadow branch in that case:
	 *   if (ar->hw_params.shadow_reg_support)
	 *           ath10k_ce_shadow_src_ring_write_index_set(...)
	 *   else
	 *           ath10k_ce_src_ring_write_index_set(...)
	 * so the doorbell the CE actually watches is 0x32000/0x3200C, not the
	 * per-CE 0x3c register. */
	if (ce == 0) shadow = (uint8_t *)(uintptr_t)CE0_SHADOW_SR_WR;
	else if (ce == 3) shadow = (uint8_t *)(uintptr_t)CE3_SHADOW_SR_WR;
	else if (ce == 4) shadow = (uint8_t *)(uintptr_t)CE4_SHADOW_SR_WR;
	if (shadow) {
		*(volatile uint32_t *)(uintptr_t)(g_ce_va + (uintptr_t)shadow) = *wr;
		hif_mb();
	} else {
		hif_reg_write(ce, CE_SR_WR_INDEX, *wr);
	}

	HIF_LOG("ce%u send: desc %u pa %llx len %u -> wr %u (shadow %u)",
	    ce, idx, (unsigned long long)pa, len, *wr, shadow ? 1u : 0u);
	return 0;
}

/* Two-segment send for HTT data frames (wlan-wip wc_send_gather): the first
 * descriptor carries the 28-byte HTT TX_FRM header and the CE_WCN3990 GATHER
 * flag (bit 31 of the high address word), the second points at the frame
 * body.  CE4 is the only OUT pipe whose src_sz_max (256) could not hold a
 * full 802.11 frame, hence the gather. */
static int
hif_ce_send_gather(uint32_t ce, const uint8_t *head, uint32_t head_len,
    uint64_t body_pa, uint32_t body_len)
{
	struct wcn_ce_desc_64 *ring;
	uint32_t entries, *wr, idx, next;
	uint8_t *shadow = NULL, *hbuf;
	uint64_t head_pa;

	if (!g_regs_live || g_stage < 3 || ce != 4 || !g_dma) return -1;
	if (body_len > (uint32_t)CE_BUF_BYTES || head_len > 256u) return -1;
	ring = g_dma->src4;
	entries = hif_shape[4].src_n;
	if (entries > (sizeof(g_dma->tx_head) / sizeof(g_dma->tx_head[0])))
		return -1;
	wr = &g_wr4;
	idx = *wr & (entries - 1);

	/* Trogdor wc_send_gather(): read the CE's DDR source read index and
	 * refuse the send unless at least two descriptors are free.  WCN3990
	 * advances SRRI early (CVE-2022-50700 / mainline ath10k tx-credit=1
	 * and delayed-unmap patches), so without this the next gather rewrites
	 * a descriptor the copy engine is still fetching.  The DDR RRI array
	 * is written by the CE through the wrapper programmed in
	 * hif_program_bases(); read it volatile, same as wc.rri[]. */
	if (g_tx_guard) {
		uint32_t srri = (*(volatile uint32_t *)&g_dma->rri[4]) & 0xffffu;
		if ((((srri - 1u) - *wr) & (entries - 1u)) < 2u) {
			g_ce4_busy++;
			if (g_ce4_busy_logged < 16u ||
			    (g_ce4_busy & 0x3ffu) == 0u) {
				g_ce4_busy_logged++;
				HIF_LOG("ce4 gather busy=%u srri=%u wr=%u (txguard)",
				    g_ce4_busy, srri, *wr);
			}
			return -2;
		}
	}

	hbuf = g_dma->tx_head[idx];
	memcpy(hbuf, head, head_len);
	head_pa = hif_pa_of(hbuf);
	if (!head_pa) return -1;

	/* wlan-wip wc_send_gather writes the gather flag twice: bit 31 of the
	 * address high word (bit 63 here, outside the 35-bit address mask) and
	 * the descriptor flags field. */
	ring[idx].addr = (head_pa & CE_DESC_ADDR_MASK) | (1ULL << 63);
	ring[idx].nbytes = (uint16_t)head_len;
	ring[idx].flags = 1;                     /* CE_DESC_FLAGS_GATHER */
	ring[idx].toeplitz = 0;
	next = (idx + 1) & (entries - 1);
	ring[next].addr = body_pa & CE_DESC_ADDR_MASK;
	ring[next].nbytes = (uint16_t)body_len;
	ring[next].flags = 0;
	ring[next].toeplitz = 0;
	hif_mb();

	*wr = (next + 1) & (entries - 1);
	shadow = (uint8_t *)(uintptr_t)CE4_SHADOW_SR_WR;
	*(volatile uint32_t *)(uintptr_t)(g_ce_va + (uintptr_t)shadow) = *wr;
	hif_mb();
	return 0;
}

/* Post one more receive buffer on CE2 and advance the dest write index.
 * ce.c: ath10k_ce_rx_update_write_idx() keeps one slot free; we start with
 * CE2_ENTRIES-1 posted buffers for the same reason.  The buffer must be filled
 * at the *write* index (the one slot the target does not own yet), not at the
 * slot just consumed. */
static void
hif_ce2_repost(uint32_t idx)
{
	uint32_t slot = g_post2 & (CE2_ENTRIES - 1);
	struct wcn_ce_desc_64 *d = &s_dst2[slot];

	(void)idx;
	d->addr = (g_rx2_pa + (uint64_t)slot * CE_BUF_BYTES) & CE_DESC_ADDR_MASK;
	d->nbytes = 0;   /* posted, not completed; the CE writes the length */
	d->flags = 0;
	d->toeplitz = 0;
	hif_mb();

	g_post2 = (slot + 1) & (CE2_ENTRIES - 1);
	/* Always-on shadow doorbell; the CE-body 0x40 store is unsafe
	 * once the target has idled (IMPS patch 2/4). */
	*(volatile uint32_t *)(uintptr_t)(g_ce_va + CE2_SHADOW_DST_WR) = g_post2;
	hif_mb();
}

/* ---- HTT receive path (grafted wholesale from wlan-wip wr_*, L4169-4270) -
 * chain-536: after vdev up the firmware starts putting received frames into
 * this ring, and with nobody draining it the target answered the 0x401
 * early-crash message and the kernel panicked.  This is also the path EAPOL
 * frames arrive on, which makes it the prerequisite for the four-way
 * handshake.  Scope for this round: drain, refill, count and classify -
 * Ethernet payloads are logged, not yet delivered to the stack. */
static uint32_t g_htt_alloc;      /* refill index, mirrors htt_alloc_idx */
static uint32_t g_htt_read;       /* RX_IND consumption cursor */
static uint64_t g_htt_buf_pa;     /* physical base of htt_buf[0] */
static uint32_t g_wr_rx_ind, g_wr_rx_in_ord, g_wr_rx_frames, g_wr_rx_eapol,
    g_wr_rx_bad, g_wr_rx_undone;
static void hif_wpa_handle_eapol(const uint8_t *e, uint32_t n);

static uint8_t *
hif_wr_buf_va(uint64_t pa)
{
	uint64_t base = g_htt_buf_pa;

	if (!g_dma || !base ||
	    pa < base ||
	    pa >= base + (uint64_t)HTT_RX_RING_ENTRIES * HTT_RX_BUF_SIZE ||
	    (pa - base) % HTT_RX_BUF_SIZE)
		return NULL;
	return g_dma->htt_buf[0] + (pa - base);
}

/* wlan-wip wr_post(): clear the buffer's attention word, put its address
 * back on the ring at the alloc index and publish the new index.  The
 * target only keeps handing out buffers while the host keeps refilling. */
static void
hif_wr_post(uint64_t pa)
{
	uint8_t *buf = hif_wr_buf_va(pa);
	uint32_t i = g_htt_alloc & (HTT_RX_RING_ENTRIES - 1);
	const uint32_t zero = 0;

	if (buf) memset(buf + 4, 0, 4);            /* rx_attention */
	g_dma->htt_ring[i] = pa;
	g_htt_alloc = (g_htt_alloc + 1u) & (HTT_RX_RING_ENTRIES - 1u);
	hif_mb();
	g_dma->htt_alloc_idx = g_htt_alloc;
}

/* wlan-wip wr_msdu(): validate the descriptor, convert native Wi-Fi to
 * Ethernet and hand EAPOL over.
 *
 * chain-572 (initfix=3) delivered real EAPOL frames but with garbage headers
 * (ver=247 type=28 code=96), and the AP deauthed with reason 6 after we
 * dropped message 1.  wlan-wip wr_msdu() places the converted Ethernet frame
 * at frame+32-14 = frame+18, i.e. the EAPOL payload starts at frame+32
 * (24-byte 802.11 header + 8-byte LLC/SNAP).  Our old code left eth at
 * frame+0, so eth+14 landed in the middle of the 802.11 header, 18 bytes
 * early.  rxfix bit0 restores the reference layout. */
/* chain-573 single-variable rxfix=0/1: rxfix=1 makes EAPOL M1 parse
 * (ver=1 type=3 info=008a), M2 is built and the target ACKs all four TX
 * frames.  rxfix=0 is kept as the old-behaviour control. */
static int g_rx_fix = 1;

/* dpdiag=1: log why the first dozen post-key frames were rejected.  After
 * the four-way handshake the target starts delivering encrypted data frames;
 * chain-574 x0 counted 21 rejects (bad=21) with only 2 accepted MSDUs, and
 * a counter alone cannot tell a decryption error from a header layout one. */
static void
hif_wr_bad(uint32_t reason, const uint8_t *buf, uint32_t attention,
    uint32_t start, uint32_t len, uint32_t decap)
{
	g_wr_rx_bad++;
	/* Always trace the first rejections: a TCP handshake that never
	 * completes is either "no reply arrived" or "reply rejected here",
	 * and only this line can tell the two apart.  Bounded, never floods. */
	if (g_wr_rx_bad <= 16u) {
		HIF_LOG("DP: reject #%u r=%u attn=%08x start=%08x len=%u decap=%u "
		    "frame=%02x%02x%02x%02x%02x%02x%02x%02x",
		    g_wr_rx_bad, reason, attention, start, len, decap,
		    buf[360], buf[361], buf[362], buf[363],
		    buf[364], buf[365], buf[366], buf[367]);
	}
	if (!g_dp_diag || g_dp_bad_logged >= 12u) return;
	g_dp_bad_logged++;
	HIF_LOG("DP: bad r=%u attn=%08x start=%08x len=%u decap=%u "
	    "frame=%02x%02x%02x%02x%02x%02x%02x%02x",
	    reason, attention, start, len, decap,
	    buf[360], buf[361], buf[362], buf[363],
	    buf[364], buf[365], buf[366], buf[367]);
	hif_dump_hex("DP: bad", buf + 360, 32);
}

/* Accepted-frame trace: the first 64 data frames with their L3/L4 tuple.
 * Answers the only question the EAPOL-era logs cannot: does the SYN-ACK /
 * ARP reply reach this driver at all, and with which addresses and flags. */
static void
hif_rx_trace(const uint8_t *eth, uint32_t bytes, uint32_t decap,
    uint32_t attention)
{
	static uint32_t traced;
	char line[192];
	size_t at = 0;
	uint16_t type;

	if (traced >= 64u || bytes < 14u) return;
	traced++;
	type = (uint16_t)((eth[12] << 8) | eth[13]);
	int n = snprintf(line, sizeof(line),
	    "RX: #%u decap=%u bytes=%u type=%04x dst=%02x:%02x:%02x:%02x:%02x:%02x "
	    "src=%02x:%02x:%02x:%02x:%02x:%02x",
	    traced, decap, bytes, type,
	    eth[0], eth[1], eth[2], eth[3], eth[4], eth[5],
	    eth[6], eth[7], eth[8], eth[9], eth[10], eth[11]);
	if (n < 0) return;
	at = (size_t)n < sizeof(line) - 1u ? (size_t)n : sizeof(line) - 1u;
	if (type == 0x0800 && bytes >= 34u) {
		const uint8_t *ip = eth + 14;
		uint32_t ihl = (uint32_t)(ip[0] & 0x0fu) * 4u;
		if (ihl >= 20u && ihl <= 60u && bytes >= 14u + ihl) {
			n = snprintf(line + at, sizeof(line) - at,
			    " ip=%u.%u.%u.%u>%u.%u.%u.%u proto=%u",
			    ip[12], ip[13], ip[14], ip[15],
			    ip[16], ip[17], ip[18], ip[19], ip[9]);
			if (n > 0)
				at = (size_t)n + at < sizeof(line) - 1u ?
				    at + (size_t)n : sizeof(line) - 1u;
			if (ip[9] == 6 && bytes >= 14u + ihl + 20u) {
				const uint8_t *tcp = ip + ihl;
				n = snprintf(line + at, sizeof(line) - at,
				    " tcp %u>%u fl=%02x",
				    (unsigned)((tcp[0] << 8) | tcp[1]),
				    (unsigned)((tcp[2] << 8) | tcp[3]), tcp[13]);
				if (n > 0)
					at = (size_t)n + at < sizeof(line) - 1u ?
					    at + (size_t)n : sizeof(line) - 1u;
			} else if (ip[9] == 17 && bytes >= 14u + ihl + 8u) {
				const uint8_t *udp = ip + ihl;
				n = snprintf(line + at, sizeof(line) - at,
				    " udp %u>%u",
				    (unsigned)((udp[0] << 8) | udp[1]),
				    (unsigned)((udp[2] << 8) | udp[3]));
				if (n > 0)
					at = (size_t)n + at < sizeof(line) - 1u ?
					    at + (size_t)n : sizeof(line) - 1u;
			}
		}
	} else if (type == 0x0806 && bytes >= 42u) {
		const uint8_t *arp = eth + 14;
		n = snprintf(line + at, sizeof(line) - at,
		    " arp op=%u %u.%u.%u.%u>%u.%u.%u.%u",
		    (unsigned)((arp[6] << 8) | arp[7]),
		    arp[14], arp[15], arp[16], arp[17],
		    arp[24], arp[25], arp[26], arp[27]);
	}
	line[sizeof(line) - 1u] = '\0';
	HIF_LOG("%s", line);
}

static void
hif_wr_msdu(uint8_t *buf, uint32_t len)
{
	/* wlan-wip wr_msdu() copies the MSDU out of the DMA buffer BEFORE
	 * folding the 802.11 header in place.  chain-575 y0 proved why: the
	 * target can point several RX_IN_ORD records at the SAME buffer, and
	 * folding in place corrupts it for the second pass (DP: bad r=3,
	 * attn=82300043, the LLC/SNAP bytes aa aa 03 replaced by a BSSID).
	 * With a private copy every pass sees the original frame. */
	static uint8_t frame[HTT_RX_BUF_SIZE];
	uint32_t attention, start, decap, bytes;
	uint8_t *eth;
	uint8_t da[6], sa[6];

	memcpy(&attention, buf + 4, 4);
	memcpy(&start, buf + 36, 4);
	decap = (start >> 8) & 3u;
	if (!(attention & (1u << 31))) { g_wr_rx_undone++; return; }
	/* chain-579: on the real data path the target delivered decrypted
	 * broadcast ARP / IPv4 / IPv6 frames (LLC/SNAP + EtherType visible in
	 * the dump) with bit28 set, while the fatal bits -- 27 TKIP MIC, 29
	 * FCS, 30 length, 16/17 -- stayed clear.  wlan-wip's mask treats bit28
	 * as an error because its CCMP AP never sets it; this AP uses a TKIP
	 * group key and does.  Keep the old mask for the netpath=0 control and
	 * ignore bit28 only when real frames are being handed to the stack. */
	{
		uint32_t bad = (1u << 29 | 1u << 30 | 1u << 16 | 1u << 17 |
		    1u << 27);

		if (!g_net_path) bad |= 1u << 28;
		if (attention & bad) {
			hif_wr_bad(1, buf, attention, start, len, decap);
			return;
		}
	}
	if (len < 14u || len > HTT_RX_BUF_SIZE - 360u) {
		hif_wr_bad(2, buf, attention, start, len, decap);
		return;
	}

	memcpy(frame, buf + 360, len);
	eth = frame;
	bytes = len;
	if (decap == 1) {
		if (len < 32u || eth[24] != 0xaa || eth[25] != 0xaa ||
		    eth[26] != 3) {
			hif_wr_bad(3, buf, attention, start, len, decap);
			return;
		}
		if (g_rx_fix & 1) {
			/* wlan-wip wr_msdu(): DA = addr1 at frame+4,
			 * SA = addr2 at frame+16, EtherType stays at
			 * frame+30, payload starts at frame+32.  Folding in
			 * place is safe: frame+18..29 is addr3/seq/LLC, all
			 * overwritten; frame+30..31 and frame+32+ survive. */
			memcpy(da, eth + 4, 6);
			memcpy(sa, eth + 16, 6);
			eth += 32u - 14u;
			memcpy(eth, da, 6);
			memcpy(eth + 6, sa, 6);
			/* EtherType at eth+12 == frame+30 is already there. */
		} else {
			memmove(eth, eth + 4, 6);
			memcpy(eth + 6, eth + 16, 6);
			memcpy(eth + 12, eth + 30, 2);
		}
		bytes = len - 32u + 14u;
	} else if (decap != 2) {
		hif_wr_bad(4, buf, attention, start, len, decap);
		return;
	}

	g_wr_rx_frames++;
	if (eth[12] == 0x88 && eth[13] == 0x8e) {
		g_wr_rx_eapol++;
		HIF_LOG("JOIN: EAPOL frame %u bytes ver=%u type=%u len=%02x%02x "
		    "desc=%u info=%02x%02x rxfix=%u",
		    bytes, eth[14], eth[15], eth[16], eth[17], eth[18],
		    eth[19], eth[20], (unsigned)g_rx_fix);
		hif_wpa_handle_eapol(eth + 14, bytes - 14u);
		return;
	}
	/* Real data plane: hakios_net_input() copies the Ethernet frame into
	 * an mbuf, so the DMA buffer can be reposted immediately afterwards. */
	if (g_net_path && g_link_up) {
		hif_rx_trace(eth, bytes, decap, attention);
		if (g_dhcp) hakios_dhcp_input(eth, bytes);
		hakios_net_input(eth, bytes);
		return;
	}
	if ((g_wr_rx_frames & 63u) == 1u)
		HIF_LOG("HTT: msdu #%u bytes=%u decap=%u type=%02x%02x",
		    g_wr_rx_frames, bytes, decap, eth[12], eth[13]);
}

/* wlan-wip wr_in_ord(): HTT_T2H_MSG_TYPE_RX_IN_ORD_PADDR_IND (0x12) with
 * 64-bit addresses: an 8-byte header, then {pa, len, fw_desc, reserved}
 * 12-byte records.  Offloaded/fragment entries (info bits 5-6) go straight
 * back to the ring. */
static void
hif_wr_in_ord(const uint8_t *p, uint32_t n)
{
	uint32_t i, count;
	int skip;

	g_wr_rx_in_ord++;
	if (n < 8) return;
	count = (uint32_t)(p[6] | (uint32_t)p[7] << 8);
	skip = (p[1] & 0x60u) != 0;
	for (i = 0; i < count && 8u + 12u * (i + 1u) <= n; i++) {
		uint64_t pa;
		uint16_t len;
		uint8_t *buf;

		memcpy(&pa, p + 8 + 12 * i, 8);
		memcpy(&len, p + 16 + 12 * i, 2);
		buf = hif_wr_buf_va(pa);
		if (!buf) { g_wr_rx_bad++; continue; }
		if (!skip) hif_wr_msdu(buf, len);
		hif_wr_post(pa);
	}
}

/* wlan-wip wr_rx_ind(): HTT_T2H_MSG_TYPE_RX_IND (1) - the ring's buffers in
 * order, MSDUs up to each MPDU's last one (ath10k_htt_rx_amsdu_pop). */
static void
hif_wr_rx_ind(const uint8_t *p, uint32_t n)
{
	uint32_t info1, ranges, fw, i;
	const uint8_t *r;

	g_wr_rx_ind++;
	if (n < 48) return;
	memcpy(&info1, p + 4, 4);
	ranges = info1 >> 24;
	fw = (((uint32_t)(p[44] | (uint32_t)p[45] << 8)) + 3u) & ~3u;
	r = p + 48 + fw;
	if (48u + fw + 4u * ranges > n) return;
	for (i = 0; i < ranges; i++, r += 4) {
		uint32_t m;

		for (m = 0; m < r[0]; m++) {
			unsigned guard;

			for (guard = 0; guard < 64; guard++) {
				uint64_t pa;
				uint8_t *buf;
				uint32_t start, end;

				memcpy(&pa, &g_dma->htt_ring[g_htt_read], 8);
				g_htt_read = (g_htt_read + 1u) &
				    (HTT_RX_RING_ENTRIES - 1u);
				buf = hif_wr_buf_va(pa);
				if (!buf) return;
				memcpy(&start, buf + 28, 4);
				memcpy(&end, buf + 64, 4);
				if (r[1] == 1) hif_wr_msdu(buf, start & 0x3fffu);
				hif_wr_post(pa);
				if (end & (1u << 15)) break;
			}
		}
	}
}

/* CE1 is the target->host HTT pipe.  Until 491 it was only *posted* (31
 * buffers) and never serviced, so every HTT response the firmware sent -
 * including the VERSION_CONF that ath10k_htt_setup() waits for - was dropped
 * into a ring nobody read.  Service it exactly like CE2, but keep the two
 * paths separate so a fault here cannot disturb the WMI ring. */
static void
hif_service_ce1(void)
{
	uint32_t n = hif_shape[1].dst_n;
	uint32_t budget = n;
	if (g_dr_desc[1] == NULL || g_buf_va[1] == NULL) return;
	/* CoachZ wc_service() / Linux _ath10k_ce_completed_recv_next_nolock_64():
	 * completion is decided by the host-memory descriptor nbytes only.
	 * CE_CURRENT_DRRI is outside WCN3990's always-on shadow window, so a
	 * read while the target idles is an un-clocked access -> SError. */
	while (budget-- > 0) {
		struct wcn_ce_desc_64 *d = &g_dr_desc[1][g_sw1];
		const uint8_t *b = g_buf_va[1] +
		    (uint64_t)g_sw1 * hif_shape[1].buf_sz;
		uint32_t nbytes = d->nbytes;

		if (nbytes == 0) break;          /* not completed yet */
		hif_reg_write(1, CE_STATUS, 0x1u | 0x1eu);  /* CE is awake now */
		if (nbytes > hif_shape[1].buf_sz) nbytes = hif_shape[1].buf_sz;
		d->nbytes = 0;                   /* claim before reposting */
		/* Every CE buffer starts with the 8-byte HTC header; the
		 * htt_resp (and therefore the message type) follows it.  Reading
		 * b[0] here returns the endpoint id, not the message type. */
		if (g_htt_rx < 64) {
			/* htt.h: struct htt_resp_hdr is one byte and
			 * struct htt_ver_resp is {minor, major, rsvd0}, so
			 * VERSION_CONF carries the target's HTT version at
			 * b[9]/b[10] - the value ath10k_htt_verify_version()
			 * insists must be major 3. */
			HIF_LOG("htt rx #%u len=%u eid=%u type=%u ver=%u.%u",
			    g_htt_rx, nbytes, nbytes ? b[0] : 0,
			    nbytes > HTC_HDR_BYTES ? b[HTC_HDR_BYTES] : 0xff,
			    nbytes > HTC_HDR_BYTES + 1 ? b[HTC_HDR_BYTES + 1] : 0xff,
			    nbytes > HTC_HDR_BYTES + 2 ? b[HTC_HDR_BYTES + 2] : 0xff);
			/* Every T2H message, including the 32-byte type=24 that
			 * chain-570 could only count.  Its payload tells us
			 * whether the target is announcing a peer map in V2
			 * layout or something else entirely. */
			if (g_htt_rx < 12 && nbytes > HTC_HDR_BYTES)
				hif_dump_hex("HTT t2h", b + HTC_HDR_BYTES,
				    (nbytes - HTC_HDR_BYTES) > 32 ?
				    32 : (nbytes - HTC_HDR_BYTES));
			g_htt_rx++;
		}
		/* wlan-wip wj_htt_message(): the whole HTT T2H dispatch.  The
		 * 8-byte HTC header precedes the message type byte.  RX_IND(1)
		 * and RX_IN_ORD(0x12) are the data path the firmware starts
		 * using right after vdev up; never draining it is what killed
		 * the target in chain-535/536. */
		if (g_join >= 2 && nbytes > HTC_HDR_BYTES) {
			const uint8_t *msg = b + HTC_HDR_BYTES;
			uint32_t msglen = nbytes - HTC_HDR_BYTES;

			switch (msg[0]) {
			case HTT_T2H_MSG_TYPE_PEER_MAP:
			case HTT_T2H_MSG_TYPE_PEER_UNMAP:
				hif_join_peer_event(msg, msglen);
				break;
			case 1:
				hif_wr_rx_ind(msg, msglen);
				break;
			case 0x12:
				hif_wr_in_ord(msg, msglen);
				break;
			case HTT_T2H_MSG_TYPE_TX_COMPL:
				hif_wpa_tx_compl(msg, msglen);
				break;
			default:
				break;
			}
		}
		/* htt.c ath10k_htt_setup() waits for VERSION_CONF before it
		 * sends the RX ring and the aggregation limits.  For the very
		 * first bring-up the message type is not yet trusted - 492
		 * showed a single 12-byte T2H reply whose type byte has never
		 * been read correctly - so any completed htt_resp counts as the
		 * go-ahead and the type is logged for the record. */
		if (g_htt_ring && !g_htt_ring_done && nbytes > HTC_HDR_BYTES) {
			HIF_LOG("HTT: first T2H reply type=%u, continuing htt_setup",
			    b[HTC_HDR_BYTES]);
			hif_htt_setup_finish();
		}
		{
			uint32_t slot = g_post1 & (n - 1);
			struct wcn_ce_desc_64 *w = &g_dr_desc[1][slot];

			w->addr = (g_buf_pa[1] +
			    (uint64_t)slot * hif_shape[1].buf_sz) & CE_DESC_ADDR_MASK;
			w->nbytes = 0;
			w->flags = 0;
			w->toeplitz = 0;
			hif_mb();
			g_post1 = (slot + 1) & (n - 1);
			*(volatile uint32_t *)(uintptr_t)(g_ce_va + CE1_SHADOW_DST_WR) = g_post1;
			hif_mb();
		}
		g_sw1 = (g_sw1 + 1) & (n - 1);
	}
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
#define HTC_SVC_HTT_DATA_MSG          0x0300  /* htc.h: SVC(3, 0) */
#define HTC_CONN_DISABLE_CREDIT_FLOW  0x0008  /* ATH10K_HTC_CONN_FLAGS_... */
#define HTC_EP0                       0

/* wmi.h: WMI_CMD_GRP(grp) = (grp << 12) | 1, WMI_GRP_START = 3 */
#define WMI_INIT_CMDID                0x0001
#define WMI_SERVICE_READY_EVENTID     0x0001
#define WMI_READY_EVENTID             0x0002
#define WMI_START_SCAN_CMDID          0x3001   /* WMI_GRP_SCAN  = 3 */
#define WMI_VDEV_CREATE_CMDID         0x5001   /* wmi.h: WMI_CMD_GRP(WMI_GRP_VDEV) */
#define WMI_PDEV_SET_BASE_MACADDR_CMDID 0x400d /* WMI_TLV_PDEV_SET_BASE_MACADDR_CMDID */
#define WMI_PDEV_SET_REGDOMAIN_CMDID  0x4001   /* WMI_CMD_GRP(WMI_GRP_PDEV) */
#define WMI_SCAN_CHAN_LIST_CMDID      0x3003   /* WMI_TLV_SCAN_CHAN_LIST_CMDID */
#define WMI_VDEV_UP_CMDID             0x5005   /* WMI_TLV_VDEV_UP_CMDID */
#define WMI_VDEV_START_CMDID          0x5003   /* WMI_TLV_VDEV_START_CMDID */
#define WMI_SCAN_EVENTID              0x3001
#define WMI_MGMT_RX_EVENTID           0x7001   /* WMI_GRP_MGMT  = 7 */
#define WMI_CHAN_INFO_EVENTID         0x4002   /* WMI_GRP_PDEV  = 4 */
/* join sequence (grafted from wlan-wip drivers/NativeWlan/WlanRadio.c).
 * Command/event ids per wmi-master.h: WMI_CMD_GRP(grp) = (grp << 12) | 1.
 * PEER group base 0x6001 (PEER_CREATE), MGMT group base 0x7001 (BCN_TX):
 *   PEER_SET_PARAM  = 0x6001 + 3, PEER_ASSOC = 0x6001 + 4,
 *   MGMT_TX_SEND    = 0x7001 + 7 (wmi-tlv.c .mgmt_tx_send_cmdid, the
 *                    by-reference variant ath10k_wmi_tlv_op_gen_mgmt_tx_send
 *                    builds; plain MGMT_TX would be 0x7006),
 *   MGMT_TX_COMPL   = 0x7001 + 5 (wmi-tlv.h event enum: MGMT_RX 0x7001,
 *                    HOST_SWBA 0x7002, TBTTOFFSET 0x7003, two offload
 *                    statuses, then MGMT_TX_COMPLETION 0x7006). */
#define WMI_PEER_SET_PARAM_CMDID      0x6004
#define WMI_PEER_ASSOC_CMDID          0x6005
#define WMI_MGMT_TX_SEND_CMDID        0x7008
#define WMI_MGMT_TX_COMPL_EVENTID     0x7006
/* wmi-tlv.h enum wmi_tlv_tag (verified against the Linux enum): */
#define WMI_TLV_PEER_SET_PARAM_CMD    100   /* WMI_TLV_TAG_STRUCT_PEER_SET_PARAM_CMD */
#define WMI_TLV_PEER_ASSOC_COMPLETE_CMD        101   /* WMI_TLV_TAG_STRUCT_PEER_ASSOC_COMPLETE_CMD */
#define WMI_TLV_VHT_RATE_SET          102   /* WMI_TLV_TAG_STRUCT_VHT_RATE_SET */
#define WMI_TLV_MGMT_TX_CMD           422   /* WMI_TLV_TAG_STRUCT_MGMT_TX_CMD */
#define WMI_TLV_MGMT_TX_COMPL_EVENT   423   /* WMI_TLV_TAG_STRUCT_MGMT_TX_COMPL_EVENT */
/* The two join prerequisites that chain-532c proved were missing.  Values
 * are taken from the same-chip wlan-wip reference (wj_send_peer_create uses
 * tag 0x61, wj_wmi_event parses VDEV_START_RESP with tag 0x28). */
#define WMI_PEER_CREATE_CMDID         0x6001
#define WMI_VDEV_START_RESP_EVENTID   0x5001
#define WMI_TLV_PEER_CREATE_CMD        97   /* 0x61, struct wmi_tlv_peer_create_cmd */
#define WMI_TLV_VDEV_START_RESP        40   /* 0x28, struct wmi_tlv_vdev_start_response_event */
/* The wj_begin() once-block commands (wlan-wip hard-codes the same tags). */
#define WMI_VDEV_SET_PARAM_CMDID       0x5008
#define WMI_PDEV_SET_PARAM_CMDID       0x4003
#define WMI_STA_POWERSAVE_PARAM_CMDID  0x9002
#define WMI_STA_POWERSAVE_MODE_CMDID   0x9001
#define WMI_TLV_VDEV_SET_PARAM         95   /* 0x5f */
#define WMI_TLV_PDEV_SET_PARAM         82   /* 0x52 */
#define WMI_TLV_STA_POWERSAVE_PARAM   109   /* 0x6d */
#define WMI_TLV_STA_POWERSAVE_MODE    108   /* 0x6c */

/* wmi-tlv.h: enum wmi_tlv_tag (values computed from the enum order) */
/* wmi-tlv.h enum wmi_tlv_tag:
 *   WMI_TLV_TAG_LAST_RESERVED      = 15
 *   WMI_TLV_TAG_FIRST_ARRAY_ENUM   = 16   <- not a usable tag
 *   WMI_TLV_TAG_ARRAY_UINT32       = 17
 *   WMI_TLV_TAG_ARRAY_BYTE         = 18
 *   WMI_TLV_TAG_ARRAY_STRUCT       = 19
 *   WMI_TLV_TAG_ARRAY_FIXED_STRUCT = 20
 *
 * These four were all one too low (16..19), i.e. every array TLV the driver
 * ever emitted carried the wrong tag: the frequency list of START_SCAN was
 * labelled FIRST_ARRAY_ENUM, its SSID/BSSID arrays were labelled
 * ARRAY_STRUCT, and SCAN_CHAN_LIST's channel array too.  Single-struct
 * commands (WMI_INIT up to RX_RING_CFG) all worked because they contain no
 * arrays at all, and a zero-length array is silently ignored - which is why
 * the bug stayed hidden until the first command that really carries one. */
/* ath10k wmi-tlv.h:586-592: the array tags start at 16, one BELOW where
 * this driver had them.  The 17/18/19/20 values are the ath11k numbering -
 * a 13/13 tag check had blessed them against the wrong reference tree - so
 * every array TLV we sent was off by one.  The firmware's own verdict
 * agrees with ath10k: a 39-channel START_SCAN array with tag 16 ran the
 * whole capture (504 sh4) while tag 17 died (sh3), and INIT's empty array
 * booted with 18 (501 ia2) and died with 19 (500). */
#define WMI_TLV_ARRAY_UINT32          16
#define WMI_TLV_ARRAY_BYTE            17
#define WMI_TLV_ARRAY_STRUCT          18
#define WMI_TLV_ARRAY_FIXED_STRUCT    19
#define WMI_TLV_MGMT_RX_HDR           44
#define WMI_TLV_INIT_CMD              74
#define WMI_TLV_RESOURCE_CONFIG       75
#define WMI_TLV_START_SCAN_CMD        77
#define WMI_TLV_PDEV_SET_REGDOMAIN_CMD 81
#define WMI_TLV_PDEV_SET_PARAM_CMD     82   /* 0x52, 12-byte body */
#define WMI_PDEV_SET_PARAM_CMDID       0x4003
#define WMI_TLV_SCAN_CHAN_LIST_CMD    79
#define WMI_TLV_CHANNEL               80
#define WMI_TLV_VDEV_CREATE_CMD       86      /* wmi-tlv.h enum wmi_tlv_tag */
#define WMI_TLV_VDEV_UP_CMD           92
#define WMI_TLV_VDEV_START_CMD        88      /* STRUCT_VDEV_START_REQUEST_CMD */
#define WMI_TLV_PDEV_SET_BASE_MACADDR_CMD 172 /* enum wmi_tlv_tag, master */
/* wmi-tlv.c: WMI_TLV_ABI_VER0 / VER1 / VER_NS0 */
#define WMI_TLV_ABI_VER0              0x01000000u
#define WMI_TLV_ABI_VER1              53u
#define WMI_TLV_ABI_VER_NS0           0x5f414351u
#define WMI_TLV_ABI_VER_NS1           0x00004c4du   /* wmi-tlv.c: "ML" */
/* wmi.h: WMI_SCAN_FILTER_PROBE_REQ */
#define WMI_SCAN_FILTER_PROBE_REQ     0x20
#define WMI_SCAN_CHAN_STAT_EVENT      0x10
#define WMI_SCAN_FLAG_PASSIVE         0x01
#define WMI_HOST_SCAN_REQ_ID_PREFIX   0xa000u
#define WMI_HOST_SCAN_REQUESTOR_PREFIX 0xa000u

/* HTT constants (HTC_HDR_BYTES, HTT_HDR_BYTES, the H2T/T2H message types and
 * the RX ring sizes) live up with CE_BUF_BYTES so the DMA block can use them. */

/* htt_rx.c ath10k_htt_rx_ring_fill_n() / ath10k_htt_rx_msdu_buff_replenish():
 * the target writes the index of the next descriptor it will use into the
 * shadow word handed over in RX_RING_CFG; everything between the host read
 * index and that value is a received MPDU.  The buffers themselves never move,
 * so draining is only a matter of walking the window and moving the index -
 * but it must happen, otherwise the ring wraps and the target stalls. */
static uint32_t g_htt_ring_rd;
static uint32_t g_htt_ring_seen;
static uint32_t g_htt_ring_done_cnt;   /* MPDUs actually consumed from the ring */
static uint32_t g_wmi_peer_events;     /* any 0x600x WMI event seen */
/* dpfix bit0: consume MSDUs from the HTT index page (the target may deliver
 * frames without an RX_IND/RX_IN_ORD message on CE1); bit1: post only
 * fw_idx_init_val buffers, exactly like trogdor ws_htt_prepare();
 * bit2: attention-driven drain (see hif_service_htt_ring).
 *
 * chain-570 finding: g_dma->htt_alloc_idx is the HOST refill pointer, not
 * target progress.  Linux htt_rx.c:151 reads it as "next slot to refill" and
 * :209 writes it back after refilling; fw_idx_shadow_reg_paddr (htt_tx.c:940)
 * is simply that word's physical address.  The old loop used it as the target
 * write index, so the first call walked rd 0..1022 (printing eight empty
 * buffers) and then rd == fw_idx held forever.  That is why every "zero
 * datapath" capture showed htt ring #0..7 attn=0: those were empty buffers
 * scanned BEFORE vdev up, not evidence that the target wrote nothing.
 *
 * chain-571 proved bit2 (attention-driven drain) is a real fix but not the
 * cause of the zero datapath; chain-572/573 both ran with bit0+bit2=5 and
 * received peer maps, RX_IN_ORD and EAPOL frames.  Default is now 5;
 * dpfix=0 remains the old-behaviour control. */
static int g_dp_fix = 5;
/* authz=1: send WMI PEER_AUTHORIZE(3,1) for a SECURE AP right after
 * VDEV_UP.  Linux mac.c and the working WCN7860 port both expose a
 * PEER_AUTHORIZE path; trogdor sends it only after the keys are in. */
static int g_authz;

static void
hif_service_htt_ring(void)
{
	uint32_t budget = HTT_RX_RING_ENTRIES;

	if (!(g_htt_ring & 1) || !g_htt_ring_done || g_dma == NULL) return;

	/* dpfix bit2: attention-driven drain.  The target marks a filled
	 * buffer by setting rx_attention bit31; the host stops at the first
	 * slot that is not done.  No target register is read and no shared
	 * index word is used as a progress boundary. */
	if (g_dp_fix & 0x4) {
		while (budget-- > 0) {
			uint8_t *b = g_dma->htt_buf[g_htt_ring_rd];
			uint32_t attention, start, len;

			memcpy(&attention, b + 4, 4);
			if (!(attention & (1u << 31))) break;   /* ring caught up */
			memcpy(&start, b + 28, 4);
			len = start & 0x3fffu;
			if (g_htt_ring_seen < 16)
				HIF_LOG("htt dp #%u idx=%u attn=%08x len=%u "
				    "hdr0=%02x%02x", g_htt_ring_seen,
				    g_htt_ring_rd, attention, len,
				    b[297], b[296]);
			g_htt_ring_seen++;
			if ((g_dp_fix & 0x1) && len >= 14u &&
			    len <= HTT_RX_BUF_SIZE - 360u)
				hif_wr_msdu(b, len);
			g_htt_ring_done_cnt++;
			/* Static buffers never move: clear attention so the
			 * target can reuse the slot.  Do not repost here; the
			 * index word is the host's own refill pointer. */
			*(volatile uint32_t *)(uintptr_t)(b + 4) = 0;
			hif_mb();
			g_htt_ring_rd = (g_htt_ring_rd + 1) &
			    (HTT_RX_RING_ENTRIES - 1);
		}
		return;
	}

	/* Original (chain-567) behaviour, kept as the single-variable control:
	 * walk from rd up to htt_alloc_idx once, then never enter again. */
	{
		uint32_t fw_idx = g_dma->htt_alloc_idx &
		    (HTT_RX_RING_ENTRIES - 1);

		budget = HTT_RX_RING_ENTRIES;
		while (g_htt_ring_rd != fw_idx && budget-- > 0) {
			uint8_t *b = g_dma->htt_buf[g_htt_ring_rd];
			uint32_t attention, start;

			memcpy(&attention, b + 4, 4);
			memcpy(&start, b + 28, 4);
			if (g_htt_ring_seen < 8)
				HIF_LOG("htt ring #%u idx=%u attn=%02x%02x%02x%02x hdr0=%02x%02x",
				    g_htt_ring_seen, g_htt_ring_rd,
				    b[7], b[6], b[5], b[4], b[297], b[296]);
			g_htt_ring_seen++;
			if ((g_dp_fix & 0x1) && (attention & (1u << 31))) {
				uint32_t l = start & 0x3fffu;

				if (l >= 14u &&
				    l <= HTT_RX_BUF_SIZE - 360u)
					hif_wr_msdu(b, l);
				*(volatile uint32_t *)(uintptr_t)(b + 4) = 0;
			}
			g_htt_ring_rd = (g_htt_ring_rd + 1) &
			    (HTT_RX_RING_ENTRIES - 1);
		}
	}
}

enum hif_state {
	HIF_OFF = 0,
	HIF_CE_UP,          /* rings programmed, waiting for HTC READY */
	HIF_CONNECT_SENT,   /* CONNECT_SERVICE sent, waiting for its response */
	HIF_SETUP_SENT,     /* SETUP_COMPLETE sent, waiting for WMI service ready */
	HIF_INIT_SENT,      /* WMI_INIT sent, waiting for WMI_READY */
	HIF_READY
};

static enum hif_state g_state = HIF_OFF;
static bool g_init_sent;
static uint8_t g_wmi_eid = 1;
static uint8_t g_tx_seq;
static uint16_t g_credit_count, g_credit_size;
static uint32_t g_polls;
static uint32_t g_rx_packets;
static bool g_scan_active;
static uint32_t g_scan_polls;
static uint32_t g_smp2p_last;
static int g_smp2p_seen;
static uint32_t g_beacons;
static uint32_t g_mgmt_seen;
/* Every WMI_MGMT_RX_EVENTID (0x7001) that reaches the dispatcher, counted
 * BEFORE any parsing.  The 526-d boot scanned 39 channels and reported 32
 * BSS_CHANNEL events but zero beacons; without this counter we cannot tell
 * "firmware never sent mgmt frames" from "our parser dropped them". */
static uint32_t g_mgmt_events;
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
	/* idx in hif_ce_send() is g_wr3, so use the same index here: no two
	 * in-flight CE3 descriptors can ever point at the same bytes. */
	uint8_t *b = g_tx3_rotate ? s_tx3_slot(g_wr3) : s_tx3;
	uint32_t total = HTC_HDR_BYTES + 4 + len;

	if (total > CE_BUF_BYTES) return -1;
	memset(b, 0, total);
	b[0] = g_wmi_eid;
	/* htc.c: ath10k_htc_tx_complete_skb()/ath10k_htc_build_tx_skb sets
	 * NEED_CREDIT_UPDATE on every message of an endpoint whose transmit
	 * credit flow control is on (WMI is the only such service), and stamps
	 * a per-endpoint sequence number in control_byte1.  Without the flag
	 * the target has no reason to hand credits back in a trailer and the
	 * pipe dries up after the initial READY allocation. */
	b[1] = 0x01;                 /* ATH10K_HTC_FLAG_NEED_CREDIT_UPDATE */
	hif_put16(b + 2, (uint16_t)(4 + len));
	b[5] = g_tx_seq++;
	hif_put32(b + 8, cmd_id);
	if (len && tlvs) memcpy(b + 12, tlvs, len);

	return hif_ce_send(3, b, total);
}

/* HTC frame carrying an htt_cmd on the HTT endpoint (CE4).  htc.c wraps every
 * endpoint message in the same 8-byte htc_hdr: eid, flags, payload length. */
static int
hif_htt_send(uint8_t msg_type, const uint8_t *payload, uint32_t len)
{
	uint8_t *b = g_dma->tx4[g_wr4 & 3];
	uint32_t total = HTC_HDR_BYTES + HTT_HDR_BYTES + len;

	if (g_htt_eid == 0) return -1;   /* HTT service was never connected */
	if (total > 256) return -1;      /* CE4 src_sz_max */
	memset(b, 0, total);
	b[0] = g_htt_eid;
	/* htc.c: HTT data service runs with credit flow control disabled, so no
	 * NEED_CREDIT_UPDATE flag.  htc.h puts seq_no in a single byte at
	 * offset 5 (union with control_byte1) and ath10k_htc_prepare_tx_skb()
	 * increments it for every send regardless of credit flow, so do the
	 * same here - the control endpoint already does (g_tx_seq). */
	b[1] = 0;
	hif_put16(b + 2, (uint16_t)(HTT_HDR_BYTES + len));
	b[5] = g_htt_seq++;
	b[8] = msg_type;
	if (len && payload) memcpy(b + HTC_HDR_BYTES + HTT_HDR_BYTES, payload, len);
	return hif_ce_send(4, b, total);
}

/* htt_tx.c ath10k_htt_h2t_ver_req_msg(): hdr + 3 pad bytes. */
static int
hif_htt_version_req(void)
{
	/* htt.h: struct htt_ver_req is pad[sizeof(u32) - sizeof(htt_cmd_hdr)],
	 * i.e. exactly three zero bytes, so the whole message is 4 bytes. */
	static const uint8_t pad[3] = { 0, 0, 0 };

	HIF_LOG("HTT: send VERSION_REQ (eid=%u)", g_htt_eid);
	return hif_htt_send(HTT_H2T_MSG_TYPE_VERSION_REQ, pad, sizeof(pad));
}

/* htt_tx.c ath10k_htt_send_rx_ring_cfg_64().  This is the message that gives
 * the target somewhere to put received frames; until it is sent the firmware
 * has a null RX ring.  The ten offsets are
 * offsetof(struct htt_rx_desc_v2, member) / 4 for the wcn3990 descriptor
 * (rx_desc.h), computed offline by work/rxdesc-offsets.py. */
static const uint16_t g_htt_rx_offsets[10] = {
	74,  /* mac80211_hdr_offset  = rx_hdr_status  */
	90,  /* msdu_payload_offset  = msdu_payload  */
	27,  /* ppdu_start_offset                    */
	37,  /* ppdu_end_offset                      */
	4,   /* mpdu_start_offset                    */
	26,  /* mpdu_end_offset                      */
	7,   /* msdu_start_offset                    */
	12,  /* msdu_end_offset                      */
	1,   /* rx_attention_offset                  */
	2,   /* frag_info_offset                     */
};

static int
hif_htt_rx_ring_cfg(void)
{
	uint8_t p[3 + 44];
	uint64_t ring_pa, idx_pa;
	uint32_t i, fill;

	if (!g_dma) return -1;
	ring_pa = hif_pa_of(g_dma->htt_ring);
	idx_pa = hif_pa_of(&g_dma->htt_alloc_idx);
	if (!ring_pa || !idx_pa) return -1;

	/* Verified on device (chain-531): posting buffers is REQUIRED.  With
	 * fw_idx_init_val = 0 the firmware never delivers a single frame
	 * (mgmtev=0 / httidx=0); with 1023 posted it delivers WMI_MGMT_RX
	 * beacons with real SSIDs within seconds.  Linux htt_rx.c fills
	 * [0, fill_level) and leaves the rest zero; we keep every slot
	 * addressable (the buffers are static, so a target that walks past
	 * fill_level still finds valid memory). */
	fill = HTT_RX_FILL_LEVEL;
	/* trogdor posts exactly WS_RX_FILL addresses; our old loop filled the
	 * whole ring while still telling the target fw_idx_init_val = 1023. */
	{
		uint32_t nfill = (g_dp_fix & 0x2) ? fill : HTT_RX_RING_ENTRIES - 1u;
		for (i = 0; i < nfill; i++) {
			g_dma->htt_ring[i] = hif_pa_of(g_dma->htt_buf[i]);
		}
	}
	g_htt_buf_pa = hif_pa_of(g_dma->htt_buf[0]);
	g_htt_alloc = fill;
	g_dma->htt_alloc_idx = fill;
	hif_mb();

	memset(p, 0, sizeof(p));
	p[0] = 1;                              /* num_rings */
	/* p[1..2] = rsvd0 */
	hif_put64(p + 3, idx_pa);              /* fw_idx_shadow_reg_paddr */
	hif_put64(p + 11, ring_pa);            /* rx_ring_base_paddr */
	hif_put16(p + 19, HTT_RX_RING_ENTRIES);/* rx_ring_len (entries) */
	hif_put16(p + 21, HTT_RX_BUF_SIZE);    /* rx_ring_bufsize */
	hif_put16(p + 23, 0xffff);             /* every report type */
	hif_put16(p + 25, (uint16_t)fill);     /* fw_idx_init_val = posted count */
	for (i = 0; i < 10; i++) hif_put16(p + 27 + i * 2, g_htt_rx_offsets[i]);

	HIF_LOG("HTT: send RX_RING_CFG ring=%llx idx=%llx n=%u buf=%u posted=%u",
	    (unsigned long long)ring_pa, (unsigned long long)idx_pa,
	    HTT_RX_RING_ENTRIES, HTT_RX_BUF_SIZE, fill);
	return hif_htt_send(HTT_H2T_MSG_TYPE_RX_RING_CFG, p, (uint32_t)sizeof(p));
}

/* htt_tx.c ath10k_htt_h2t_aggr_cfg_msg_v2(): 3 bytes, no padding. */
static int
hif_htt_aggr_cfg(void)
{
	uint8_t p[3];

	p[0] = 64;   /* max_num_ampdu_subframes */
	p[1] = 6;    /* max_num_amsdu_subframes (0x1f mask) */
	p[2] = 0;
	HIF_LOG("HTT: send AGGR_CFG ampdu=%u amsdu=%u", p[0], p[1]);
	return hif_htt_send(HTT_H2T_MSG_TYPE_AGGR_CFG, p, (uint32_t)sizeof(p));
}

/* wlan-wip ws_htt_next() step 0 / htt_tx.c frag desc bank cfg, 64-bit
 * addresses.  The message is one htt header byte plus the packed 63-byte
 * struct htt_frag_desc_bank_cfg64:
 *   info(1) num_banks(1) desc_size(1) bank_base_addrs[4](32)
 *   bank_id[4](16, {le16 min, le16 max}) q_state(12, record_size @+8)
 * trogdor sends exactly these 64 bytes with info=0 (no Q-state validity),
 * one bank, 72-byte descriptors and bank_max_id = 1055. */
#define HTT_FRAG_DESC_COUNT 1056
#define HTT_FRAG_DESC_SIZE  72
static int
hif_htt_frag_bank_cfg(void)
{
	uint8_t p[63];
	uint64_t bank_pa;

	if (!g_dma) return -1;
	bank_pa = hif_pa_of(g_dma->htt_frag);
	if (!bank_pa) return -1;
	memset(p, 0, sizeof(p));
	p[0] = 0;                        /* info: pdev 0, q_state not valid */
	p[1] = 1;                        /* num_banks */
	p[2] = HTT_FRAG_DESC_SIZE;       /* desc_size */
	hif_put64(p + 3, bank_pa);       /* bank_base_addrs[0] */
	hif_put16(p + 35, 0);            /* bank_id[0].bank_min_id */
	hif_put16(p + 37, HTT_FRAG_DESC_COUNT - 1); /* bank_max_id */
	p[59] = 1;                       /* q_state.record_size */
	HIF_LOG("HTT: send FRAG_DESC_BANK_CFG bank=%llx n=%u desc=%u",
	    (unsigned long long)bank_pa, HTT_FRAG_DESC_COUNT, HTT_FRAG_DESC_SIZE);
	return hif_htt_send(HTT_H2T_MSG_TYPE_FRAG_DESC_BANK_CFG, p,
	    (uint32_t)sizeof(p));
}

/* htt.c ath10k_htt_setup() after VERSION_CONF: frag-desc bank (TX only, not
 * needed for a scan), RX ring, aggregation limits. */
static void
hif_htt_setup_finish(void)
{
	if (g_htt_ring_done) return;
	if (!g_htt_ring) {
		HIF_LOG("HTT: VERSION_CONF seen; httring=0 so htt_setup is skipped");
		return;
	}
	g_htt_ring_done = 1;
	/* bit3 = 8: the "second message" control.  RX_RING_CFG (bit0) and
	 * AGGR_CFG (bit1) each kill the target on their own, so the content
	 * may not be what it hates - resending VERSION_REQ, which the target
	 * already accepted once, tells the two apart. */
	if (g_htt_ring & 8) {
		HIF_LOG("HTT: resend VERSION_REQ as the second H2T message");
		(void)hif_htt_version_req();
		return;
	}
	/* bit4/bit5 separate "the target rejects this message type" from "the
	 * target rejects this payload": bit4 sends an empty RX_RING_CFG, bit5
	 * sends a type the protocol does not define at all. */
	if (g_htt_ring & 16) {
		HIF_LOG("HTT: probe RX_RING_CFG with an empty payload");
		(void)hif_htt_send(HTT_H2T_MSG_TYPE_RX_RING_CFG, NULL, 0);
		return;
	}
	if (g_htt_ring & 32) {
		HIF_LOG("HTT: probe undefined message type 99");
		(void)hif_htt_send(99, NULL, 0);
		return;
	}
	/* bit0 = RX_RING_CFG, bit1 = AGGR_CFG, bit2 = FRAG_DESC_BANK_CFG.
	 * trogdor's order is frag bank -> rx ring -> aggr, so honour it here.
	 * bit2 exists because VDEV_UP (which enables the TX data path) kills
	 * the target when no frag bank was ever registered (chain-543). */
	if (g_htt_ring & 4) (void)hif_htt_frag_bank_cfg();
	if (g_htt_ring & 1) (void)hif_htt_rx_ring_cfg();
	if (g_htt_ring & 2) (void)hif_htt_aggr_cfg();
}

/* wmi-tlv.h: struct wmi_tlv { __le16 len; __le16 tag; u8 value[]; }.
 * wmi-tlv.c ath10k_wmi_tlv_iter() advances by *exactly* tlv_len; there is
 * no roundup in either the host iterator or the command builders.  Rounding
 * up injected two zero bytes after each 18/6-byte TLV (VDEV_CREATE, scan
 * bssid, base MAC), which the target parses as a truncated extra TLV. */
static uint8_t *
hif_put_tlv(uint8_t *p, uint16_t tag, uint16_t len, const void *value)
{
	hif_put16(p, len);
	hif_put16(p + 2, tag);
	if (len && value) memcpy(p + 4, value, len);
	if (g_tlv_pad) return p + 4 + ((len + 3) & ~3u);   /* legacy bug repro */
	return p + 4 + len;
}

static int
hif_send_connect_service(uint16_t svc)
{
	struct {
		uint16_t service_id;
		uint16_t flags;
		uint8_t pad[2];
	} __attribute__((packed)) req;

	req.service_id = svc;
	/* htc.c: ath10k_htc_get_credit_allocation() hands the WMI control
	 * service every credit advertised in HTC READY and passes the count in
	 * ATH10K_HTC_CONN_FLAGS_RECV_ALLOC (bits 8-15).  Only WMI control uses
	 * credit flow control; every other service gets DISABLE_CREDIT_FLOW_CTRL
	 * and no receive allocation. */
	if (svc == HTC_SVC_WMI_CONTROL) {
		req.flags = (uint16_t)(((g_credit_count ? g_credit_count : 1) & 0xffu) << 8);
	} else {
		req.flags = HTC_CONN_DISABLE_CREDIT_FLOW;
	}
	req.pad[0] = req.pad[1] = 0;

	HIF_LOG("HTC: send CONNECT_SERVICE svc=0x%04x flags=0x%04x", svc, req.flags);
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
/* wmi-tlv.c ath10k_wmi_tlv_op_gen_init() for ATH10K_FW_WMI_OP_VERSION_TLV
 * (WCN3990), with hw.h TARGET_TLV_* values.  Index map is the field order of
 * struct wmi_tlv_resource_config (44 __le32).  The legacy config below left
 * 32 of these at zero - including the RX reorder timeouts, max_frag_entries,
 * rx_decap_mode (comment said native wifi, value was 0 = RAW), num_msdu_desc
 * (128 instead of 1056) and host_capab.  The firmware only touches those
 * pools when a vdev is created or a scan starts, which is exactly when this
 * port takes an ARM64 SError with clean SMMU fault registers. */
static const uint32_t g_wmi_cfg_full[44] = {
	4,                        /* 0  num_vdevs   TARGET_TLV_NUM_VDEVS */
	33,                       /* 1  num_peers   TARGET_TLV_NUM_PEERS */
	0, 0,                     /* 2  num_offload_peers / reorder bufs */
	2,                        /* 4  num_peer_keys */
	66,                       /* 5  num_tids   2 * TARGET_TLV_NUM_PEERS */
	0,                        /* 6  ast_skid_limit */
	7, 7,                     /* 7  tx_chain_mask / 8 rx_chain_mask */
	0x64, 0x64, 0x64, 0x28,   /* 9..12 rx_timeout_pri[4] */
	1,                        /* 13 rx_decap_mode ATH10K_HW_TXRX_NATIVE_WIFI */
	4,                        /* 14 scan_max_pending_reqs */
	4, 4,                     /* 15 bmiss / 16 roam offload max vdev */
	8,                        /* 17 roam_offload_max_ap_profiles */
	0, 0, 0,                  /* 18..20 mcast */
	0x400,                    /* 21 tx_dbg_log_size */
	0,                        /* 22 num_wds_entries */
	0, 0, 0, 0,               /* 23..26 dma/mac/defrag/vow */
	2,                        /* 27 gtk_offload_max_vdev */
	1056,                     /* 28 num_msdu_desc TARGET_TLV_NUM_MSDU_DESC */
	2,                        /* 29 max_frag_entries */
	1,                        /* 30 num_tdls_vdevs */
	0x20,                     /* 31 num_tdls_conn_table_entries */
	2,                        /* 32 beacon_tx_offload_max_vdev */
	5,                        /* 33 num_multicast_filter_entries */
	22,                       /* 34 num_wow_filters */
	6,                        /* 35 num_keep_alive_pattern */
	0,                        /* 36 keep_alive_pattern_size */
	1, 1,                     /* 37..38 tdls sleep/buffer sta */
	0,                        /* 39 wmi_send_separate */
	0, 0, 0,                  /* 40..42 ocb */
	0x200,                    /* 43 host_capab WMI_TLV_FLAG_MGMT_BUNDLE_TX_COMPL */
};

static int
hif_send_wmi_init(void)
{
	uint8_t tlv[512];
	uint8_t *p = tlv;
	struct {
		uint32_t abi[6];
		uint32_t num_host_mem_chunks;
	} __attribute__((packed)) init;
	/* wmi-tlv.h: struct wmi_tlv_resource_config is exactly 44 __le32 once
	 * rx_timeout_pri[4] is expanded.  Sending 43 made the TLV four bytes
	 * short of sizeof(struct), which the firmware can reject outright. */
	uint32_t cfg[44];
	uint32_t i;

	memset(&init, 0, sizeof(init));
	init.abi[0] = WMI_TLV_ABI_VER0;
	init.abi[1] = WMI_TLV_ABI_VER1;
	init.abi[2] = WMI_TLV_ABI_VER_NS0;
	init.abi[3] = WMI_TLV_ABI_VER_NS1;
	init.num_host_mem_chunks = 0;

	/* wmi-tlv.h: struct wmi_tlv_resource_config is 44 u32 once the
	 * rx_timeout_pri[4] array is counted.  Index map used below:
	 * 0 num_vdevs, 1 num_peers, 4 num_peer_keys, 5 num_tids,
	 * 7/8 tx/rx chain masks, 13 rx_decap_mode, 14 scan_max_pending_reqs,
	 * 22 num_wds_entries, 28 num_msdu_desc, 34 num_wow_filters,
	 * 39 wmi_send_separate, 43 host_capab. */
	for (i = 0; i < 44; i++) cfg[i] = 0;
	if (g_wmi_init_full) {
		memcpy(cfg, g_wmi_cfg_full, sizeof(cfg));
		/* num_msdu_desc should describe the host's RX ring, not Linux's
		 * 1056: this port only hands the target 128 ring entries. */
		if (g_msdu_desc) cfg[28] = (uint32_t)g_msdu_desc;
		/* initfix: per-field gates, see the declaration for the mapping.
		 * Every value is copied verbatim from wm_send_init()'s cfg[46]. */
		if (g_init_fix & 1) { cfg[2] = 4; cfg[3] = 4; }
		if (g_init_fix & 2) cfg[6] = 16;
		if (g_init_fix & 4) cfg[22] = 2;
		HIF_LOG("WMI: INIT full Linux cfg vdevs=%u peers=%u tids=%u "
		    "msdu=%u decap=%u max_frag=%u host_capab=0x%x "
		    "offload=%u reorder=%u ast=%u wds=%u initfix=%u",
		    cfg[0], cfg[1], cfg[5], cfg[28], cfg[13], cfg[29], cfg[43],
		    cfg[2], cfg[3], cfg[6], cfg[22], (unsigned)g_init_fix);
	} else {
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
	}

	p = hif_put_tlv(p, WMI_TLV_INIT_CMD, (uint16_t)sizeof(init), &init);
	p = hif_put_tlv(p, WMI_TLV_RESOURCE_CONFIG, (uint16_t)sizeof(cfg), cfg);
	/* Linux always terminates INIT with an empty ARRAY_STRUCT (tag 18;
	 * wmi-tlv.c gen_init: sizeof(*tlv) + chunks_len, chunks_len 0 when
	 * num_mem_chunks is 0).  This firmware wants exactly that: with 18 it
	 * boots (501 ia2), with 19 - the ath11k-shifted value this driver
	 * used to send - it dies right after INIT (500).  initarr=0 omits the
	 * TLV entirely, 1 sends the Linux tag, 2 sends 19 as a negative
	 * control - so one image can decide. */
	if (g_init_arr == 1) {
		p = hif_put_tlv(p, WMI_TLV_ARRAY_STRUCT, 0, NULL);
	} else if (g_init_arr == 2) {
		p = hif_put_tlv(p, 19, 0, NULL);
	}

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
/* wmi-tlv.c: ath10k_wmi_tlv_op_gen_vdev_create().  A START_SCAN names a
 * vdev_id, and the firmware rejects the scan if that vdev was never created -
 * which is why a scan attempt on a bare WMI session cannot work.  wmi.h:
 * struct wmi_vdev_create_cmd is 18 packed bytes, WMI_VDEV_TYPE_STA = 2,
 * WMI_VDEV_SUBTYPE_NONE = 0.  VDEV_CREATE has no response event. */
/* wmi-tlv.c: ath10k_wmi_pdev_set_regdomain() is part of the normal start
 * sequence.  Without a regdomain the firmware can refuse scan requests.  The
 * values are the world domain, which is what ath10k sends before the
 * regulatory core has pushed anything else.  There is no response event. */
static int
hif_send_pdev_regdomain(void)
{
	uint8_t tlv[40];
	uint8_t *p = tlv;
	/* Linux struct wmi_tlv_pdev_set_regdomain_cmd has SIX u32 words
	 * (regdmn, 2g, 5g, ctl2g, ctl5g, ...); trogdor emits a 24-byte TLV
	 * with the world SKU 0x6c from SERVICE_READY.  The old 5-word/20-byte
	 * all-zero command is kept as the A/B control. */
	uint32_t cmd[6];
	uint16_t cmdlen;

	memset(tlv, 0, sizeof(tlv));
	memset(cmd, 0, sizeof(cmd));
	if (g_scan_fix & SCANFIX_REGDOM) {
		cmd[1] = 0x6c;
		cmd[2] = 0x6c;
		cmd[3] = 0x6c;
		cmd[4] = 0xff;
		cmd[5] = 0xff;
		cmdlen = 24;
	} else {
		cmdlen = 20;
	}
	p = hif_put_tlv(p, WMI_TLV_PDEV_SET_REGDOMAIN_CMD, cmdlen, cmd);
	HIF_LOG("WMI: send PDEV_SET_REGDOMAIN (%u bytes, %s)",
	    cmdlen, (g_scan_fix & SCANFIX_REGDOM) ? "world 0x6c" : "zero legacy");
	return hif_wmi_command(WMI_PDEV_SET_REGDOMAIN_CMDID, tlv, (uint32_t)(p - tlv));
}

/* trogdor ws_next(): SEVEN PDEV_SET_PARAM commands before regdomain.
 * The first three (PMF_QOS, DYNAMIC_BW, IDLE_PS_CONFIG) were missing.
 * SCANFIX_PDEV7 gates them, so the old four-command sequence stays
 * available as a single-variable control on the same image. */
static int
hif_send_pdev_params(void)
{
	static const uint32_t param[7][2] = {
	    {0x22, 1},   /* PMF_QOS */
	    {0x0a, 1},   /* DYNAMIC_BW */
	    {0x2c, 1},   /* IDLE_PS_CONFIG: WCN3990 chip-level idle PS;
	                  * only safe with shadow doorbells + DDR RRI (IMPS) */
	    {0x01, 3}, {0x02, 3}, {0x23, 0}, {0x25, 1},
	};
	const uint32_t (*sel)[2] = param;
	unsigned i, n = 4;

	if (g_scan_fix & SCANFIX_PDEV7) {
		n = 7;
	} else {
		sel = param + 3;
	}
	for (i = 0; i < n; i++) {
		uint8_t tlv[32];
		uint8_t *q = tlv;
		uint32_t body[3];

		body[0] = 0;
		body[1] = sel[i][0];
		body[2] = sel[i][1];
		q = hif_put_tlv(q, WMI_TLV_PDEV_SET_PARAM_CMD, 12, body);
		(void)hif_wmi_command(WMI_PDEV_SET_PARAM_CMDID, tlv,
		    (uint32_t)(q - tlv));
	}
	HIF_LOG("WMI: send %u PDEV_SET_PARAM (trogdor %s)",
	    n, n == 7 ? "7-command" : "4-command");
	return 0;
}

/* wmi-tlv.c ath10k_wmi_tlv_op_gen_scan_chan_list() + mac.c
 * ath10k_update_channel_list(): the regulatory channel list is programmed
 * before WMI_PDEV_SET_REGDOMAIN and long before the first scan.  Each entry
 * is a nested 24-byte STRUCT_CHANNEL TLV (wmi.h struct wmi_channel) inside
 * an ARRAY_STRUCT.  chanlist=1 adds this Linux step. */
static int
hif_send_scan_chan_list(void)
{
	static uint8_t tlv[1200];
	static uint8_t arr[39 * 28];
	uint8_t *p = tlv, *q = arr;
	uint8_t nbuf[4];
	uint32_t i, n = (uint32_t)(sizeof(g_scan_freqs) / sizeof(g_scan_freqs[0]));

	if (g_scan_chans >= 0 && (uint32_t)g_scan_chans < n) n = (uint32_t)g_scan_chans;
	memset(tlv, 0, sizeof(tlv));
	memset(arr, 0, sizeof(arr));
	for (i = 0; i < n; i++) {
		uint8_t ch[24];
		uint32_t f = g_scan_freqs[i];

		memset(ch, 0, sizeof(ch));
		hif_put32(ch + 0, f);                  /* mhz */
		hif_put32(ch + 4, f);                  /* band_center_freq1 */
		hif_put32(ch + 8, 0);                  /* band_center_freq2 */
		if (g_scan_fix & SCANFIX_CHANV2) {
			/* coachz_wifi.c ws_next(): six u32 per channel.  The
			 * flags word carries 11G/11A, PASSIVE, DFS, ALLOW_HT
			 * and ALLOW_VHT; 5 GHz is passive (WCN3990 WLAN.HL.3.2
			 * active-scan firmware bug, linux-wireless 2026-03). */
			uint32_t flags = (f < 3000 ? 1u : 0u) |
			    (f > 2462 ? 1u << 7 : 0u) |
			    ((f >= 5260 && f <= 5720) ? 1u << 10 : 0u) |
			    (1u << 11) | (1u << 12);
			hif_put32(ch + 12, flags);
			hif_put32(ch + 16, (40u << 8) | (40u << 16));
			hif_put32(ch + 20, 6u | (40u << 8));
		} else {
			hif_put32(ch + 12, f < 3000 ? 1u : 0u); /* mode: 11G(1) / 11A(0) */
			ch[16] = 0;                        /* min_power */
			ch[17] = 40;                       /* max_power, 0.5 dBm */
			ch[18] = 40;                       /* reg_power, 0.5 dBm */
			ch[19] = 0;                        /* reg_classid */
			ch[20] = 0;                        /* antenna_max */
			ch[21] = 40;                       /* max_tx_power */
		}
		q = hif_put_tlv(q, WMI_TLV_CHANNEL, (uint16_t)sizeof(ch), ch);
	}
	hif_put32(nbuf, n);
	p = hif_put_tlv(p, WMI_TLV_SCAN_CHAN_LIST_CMD,
	    (uint16_t)sizeof(nbuf), nbuf);
	p = hif_put_tlv(p, WMI_TLV_ARRAY_STRUCT, (uint16_t)(q - arr), arr);
	HIF_LOG("WMI: send SCAN_CHAN_LIST (%u channels, %u TLV bytes)",
	    n, (unsigned)(p - tlv));
	return hif_wmi_command(WMI_SCAN_CHAN_LIST_CMDID, tlv,
	    (uint32_t)(p - tlv));
}

/* wmi-tlv.c ath10k_wmi_tlv_op_gen_vdev_start(): three TLVs - the request,
 * one STRUCT_CHANNEL, and an empty ARRAY_STRUCT for the nested p2p-noa
 * descriptor list.  mac.c ath10k_vdev_start_restart() gives the vdev a home
 * channel; without one the firmware's scan scheduler has no channel context.
 *
 * The request is `struct wmi_tlv_vdev_start_cmd` (wmi-tlv.h), 72 packed
 * bytes, in this order: vdev_id, requestor_id, bcn_intval, dtim_period,
 * flags, struct wmi_ssid (ssid_len + ssid[32]), bcn_tx_rate, bcn_tx_power,
 * num_noa_descr, disable_hw_ack.  The previous 64-byte layout here had no
 * requestor_id and put ssid_len after disable_hw_ack, so the firmware read
 * num_noa_descr out of the following TLV header (tag | len << 16) and walked
 * a NOA list that does not exist.  That is the reset seen right after
 * VDEV_START. */
struct hif_vdev_start_cmd {
	uint32_t vdev_id;
	uint32_t requestor_id;
	uint32_t bcn_intval;
	uint32_t dtim_period;
	uint32_t flags;
	uint32_t ssid_len;
	uint8_t ssid[32];
	uint32_t bcn_tx_rate;
	uint32_t bcn_tx_power;
	uint32_t num_noa_descr;
	uint32_t disable_hw_ack;
} __attribute__((packed));

/* wmi_tlv_vdev_start_cmd is 72 bytes; a mismatch is a build error. */
typedef char hif_vdev_start_size_check[
    sizeof(struct hif_vdev_start_cmd) == 72 ? 1 : -1];

static int
hif_send_vdev_start(void)
{
	uint8_t tlv[160];
	uint8_t *p = tlv;
	struct hif_vdev_start_cmd cmd;
	uint8_t ch[24];

	memset(tlv, 0, sizeof(tlv));
	memset(&cmd, 0, sizeof(cmd));
	memset(ch, 0, sizeof(ch));
	cmd.vdev_id = 0;
	cmd.bcn_intval = 100;
	cmd.dtim_period = 2;
	hif_put32(ch + 0, 2412);            /* mhz */
	hif_put32(ch + 4, 2412);            /* band_center_freq1 */
	hif_put32(ch + 12, 1);              /* mode = MODE_11G */
	ch[17] = 40;                        /* max_power, 0.5 dBm */
	ch[18] = 40;                        /* reg_power, 0.5 dBm */
	ch[21] = 40;                        /* max_tx_power */

	p = hif_put_tlv(p, WMI_TLV_VDEV_START_CMD, (uint16_t)sizeof(cmd), &cmd);
	p = hif_put_tlv(p, WMI_TLV_CHANNEL, (uint16_t)sizeof(ch), ch);
	p = hif_put_tlv(p, WMI_TLV_ARRAY_STRUCT, 0, NULL);
	HIF_LOG("WMI: send VDEV_START id=0 channel 2412 (MODE_11G)");
	return hif_wmi_command(WMI_VDEV_START_CMDID, tlv, (uint32_t)(p - tlv));
}

/* wmi-tlv.c ath10k_wmi_tlv_op_gen_vdev_up(): mac80211 brings the interface
 * up (ath10k_mac_op_start) before any scan.  The payload is 14 packed bytes:
 * vdev_id, assoc_id, bssid. */
static int
hif_send_vdev_up(void)
{
	uint8_t tlv[32];
	uint8_t *p = tlv;
	struct {
		uint32_t vdev_id;
		uint32_t vdev_assoc_id;
		uint8_t bssid[8];
	} __attribute__((packed)) cmd;
	uint16_t cmdlen = (g_scan_fix & SCANFIX_MAC) ? 16u : 14u;

	memset(tlv, 0, sizeof(tlv));
	memset(&cmd, 0, sizeof(cmd));
	cmd.bssid[0] = 0x02;
	cmd.bssid[5] = 0x01;
	p = hif_put_tlv(p, WMI_TLV_VDEV_UP_CMD, cmdlen, &cmd);
	HIF_LOG("WMI: send VDEV_UP id=0 (%u TLV bytes, cmd=%uB)",
	    (unsigned)(p - tlv), cmdlen);
	return hif_wmi_command(WMI_VDEV_UP_CMDID, tlv, (uint32_t)(p - tlv));
}

static int
hif_send_vdev_create(void)
{
	uint8_t tlv[64];
	uint8_t *p = tlv;
	struct {
		uint32_t vdev_id;
		uint32_t vdev_type;
		uint32_t vdev_subtype;
		uint8_t mac[8];
	} __attribute__((packed)) cmd;
	uint16_t cmdlen = (g_scan_fix & SCANFIX_MAC) ? 20u : 18u;

	memset(tlv, 0, sizeof(tlv));
	memset(&cmd, 0, sizeof(cmd));
	cmd.vdev_id = 0;
	cmd.vdev_type = 2;                     /* WMI_VDEV_TYPE_STA */
	cmd.vdev_subtype = 0;                  /* WMI_VDEV_SUBTYPE_NONE */
	cmd.mac[0] = 0x02;                     /* locally administered */
	cmd.mac[1] = 0x00; cmd.mac[2] = 0x00;
	cmd.mac[3] = 0x00; cmd.mac[4] = 0x00; cmd.mac[5] = 0x01;
	/* The last two bytes of wmi_mac_addr stay zero in both layouts, so
	 * truncating to 18 reproduces the legacy wire image byte-for-byte. */

	p = hif_put_tlv(p, WMI_TLV_VDEV_CREATE_CMD, cmdlen, &cmd);
	HIF_LOG("WMI: send VDEV_CREATE id=0 type=STA (cmd=%uB)", cmdlen);
	return hif_wmi_command(WMI_VDEV_CREATE_CMDID, tlv, (uint32_t)(p - tlv));
}

static int
hif_send_wmi_start_scan(uint32_t dwell_ms)
{
	uint8_t tlv[768];
	uint8_t *p = tlv;
	struct hif_start_scan_cmd {
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
		/* Linux struct wmi_mac_addr is "2 word representation of MAC
		 * addr": union { u8 addr[6]; struct { u32 word0, word1; } },
		 * sizeof == 8.  Both mac_addr and mac_mask are 8 bytes, making
		 * wmi_tlv_start_scan_cmd exactly 100 bytes.  Our old 6+6 sent
		 * 96, so the firmware read four bytes of the following array
		 * TLV as the tail of the command struct. */
		uint8_t mac_addr[8];
		uint8_t mac_mask[8];
	} __attribute__((packed));
	struct hif_start_scan_cmd cmd;
	/* 100 bytes is the ABI; 96 is the legacy control (the last 12 bytes
	 * are all zero in both layouts, so truncating reproduces the old
	 * wire image byte-for-byte). */
	typedef char hif_start_scan_size_check[
	    sizeof(struct hif_start_scan_cmd) == 100 ? 1 : -1];
	uint16_t cmdlen = (g_scan_fix & SCANFIX_CMD) ? 100u : 96u;
	uint32_t i, nch = (uint32_t)(sizeof(g_scan_freqs) / sizeof(g_scan_freqs[0]));
	uint8_t chans[256];
	uint8_t bssid[8];
	uint16_t bssid_len = (g_scan_fix & SCANFIX_CMD) ? 8u : 6u;

	if (nch * 4 > sizeof(chans)) nch = sizeof(chans) / 4;
	if (g_scan_chans >= 0) nch = (uint32_t)g_scan_chans;
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
	cmd.common[8] = 5000;                    /* max_rest_time (ath10k default) */
	cmd.common[9] = 0;                       /* repeat_probe_time */
	cmd.common[10] = 0;                      /* probe_spacing_time */
	cmd.common[11] = 0;                      /* idle_time */
	/* 39 channels x (dwell + rest) needs about 4 s; the previous 3000 ms
	 * cut every full-band scan short. */
	cmd.common[12] = 20000;                  /* max_scan_time */
	cmd.common[13] = 5;                      /* probe_delay (ath10k: 5) */
	/* ath10k's TLV path XORs WMI_SCAN_FILTER_PROBE_REQ into the flags, so
	 * this is the post-XOR value of "no scan flags" - report beacons and
	 * probe responses, nothing else. */
	cmd.common[14] = WMI_SCAN_FILTER_PROBE_REQ;
	cmd.burst_duration_ms = dwell_ms;
	cmd.num_channels = nch;
	cmd.num_bssids = 1;
	cmd.num_ssids = 0;
	cmd.ie_len = 0;
	cmd.num_probes = 3;

	if (g_scan_linux) {
		/* mac.c ath10k_wmi_start_scan_init(), wmi.c
		 * ath10k_wmi_put_start_scan_common() and the TLV path's XOR.
		 * The two ID prefixes are ORed in by the host, not invented:
		 * WMI_HOST_SCAN_REQ_ID_PREFIX / REQUESTOR_ID_PREFIX = 0xA000.
		 * With no SSIDs Linux also asks for a passive scan. */
		cmd.common[0] = WMI_HOST_SCAN_REQ_ID_PREFIX;
		cmd.common[1] = WMI_HOST_SCAN_REQUESTOR_PREFIX | 1u;
		cmd.common[4] = 0x11f;       /* started/completed/chan/dequeued */
		cmd.common[5] = 50;
		cmd.common[6] = 150;
		cmd.common[7] = 50;
		cmd.common[8] = 500;
		cmd.common[14] = (WMI_SCAN_CHAN_STAT_EVENT |
		    WMI_SCAN_FLAG_PASSIVE) ^ WMI_SCAN_FILTER_PROBE_REQ;
		cmd.burst_duration_ms = 0;
	}

	/* SCANFIX_CMD copies coachz_wifi.c ws_start_scan() /
	 * ath10k_wmi_start_scan_init() values verbatim: the 0xA000 ID
	 * prefixes, passive scan, 50/150 ms dwell, 50/500 ms rest, 20 s max,
	 * three probes and the WMI-TLV inverted filter flag. */
	if (g_scan_fix & SCANFIX_CMD) {
		cmd.common[0] = 0xa000u;
		cmd.common[1] = 0xa001u;
		cmd.common[4] = 0x11f;
		cmd.common[5] = 50;
		cmd.common[6] = 150;
		cmd.common[7] = 50;
		cmd.common[8] = 500;
		cmd.common[12] = 20000;
		cmd.common[13] = 5;
		cmd.common[14] = 0x01u | 0x10u | 0x20u;
		cmd.burst_duration_ms = 0;
		cmd.num_probes = 3;
	}

	/* ath10k_wmi_tlv_op_gen_start_scan() always asks for one broadcast
	 * BSSID; without it the firmware does not send broadcast probe
	 * requests.  wmi_mac_addr is 8 bytes (last two zero). */
	/* wmi_mac_addr is 8 bytes: 6 broadcast bytes plus a zero tail. */
	memset(bssid, 0, sizeof(bssid));
	for (i = 0; i < 6; i++) bssid[i] = 0xff;

	for (i = 0; i < nch; i++) hif_put32(chans + i * 4, g_scan_freqs[i]);

	/* scanshp bisects the four array TLVs.  503 proved shape 1 (struct
	 * only, 100 bytes) is accepted and the target answers with a scan
	 * event, while the old shape 0 killed it - and 504 plus the ath10k
	 * wmi-tlv.h enum settled why: the array tags were off by one (the
	 * ath11k numbers 17/18/19/20 had replaced the ath10k 16/17/18/19).
	 * With the defines fixed, shape 0 is the full Linux command
	 * (16/19/19/17); the other shapes stay as A/B controls.
	 *   0 full Linux command (tags 16/19/19/17)
	 *   1 struct only
	 *   2 struct + one empty channel array
	 *   3 struct + 39-channel array (define value)
	 *   4 struct + 39-channel array, hard-coded 16 (the value that
	 *     survived 504 while 17 died)
	 *   5 struct + broadcast-BSSID array only
	 *   6 all four arrays, hard-coded 16/19/19/17 */
	switch (g_scan_shape) {
	case 1:
	case 2:
		cmd.num_channels = 0;
		cmd.num_bssids = 0;
		break;
	case 5:
		cmd.num_channels = 0;
		cmd.num_bssids = 1;
		break;
	case 3:
	case 4:
		cmd.num_channels = nch;
		cmd.num_bssids = 0;
		break;
	default:
		cmd.num_channels = nch;
		cmd.num_bssids = 1;
		break;
	}
	p = hif_put_tlv(p, WMI_TLV_START_SCAN_CMD, cmdlen, &cmd);
	switch (g_scan_shape) {
	case 1:
		break;
	case 2:
		p = hif_put_tlv(p, WMI_TLV_ARRAY_UINT32, 0, NULL);
		break;
	case 3:
		p = hif_put_tlv(p, WMI_TLV_ARRAY_UINT32, (uint16_t)(nch * 4), chans);
		break;
	case 4:
		p = hif_put_tlv(p, 16, (uint16_t)(nch * 4), chans);
		break;
	case 5:
		p = hif_put_tlv(p, WMI_TLV_ARRAY_FIXED_STRUCT,
		    bssid_len, bssid);
		break;
	case 6:
		p = hif_put_tlv(p, 16, (uint16_t)(nch * 4), chans);
		p = hif_put_tlv(p, 19, 0, NULL);
		p = hif_put_tlv(p, 19, bssid_len, bssid);
		p = hif_put_tlv(p, 17, 0, NULL);
		break;
	default:
		p = hif_put_tlv(p, WMI_TLV_ARRAY_UINT32, (uint16_t)(nch * 4), chans);
		p = hif_put_tlv(p, WMI_TLV_ARRAY_FIXED_STRUCT, 0, NULL);  /* ssids */
		p = hif_put_tlv(p, WMI_TLV_ARRAY_FIXED_STRUCT, bssid_len, bssid);
		p = hif_put_tlv(p, WMI_TLV_ARRAY_BYTE, 0, NULL);          /* ie */
		break;
	}

	HIF_LOG("WMI: send START_SCAN (scan_id 0x%x, linux=%d, %u channels, "
	    "%u ms dwell, %u TLV bytes, cmd=%uB bssid=%uB)",
	    cmd.common[0], g_scan_linux, nch, dwell_ms, (unsigned)(p - tlv),
	    cmdlen, bssid_len);
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

/* =========================== join ===========================
 * Grafted from the wlan-wip reference implementation
 * (drivers/NativeWlan/WlanRadio.c, branch wlan-wip): authentication and
 * association as management frames over WMI by reference, then
 * ath10k_bss_assoc's peer assoc and vdev up.  The reference's per-function
 * comment "wmi_tlv_op_gen_mgmt_tx_send: by reference, the first bytes also
 * inline" matches ath10k_wmi_tlv_op_gen_mgmt_tx_send() (wmi-tlv.c:3087).
 *
 * Every path here is gated on the join=<n> boot-arg (n = 1-based BSS table
 * entry, picked when a scan round completes).  join=0 or absent leaves the
 * scan/HTT baseline untouched.
 *
 * Field-by-field ABI notes, checked against wlan-wip AND the local Linux
 * refs (work/refs/ath10k/wmi-tlv.h):
 *  - MGMT_TX_SEND (0x7008): TLV tag 422 (WMI_TLV_TAG_STRUCT_MGMT_TX_CMD),
 *    struct wmi_tlv_mgmt_tx_cmd (wmi-tlv.h:2675) = vdev_id, desc_id,
 *    chanfreq, paddr(8B), frame_len, buf_len = 28 bytes; then one
 *    WMI_TLV_TAG_ARRAY_BYTE (17) TLV of min(frame,64) rounded to 4.
 *  - MGMT_TX_COMPL (0x7006): TLV tag 423, wmi_tlv_mgmt_tx_compl_ev
 *    {desc_id, status, pdev_id, ppdu_id, ack_rssi}; status != 0 is a fail.
 *  - PEER_ASSOC (0x6005): TLV tag 101, 76 bytes, field-for-field the same
 *    as struct wmi_tlv_peer_assoc_cmd (wmi-tlv.h:1899) = mac_addr(8),
 *    vdev_id, new_assoc, assoc_id, flags, caps, listen_intval, ht_caps,
 *    max_mpdu, mpdu_density, rate_caps, nss, vht_caps, phy_mode, ht_info[2],
 *    num_legacy_rates, num_ht_rates; then ARRAY_BYTE legacy rates,
 *    ARRAY_BYTE HT rates and tag 102 (WMI_TLV_TAG_STRUCT_VHT_RATE_SET, 16B
 *    wmi_vht_rate_set: rx_max_rate, rx_mcs_set, tx_max_rate, tx_mcs_set).
 *  - VDEV_UP (0x5005): TLV tag 92, struct wmi_vdev_up_cmd 16 bytes.
 *  - PEER_SET_PARAM (0x6004): TLV tag 100, 20 bytes: vdev_id, mac(8B),
 *    param_id, value. ==================================================== */

#define HIF_JOIN_BSS_MAX   96u        /* wlan-wip: #define WS_BSS 96u */
#define HIF_JOIN_IE_MAX    384u       /* wlan-wip: nw_bss_info.ie[384] */
#define HIF_JOIN_SLOTS     8u         /* wlan-wip: WJ_MGMT_SLOTS */
#define HIF_JOIN_SLOT      1024u      /* wlan-wip: WJ_MGMT_SLOT */

enum hif_join_state {
	HIF_JOIN_IDLE = 0,
	HIF_JOIN_VDEV_START,          /* VDEV_START sent, waiting for 0x5001 */
	HIF_JOIN_PEER_CREATE,         /* PEER_CREATE sent, waiting for HTT peer map */
	HIF_JOIN_AUTH_SENT,           /* auth frame sent, waiting for 0xb0 */
	HIF_JOIN_ASSOC_SENT,          /* assoc sent, waiting for 0x10/0x30 */
	HIF_JOIN_ASSOC_OK             /* peer_assoc + VDEV_UP are out */
};

struct hif_join_bss {
	uint8_t bssid[6];
	uint8_t ssid_len;
	uint8_t ssid[32];
	uint32_t freq_mhz;
	int8_t signal_dbm;
	uint16_t caps, beacon_int;
	uint16_t ie_len;
	uint8_t ie[HIF_JOIN_IE_MAX];
};

/* Our HT and VHT capability, as the wlan-wip reference pins it (iw phy for
 * this radio): A-MPDU exponent 3 with 8 us spacing, two streams. */
#define HIF_JOIN_HT_CAP  0x19efu
#define HIF_JOIN_VHT_CAP (0x739139b2u & ~(1u << 11 | 1u << 12 | 7u << 13 | \
					   7u << 16 | 1u << 19 | 1u << 20))

static struct hif_join_bss g_join_bss[HIF_JOIN_BSS_MAX];
static uint32_t g_join_bss_count;
static int g_join_state;
static uint32_t g_join_seq;                 /* 802.11 sequence counter */
static uint32_t g_join_tries;               /* auth/assoc retransmits */
static uint32_t g_join_tx_at;               /* poll of the last frame sent */
static uint32_t g_join_wait_at;             /* poll the current state began */
static uint32_t g_join_mgmt_next;           /* rotating slot index */
static uint32_t g_join_mgmt_tx, g_join_mgmt_ok, g_join_mgmt_fail;
/* Prerequisites from the reference state machine: the VDEV_START response
 * and the HTT peer-map event must both arrive before the first auth frame. */
static int g_join_started;        /* 0 = pending, 1 = ok, 2 = refused */
static int g_join_peer_mapped;
static uint16_t g_join_peer_id;
static const uint8_t g_join_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
/* The access point being joined (copied out of the BSS table). */
static struct {
	struct hif_join_bss b;
	uint8_t secure, wmm, ht, vht, width;
	uint32_t center_mhz, mode;
	uint16_t aid;
	uint8_t rsn[22];           /* the RSN element we answer with */
} g_join_ap;

/* wlan-wip wj_ie(): find one information element in the AP's beacon. */
static const uint8_t *
hif_join_ie(uint8_t id)
{
	const uint8_t *e = g_join_ap.b.ie;
	uint32_t at;

	for (at = 0; at + 2u <= g_join_ap.b.ie_len &&
	    at + 2u + e[at + 1] <= g_join_ap.b.ie_len;
	    at += 2u + e[at + 1])
		if (e[at] == id) return e + at;
	return NULL;
}

/* wlan-wip wj_has_wmm(): the WMM element, vendor 00:50:f2 type 2. */
static int
hif_join_has_wmm(void)
{
	const uint8_t *e = g_join_ap.b.ie;
	uint32_t at;

	for (at = 0; at + 2u <= g_join_ap.b.ie_len &&
	    at + 2u + e[at + 1] <= g_join_ap.b.ie_len;
	    at += 2u + e[at + 1])
		if (e[at] == 221 && e[at + 1] >= 5 && e[at + 2] == 0 &&
		    e[at + 3] == 0x50 && e[at + 4] == 0xf2 && e[at + 5] == 2)
			return 1;
	return 0;
}

/* wlan-wip wj_channel_plan(): what the access point's beacon says about the
 * link - 20, 40 or 80 MHz, the centre, and the phy mode number. */
static void
hif_join_channel_plan(void)
{
	const uint8_t *htop = hif_join_ie(61), *vhtop = hif_join_ie(192);
	const uint32_t f = g_join_ap.b.freq_mhz;
	const int five = f >= 4900;

	g_join_ap.ht = hif_join_ie(45) != NULL;
	g_join_ap.vht = five && g_join_ap.ht && hif_join_ie(191) != NULL;
	g_join_ap.width = 20;
	g_join_ap.center_mhz = f;
	{
		uint32_t chan = five ? (f - 5000u) / 5u : 0;

		if (g_join_ap.ht && htop && htop[1] >= 2 && (htop[3] & 4) &&
		    (htop[3] & 3)) {
			g_join_ap.width = 40;
			g_join_ap.center_mhz = (htop[3] & 3) == 1 ?
			    f + 10u : f - 10u;
		}
		if (g_join_ap.vht && vhtop && vhtop[1] >= 3 && vhtop[2] >= 1 &&
		    g_join_ap.width == 40) {
			static const uint8_t centres[] =
			    { 42, 58, 106, 122, 138, 155 };
			uint32_t i;

			for (i = 0; i < sizeof(centres); i++) {
				if (chan + 6u >= centres[i] &&
				    chan <= centres[i] + 6u) {
					g_join_ap.width = 80;
					g_join_ap.center_mhz =
					    5000u + 5u * centres[i];
				}
			}
		}
	}
	if (!five)
		g_join_ap.mode = !g_join_ap.ht ? 1u :
		    g_join_ap.width == 40 ? 7u : 5u;
	else
		g_join_ap.mode = !g_join_ap.ht ? 0u :
		    g_join_ap.width == 80 ? 10u :
		    g_join_ap.width == 40 ?
		    (g_join_ap.vht ? 9u : 6u) : (g_join_ap.vht ? 8u : 4u);
}

/* wlan-wip wj_rsn_prepare(): only WPA2-Personal with CCMP pairwise and CCMP
 * group, management protection not required.  Writes the 22-byte RSN element
 * we answer with. */
static int
hif_join_rsn_prepare(const uint8_t *e)
{
	static const uint8_t oui[3] = { 0x00, 0x0f, 0xac };
	uint8_t rsn[22];
	const uint8_t *p, *end;
	unsigned n, found, i;
	uint8_t group;

	if (!e || e[1] < 18) return -1;
	p = e + 2;
	end = e + 2 + e[1];
	p += 2;                                /* version */
	if (memcmp(p, oui, 3)) return -1;
	group = p[3];
	/* The group cipher must be echoed exactly: ChinaUnicom's 2.4 GHz BSS
	 * advertises a TKIP group with CCMP in the pairwise list, and a
	 * CCMP-group request makes it answer assoc status 41 ("invalid group
	 * cipher", 802.11-2020) - chain-554's RSN dump settled this. */
	if (group != 2 && group != 4) return -1;
	p += 4;
	n = (unsigned)(p[0] | p[1] << 8);      /* pairwise cipher count */
	found = 0;
	p += 2;
	if (end - p < (long)(4u * n + 2u)) return -1;
	for (i = 0; i < n; i++, p += 4)
		if (!memcmp(p, oui, 3) && p[3] == 4) found = 1;
	if (!found) return -1;                 /* the PTK should stay CCMP */
	n = (unsigned)(p[0] | p[1] << 8);      /* AKM count */
	found = 0;
	p += 2;
	if (end - p < (long)(4u * n)) return -1;
	for (i = 0; i < n; i++, p += 4)
		if (!memcmp(p, oui, 3) && p[3] == 2) found = 1;
	if (!found) return -1;
	if (end - p >= 2 && (p[0] & 0x40)) return -1;  /* PMF required */

	memset(rsn, 0, sizeof(rsn));
	rsn[0] = 48; rsn[1] = 20;                     /* RSN element, len 20 */
	rsn[2] = 1; rsn[3] = 0;                       /* version 1 */
	memcpy(rsn + 4, oui, 3); rsn[7] = group;      /* group = AP's choice */
	rsn[8] = 1; rsn[9] = 0;                       /* pairwise count 1 */
	memcpy(rsn + 10, oui, 3); rsn[13] = 4;        /* pairwise CCMP */
	rsn[14] = 1; rsn[15] = 0;                     /* AKM count 1 */
	memcpy(rsn + 16, oui, 3); rsn[19] = 2;        /* PSK */
	memcpy(g_join_ap.rsn, rsn, sizeof(rsn));
	HIF_LOG("JOIN: RSN built group=%s pairwise=CCMP akm=PSK",
	    group == 2 ? "TKIP" : "CCMP");
	return 0;
}

/* ===================== WPA2-PSK four-way handshake (wpa=1) ==============
 * Grafted from wlan-wip (drivers/NativeWlan/WlanRadio.c): wk_sha1/wk_hmac/
 * wk_prf/wk_pmk L3231-3383, wk_aes_* and wk_unwrap L3390-3450, wk_reply L4277,
 * wj_eapol L4316, wj_send_key L3965.  Credentials come from boot-args only
 * (wpa=1, wpass=<passphrase>) so no secret lives in this source file. */
static void hif_join_peer_param(uint32_t id, uint32_t value);
static void hif_join_send_vdev_up(void);

#define WPA_EAPOL_FIXED 99u

static int g_wpa;
static char g_wpa_pass[64];

static struct {
	uint8_t stage;                 /* 0 idle, 1 msg1 processed */
	uint8_t eapol_ver, have_replay, keyed;
	uint8_t anonce[32], snonce[32], pmk[32], ptk[48], replay[8];
	uint8_t gtk[32], gtk_idx, gtk_len;   /* TKIP group keys are 32 B */
	uint32_t tx_slot;
} g_wpa_st;

static inline uint32_t
wpa_rol(uint32_t v, unsigned n)
{
	return v << n | v >> (32 - n);
}

static void
wpa_sha1_block(uint32_t h[5], const uint8_t *p)
{
	uint32_t w[80], i, a, b, c, d, e;

	for (i = 0; i < 16; i++)
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
		    (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
	for (i = 16; i < 80; i++)
		w[i] = wpa_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
	for (i = 0; i < 80; i++) {
		uint32_t f, t;

		if (i < 20) f = ((b & c) | (~b & d)) + 0x5a827999u;
		else if (i < 40) f = (b ^ c ^ d) + 0x6ed9eba1u;
		else if (i < 60) f = ((b & c) | (b & d) | (c & d)) + 0x8f1bbcdcu;
		else f = (b ^ c ^ d) + 0xca62c1d6u;
		t = wpa_rol(a, 5) + f + e + w[i];
		e = d; d = c; c = wpa_rol(b, 30); b = a; a = t;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static void
wpa_sha1(const uint8_t *a, uint32_t an, const uint8_t *b, uint32_t bn,
    uint8_t out[20])
{
	uint32_t h[5] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu,
	    0x10325476u, 0xc3d2e1f0u };
	uint8_t block[64];
	uint32_t fill = 0, piece, i;
	uint64_t bits = ((uint64_t)an + bn) * 8u;

	for (piece = 0; piece < 2; piece++) {
		const uint8_t *p = piece ? b : a;
		uint32_t n = piece ? bn : an;

		while (n) {
			uint32_t take;

			if (!fill && n >= 64) {
				wpa_sha1_block(h, p);
				p += 64; n -= 64; continue;
			}
			take = n < 64 - fill ? n : 64 - fill;
			memcpy(block + fill, p, take);
			fill += take; p += take; n -= take;
			if (fill == 64) { wpa_sha1_block(h, block); fill = 0; }
		}
	}
	block[fill++] = 0x80;
	if (fill > 56) {
		memset(block + fill, 0, 64 - fill);
		wpa_sha1_block(h, block);
		fill = 0;
	}
	memset(block + fill, 0, 56 - fill);
	for (i = 0; i < 8; i++) block[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
	wpa_sha1_block(h, block);
	for (i = 0; i < 5; i++) {
		unsigned k;

		for (k = 0; k < 4; k++)
			out[4 * i + k] = (uint8_t)(h[i] >> (24 - 8 * k));
	}
}

static void
wpa_hmac(const uint8_t *key, uint32_t kn, const uint8_t *data, uint32_t n,
    uint8_t out[20])
{
	uint8_t pad[64], inner[20];
	uint32_t i;

	memset(pad, 0x36, sizeof(pad));
	if (kn > 64) kn = 64;
	for (i = 0; i < kn; i++) pad[i] ^= key[i];
	wpa_sha1(pad, 64, data, n, inner);
	for (i = 0; i < 64; i++) pad[i] ^= 0x36 ^ 0x5c;
	wpa_sha1(pad, 64, inner, 20, out);
}

static void
wpa_prf(const uint8_t *key, uint32_t kn, const char *label,
    const uint8_t *data, uint32_t n, uint8_t *out, uint32_t bytes)
{
	uint8_t m[128], digest[20];
	uint32_t ln = (uint32_t)strlen(label), i;

	if (ln + 2u + n > sizeof(m)) return;
	memcpy(m, label, ln);
	m[ln] = 0;
	memcpy(m + ln + 1u, data, n);
	for (i = 0; i * 20u < bytes; i++) {
		uint32_t take = bytes - i * 20u;

		m[ln + 1u + n] = (uint8_t)i;
		wpa_hmac(key, kn, m, ln + 2u + n, digest);
		if (take > 20u) take = 20u;
		memcpy(out + i * 20u, digest, take);
	}
}

static void
wpa_pmk(const char *pass, uint32_t pn, const uint8_t *ssid, uint32_t sn,
    uint8_t out[32])
{
	uint8_t salt[40];
	uint32_t done = 0, block;

	memcpy(salt, ssid, sn);
	for (block = 1; done < 32; block++) {
		uint8_t u[20], t[20];
		unsigned round, i;
		uint32_t take;

		salt[sn] = salt[sn + 1] = salt[sn + 2] = 0;
		salt[sn + 3] = (uint8_t)block;
		wpa_hmac((const uint8_t *)pass, pn, salt, sn + 4u, u);
		memcpy(t, u, 20);
		for (round = 1; round < 4096; round++) {
			wpa_hmac((const uint8_t *)pass, pn, u, 20, u);
			for (i = 0; i < 20; i++) t[i] ^= u[i];
		}
		take = 32 - done < 20 ? 32 - done : 20;
		memcpy(out + done, t, take);
		done += take;
	}
}

static uint8_t wpa_sbox[256], wpa_inv[256];
static inline uint8_t wpa_xt(uint8_t v)
{
	return (uint8_t)(v << 1) ^ (v & 0x80 ? 0x1b : 0);
}

static uint8_t
wpa_mul(uint8_t a, uint8_t b)
{
	uint8_t r = 0;

	while (b) {
		if (b & 1) r ^= a;
		a = wpa_xt(a);
		b >>= 1;
	}
	return r;
}

static void
wpa_tables(void)
{
	uint8_t p = 1, q = 1;
	unsigned i;

	if (wpa_sbox[0]) return;
	do {
		uint8_t x;

		p = p ^ wpa_xt(p);
		q ^= (uint8_t)(q << 1);
		q ^= (uint8_t)(q << 2);
		q ^= (uint8_t)(q << 4);
		if (q & 0x80) q ^= 0x09;
		x = q ^ (uint8_t)(q << 1 | q >> 7) ^ (uint8_t)(q << 2 | q >> 6) ^
		    (uint8_t)(q << 3 | q >> 5) ^ (uint8_t)(q << 4 | q >> 4);
		wpa_sbox[p] = x ^ 0x63;
	} while (p != 1);
	wpa_sbox[0] = 0x63;
	for (i = 0; i < 256; i++) wpa_inv[wpa_sbox[i]] = (uint8_t)i;
}

static void
wpa_aes_expand(const uint8_t key[16], uint8_t rk[176])
{
	uint8_t rcon = 1;
	unsigned i, k;

	memcpy(rk, key, 16);
	for (i = 16; i < 176; i += 4) {
		uint8_t t[4];

		memcpy(t, rk + i - 4, 4);
		if (!(i % 16)) {
			uint8_t first = t[0];

			t[0] = wpa_sbox[t[1]] ^ rcon;
			t[1] = wpa_sbox[t[2]];
			t[2] = wpa_sbox[t[3]];
			t[3] = wpa_sbox[first];
			rcon = wpa_xt(rcon);
		}
		for (k = 0; k < 4; k++) rk[i + k] = rk[i - 16 + k] ^ t[k];
	}
}

static void
wpa_aes_decrypt(const uint8_t rk[176], uint8_t s[16])
{
	int round;
	unsigned k, c, r;

	for (k = 0; k < 16; k++) s[k] ^= rk[160 + k];
	for (round = 9; round >= 0; round--) {
		uint8_t t[16];

		for (c = 0; c < 4; c++)
			for (r = 0; r < 4; r++)
				t[(4 * (c + r) + r) % 16] = wpa_inv[s[4 * c + r]];
		for (k = 0; k < 16; k++) t[k] ^= rk[16 * (unsigned)round + k];
		if (round) {
			for (c = 0; c < 4; c++) {
				const uint8_t *i = t + 4 * c;

				s[4 * c] = wpa_mul(i[0], 14) ^ wpa_mul(i[1], 11) ^
				    wpa_mul(i[2], 13) ^ wpa_mul(i[3], 9);
				s[4 * c + 1] = wpa_mul(i[0], 9) ^ wpa_mul(i[1], 14) ^
				    wpa_mul(i[2], 11) ^ wpa_mul(i[3], 13);
				s[4 * c + 2] = wpa_mul(i[0], 13) ^ wpa_mul(i[1], 9) ^
				    wpa_mul(i[2], 14) ^ wpa_mul(i[3], 11);
				s[4 * c + 3] = wpa_mul(i[0], 11) ^ wpa_mul(i[1], 13) ^
				    wpa_mul(i[2], 9) ^ wpa_mul(i[3], 14);
			}
		} else {
			memcpy(s, t, 16);
		}
	}
}

static int
wpa_unwrap(const uint8_t kek[16], const uint8_t *in, uint32_t n, uint8_t *out)
{
	uint8_t rk[176], a[8], b[16];
	uint32_t blocks, i;
	int j;
	unsigned k;

	if (n < 24 || n % 8) return -1;
	wpa_tables();
	wpa_aes_expand(kek, rk);
	blocks = n / 8u - 1u;
	memcpy(a, in, 8);
	memcpy(out, in + 8, n - 8u);
	for (j = 5; j >= 0; j--) {
		for (i = blocks; i >= 1; i--) {
			uint64_t t = (uint64_t)blocks * (unsigned)j + i;

			memcpy(b, a, 8);
			for (k = 0; k < 8; k++) b[7 - k] ^= (uint8_t)(t >> (8 * k));
			memcpy(b + 8, out + 8u * (i - 1u), 8);
			wpa_aes_decrypt(rk, b);
			memcpy(a, b, 8);
			memcpy(out + 8u * (i - 1u), b + 8, 8);
		}
	}
	for (k = 0; k < 8; k++)
		if (a[k] != 0xa6) return -1;
	return 0;
}

/* wlan-wip wj_tx_compl(): retire the TX slots the target has finished and
 * expose its status.  chain-573 w1 produced one of these per EAPOL M2 with
 * status 0, which is why "M2 never left" is not the reason M3 never came. */
static void
hif_wpa_tx_compl(const uint8_t *p, uint32_t n)
{
	uint32_t i;
	uint8_t status, count;

	if (n < 4u) return;
	status = (uint8_t)(p[1] & 7u);
	count = p[2];
	for (i = 0; i < count && 4u + 2u * i + 2u <= n; i++) {
		uint16_t id = (uint16_t)(p[4 + 2 * i] |
		    (uint16_t)p[5 + 2 * i] << 8);

		if (status == 0) g_wpa_tx_ack++;
		else if (status == 2) g_wpa_tx_noack++;
		else g_wpa_tx_discard++;
		if (id < WPA_TX_SLOTS) g_wpa_tx_busy[id] = 0;
	}
	HIF_LOG("WPA: tx compl status=%u count=%u ack=%u noack=%u discard=%u",
	    (unsigned)status, (unsigned)count, g_wpa_tx_ack,
	    g_wpa_tx_noack, g_wpa_tx_discard);
}

/* ---- TX path: 28-byte HTT TX_FRM + frag desc, CE4 two-segment gather ---- */
static int
hif_wpa_tx_frame(const uint8_t *frame, uint32_t len, int eapol)
{
	uint32_t slot, prefetch, lo, fl, i;
	uint64_t pa, fr_pa, desc_pa;
	uint16_t hi, htc, flags1, id, peer = 0xffff, freq = 0;
	uint8_t h[28], *ext;
	int rc;

	if (!g_dma || len > 2048u || len < 14u) return -1;
	/* Prefer a slot the target has already completed.  wlan-wip keeps a
	 * free list driven by TX_COMPL_IND; reusing a live slot would let a
	 * late M1 overwrite the M4 frame before the target reads it. */
	for (i = 0; i < WPA_TX_SLOTS; i++)
		if (!g_wpa_tx_busy[(g_wpa_st.tx_slot + i) & (WPA_TX_SLOTS - 1u)])
			break;
	/* Trogdor wj_tx_put() returns -2 when the free list is empty instead
	 * of wrapping around: the old code wrapped to g_wpa_st.tx_slot and
	 * reused a slot whose frame the target is still DMA-ing.  A dropped
	 * frame is recoverable (DHCP/TCP retry); a torn DMA buffer is a
	 * whole-machine SError.  txguard=0 keeps the old wrap for the A/B. */
	if (g_tx_guard && i == WPA_TX_SLOTS) {
		static uint32_t drops;
		drops++;
		if (drops <= 8u || (drops & 0xffu) == 0u)
			HIF_LOG("WPA: tx slots busy drops=%u drop %u bytes (txguard)",
			    drops, len);
		return -1;
	}
	slot = (g_wpa_st.tx_slot + i) & (WPA_TX_SLOTS - 1u);
	g_wpa_st.tx_slot = slot + 1u;
	g_wpa_tx_busy[slot] = 1;
	fr_pa = hif_pa_of(g_dma->tx_frame[slot]);
	if (!fr_pa) return -1;
	memcpy(g_dma->tx_frame[slot], frame, len);
	prefetch = len < 50u ? len : 50u;
	prefetch = (prefetch + 3u) & ~3u;

	desc_pa = hif_pa_of(&g_dma->htt_frag[slot * HTT_FRAG_DESC_SIZE]);
	if (!desc_pa) return -1;
	ext = &g_dma->htt_frag[slot * HTT_FRAG_DESC_SIZE];
	memset(ext, 0, HTT_FRAG_DESC_SIZE);
	lo = (uint32_t)fr_pa;
	hi = (uint16_t)(fr_pa >> 32);
	fl = len;
	memcpy(ext + 24, &lo, 4);
	memcpy(ext + 28, &hi, 2);
	memcpy(ext + 30, &fl, 2);

	memset(h, 0, sizeof(h));
	htc = (uint16_t)(20u + prefetch);
	flags1 = (uint16_t)((!g_join_ap.wmm ? 16u : eapol ? 7u : 0u) << 6 | 1u << 11);
	id = (uint16_t)slot;
	h[0] = g_htt_eid;
	hif_put16(h + 2, htc);
	h[8] = 1;   /* HTT_H2T_MSG_TYPE_TX_FRM */
	h[9] = (uint8_t)(1u | 1u << 5 | (eapol && !g_wpa_st.keyed ? 4u : 0u));
	hif_put16(h + 10, flags1);
	hif_put16(h + 12, (uint16_t)fl);
	hif_put16(h + 14, id);
	hif_put64(h + 16, desc_pa);
	hif_put16(h + 24, peer);
	hif_put16(h + 26, freq);

	rc = hif_ce_send_gather(4, h, (uint32_t)sizeof(h), fr_pa, prefetch);
	HIF_LOG("WPA: tx frame %u bytes slot=%u prefetch=%u rc=%d", len, slot,
	    prefetch, rc);
	return rc;
}

/* wlan-wip wj_net_output() (WlanRadio.c:4769): the stack hands us an
 * Ethernet frame; wrap it in a 24-byte native-Wi-Fi data header plus
 * LLC/SNAP and send it over the same HTT TX_FRM path as EAPOL.  The keys
 * are installed by now, so frame[1] carries the protected bit and
 * hif_wpa_tx_frame() asks the target to encrypt it. */
extern "C" int
wcn3990_hif_net_tx(const uint8_t *eth, size_t len)
{
	static uint8_t frame[32 + 1536];
	uint32_t n;

	if (!g_dma || !g_link_up || len < 14u || len > 1514u) return -1;
	n = (uint32_t)len - 14u;
	memset(frame, 0, 32);
	frame[0] = 0x08;
	frame[1] = (uint8_t)(0x01u | 0x40u);
	memcpy(frame + 4, g_join_ap.b.bssid, 6);   /* addr1: the AP */
	memcpy(frame + 10, g_join_mac, 6);          /* addr2: us */
	memcpy(frame + 16, eth, 6);                 /* addr3: the frame's DA */
	frame[24] = frame[25] = 0xaa;
	frame[26] = 3;
	frame[30] = eth[12];
	frame[31] = eth[13];
	memcpy(frame + 32, eth + 14, n);
	return hif_wpa_tx_frame(frame, 32u + n, 0);
}

/* C-linkage bridge for hakios.dhcp.c, which owns no WCN3990 state. */
extern "C" int
picasso_dhcp_send(const uint8_t *eth, size_t len)
{
	return wcn3990_hif_net_tx(eth, len);
}

extern "C" int
wcn3990_hif_net_ready(void)
{
	return (g_net_path && g_link_up && g_dma) ? 1 : 0;
}

/* wlan-wip wj_send_key(): WMI_VDEV_INSTALL_KEY (0x5009), TLV 0x60 with the
 * 92-byte struct, then the key bytes in an ARRAY_BYTE TLV (tag 0x11). */
static void
hif_wpa_send_key(int pairwise, unsigned index, const uint8_t *key,
    unsigned key_len, unsigned cipher)
{
	uint8_t tlv[160], *p = tlv;
	uint32_t body[23];

	if (key_len > 32u) key_len = 32u;
	memset(tlv, 0, sizeof(tlv));
	memset(body, 0, sizeof(body));
	body[0] = 0;                                   /* vdev id */
	memcpy((uint8_t *)body + 4, g_join_ap.b.bssid, 6); /* peer mac (8 B) */
	body[3] = index;
	body[4] = pairwise ? 0u : 1u;                  /* key type */
	body[5] = cipher;                              /* 2 TKIP, 4 AES_CCM */
	/* body[6..19]: rx/tx sequence counters, all zero */
	body[20] = key_len;
	p = hif_put_tlv(p, 0x60, 92, body);
	p = hif_put_tlv(p, 0x11, (uint16_t)key_len, key);
	HIF_LOG("WPA: install %s key idx=%u len=%u cipher=%u",
	    pairwise ? "pairwise" : "group", index, key_len, cipher);
	(void)hif_wmi_command(0x5009, tlv, (uint32_t)(p - tlv));
}

/* One EAPOL frame out: 32-byte 802.11 data header + LLC/SNAP 0x888e + body. */
static void
hif_wpa_reply(uint16_t info, const uint8_t *replay, const uint8_t *nonce,
    const uint8_t *data, uint16_t data_len)
{
	static uint8_t f[WPA_EAPOL_FIXED + 64];
	static uint8_t frame[32 + WPA_EAPOL_FIXED + 64];
	uint8_t digest[20];
	uint16_t body = (uint16_t)(WPA_EAPOL_FIXED - 4u + data_len);
	uint32_t n;

	memset(f, 0, sizeof(f));
	f[0] = g_wpa_st.eapol_ver;
	f[1] = 3;
	f[2] = (uint8_t)(body >> 8);
	f[3] = (uint8_t)body;
	f[4] = 2;
	f[5] = (uint8_t)(info >> 8);
	f[6] = (uint8_t)info;
	memcpy(f + 9, replay, 8);
	if (nonce) memcpy(f + 17, nonce, 32);
	f[97] = (uint8_t)(data_len >> 8);
	f[98] = (uint8_t)data_len;
	if (data_len) memcpy(f + 99, data, data_len);
	wpa_hmac(g_wpa_st.ptk, 16, f, WPA_EAPOL_FIXED + data_len, digest);
	memcpy(f + 81, digest, 16);

	memset(frame, 0, sizeof(frame));
	frame[0] = 0x08;
	frame[1] = (uint8_t)(0x01u | (g_wpa_st.keyed ? 0x40u : 0));
	memcpy(frame + 4, g_join_ap.b.bssid, 6);       /* addr1 */
	frame[10] = 0x02;                              /* addr2: our base MAC */
	frame[15] = 0x01;
	memcpy(frame + 16, g_join_ap.b.bssid, 6);      /* addr3 */
	frame[24] = frame[25] = 0xaa;
	frame[26] = 3;
	frame[30] = 0x88;
	frame[31] = 0x8e;
	n = (uint32_t)(WPA_EAPOL_FIXED + data_len);
	memcpy(frame + 32, f, n);
	if (g_wpa_diag) {
		/* Everything needed to recompute the M2 MIC offline with
		 * hashlib: the exact SSID/passphrase lengths, the PMK/KCK
		 * fingerprints, both nonces and the full outgoing frame.
		 * No plaintext passphrase is ever logged. */
		HIF_LOG("WPA: diag info=%04x ssid=%02x%02x%02x%02x "
		    "ssid_len=%u passlen=%u "
		    "pmk=%02x%02x%02x%02x kck=%02x%02x%02x%02x "
		    "mic=%02x%02x%02x%02x",
		    (unsigned)info, g_join_ap.b.ssid[0],
		    g_join_ap.b.ssid[1], g_join_ap.b.ssid[2],
		    g_join_ap.b.ssid[3],
		    (unsigned)g_join_ap.b.ssid_len,
		    (unsigned)strlen(g_wpa_pass),
		    g_wpa_st.pmk[0], g_wpa_st.pmk[1], g_wpa_st.pmk[2],
		    g_wpa_st.pmk[3], g_wpa_st.ptk[0], g_wpa_st.ptk[1],
		    g_wpa_st.ptk[2], g_wpa_st.ptk[3],
		    digest[0], digest[1], digest[2], digest[3]);
		HIF_LOG("WPA: diag snonce=%02x%02x%02x%02x "
		    "anonce=%02x%02x%02x%02x replay=%02x%02x%02x%02x "
		    "datalen=%u",
		    g_wpa_st.snonce[0], g_wpa_st.snonce[1],
		    g_wpa_st.snonce[2], g_wpa_st.snonce[3],
		    g_wpa_st.anonce[0], g_wpa_st.anonce[1],
		    g_wpa_st.anonce[2], g_wpa_st.anonce[3],
		    replay[0], replay[1], replay[2], replay[3],
		    (unsigned)data_len);
		hif_dump_hex("WPA: m2", frame,
		    32u + n > 128u ? 128u : 32u + n);
	}
	(void)hif_wpa_tx_frame(frame, 32u + n, 1);
}

/* 802.11i PTK derivation; lower MAC first, then the lower nonce. */
static void
hif_wpa_derive(void)
{
	uint8_t d[76];
	const uint8_t own[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
	int own_low = memcmp(own, g_join_ap.b.bssid, 6) < 0;
	int nonce_low = memcmp(g_wpa_st.snonce, g_wpa_st.anonce, 32) < 0;

	memcpy(d, own_low ? own : g_join_ap.b.bssid, 6);
	memcpy(d + 6, own_low ? g_join_ap.b.bssid : own, 6);
	memcpy(d + 12, nonce_low ? g_wpa_st.snonce : g_wpa_st.anonce, 32);
	memcpy(d + 44, nonce_low ? g_wpa_st.anonce : g_wpa_st.snonce, 32);
	wpa_prf(g_wpa_st.pmk, 32, "Pairwise key expansion", d, sizeof(d),
	    g_wpa_st.ptk, sizeof(g_wpa_st.ptk));
}

/* wlan-wip wj_eapol(): message 1 -> 2, message 3 -> 4 + keys + authorize. */
static void
hif_wpa_handle_eapol(const uint8_t *e, uint32_t n)
{
	static uint8_t copy[512], plain[400];
	uint32_t total, data_len, kn, gtk_bytes = 0, gtk_id = 0, at;
	uint16_t info;
	int pairwise, ack, mic;
	uint8_t digest[20];
	const uint8_t *kd, *gtk = NULL;

	if (!g_wpa || !g_join_ap.secure || n < WPA_EAPOL_FIXED) return;
	if (e[1] != 3 || (e[4] != 2 && e[4] != 254)) return;
	total = 4u + ((uint32_t)e[2] << 8 | e[3]);
	if (total < WPA_EAPOL_FIXED || total > n || total > sizeof(copy)) return;
	info = (uint16_t)(e[5] << 8 | e[6]);
	data_len = (uint16_t)(e[97] << 8 | e[98]);
	if (WPA_EAPOL_FIXED + data_len > total) return;
	if ((info & 7u) != 2u) {
		HIF_LOG("WPA: key descriptor version %u not handled", info & 7u);
		return;
	}
	pairwise = (info & 0x0008u) != 0;
	ack = (info & 0x0080u) != 0;
	mic = (info & 0x0100u) != 0;
	g_wpa_st.eapol_ver = e[0];
	if (pairwise && ack && !mic) {
		uint32_t i, seed;

		memcpy(g_wpa_st.anonce, e + 17, 32);
		if (g_wpa_diag)
			hif_dump_hex("WPA: m1", e,
			    total > 128u ? 128u : total);
		seed = g_polls * 2654435761u ^ 0x9e3779b9u;
		for (i = 0; i < 32; i++) {
			seed ^= seed << 13;
			seed ^= seed >> 17;
			seed ^= seed << 5;
			g_wpa_st.snonce[i] = (uint8_t)seed;
		}
		wpa_pmk(g_wpa_pass, (uint32_t)strlen(g_wpa_pass),
		    g_join_ap.b.ssid, g_join_ap.b.ssid_len, g_wpa_st.pmk);
		hif_wpa_derive();
		g_wpa_st.stage = 1;
		hif_wpa_reply(0x010a, e + 9, g_wpa_st.snonce, g_join_ap.rsn,
		    (uint16_t)sizeof(g_join_ap.rsn));
		HIF_LOG("WPA: msg1 seen, msg2 sent (ANonce=%02x%02x%02x%02x...)",
		    g_wpa_st.anonce[0], g_wpa_st.anonce[1], g_wpa_st.anonce[2],
		    g_wpa_st.anonce[3]);
		return;
	}
	if (!mic || !g_wpa_st.stage) return;
	memcpy(copy, e, total);
	memset(copy + 81, 0, 16);
	wpa_hmac(g_wpa_st.ptk, 16, copy, total, digest);
	if (memcmp(digest, e + 81, 16)) {
		HIF_LOG("WPA: MIC mismatch - wrong passphrase?");
		return;
	}
	if (g_wpa_st.have_replay && memcmp(e + 9, g_wpa_st.replay, 8) < 0)
		return;
	memcpy(g_wpa_st.replay, e + 9, 8);
	g_wpa_st.have_replay = 1;
	kd = e + WPA_EAPOL_FIXED;
	kn = data_len;
	if (info & 0x1000u) {
		if (data_len > sizeof(plain) + 8u ||
		    wpa_unwrap(g_wpa_st.ptk + 16, e + WPA_EAPOL_FIXED, data_len,
		    plain)) {
			HIF_LOG("WPA: key data does not unwrap");
			return;
		}
		kd = plain;
		kn = data_len - 8u;
	}
	for (at = 0; at + 2u <= kn && at + 2u + kd[at + 1] <= kn;
	    at += 2u + kd[at + 1]) {
		if (kd[at] == 0xdd && kd[at + 1] >= 6 && kd[at + 2] == 0x00 &&
		    kd[at + 3] == 0x0f && kd[at + 4] == 0xac && kd[at + 5] == 1) {
			gtk_id = kd[at + 6] & 3u;
			gtk = kd + at + 8;
			gtk_bytes = kd[at + 1] - 6u;
		}
		if (!kd[at] && !kd[at + 1]) break;
	}
	if (pairwise && ack) {
		if (memcmp(e + 17, g_wpa_st.anonce, 32)) return;
		hif_wpa_reply(0x030a, e + 9, NULL, NULL, 0);
		/* A TKIP group key is 32 bytes, CCMP 16.  The pairwise key is
		 * always CCMP because that is what our RSN element asks for. */
		g_wpa_st.gtk_len = gtk && gtk_bytes >= 16 ?
		    (uint8_t)(gtk_bytes >= 32 ? 32 : 16) : 0;
		g_wpa_st.gtk_idx = (uint8_t)gtk_id;
		if (g_wpa_st.gtk_len)
			memcpy(g_wpa_st.gtk, gtk, g_wpa_st.gtk_len);
		hif_wpa_send_key(1, 0, g_wpa_st.ptk + 32, 16, 4);
		if (g_wpa_st.gtk_len)
			hif_wpa_send_key(0, g_wpa_st.gtk_idx, g_wpa_st.gtk,
			    g_wpa_st.gtk_len, g_wpa_st.gtk_len == 32 ? 2 : 4);
		hif_join_peer_param(3, 1);            /* PEER_AUTHORIZE */
		if (g_join_skip & 8) {
			HIF_LOG("WPA: keys in, sending the deferred VDEV_UP");
			hif_join_send_vdev_up();
		}
		g_wpa_st.keyed = 1;
		g_link_up = 1;
		/* The radio is on air.  configd waits for KEV_DL_LINK_ON before it
		 * builds the IPv4 service and runs DHCP; autojoin ran with 22 MSDUs
		 * delivered into the stack yet no SIOCSIFADDR ever appeared.
		 * HAKIOS_STA_LINK (3, 1) keeps wifid's status in sync. */
		hakios_net_link_state(true);
		hakios_apple80211_event(3, 1);
		HIF_LOG("WPA: msg3 seen, msg4 sent, keys installed, peer authorized, link up");
		return;
	}
	if (!pairwise && ack) {
		if (gtk && gtk_bytes >= 16)
			hif_wpa_send_key(0, gtk_id, gtk,
			    gtk_bytes >= 32 ? 32 : 16, gtk_bytes >= 32 ? 2 : 4);
		hif_wpa_reply(0x0302, e + 9, NULL, NULL, 0);
		HIF_LOG("WPA: group key updated");
	}
}

/* wlan-wip nw_wifi_connect(): the 802.11 stack (or wifiauto at boot) hands
 * down the SSID and passphrase as a pair.  The join state machine matches
 * that SSID exactly, so the PMK always belongs to the network we joined --
 * the chain-573 failure was a passphrase/SSID mismatch, not a bad MIC. */
extern "C" int
wcn3990_hif_connect(const uint8_t *ssid, size_t slen,
    const uint8_t *pass, size_t plen)
{
	char saved[64];
	size_t n = 0;

	if (!ssid || slen == 0 || slen > sizeof(g_join_req.ssid)) return -1;
	/* wifiauto passes g_wpa_pass itself as the source.  Save it BEFORE
	 * clearing the destination, or the memset wipes the passphrase too
	 * (chain-576 z1 logged "connect requested ssid_len=18 passlen=0"). */
	if (pass && plen) {
		n = plen < sizeof(saved) - 1u ? plen : sizeof(saved) - 1u;
		memcpy(saved, pass, n);
		saved[n] = 0;
	}
	memset(&g_join_req, 0, sizeof(g_join_req));
	memcpy(g_join_req.ssid, ssid, slen);
	g_join_req.ssid_len = (uint8_t)slen;
	memset(g_wpa_pass, 0, sizeof(g_wpa_pass));
	if (n) memcpy(g_wpa_pass, saved, n);
	g_join_req.pending = 1;
	g_join = 2;
	g_wpa = 1;
	HIF_LOG("JOIN: connect requested, ssid_len=%u passlen=%u",
	    (unsigned)slen, (unsigned)strlen(g_wpa_pass));
	return 0;
}

/* wlan-wip wj_mgmt_header(): a 24-byte management header to the AP. */
static void
hif_join_mgmt_header(uint8_t *f, uint8_t type)
{
	uint16_t seq;

	memset(f, 0, 24);
	f[0] = type;
	memcpy(f + 4, g_join_ap.b.bssid, 6);
	memcpy(f + 10, g_join_mac, 6);
	memcpy(f + 16, g_join_ap.b.bssid, 6);
	seq = (uint16_t)(g_join_seq++ << 4);
	f[22] = (uint8_t)seq;
	f[23] = (uint8_t)(seq >> 8);
}

/* wlan-wip wj_send_mgmt(): WMI_MGMT_TX_SEND, by reference.  The frame body
 * goes into one of eight rotating DMA slots; the WMI command carries the
 * slot's physical address plus the first <= 64 bytes inline. */
static void
hif_join_send_mgmt(const uint8_t *f, uint32_t n)
{
	struct {
		uint32_t vdev_id;
		uint32_t desc_id;
		uint32_t chanfreq;
		uint64_t paddr;
		uint32_t frame_len;
		uint32_t buf_len;
	} __attribute__((packed)) cmd;         /* wmi_tlv_mgmt_tx_cmd, 28 B */
	uint8_t tlv[128], inline_buf[64];
	uint8_t *p = tlv;
	uint32_t slot, inline_bytes;
	uint64_t pa;

	if (g_dma == NULL || n == 0 || n > HIF_JOIN_SLOT) return;
	slot = g_join_mgmt_next++ % HIF_JOIN_SLOTS;
	memcpy(g_dma->mgmt_tx[slot], f, n);
	hif_mb();      /* the CE fetches the slot by DMA, not through our map */
	pa = hif_pa_of(g_dma->mgmt_tx[slot]);
	inline_bytes = (n < 64u ? n : 64u);
	inline_bytes = (inline_bytes + 3u) & ~3u;
	memset(&cmd, 0, sizeof(cmd));
	cmd.vdev_id = 0;
	cmd.desc_id = slot;
	cmd.chanfreq = 0;
	cmd.paddr = pa;
	cmd.frame_len = n;
	cmd.buf_len = inline_bytes;
	memset(inline_buf, 0, sizeof(inline_buf));
	memcpy(inline_buf, f, n < inline_bytes ? n : inline_bytes);
	p = hif_put_tlv(p, WMI_TLV_MGMT_TX_CMD, (uint16_t)sizeof(cmd), &cmd);
	p = hif_put_tlv(p, WMI_TLV_ARRAY_BYTE, (uint16_t)inline_bytes,
	    inline_buf);
	g_join_mgmt_tx++;
	HIF_LOG("JOIN: MGMT_TX slot=%u pa=%llx frame=%u inline=%u (%u TLV bytes)",
	    slot, (unsigned long long)pa, n, inline_bytes,
	    (unsigned)(p - tlv));
	(void)hif_wmi_command(WMI_MGMT_TX_SEND_CMDID, tlv,
	    (uint32_t)(p - tlv));
}

/* wlan-wip wj_send_auth(): 30-byte open-system authentication, transaction 1. */
static void
hif_join_send_auth(void)
{
	uint8_t f[30];

	hif_join_mgmt_header(f, 0xb0);
	memset(f + 24, 0, 6);
	f[26] = 1;             /* open system, transaction 1 */
	hif_join_send_mgmt(f, sizeof(f));
}

/* wlan-wip wj_send_assoc(): capability, listen interval, SSID, rates, power
 * and channels if the AP manages spectrum, RSN, HT, VHT and WMM. */
static void
hif_join_send_assoc(void)
{
	static uint8_t f[600];
	uint8_t rates[16], nr = 0, pass;
	uint16_t apcap = g_join_ap.b.caps;
	int five = g_join_ap.b.freq_mhz >= 4900;
	uint16_t cap = (uint16_t)(1u | (g_join_ap.secure ? 0x10u : 0) |
	    (apcap & 0x20u) |
	    (!five ? (apcap & 0x400u) : 0) |
	    (five ? (apcap & 0x100u) : 0));
	uint32_t at = 28;

	hif_join_mgmt_header(f, 0x00);
	f[24] = (uint8_t)cap;
	f[25] = (uint8_t)(cap >> 8);
	f[26] = 10;            /* listen interval */
	f[27] = 0;
	f[at++] = 0;           /* SSID element */
	f[at++] = g_join_ap.b.ssid_len;
	memcpy(f + at, g_join_ap.b.ssid, g_join_ap.b.ssid_len);
	at += g_join_ap.b.ssid_len;
	/* The access point's rates that this radio has, plain (no basic bit). */
	for (pass = 0; pass < 2; pass++) {
		const uint8_t *e = hif_join_ie(pass ? 50 : 1);
		uint32_t i;

		for (i = 0; e && i < e[1] && nr < sizeof(rates); i++) {
			const uint8_t r = e[2 + i] & 0x7f;

			if (r == 2 || r == 4 || r == 11 || r == 22) {
				if (!five) rates[nr++] = r;
			} else if (r == 12 || r == 18 || r == 24 ||
			    r == 36 || r == 48 || r == 72 || r == 96 ||
			    r == 108)
				rates[nr++] = r;
		}
	}
	{
		const uint8_t first = nr < 8 ? nr : 8;

		f[at++] = 1;
		f[at++] = first;
		memcpy(f + at, rates, first);
		at += first;
		if (nr > 8) {
			f[at++] = 50;
			f[at++] = (uint8_t)(nr - 8u);
			memcpy(f + at, rates + 8, nr - 8u);
			at += nr - 8u;
		}
	}
	if (cap & 0x100u) {    /* spectrum managed: power + channels elements */
		static const uint8_t power[4] = { 33, 2, 0, 20 };
		static const uint8_t channels[10] =
		    { 36, 8, 36, 4, 52, 4, 100, 12, 149, 5 };

		memcpy(f + at, power, sizeof(power));
		at += sizeof(power);
		memcpy(f + at, channels, sizeof(channels));
		at += sizeof(channels);
	}
	if (g_join_ap.secure) {
		memcpy(f + at, g_join_ap.rsn, sizeof(g_join_ap.rsn));
		at += sizeof(g_join_ap.rsn);
	}
	if (g_join_ap.ht) {
		const uint16_t ht = g_join_ap.width >= 40 ? HIF_JOIN_HT_CAP :
		    HIF_JOIN_HT_CAP & ~0x42u;   /* no 40 MHz claim on 20 MHz */

		f[at++] = 45;
		f[at++] = 26;
		memset(f + at, 0, 26);
		f[at] = (uint8_t)ht;
		f[at + 1] = (uint8_t)(ht >> 8);
		f[at + 2] = 0x1b;
		f[at + 3] = f[at + 4] = 0xff;
		f[at + 15] = 1;    /* transmit MCS set defined */
		at += 26;
	}
	if (g_join_ap.vht) {
		const uint32_t vht = HIF_JOIN_VHT_CAP;

		f[at++] = 191;
		f[at++] = 12;
		f[at] = (uint8_t)vht;
		f[at + 1] = (uint8_t)(vht >> 8);
		f[at + 2] = (uint8_t)(vht >> 16);
		f[at + 3] = (uint8_t)(vht >> 24);
		f[at + 4] = 0xfa;
		f[at + 5] = 0xff;
		f[at + 6] = f[at + 7] = 0;
		f[at + 8] = 0xfa;
		f[at + 9] = 0xff;
		f[at + 10] = f[at + 11] = 0;
		at += 12;
	}
	if (g_join_ap.wmm) {
		static const uint8_t wmm[9] =
		    { 221, 7, 0x00, 0x50, 0xf2, 2, 0, 1, 0 };

		memcpy(f + at, wmm, sizeof(wmm));
		at += sizeof(wmm);
	}
	HIF_LOG("JOIN: assoc sent (%u bytes, cap=%04x)", at, cap);
	hif_join_send_mgmt(f, at);
}

/* wlan-wip wj_peer_param(): WMI_PEER_SET_PARAM. */
static void
hif_join_peer_param(uint32_t id, uint32_t value)
{
	struct {
		uint32_t vdev_id;
		uint8_t mac[8];        /* wmi_mac_addr is 8 bytes */
		uint32_t param_id;
		uint32_t value;
	} __attribute__((packed)) cmd;         /* 20 bytes */
	uint8_t tlv[32];
	uint8_t *p = tlv;

	memset(tlv, 0, sizeof(tlv));
	memset(&cmd, 0, sizeof(cmd));
	cmd.vdev_id = 0;
	memcpy(cmd.mac, g_join_ap.b.bssid, 6);
	cmd.param_id = id;
	cmd.value = value;
	p = hif_put_tlv(p, WMI_TLV_PEER_SET_PARAM_CMD, 20, &cmd);
	HIF_LOG("JOIN: peer param %u=%u", id, value);
	(void)hif_wmi_command(WMI_PEER_SET_PARAM_CMDID, tlv,
	    (uint32_t)(p - tlv));
}

/* wlan-wip wj_send_peer_assoc(): ath10k_peer_assoc_prepare - the AP's
 * beacon capabilities narrowed to this radio's, as peer_assoc_h_* fields. */
static void
hif_join_send_peer_assoc(void)
{
	uint32_t flags = 1u, rate_caps = 0, max_mpdu = 0, density = 0, nss = 1;
	uint32_t ht_caps = 0, vht_caps = 0, vht_rx = 0, vht_tx = 0;
	uint32_t vht_rx_max = 0, vht_tx_max = 0;
	uint8_t legacy[16], ht_rates[16], nl = 0, nh = 0;
	int five = g_join_ap.b.freq_mhz >= 4900;
	uint32_t phy;
	uint8_t tlv[256], *p = tlv, pass;
	uint32_t lr, hr;
	uint8_t *at;
	const uint8_t *ht = hif_join_ie(45);

	if (g_join_ap.secure) flags |= 4u;
	if (g_join_ap.wmm) flags |= 2u;
	for (pass = 0; pass < 2; pass++) {
		const uint8_t *e = hif_join_ie(pass ? 50 : 1);
		uint32_t i;

		for (i = 0; e && i < e[1] && nl < 12; i++) {
			const uint8_t r = e[2 + i] & 0x7f;
			const int cck = r == 2 || r == 4 || r == 11 || r == 22;

			if (cck && five) continue;
			if (cck || r == 12 || r == 18 || r == 24 ||
			    r == 36 || r == 48 || r == 72 || r == 96 ||
			    r == 108)
				legacy[nl++] = cck ? (r | 0x80) : r;
		}
	}
	if (g_join_ap.ht && ht && ht[1] >= 26) {
		uint32_t cap = (uint32_t)(ht[2] | ht[3] << 8);
		const uint8_t ampdu = ht[4];
		const uint8_t m0 = ht[5], m1 = ht[6];
		uint8_t d;
		uint32_t i;

		if (g_join_ap.width < 40) cap &= ~0x42u;
		flags |= 0x1000u;
		rate_caps |= 8u;
		max_mpdu = (1u << (13u + (ampdu & 3u))) - 1u;
		d = (ampdu >> 2) & 7u;
		density = d == 0 ? 0u : d <= 3 ? 1u :
		    d == 4 ? 2u : d == 5 ? 4u : d == 6 ? 8u : 16u;
		if (cap & 1u) flags |= 0x10000u;
		if (g_join_ap.width >= 40) {
			flags |= 0x2000u;
			rate_caps |= 2u;
		}
		if ((cap & 0x20u) || (cap & 0x40u)) rate_caps |= 4u;
		if (cap & 0x80u) {
			rate_caps |= 0x20u;
			flags |= 0x8000u;
		}
		if (cap & 0x300u) {
			rate_caps |= ((cap >> 8) & 3u) << 6;
			flags |= 0x8000u;
		}
		if (m1) rate_caps |= 1u;
		for (i = 0; i < 16; i++) {
			if (((i < 8 ? m0 : m1) >> (i % 8)) & 1u) {
				ht_rates[nh++] = (uint8_t)i;
				nss = i / 8u + 1u;
			}
		}
		if (!nh) for (nh = 0; nh < 8; nh++) ht_rates[nh] = nh;
		ht_caps = cap;
	}
	{
		const uint8_t *vht = hif_join_ie(191);

		if (g_join_ap.vht && vht && vht[1] >= 12) {
			uint32_t factor, mpdu;
			unsigned streams = 0, i;

			vht_caps = (uint32_t)(vht[2] | vht[3] << 8 |
			    vht[4] << 16 | vht[5] << 24);
			vht_caps &= ~(1u << 11 | 1u << 12 | 7u << 13 |
			    7u << 16 | 1u << 19 | 1u << 20);
			flags |= 0x2000000u;
			factor = (vht_caps >> 23) & 7u;
			mpdu = (1u << (13u + factor)) - 1u;
			if (mpdu > max_mpdu) max_mpdu = mpdu;
			if (g_join_ap.width == 80) flags |= 0x4000000u;
			vht_rx = (uint32_t)(vht[6] | vht[7] << 8);
			vht_rx_max = (uint32_t)(vht[8] | vht[9] << 8);
			vht_tx = (uint32_t)(vht[10] | vht[11] << 8);
			vht_tx_max = (uint32_t)(vht[12] | vht[13] << 8);
			/* Beyond two streams this radio has nothing. */
			vht_rx |= 0xfff0u;
			vht_tx |= 0xfff0u;
			for (i = 0; i < 2; i++)
				if (((vht_rx >> (2 * i)) & 3u) != 3u)
					streams = i + 1u;
			if (streams) nss = streams;
		}
	}
	if (nss > 2) nss = 2;
	phy = g_join_ap.mode;
	/* struct wmi_tlv_peer_assoc_cmd, 76 bytes, in wlan-wip's order:
	 * mac(8) vdev new_assoc assoc_id flags caps listen ht_caps max_mpdu
	 * density rate_caps nss vht_caps phy ht_info[2](zero) num_legacy
	 * num_ht. */
	memset(tlv, 0, sizeof(tlv));
	hif_put_tlv(p, WMI_TLV_PEER_ASSOC_COMPLETE_CMD, 76, NULL);
	memcpy(tlv + 4, g_join_ap.b.bssid, 6);          /* mac_addr */
	hif_put32(tlv + 4 + 8, 0);                      /* vdev_id */
	hif_put32(tlv + 16, 1);                         /* new_assoc */
	hif_put32(tlv + 20, g_join_ap.aid);             /* assoc_id */
	hif_put32(tlv + 24, flags);
	hif_put32(tlv + 28, g_join_ap.b.caps);
	hif_put32(tlv + 32, 10);                        /* listen_intval */
	hif_put32(tlv + 36, ht_caps);
	hif_put32(tlv + 40, max_mpdu);
	hif_put32(tlv + 44, density);
	hif_put32(tlv + 48, rate_caps);
	hif_put32(tlv + 52, nss);
	hif_put32(tlv + 56, vht_caps);
	hif_put32(tlv + 60, phy);
	/* +64..71 ht_info[2] stay zero */
	hif_put32(tlv + 72, nl);                        /* num_legacy_rates */
	hif_put32(tlv + 76, nh);                        /* num_ht_rates */
	lr = (nl + 3u) & ~3u;
	hr = (nh + 3u) & ~3u;
	at = hif_put_tlv(tlv + 80, WMI_TLV_ARRAY_BYTE, (uint16_t)lr, legacy);
	at = hif_put_tlv(at, WMI_TLV_ARRAY_BYTE, (uint16_t)hr, ht_rates);
	{
		uint32_t v[4];

		v[0] = vht_rx_max;
		v[1] = vht_rx;
		v[2] = vht_tx_max;
		v[3] = vht_tx;
		at = hif_put_tlv(at, WMI_TLV_VHT_RATE_SET, 16, v);
	}
	HIF_LOG("JOIN: peer_assoc flags=%08x caps=%08x mode=%u nss=%u "
	    "legacy=%u ht=%u ht_caps=%08x vht_caps=%08x mpdu=%u density=%u",
	    flags, rate_caps, phy, nss, nl, nh, ht_caps, vht_caps, max_mpdu,
	    density);
	(void)hif_wmi_command(WMI_PEER_ASSOC_CMDID, tlv, (uint32_t)(at - tlv));
	if (g_join_ap.ht && ht) {
		/* ath10k_smps_map */
		static const uint8_t smps[4] = { 1, 2, 0, 0 };

		hif_join_peer_param(1, smps[(ht_caps >> 2) & 3u]);
	}
}

/* wlan-wip wj_send_vdev_up(): VDEV_UP with the associated AID and BSSID. */
static void
hif_join_send_vdev_up(void)
{
	struct {
		uint32_t vdev_id;
		uint32_t assoc_id;
		uint8_t bssid[8];      /* wmi_mac_addr is 8 bytes */
	} __attribute__((packed)) cmd;
	uint8_t tlv[32];
	uint8_t *p = tlv;

	memset(tlv, 0, sizeof(tlv));
	memset(&cmd, 0, sizeof(cmd));
	cmd.vdev_id = 0;
	cmd.assoc_id = g_join_ap.aid;
	memcpy(cmd.bssid, g_join_ap.b.bssid, 6);
	p = hif_put_tlv(p, WMI_TLV_VDEV_UP_CMD, 16, &cmd);
	HIF_LOG("JOIN: vdev up sent (aid=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x)",
	    g_join_ap.aid, (unsigned)g_join_ap.b.bssid[0],
	    (unsigned)g_join_ap.b.bssid[1], (unsigned)g_join_ap.b.bssid[2],
	    (unsigned)g_join_ap.b.bssid[3], (unsigned)g_join_ap.b.bssid[4],
	    (unsigned)g_join_ap.b.bssid[5]);
	(void)hif_wmi_command(WMI_VDEV_UP_CMDID, tlv, (uint32_t)(p - tlv));
}

static void
hif_join_reset(const char *why)
{
	HIF_LOG("JOIN: stopped in state %d (%s)", g_join_state, why);
	if (g_link_up) {
		/* The session is gone: withdraw the link so configd tears the
		 * IPv4 service down instead of DHCP-ing into a dead radio. */
		g_link_up = 0;
		hakios_net_link_state(false);
		hakios_apple80211_event(3, 0);
	}
	memset(&g_join_ap, 0, sizeof(g_join_ap));
	g_join_state = HIF_JOIN_IDLE;
	g_join_tries = 0;
	/* Back off 10 s before the next attempt: an AP that answered
	 * "temporarily rejected" (or went silent) needs its stale session
	 * entry to expire; hammering it every scan round keeps it refusing. */
	g_join_backoff_at = g_polls + 10000u;
	if (g_wifi_auto && g_join_req.ssid_len != 0) {
		/* wlan-wip wj_step() re-arms ws_scan_wanted while WJ_IDLE.
		 * hif_join_begin() cleared pending when it picked the BSS, which
		 * also disabled the 10 s rescan loop; g_autoscan_sent stayed 1,
		 * so no further scan-completed event could trigger a rejoin.
		 * After one deauth (frame 0xc0 reason 3 in live-dnsfix-status.cap)
		 * the daily-use baseline stayed join=0 forever with ord/msdu frozen.
		 * Re-arm both gates; the 10 s backoff above still applies. */
		g_join_req.pending = 1;
		g_autoscan_sent = 0;
		g_scan_active = 0;
		g_last_rescan_at = g_polls;
		HIF_LOG("JOIN: wifiauto re-arming connect after %s", why);
	}
}

/* ---- the wj_begin() once-block (chain-537 gap) ---------------------------
 * The reference sends these eight parameters exactly once per radio before
 * the first VDEV_START: TX power limits, the vdev's NSS / TX_ENCAP_TYPE /
 * RTS threshold, the three STA power-save policies and STA_POWERSAVE_MODE
 * off.  TX_ENCAP_TYPE decides how the firmware frames data-path MSDUs; our
 * join path never sent any of them, and chain-537 showed the firmware dying
 * ~20 s after vdev up with no HTT frame delivered. */
static int
hif_join_vdev_param(uint32_t id, uint32_t value)
{
	uint8_t tlv[32];
	uint8_t *p = tlv;
	struct { uint32_t vdev_id, param_id, value; } cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.vdev_id = 0;
	cmd.param_id = id;
	cmd.value = value;
	p = hif_put_tlv(p, WMI_TLV_VDEV_SET_PARAM, (uint16_t)sizeof(cmd), &cmd);
	HIF_LOG("JOIN: vdev param %u=%u", id, value);
	return hif_wmi_command(WMI_VDEV_SET_PARAM_CMDID, tlv,
	    (uint32_t)(p - tlv));
}

static int
hif_join_pdev_param(uint32_t id, uint32_t value)
{
	uint8_t tlv[32];
	uint8_t *p = tlv;
	struct { uint32_t pdev_id, param_id, value; } cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.pdev_id = 0;
	cmd.param_id = id;
	cmd.value = value;
	p = hif_put_tlv(p, WMI_TLV_PDEV_SET_PARAM, (uint16_t)sizeof(cmd), &cmd);
	HIF_LOG("JOIN: pdev param %u=%u", id, value);
	return hif_wmi_command(WMI_PDEV_SET_PARAM_CMDID, tlv,
	    (uint32_t)(p - tlv));
}

static int
hif_join_sta_ps(uint32_t id, uint32_t value)
{
	uint8_t tlv[32];
	uint8_t *p = tlv;
	struct { uint32_t vdev_id, param_id, value; } cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.vdev_id = 0;
	cmd.param_id = id;
	cmd.value = value;
	p = hif_put_tlv(p, WMI_TLV_STA_POWERSAVE_PARAM, (uint16_t)sizeof(cmd),
	    &cmd);
	HIF_LOG("JOIN: sta ps %u=%u", id, value);
	return hif_wmi_command(WMI_STA_POWERSAVE_PARAM_CMDID, tlv,
	    (uint32_t)(p - tlv));
}

static int
hif_join_sta_powersave_mode(uint32_t mode)
{
	uint8_t tlv[24];
	uint8_t *p = tlv;
	struct { uint32_t vdev_id, mode; } cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.vdev_id = 0;
	cmd.mode = mode;
	p = hif_put_tlv(p, WMI_TLV_STA_POWERSAVE_MODE, (uint16_t)sizeof(cmd),
	    &cmd);
	HIF_LOG("JOIN: STA_POWERSAVE_MODE %u", mode);
	return hif_wmi_command(WMI_STA_POWERSAVE_MODE_CMDID, tlv,
	    (uint32_t)(p - tlv));
}

static void
hif_join_once(void)
{
	if (g_join_once) return;
	g_join_once = 1;
	/* wlan-wip wj_begin(): ath10k_add_interface's settings, for the one
	 * vdev.  Half-dB power units, native Wi-Fi encapsulation, and the
	 * station kept awake (RX wake policy, TX wake threshold, no PSPOLL
	 * maximum, power save off) because the bring-up polls, no IRQs.
	 * joinonce=<n> gates each command: chain-538 sent all nine and auth
	 * regressed to status=3 (534/535/537 sent none and auth worked), so
	 * the bisect runs per command.  bit1/2 power, bit4 NSS, bit8
	 * TX_ENCAP_TYPE, bit16 RTS, bit32/64/128 sta_ps, bit256 ps_mode. */
	if (g_join_once_mask & 0x01u)
		(void)hif_join_pdev_param(0x3, 40);   /* TXPOWER_LIMIT2G */
	if (g_join_once_mask & 0x02u)
		(void)hif_join_pdev_param(0x4, 40);   /* TXPOWER_LIMIT5G */
	if (g_join_once_mask & 0x04u)
		(void)hif_join_vdev_param(34, 2);     /* NSS */
	if (g_join_once_mask & 0x08u)
		(void)hif_join_vdev_param(48, 1);     /* TX_ENCAP_TYPE */
	if (g_join_once_mask & 0x10u)
		(void)hif_join_vdev_param(1, 2347);   /* RTS_THRESHOLD */
	if (g_join_once_mask & 0x20u)
		(void)hif_join_sta_ps(0, 0);          /* RX_WAKE_POLICY: wake */
	if (g_join_once_mask & 0x40u)
		(void)hif_join_sta_ps(1, 1);          /* TX_WAKE_THRESHOLD */
	if (g_join_once_mask & 0x80u)
		(void)hif_join_sta_ps(2, 0);          /* PSPOLL_COUNT */
	if (g_join_once_mask & 0x100u)
		(void)hif_join_sta_powersave_mode(0); /* off */
}

/* ---- join prerequisites (chain-532c) ------------------------------------
 * With only VDEV_CREATE in place, every MGMT_TX_SEND came back status=3.
 * The same-chip reference (wlan-wip WlanRadio.c wj_step) does
 *   VDEV_START -> PEER_CREATE -> wait for HTT peer map -> auth
 * because the target needs a home channel and a peer entry before it will
 * transmit a management frame.  These steps are gated on join>=2; join=1
 * keeps the old direct-auth path as the single-variable control.
 */
static int
hif_join_send_vdev_start(void)
{
	uint8_t tlv[160];
	uint8_t *p = tlv;
	struct hif_vdev_start_cmd cmd;
	uint8_t ch[24];
	const uint8_t *tim = hif_join_ie(5);

	memset(tlv, 0, sizeof(tlv));
	memset(&cmd, 0, sizeof(cmd));
	memset(ch, 0, sizeof(ch));
	cmd.vdev_id = 0;
	cmd.requestor_id = 0;
	cmd.bcn_intval = g_join_ap.b.beacon_int ? g_join_ap.b.beacon_int : 100u;
	cmd.dtim_period = (tim && tim[1] >= 2) ? tim[3] : 1u;
	cmd.ssid_len = g_join_ap.b.ssid_len;
	memcpy(cmd.ssid, g_join_ap.b.ssid, g_join_ap.b.ssid_len);
	/* struct wmi_tlv_channel, 24 bytes: mhz, centre, 0, mode,
	 * max/reg power, min/max tx power (wlan-wip wj_send_vdev_start). */
	hif_put32(ch + 0, g_join_ap.b.freq_mhz);
	hif_put32(ch + 4, g_join_ap.center_mhz);
	hif_put32(ch + 12, g_join_ap.mode);
	hif_put32(ch + 16, 40u << 8 | 40u << 16);
	hif_put32(ch + 20, 6u | 40u << 8);
	p = hif_put_tlv(p, WMI_TLV_VDEV_START_CMD, (uint16_t)sizeof(cmd), &cmd);
	p = hif_put_tlv(p, WMI_TLV_CHANNEL, (uint16_t)sizeof(ch), ch);
	p = hif_put_tlv(p, WMI_TLV_ARRAY_STRUCT, 0, NULL);
	HIF_LOG("JOIN: VDEV_START freq=%u centre=%u mode=%u ssid '%.*s'",
	    g_join_ap.b.freq_mhz, g_join_ap.center_mhz, g_join_ap.mode,
	    (int)g_join_ap.b.ssid_len, (const char *)g_join_ap.b.ssid);
	return hif_wmi_command(WMI_VDEV_START_CMDID, tlv, (uint32_t)(p - tlv));
}

static int
hif_join_send_peer_create(void)
{
	uint8_t tlv[32];
	uint8_t *p = tlv;
	struct {
		uint32_t vdev_id;
		uint8_t mac[8];        /* struct wmi_mac_addr is 8 bytes */
		uint32_t peer_type;
	} __attribute__((packed)) cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.vdev_id = 0;
	memcpy(cmd.mac, g_join_ap.b.bssid, 6);
	cmd.peer_type = 0;            /* WMI_TLV_PEER_TYPE_DEFAULT */
	p = hif_put_tlv(p, WMI_TLV_PEER_CREATE_CMD, (uint16_t)sizeof(cmd), &cmd);
	HIF_LOG("JOIN: PEER_CREATE %02x:%02x:%02x:%02x:%02x:%02x",
	    (unsigned)g_join_ap.b.bssid[0], (unsigned)g_join_ap.b.bssid[1],
	    (unsigned)g_join_ap.b.bssid[2], (unsigned)g_join_ap.b.bssid[3],
	    (unsigned)g_join_ap.b.bssid[4], (unsigned)g_join_ap.b.bssid[5]);
	return hif_wmi_command(WMI_PEER_CREATE_CMDID, tlv, (uint32_t)(p - tlv));
}

/* wlan-wip wj_peer_event(): HTT T2H peer map, MAC at +4, peer id at +2. */
static void
hif_join_peer_event(const uint8_t *p, uint32_t n)
{
	if (p[0] == HTT_T2H_MSG_TYPE_PEER_MAP && n >= 10 &&
	    !memcmp(p + 4, g_join_ap.b.bssid, 6)) {
		g_join_peer_mapped = 1;
		g_join_peer_id = (uint16_t)(p[2] | (uint16_t)p[3] << 8);
		HIF_LOG("JOIN: peer mapped id=%u", (unsigned)g_join_peer_id);
	}
}

/* wlan-wip wj_begin() narrowed to this phase: pick the BSS table entry,
 * derive the link plan from its beacon and send the authentication frame. */
static void
hif_join_begin(void)
{
	const struct hif_join_bss *b;
	const uint8_t *rsn;
	uint32_t pick;

	if (g_join >= 2) {
		/* Hidden / malformed entries (empty SSID) are not usable auth
		 * targets.  chain-532c saw bss[1] with an empty SSID and a bogus
		 * BSSID; the reference picks by SSID and signal strength.
		 * joinsel pins the choice by SSID because join=<n> depends on
		 * the order of first hearing, which changes every boot. */
		static const char *const sel_ssid[4] = {
			NULL, "ChinaUnicom", "1902_WIFI_CLOCK",
			"ChinaUnicom-33E6TD-5G"
		};
		const char *want;
		uint32_t i;

		want = (g_join_sel >= 1 && g_join_sel <= 3) ? sel_ssid[g_join_sel] : NULL;
		pick = g_join_bss_count;
		if (g_join_req.pending) {
			/* wlan-wip wj_begin(): match the requested SSID exactly,
			 * else wait for it to appear.  A wrong passphrase cannot
			 * be masked by "any visible AP" here, so the four-way
			 * handshake always uses the credentials of the network
			 * it actually joined. */
			for (i = 0; i < g_join_bss_count; i++) {
				const struct hif_join_bss *c = &g_join_bss[i];

				if (c->freq_mhz >= 4900) continue;
				if (c->ssid_len == g_join_req.ssid_len &&
				    !memcmp(c->ssid, g_join_req.ssid,
				    c->ssid_len)) {
					pick = i;
					break;
				}
			}
			if (pick == g_join_bss_count) {
				static uint32_t last_wait;

				if (g_polls - last_wait > 5000u) {
					last_wait = g_polls;
					HIF_LOG("JOIN: connect waiting for the "
					    "requested network (%u heard)",
					    g_join_bss_count);
				}
				return;
			}
		} else if (g_join_sel == 4) {
			/* Any visible 2.4 GHz AP: the experiment must not depend on
			 * one particular router being in the BSS table (chain-564/n2
			 * lost 220 s to a 5 GHz fallback that always fails auth).
			 * A wrong passphrase still receives EAPOL msg1, which is
			 * exactly the data-plane checkpoint we need. */
			for (i = 0; i < g_join_bss_count; i++) {
				const struct hif_join_bss *c = &g_join_bss[i];

				if (c->ssid_len == 0 || c->freq_mhz >= 4900)
					continue;
				pick = i;
				break;
			}
		} else if (want) {
			size_t wl = strlen(want);

			/* Two passes: the SSID can exist on both bands, and the
			 * 5 GHz BSS rejects MGMT_TX (status 3) on this firmware,
			 * so prefer the 2.4 GHz entry when there is one. */
			{
				unsigned pass2;

				/* joinsel=1 pins ChinaUnicom: never fall back to its
				 * 5 GHz BSS, whose MGMT_TX this firmware rejects
				 * (status=3, chain-564 n2). */
				for (pass2 = 0;
				    pass2 < (g_join_sel == 1 ? 1u : 2u) &&
				    pick == g_join_bss_count; pass2++) {
					for (i = 0; i < g_join_bss_count; i++) {
						const struct hif_join_bss *c =
						    &g_join_bss[i];

						if (c->ssid_len < wl ||
						    memcmp(c->ssid, want, wl))
							continue;
						if (pass2 == 0 &&
						    c->freq_mhz >= 4900)
							continue;
						pick = i;
						break;
					}
				}
			}
			if (pick == g_join_bss_count) {
				/* Do NOT fall back to another access point:
				 * joinsel is meant to pin one SSID, and a
				 * fallback silently joined TP-LINK in
				 * chain-548 while the clock AP had not been
				 * heard yet. */
				static uint32_t last_log;

				if (g_polls - last_log > 5000u) {
					last_log = g_polls;
					HIF_LOG("JOIN: joinsel=%d waiting for "
					    "'%s' (%u entries heard)",
					    g_join_sel, want, g_join_bss_count);
				}
				return;
			}
		} else {
			for (pick = 0; pick < g_join_bss_count; pick++)
				if (g_join_bss[pick].ssid_len) break;
		}
		if (pick == g_join_bss_count) return;
		g_join_req.pending = 0;
		b = &g_join_bss[pick];
	} else {
		if (g_join == 0 || (uint32_t)g_join > g_join_bss_count) return;
		b = &g_join_bss[g_join - 1];
	}

	memset(&g_join_ap, 0, sizeof(g_join_ap));
	memcpy(&g_join_ap.b, b, sizeof(*b));
	g_join_ap.wmm = (uint8_t)hif_join_has_wmm();
	hif_join_channel_plan();
	rsn = hif_join_ie(48);
	g_join_ap.secure = rsn != NULL;
	if (g_join_ap.secure && hif_join_rsn_prepare(rsn)) {
		/* No valid WPA2-PSK RSN (CCMP capable, PSK AKM, no PMF):
		 * authenticating anyway only produced assoc status 41 - the
		 * request has to carry the AP's own group cipher. */
		HIF_LOG("JOIN: unusable RSN (need PSK + CCMP pairwise)");
		g_join_state = HIF_JOIN_IDLE;
		memset(&g_join_ap, 0, sizeof(g_join_ap));
		return;
	}
	g_join_seq = 0;
	g_join_tries = 0;
	g_join_tx_at = g_join_wait_at = g_polls;
	g_join_state = HIF_JOIN_IDLE;   /* set below once the path is chosen */
	{
		const uint8_t *re = hif_join_ie(48);
		uint32_t ri, rn = re ? re[1] : 0;

		if (re && rn > 22) rn = 22;
		if (re) {
			char hx[3 * 22 + 1];
			uint32_t k = 0;

			for (ri = 0; ri < rn; ri++) {
				static const char h[] = "0123456789abcdef";

				hx[k++] = h[re[2 + ri] >> 4];
				hx[k++] = h[re[2 + ri] & 15];
				hx[k++] = ' ';
			}
			hx[k] = 0;
			HIF_LOG("JOIN: ap rsn len=%u %s", re[1], hx);
		}
	}
	HIF_LOG("JOIN: entry %d bssid %02x:%02x:%02x:%02x:%02x:%02x "
	    "ssid '%.*s' freq %u rssi %d, %u MHz wide at %u mode %u ht %u "
	    "vht %u wmm %u, %s",
	    g_join, (unsigned)b->bssid[0], (unsigned)b->bssid[1],
	    (unsigned)b->bssid[2], (unsigned)b->bssid[3],
	    (unsigned)b->bssid[4], (unsigned)b->bssid[5],
	    b->ssid_len, b->ssid, b->freq_mhz, (int)b->signal_dbm,
	    g_join_ap.width, g_join_ap.center_mhz, g_join_ap.mode,
	    g_join_ap.ht, g_join_ap.vht, g_join_ap.wmm,
	    g_join_ap.secure ? "WPA2" : "open");
	if (g_join >= 2) {
		hif_join_once();
		g_join_started = 0;
		g_join_peer_mapped = 0;
		g_join_state = HIF_JOIN_VDEV_START;
		(void)hif_join_send_vdev_start();
	} else {
		g_join_state = HIF_JOIN_AUTH_SENT;
		HIF_LOG("JOIN: auth sent");
		hif_join_send_auth();
	}
}

/* wlan-wip wj_step()'s WJ_AUTH/WJ_ASSOC timing: retransmit every 300 ms,
 * give up after 8 tries or 3 s in one state.  Driven by the 1 ms poll. */
static void
hif_join_poll(void)
{
	if (g_state != HIF_READY || g_join_state == HIF_JOIN_IDLE)
		return;
	if (g_join_state == HIF_JOIN_ASSOC_OK) {
		if (g_vdev_up_pending &&
		    g_polls - g_join_wait_at >= (uint32_t)g_vdev_wait_ms) {
			g_vdev_up_pending = 0;
			HIF_LOG("JOIN: deferred VDEV_UP now (%d ms after "
			    "peer_assoc)", g_vdev_wait_ms);
			hif_join_send_vdev_up();
		}
		return;
	}

	/* wlan-wip wj_step(): do not send auth until the target has accepted
	 * VDEV_START and mapped the AP's peer.  These two states never reach
	 * the retransmit path below. */
	if (g_join_state == HIF_JOIN_VDEV_START) {
		if (g_join_started == 1) {
			g_join_state = HIF_JOIN_PEER_CREATE;
			g_join_wait_at = g_polls;
			/* joinskip bit4 isolates PEER_CREATE itself (the target
			 * never answers with an HTT peer map either way). */
			if (!(g_join_skip & 4))
				(void)hif_join_send_peer_create();
			/* wlan-wip sends these two in the WJ_PEER branch, i.e. only
			 * after the target mapped the peer.  Our target never sends
			 * a peer map, so send them here instead: they are the
			 * "bss_info_changed before the first frame" values and the
			 * only assoc-side vdev params missing from our sequence. */
			hif_join_vdev_param(7,
			    (g_join_ap.b.caps & 0x400u) ? 2u : 1u); /* SLOT_TIME */
			hif_join_vdev_param(8,
			    (g_join_ap.b.caps & 0x20u) ? 2u : 1u);  /* PREAMBLE */
		} else if (g_join_started == 2 ||
		    g_polls - g_join_wait_at >= 3000) {
			hif_join_reset("vdev start refused or timed out");
		}
		return;
	}
	if (g_join_state == HIF_JOIN_PEER_CREATE) {
		if (g_join_peer_mapped) {
			g_join_state = HIF_JOIN_AUTH_SENT;
			g_join_tries = 0;
			g_join_tx_at = g_join_wait_at = g_polls;
			HIF_LOG("JOIN: auth sent (peer mapped)");
			hif_join_send_auth();
		} else if (g_polls - g_join_wait_at >= 3000) {
			/* Diagnostic fallback: if this firmware never emits an HTT peer
			 * map, still send auth once so a single boot answers whether
			 * VDEV_START alone was enough.  The log line makes the two
			 * cases distinguishable. */
			HIF_LOG("JOIN: peer map timeout, sending auth anyway "
			    "(diagnostic)");
			g_join_state = HIF_JOIN_AUTH_SENT;
			g_join_tries = 0;
			g_join_tx_at = g_join_wait_at = g_polls;
			hif_join_send_auth();
		}
		return;
	}
	if (g_polls - g_join_tx_at >= 300) {
		if (++g_join_tries >= 8) {
			hif_join_reset("no answer after 8 tries");
			return;
		}
		if (g_join_state == HIF_JOIN_AUTH_SENT) {
			HIF_LOG("JOIN: auth retransmit %u", g_join_tries);
			hif_join_send_auth();
		} else {
			HIF_LOG("JOIN: assoc retransmit %u", g_join_tries);
			hif_join_send_assoc();
		}
		g_join_tx_at = g_polls;
	}
	if (g_polls - g_join_wait_at >= 3000)
		hif_join_reset("timeout in state");
}

/* The event's channel: this firmware's MGMT_RX_HDR reports MHz (existing
 * hif_handle_mgmt_rx logs it as freq).  The wlan-wip reference received a
 * channel NUMBER and converted ch14=2484 / <14 -> 2407+5ch / 5G -> 5000+5ch;
 * keep that path in case a value below 1000 arrives. */
static uint32_t
hif_join_freq(uint32_t channel)
{
	if (channel >= 1000) return channel;
	if (channel == 14) return 2484;
	if (channel < 14) return 2407 + 5u * channel;
	return 5000 + 5u * channel;
}

/* wlan-wip ws_bss_note(): cfg80211_inform_bss from a beacon or probe
 * response - the fixed fields and the whole elements after the 24-byte
 * header and 12 fixed bytes.  Linear scan for the BSSID. */
static void
hif_join_bss_note(const uint8_t *f, uint32_t n, uint32_t channel,
    int32_t rssi)
{
	const uint8_t *bssid, *ies;
	uint32_t ie_len, kept = 0, at, freq, has_rsn = 0;
	struct hif_join_bss *b;

	if (n < 36 || channel == 0) return;
	bssid = f + 16;
	ies = f + 36;
	ie_len = n - 36;
	/* Whole elements only: a truncated last one would read as malformed. */
	while (kept + 2u <= ie_len &&
	    kept + 2u + ies[kept + 1] <= ie_len &&
	    kept + 2u + ies[kept + 1] <= HIF_JOIN_IE_MAX)
		kept += 2u + ies[kept + 1];
	freq = hif_join_freq(channel);
	at = 0;
	while (at < g_join_bss_count &&
	    memcmp(g_join_bss[at].bssid, bssid, 6)) at++;
	/* The old check compared `at` against the array size, which can only
	 * be true once count already reached it - i.e. after the out-of-bounds
	 * write.  Guard the append instead. */
	if (at == g_join_bss_count) {
		if (g_join_bss_count >= HIF_JOIN_BSS_MAX) return;
		g_join_bss_count++;
	}
	b = &g_join_bss[at];
	memset(b, 0, sizeof(*b));
	memcpy(b->bssid, bssid, 6);
	b->freq_mhz = freq;
	b->signal_dbm = (int8_t)rssi;
	b->beacon_int = (uint16_t)(f[32] | (uint16_t)f[33] << 8);
	b->caps = (uint16_t)(f[34] | (uint16_t)f[35] << 8);
	b->ie_len = (uint16_t)kept;
	memcpy(b->ie, ies, kept);
	if (kept >= 2 && ies[0] == 0 && ies[1] <= 32) {
		b->ssid_len = ies[1];
		memcpy(b->ssid, ies + 2, ies[1]);
	}
	if (at == g_join_bss_count - 1) {      /* first time heard */
		uint32_t s;

		for (s = 0; s + 2u <= b->ie_len &&
		    s + 2u + b->ie[s + 1] <= b->ie_len;
		    s += 2u + b->ie[s + 1])
			if (b->ie[s] == 48) has_rsn = 1;
		HIF_LOG("JOIN: bss[%u] %02x:%02x:%02x:%02x:%02x:%02x "
		    "ssid '%.*s' freq %u rssi %d beacons=%u %s",
		    at + 1, (unsigned)bssid[0], (unsigned)bssid[1],
		    (unsigned)bssid[2], (unsigned)bssid[3],
		    (unsigned)bssid[4], (unsigned)bssid[5], b->ssid_len,
		    (const char *)b->ssid, freq, (int)rssi, g_beacons,
		    has_rsn ? "rsn" : "open");
		/* The target entry may be heard after the scan-completed
		 * event of the boot's only autoscan round; joining then
		 * would wait forever.  Second trigger path: the moment the
		 * named entry exists. */
		if (g_join && g_state == HIF_READY &&
		    g_join_state == HIF_JOIN_IDLE &&
		    g_polls >= g_join_backoff_at &&
		    (g_join >= 2 ? b->ssid_len != 0 :
		     at + 1 == (uint32_t)g_join))
			hif_join_begin();
	}
}

/* wlan-wip wj_mgmt(): the access point's answers to our join frames. */
static void
hif_join_resp(const uint8_t *f, uint32_t n)
{
	uint8_t type;

	if (n < 24 || g_join_state < HIF_JOIN_AUTH_SENT ||
	    g_join_state > HIF_JOIN_ASSOC_OK ||
	    memcmp(f + 10, g_join_ap.b.bssid, 6))
		return;
	type = f[0] & 0xfc;
	if (type == 0xb0 && g_join_state == HIF_JOIN_AUTH_SENT && n >= 30) {
		uint16_t transaction, status;

		transaction = (uint16_t)(f[26] | (uint16_t)f[27] << 8);
		if (transaction != 2) return;
		status = (uint16_t)(f[28] | (uint16_t)f[29] << 8);
		hif_dump_hex("JOIN auth resp", f, n < 32 ? n : 32);
		HIF_LOG("JOIN: auth resp status=%d after %u tries",
		    (int)status, g_join_tries + 1u);
		if (status) {
			hif_join_reset("authentication refused");
			return;
		}
		g_join_tries = 0;
		g_join_tx_at = g_join_wait_at = g_polls;
		g_join_state = HIF_JOIN_ASSOC_SENT;
		hif_join_send_assoc();
		return;
	}
	if ((type == 0x10 || type == 0x30) &&
	    g_join_state == HIF_JOIN_ASSOC_SENT && n >= 30) {
		uint16_t status = (uint16_t)(f[26] | (uint16_t)f[27] << 8);

		hif_dump_hex("JOIN assoc resp", f, n < 40 ? n : 40);
		HIF_LOG("JOIN: assoc resp status=%d aid=%d", (int)status,
		    (int)((f[28] | (uint16_t)f[29] << 8) & 0x3fffu));
		if (status) {
			hif_join_reset("association refused");
			return;
		}
		g_join_ap.aid =
		    (uint16_t)((f[28] | (uint16_t)f[29] << 8) & 0x3fffu);
		/* ath10k_bss_assoc: peer assoc, vdev up, the ps workaround.
		 * joinskip isolates the crash: bit1 = do not send PEER_ASSOC,
		 * bit2 = do not send VDEV_UP.  Both runs still associate, so
		 * the A/B answers "which command kills the firmware". */
		if (!(g_join_skip & 1)) hif_join_send_peer_assoc();
		/* bit8 defers VDEV_UP until the four-way handshake installed the
		 * keys: the target is known to die when VDEV_UP runs with an
		 * unauthorized peer, and wlan-wip's own order was never verified
		 * on hardware. */
		if (!(g_join_skip & 2) && !(g_join_skip & 8)) {
			if (g_vdev_wait_ms) {
				g_vdev_up_pending = 1;
				g_join_wait_at = g_polls;
				HIF_LOG("JOIN: VDEV_UP armed with a %d ms delay",
				    g_vdev_wait_ms);
			} else {
				hif_join_send_vdev_up();
			}
		}
		if (g_join_skip)
			HIF_LOG("JOIN: joinskip=%d in effect", g_join_skip);
		/* wlan-wip has no PEER_SET_PARAM after vdev up: its only
		 * authorize is peer_param(3,1) in wj_step's WJ_M4, *after* the
		 * EAPOL keys are installed, and an open network never sends
		 * one.  chain-535 sent peer param id 255 right after vdev up;
		 * 255 is WMI_TLV_PEER_DUMMY_VAR, and the firmware answered the
		 * 0x401 early-crash message, then the kernel panicked. */
		if (g_join_ap.secure)
			HIF_LOG("JOIN: secure AP, %s", g_wpa ?
			    "waiting for EAPOL message 1" :
			    "key handshake not in this phase, stopping after vdev up");
		else {
			hif_join_peer_param(3, 1);  /* PEER_AUTHORIZE */
			/* An open network is on the air with no key handshake. */
			g_link_up = 1;
			hakios_net_link_state(true);
			hakios_apple80211_event(3, 1);
			HIF_LOG("JOIN: open AP authorized, link up");
		}
		/* authz=1: open the secured port before the AP's first EAPOL.
		 * Gated so the reference ordering is still the control. */
		if (g_authz && g_join_ap.secure) {
			HIF_LOG("JOIN: early PEER_AUTHORIZE (authz=1)");
			hif_join_peer_param(3, 1);
		}
		g_join_tx_at = g_join_wait_at = g_polls;
		g_join_state = HIF_JOIN_ASSOC_OK;
		return;
	}
	if (type == 0xc0 || type == 0xa0) {
		uint16_t reason = n >= 26 ?
		    (uint16_t)(f[24] | (uint16_t)f[25] << 8) : 0;

		HIF_LOG("JOIN: access point ended it, frame 0x%x reason %u",
		    type, reason);
		hif_join_reset("deauth/disassoc");
	}
}

/* ========================= join (end) ======================= */

static void
hif_handle_mgmt_rx(const uint8_t *tlvs, uint32_t len)
{
	uint32_t off = 0;
	uint32_t channel = 0, buf_len = 0;
	int32_t rssi = -60;
	const uint8_t *frame = NULL;
	uint32_t frame_len = 0;

	g_mgmt_events++;
	if (g_mgmt_events <= 8) {
		HIF_LOG("MGMT ev #%u len=%u hdr=%02x%02x%02x%02x "
		    "%02x%02x%02x%02x",
		    g_mgmt_events, len,
		    len > 0 ? tlvs[0] : 0, len > 1 ? tlvs[1] : 0,
		    len > 2 ? tlvs[2] : 0, len > 3 ? tlvs[3] : 0,
		    len > 4 ? tlvs[4] : 0, len > 5 ? tlvs[5] : 0,
		    len > 6 ? tlvs[6] : 0, len > 7 ? tlvs[7] : 0);
	}

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
		/* Same exact-length walk as ath10k_wmi_tlv_iter(). */
		off += 4 + (g_tlv_pad ? ((tlv_len + 3) & ~3u) : tlv_len);
	}

	if (!frame || !buf_len || buf_len > frame_len) {
		if (g_mgmt_events <= 8)
			HIF_LOG("MGMT drop: frame=%s buf_len=%u frame_len=%u "
			    "channel=%u rssi=%d",
			    frame ? "yes" : "no", buf_len, frame_len,
			    channel, (int)rssi);
		return;
	}
	if (rssi > 0 || rssi < -120) rssi = -60;
	if (g_mgmt_seen < 12) {
		g_mgmt_seen++;
		/* fc, dur, addr1..3 and the beacon's fixed fields: enough to tell
		 * a beacon (0x80) from a probe response (0x50) and to see the
		 * BSSID the firmware reported. */
		HIF_LOG("MGMT rx #%u: %u bytes freq %u rssi %d",
		    g_mgmt_seen, buf_len, channel, (int)rssi);
		hif_dump_hex("MGMT hdr", frame, buf_len < 48 ? buf_len : 48);
	}
	if (g_frame_sink) {
		uint16_t freq = (channel > 1000 && channel < 6000) ? (uint16_t)channel : 2412;
		g_frame_sink(frame, (size_t)buf_len, (int16_t)rssi, freq);
		g_beacons++;
	}
	/* join=<n>: fill the BSS table from beacons/probe responses and watch
	 * for the access point's answers to the join sequence.  Nothing here
	 * runs unless the boot-arg enabled the join. */
	if (g_join && buf_len >= 24) {
		uint8_t type = frame[0] & 0xfc;

		if (type == 0x80 || type == 0x50)
			hif_join_bss_note(frame, buf_len, channel, (int32_t)rssi);
		else
			hif_join_resp(frame, buf_len);
	}
}

/* Linux ath10k_core_start() sends WMI_PDEV_SET_BASE_MACADDR immediately
 * after WMI READY, before any vdev exists.  The command is one TLV holding
 * exactly the six-byte MAC (wmi.h struct wmi_pdev_set_base_macaddr_cmd). */
static int
hif_send_base_macaddr(void)
{
	uint8_t tlv[32];
	uint8_t *p = tlv;
	uint8_t mac[8] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01, 0, 0 };
	uint16_t cmdlen = (g_scan_fix & SCANFIX_MAC) ? 8u : 6u;

	memset(tlv, 0, sizeof(tlv));
	p = hif_put_tlv(p, WMI_TLV_PDEV_SET_BASE_MACADDR_CMD, cmdlen, mac);
	HIF_LOG("WMI: send PDEV_SET_BASE_MACADDR %02x:%02x:%02x:%02x:%02x:%02x (%uB)",
	    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], cmdlen);
	return hif_wmi_command(WMI_PDEV_SET_BASE_MACADDR_CMDID, tlv,
	    (uint32_t)(p - tlv));
}

static void
hif_finish_vdev(void)
{
	if (g_vdev_pre) (void)hif_send_base_macaddr();
	if (hif_send_vdev_create() == 0) {
		if (g_vdev_start) (void)hif_send_vdev_start();
		if (g_vdev_up) (void)hif_send_vdev_up();
		g_state = HIF_READY;
		HIF_LOG("WMI: scan vdev created, scanning is available");
	} else {
		HIF_LOG("WMI: VDEV_CREATE failed");
	}
}

static void
hif_handle_wmi(const uint8_t *payload, uint32_t len)
{
	uint32_t event_id;

	if (len < 4) return;
	event_id = hif_le32(payload);
	switch (event_id) {
	case WMI_SERVICE_READY_EVENTID: {
		/* wmi-tlv.h: struct wmi_tlv_svc_rdy_ev starts with fw_build_vers
		 * then the six ABI words; num_mem_reqs sits at +72 and
		 * max_num_scan_chans at +76.  Printing them tells us whether the
		 * firmware wants host memory chunks before WMI INIT. */
		const uint8_t *p = payload + 4;
		uint32_t left = len - 4;
		HIF_LOG("WMI: SERVICE_READY (0x%x, %u bytes)", event_id, len);
		while (left >= 4) {
			uint32_t tl = hif_le16(p);
			if (4 + tl > left) break;
			if (tl >= 84) {
				HIF_LOG("WMI: SERVICE_READY fw %u abi %08x/%u/%08x/%08x "
				    "mem_reqs %u scan_chans %u bd %u",
				    hif_le32(p + 4), hif_le32(p + 8), hif_le32(p + 12),
				    hif_le32(p + 16), hif_le32(p + 20),
				    hif_le32(p + 4 + 72), hif_le32(p + 4 + 76),
				    hif_le32(p + 4 + 80));
				break;
			}
			p += 4 + ((tl + 3) & ~3u);
			left -= 4 + ((tl + 3) & ~3u);
		}
		if (!g_init_sent) {
			if (hif_send_wmi_init() == 0) {
				g_init_sent = true;
				g_state = HIF_INIT_SENT;
			}
		}
		break;
	}
	case WMI_READY_EVENTID:
		HIF_LOG("WMI: READY (0x%x) - WMI is up, creating the scan vdev", event_id);
		/* core.c ath10k_htt_setup() runs here, after wait_for_unified_ready
		 * and before the interface exists.  First message is VERSION_REQ;
		 * the rest follows the target's VERSION_CONF.  httsetup=2 sends
		 * RX_RING_CFG/AGGR_CFG immediately, for firmware that never
		 * answers the version request. */
		if (g_htt_setup) {
			(void)hif_htt_version_req();
			if (g_htt_setup >= 2) hif_htt_setup_finish();
		}
		/* hifstop=1/3: skip PDEV_SET_REGDOMAIN.  The WCN3990 firmware
		 * has a documented deterministic crash shortly after vdev_create;
		 * bisecting which host command precedes it tells us whether the
		 * firmware is dying on our input or on its own. */
		if (g_scan_fix & SCANFIX_PDEVPARAM) (void)hif_send_pdev_params();
		if (g_ready_stop == 1 || g_ready_stop == 3) {
			HIF_LOG("WMI: READY stop=%u, skipping PDEV_SET_REGDOMAIN",
			    g_ready_stop);
		} else {
			hif_send_pdev_regdomain();
		}
		/* mac.c ath10k_update_channel_list() is reached through
		 * ath10k_regd_update(), i.e. *after* ath10k_core_start() has sent
		 * PDEV_SET_REGDOMAIN.  The channel table carries per-channel power
		 * rules that only exist once the domain is known, so Linux never
		 * sends it first - the old order here did. */
		if (g_chan_list) (void)hif_send_scan_chan_list();
		if (g_ready_stop == 2 || g_ready_stop == 3 ||
		    g_ready_stop == 4) {
			g_state = HIF_READY;
			if (g_ready_stop == 4) g_autoscan = 1;
			HIF_LOG("WMI: READY stop=%u, skipping VDEV_CREATE%s",
			    g_ready_stop, g_ready_stop == 4 ? ", forcing autoscan" : "");
		} else if (g_ready_stop == 5 || g_ready_stop == 6) {
			g_vdev_pending = 1;
			g_vdev_at = g_polls +
			    (g_ready_stop == 5 ? 1000u : 5000u);
			HIF_LOG("WMI: READY, VDEV_CREATE delayed %u polls",
			    g_vdev_at - g_polls);
		} else {
			hif_finish_vdev();
		}
		break;
	case WMI_MGMT_RX_EVENTID:
		hif_handle_mgmt_rx(payload + 4, len - 4);
		break;
	case WMI_VDEV_START_RESP_EVENTID: {
		/* wlan-wip wj_wmi_event(): tag 0x28, 16 bytes, status is the
		 * fourth u32 of the TLV value. */
		uint32_t off = 4;

		while (off + 4 <= len) {
			uint32_t tlv_len = hif_le16(payload + off);
			uint16_t tag = hif_le16(payload + off + 2);

			if (off + 4 + tlv_len > len) break;
			if (tag == WMI_TLV_VDEV_START_RESP && tlv_len >= 16) {
				uint32_t status = hif_le32(payload + off + 16);

				g_join_started = status ? 2 : 1;
				HIF_LOG("JOIN: VDEV_START resp status=%u", status);
			}
			off += 4 + tlv_len;
		}
		break;
	}
	case WMI_MGMT_TX_COMPL_EVENTID: {
		/* wlan-wip wj_wmi_event(): TLV tag 423,
		 * wmi_tlv_mgmt_tx_compl_ev {desc_id, status, ...}; status != 0
		 * means the target did not send the frame.  Only reachable
		 * after a join sent MGMT_TX_SEND. */
		uint32_t off = 4;

		if (!g_join) break;
		while (off + 4 <= len) {
			uint32_t tlv_len = hif_le16(payload + off);
			uint16_t tag = hif_le16(payload + off + 2);

			if (off + 4 + tlv_len > len) break;
			if (tag == WMI_TLV_MGMT_TX_COMPL_EVENT && tlv_len >= 8) {
				uint32_t status = hif_le32(payload + off + 8);

				if (status) g_join_mgmt_fail++;
				else g_join_mgmt_ok++;
				HIF_LOG("JOIN: mgmt tx compl desc=%u status=%u "
				    "(ok=%u fail=%u)",
				    hif_le32(payload + off + 4), status,
				    g_join_mgmt_ok, g_join_mgmt_fail);
			}
			off += 4 + tlv_len;
		}
		break;
	}
	case WMI_SCAN_EVENTID:
		/* TLV, not a flat struct: payload is the 4-byte WMI header, then
		 * a 4-byte TLV header, then struct wmi_scan_event
		 * { event_type, reason, channel_freq, scan_req_id, scan_id,
		 *   vdev_id, tsfl }.  Reading payload+4 used to fold the TLV
		 * length and tag into "type" (0x24001c), which is how we know
		 * the tag is 36 and the block is 28 bytes. */
		if (len >= 16) {
			HIF_LOG("WMI: SCAN event type=%u reason=%u freq=%u req=0x%x "
			    "id=0x%x vdev=%u beacons=%u",
			    hif_le32(payload + 8), hif_le32(payload + 12),
			    hif_le32(payload + 16), hif_le32(payload + 20),
			    hif_le32(payload + 24), hif_le32(payload + 28),
			    g_beacons);
		}
		/* The reason code is what matters: 0 = completed, 1 = aborted,
		 * 2 = failed, 3 = early exit.  Dump the block so the field
		 * offsets can be checked against the firmware's own bytes. */
		hif_dump_hex("WMI scan ev", payload, len < 40 ? len : 40);
		g_scan_active = false;
		/* join=<n>: a completed scan round is the trigger.  Join the
		 * 1-based BSS table entry the boot-arg named, if it exists. */
		/* A connect request is matched by SSID, not by the join=<n>
		 * table-size gate: wlan-wip wj_begin() only looks for the
		 * requested network.  Keeping the old "need at least join
		 * entries" test would refuse to associate when the scan
		 * happened to hear exactly one AP -- possibly the right one. */
		if (g_join && len >= 16 && hif_le32(payload + 8) == 0 &&
		    g_state == HIF_READY &&
		    g_join_state == HIF_JOIN_IDLE &&
		    g_polls >= g_join_backoff_at &&
		    (g_join_req.pending ||
		    g_join_bss_count >= (uint32_t)g_join)) {
			hif_join_begin();
		} else if (g_join && !g_join_req.pending && len >= 16 &&
		    hif_le32(payload + 8) == 0 &&
		    g_join_state == HIF_JOIN_IDLE &&
		    g_join_bss_count < (uint32_t)g_join) {
			HIF_LOG("JOIN: entry %d not in the BSS table yet "
			    "(%u heard), waiting for the next scan",
			    g_join, g_join_bss_count);
		}
		break;
	default:
		HIF_LOG("WMI: event 0x%x (%u bytes)", event_id, len);
		/* WMI_TLV_SERVICE_AVAILABLE_EVENTID (0x3) carries the 128+
		 * extended service bitmap; wmi-tlv.h says it precedes
		 * SERVICE_READY.  chain-570 decoded bit143
		 * PEER_MAP_UNMAP_V2_SUPPORT=1 from its bytes, which means the
		 * peer-map layout hif_join_peer_event() expects may be wrong.
		 * Print the words so a later capture settles it without a guess.
		 * Layout: event_id(4) TLV hdr(4) seg_offset(4) bitmap words. */
		if (event_id == 0x3 && len >= 20) {
			uint32_t seg = hif_le32(payload + 8);
			uint32_t w0 = hif_le32(payload + 12);
			uint32_t w1 = len >= 24 ? hif_le32(payload + 16) : 0;
			uint32_t w2 = len >= 28 ? hif_le32(payload + 20) : 0;

			HIF_LOG("WMI: SVC_AVAIL seg=%u ext=%08x %08x %08x "
			    "v2=%u dma=%u", seg, w0, w1, w2,
			    (w0 >> 15) & 1u, (w0 >> 18) & 1u);
		}
		/* Peer-group events (0x600x) are the other possible path for
		 * PEER_CREATE confirmation (WMI_TLV_PEER_ASSOC_CONF_EVENTID
		 * 0x6006 / WMI_TLV_PEER_STATE_EVENTID 0x6005).  Count them so
		 * "no peer map ever arrived" is a measured fact, not an
		 * inference from a timeout. */
		if ((event_id & 0xf000u) == 0x6000u) {
			g_wmi_peer_events++;
			HIF_LOG("WMI: PEER event 0x%x count=%u (%u bytes)",
			    event_id, g_wmi_peer_events, len);
		}
		/* wmi.h struct wmi_chan_info_event: err_code, freq, cmd_flags,
		 * noise_floor, rx_clear_count, cycle_count.  rx_clear/cycle is
		 * the only observable that says whether there was any RF energy
		 * on the channel; without it "no beacons" cannot distinguish
		 * "no AP nearby" from "firmware dropped the frames". */
		if (event_id == WMI_CHAN_INFO_EVENTID && len >= 32) {
			HIF_LOG("CHAN_INFO freq=%u err=%u noise=%d rx_clear=%u cycle=%u",
			    hif_le32(payload + 12), hif_le32(payload + 8),
			    (int32_t)hif_le32(payload + 20),
			    hif_le32(payload + 24), hif_le32(payload + 28));
		}
		if (event_id != WMI_MGMT_RX_EVENTID)
			hif_dump_hex("WMI ev", payload, len < 24 ? len : 24);
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
		if (hif_send_connect_service(HTC_SVC_WMI_CONTROL) == 0)
			g_state = HIF_CONNECT_SENT;
		break;
	case HTC_MSG_CONNECT_SERVICE_RESP: {
		uint16_t svc;

		if (len < 8) break;
		svc = hif_le16(payload + 2);
		HIF_LOG("HTC: CONNECT_SERVICE_RESP svc=0x%04x status=%u eid=%u max_msg=%u",
		    svc, payload[4], payload[5], hif_le16(payload + 6));
		if (payload[4] != 0) break;      /* ATH10K_HTC_CONN_SVC_STATUS_SUCCESS */
		if (svc == HTC_SVC_WMI_CONTROL) {
			g_wmi_eid = payload[5];
			/* core.c ath10k_htt_connect() connects HTT_DATA_MSG
			 * before ath10k_htc_start() sends SETUP_COMPLETE_EX. */
			if (g_htt_connect) {
				(void)hif_send_connect_service(HTC_SVC_HTT_DATA_MSG);
				g_state = HIF_CONNECT_SENT;
				break;
			}
		} else if (svc == HTC_SVC_HTT_DATA_MSG) {
			g_htt_eid = payload[5];
			HIF_LOG("HTC: HTT endpoint eid=%u", g_htt_eid);
		} else {
			break;
		}
		if (hif_send_setup_complete() == 0) g_state = HIF_SETUP_SENT;
		break;
	}
	default:
		/* A target that is about to assert reports it here first; the
		 * payload is only a handful of bytes, so print it. */
		HIF_LOG("HTC: control message %u (%u bytes) %02x %02x %02x %02x "
		    "%02x %02x %02x %02x %02x %02x %02x %02x",
		    msg_id, len,
		    len > 0 ? payload[0] : 0, len > 1 ? payload[1] : 0,
		    len > 2 ? payload[2] : 0, len > 3 ? payload[3] : 0,
		    len > 4 ? payload[4] : 0, len > 5 ? payload[5] : 0,
		    len > 6 ? payload[6] : 0, len > 7 ? payload[7] : 0,
		    len > 8 ? payload[8] : 0, len > 9 ? payload[9] : 0,
		    len > 10 ? payload[10] : 0, len > 11 ? payload[11] : 0);
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
	if (htc_len > nbytes - HTC_HDR_BYTES) htc_len = (uint16_t)(nbytes - HTC_HDR_BYTES);
	/* htc.c: ath10k_htc_rx_completion_handler() takes the trailer length
	 * from the *last payload byte* and then drops exactly that one byte
	 * from the payload - it is not htc_hdr.seq_no as an earlier version of
	 * this driver assumed. */
	trailer_len = 0;
	payload_len = htc_len;
	if ((flags & 0x02) && htc_len > 0) {
		trailer_len = buf[HTC_HDR_BYTES + htc_len - 1];
		payload_len = (uint32_t)htc_len - 1;
	}

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
	} else if (g_htt_connect && eid == g_htt_eid) {
		HIF_LOG("rx: HTT eid=%u (%u bytes)", eid, nbytes);
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
	/* One cheap tick for the in-kernel DHCP client; no CE register access. */
	if (g_dhcp) {
		hakios_dhcp_tick(g_polls, (g_net_path && g_link_up) ? 1 : 0, g_join_mac);
	}

	/* Heartbeat every ~1000 polls, and advance the staged ladder.  This is
	 * deliberately cheap: no cache flushes and no CE register access until
	 * the corresponding stage is entered. */
	if ((g_polls % 1000) == 0) {
		HIF_LOG("heartbeat polls=%u stage=%d state=%d rx=%u beacons=%u "
		    "desc0.nbytes=%u drri=%x htt=%u httidx=%u mgmt=%u mgmtev=%u "
		    "join=%d ind=%u ord=%u msdu=%u eapol=%u bad=%u/%u "
		    "ring=%u/%u srri4=%u wr4=%u",
		    g_polls, g_stage, (int)g_state, g_rx_packets, g_beacons,
		    (unsigned)s_dst2[0].nbytes,
		    (g_bases_programmed && g_dma) ? ((s_rri[2] >> 16) & 0xffffu) : 0,
		    g_htt_rx, g_dma ? g_dma->htt_alloc_idx : 0, g_mgmt_seen,
		    g_mgmt_events, g_join_state,
		    g_wr_rx_ind, g_wr_rx_in_ord, g_wr_rx_frames, g_wr_rx_eapol,
		    g_wr_rx_bad, g_wr_rx_undone,
		    g_htt_ring_done_cnt, g_htt_ring_seen,
		    /* CE4 source read index is the input to the txguard ring
		     * check.  Printing it every heartbeat makes a stalled RRI
		     * (which would otherwise wedge TX with repeated -2) visible
		     * without any device interaction. */
		    (g_bases_programmed && g_dma) ?
		        ((*(volatile uint32_t *)&g_dma->rri[4]) & 0xffffu) : 0,
		    g_wr4);
		/* join=<n>: a compact status line while a join is running. */
		if (g_join && g_join_state != HIF_JOIN_IDLE) {
			HIF_LOG("JOIN: state=%d tries=%u bss=%u mgmt "
			    "tx=%u ok=%u fail=%u srri3=%u drri2=%u",
			    g_join_state, g_join_tries, g_join_bss_count,
			    g_join_mgmt_tx, g_join_mgmt_ok, g_join_mgmt_fail,
			    g_dma ? (s_rri[3] & 0xffffu) : 0,
			    g_dma ? ((s_rri[2] >> 16) & 0xffffu) : 0);
		}
		/* The other dst pipes' DDR read indices: a moving index means the
		 * target consumed a buffer there, which the bring-up wants to
		 * know about even though nothing drains those rings yet. */
		if (g_bases_programmed && (g_polls % 5000) == 0) {
			uint32_t ce;

			for (ce = 0; ce < 12; ce++) {
				uint32_t now;

				if (ce == 2 || hif_shape[ce].dst_n == 0) continue;
				now = s_rri[ce] & (hif_shape[ce].dst_n - 1);
				if (now != g_rri_last[ce]) {
					HIF_LOG("CE%u rri %u -> %u",
					    ce, g_rri_last[ce], now);
					g_rri_last[ce] = now;
				}
			}
		}
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
	if (g_vdev_pending && g_polls >= g_vdev_at) {
		g_vdev_pending = 0;
		HIF_LOG("WMI: delayed VDEV_CREATE now (poll %u)", g_polls);
		hif_finish_vdev();
	}
	if (g_autoscan && !g_autoscan_sent && g_state == HIF_READY) {
		bool too_early = false;
		if (g_scan_wait != 0) {
			if (g_scan_at == 0) {
				g_scan_at = g_polls + (uint32_t)g_scan_wait * 500u;
				HIF_LOG("autoscan: deferred %d s (%u polls)",
				    g_scan_wait, g_scan_at - g_polls);
			}
			too_early = g_polls < g_scan_at;
		}
		if (!too_early) {
			g_autoscan_sent = 1;
			g_last_rescan_at = g_polls;
			HIF_LOG("autoscan: WMI ready, issuing START_SCAN");
			(void)wcn3990_hif_start_scan(40);
		}
	}
	/* wifiauto: a network that was not in the first scan round must not
	 * leave the link down forever.  wlan-wip wj_step() re-arms
	 * ws_scan_wanted while WJ_IDLE; re-scan every ~10 s until the requested
	 * SSID is picked.  g_join_req.pending is cleared once a BSS matches,
	 * so this stops as soon as the join starts. */
	if (g_wifi_auto && g_join_req.pending && g_autoscan_sent &&
	    g_state == HIF_READY && g_join_state == HIF_JOIN_IDLE &&
	    !g_scan_active && g_polls - g_last_rescan_at >= 10000u) {
		g_last_rescan_at = g_polls;
		HIF_LOG("wifiauto: rescan for the requested network");
		(void)wcn3990_hif_start_scan(40);
	}
	/* join=<n>: drive the auth/assoc state machine on the 1 ms tick
	 * (wlan-wip wj_step()'s retransmit/timeout timing). */
	if (g_join) hif_join_poll();
	if (g_scan_active) {
		g_scan_polls++;
		/* Record what the target does with the scan command before the
		 * machine can die: the CE3 source read index says whether it
		 * fetched START_SCAN at all, the CE2 destination index whether
		 * it filled an HTC buffer with an answer, and the SMP2P "wlan"
		 * bits whether the firmware reported a fatal error.  IMEM keeps
		 * these across the reset. */
		if (g_scan_polls <= 20 || (g_scan_polls % 20) == 0) {
			uint32_t smp = picasso_wlan_smp2p_bits();
			uint32_t srri3, drri2;

			/* The mailbox is plain memory: print it before touching any
			 * CE register, so a fault on the register read cannot hide
			 * the one value that says whether the target asserted. */
			HIF_LOG("scan[%u]: smp2p=%08x", g_scan_polls, smp);
			g_smp2p_seen = 1;
			g_smp2p_last = smp;
			srri3 = g_dma ? (s_rri[3] & 0xffffu) : 0;
			drri2 = g_dma ? ((s_rri[2] >> 16) & 0xffffu) : 0;
			HIF_LOG("scan[%u]: ce3 srri=%u ce2 drri=%u",
			    g_scan_polls, srri3, drri2);
			hif_bb(12, srri3, drri2);
			hif_bb(13, smp, g_scan_polls);
		}
	}

	if (g_ce_quiet) {
		/* The console dies at the doorbell, so anything learned here has to
		 * reach the next boot through the IMEM box: one probe every ~5 s,
		 * each recorded as two records (BB_SMMU_CB a1=probe# a2=DRRI,
		 * BB_CE_ACCESS a1=0x5150|probe# a2=rx bytes).  A DRRI that moves
		 * while the driver never touches the engines means the target is
		 * consuming buffers and the wlan is further along than the console
		 * ever showed. */
		if (g_quiet_probe_polls != 0 && g_polls >= g_quiet_probe_polls) {
			uint32_t drri = g_dma ? ((s_rri[2] >> 16) & 0xffffu) : 0;
			uint32_t nb = (unsigned)s_dst2[0].nbytes;
			extern void picasso_wlan_probe_paint(uint32_t probe,
			    uint32_t drri, uint32_t nbytes);

			picasso_wlan_probe_paint(g_quiet_probed, drri, nb);
			hif_bb(10, g_quiet_probed, drri);
			hif_bb(11, 0x51500000u | (g_quiet_probed & 0xffffu), nb);
			if (g_quiet_probed == 0) {
				HIF_LOG("quiet probe 0: drri=%x nbytes=%u", drri, nb);
			}
			g_quiet_probed++;
			g_quiet_probe_polls = g_polls + 5000;
		}
		if ((g_polls % 5000) == 0) {
			HIF_LOG("quiet heartbeat polls=%u probe=%d", g_polls,
			    g_quiet_probed);
		}
		return;
	}

	/* ath10k_snoc_hif_start() posts the buffers and services them right
	 * away.  No post-doorbell watch, no gate: CoachZ's pure-poll wc_poll(1)
	 * proves the CE works without a single interrupt registered. */

	/* CoachZ wc_service() acknowledges CE_STATUS only when that engine is
	 * actually serviced (it completed something, so it is awake).  An
	 * unconditional W1C on a 1 ms tick is itself a CE-body register access
	 * and is unsafe once the target idles. */

	/* The block is Normal non-cacheable, so the CE's write to the DDR read
	 * index and to each descriptor's nbytes is visible immediately: no
	 * invalidate, no clean, and no risk of writing back a stale line. */
	/* DDR RRI (CTRL1 bit19 + wrapper 0x24C004/8) packs SRRI in [15:0]
	 * and DRRI in [31:16].  Diagnostics read it from DDR; the recv loop
	 * itself needs no read index at all. */
	{
		/* Linux 64-bit recv path and CoachZ wc_service(): the descriptor's
		 * nbytes in host memory is the ONLY completion test.  Zero CE
		 * register reads in the running service loop. */
		while (budget-- > 0) {
			struct wcn_ce_desc_64 *d = &s_dst2[g_sw2];
			uint32_t idx = g_sw2, nbytes;

			/* ath10k: a zero length is a descriptor the CE has not
			 * completed yet, not a full-size frame. */
			nbytes = d->nbytes;
			if (nbytes == 0) break;   /* CE has not completed this one yet */
			hif_reg_write(2, CE_STATUS, 0x1u | 0x1eu);  /* CE is awake now */
			if (nbytes > CE_BUF_BYTES) nbytes = CE_BUF_BYTES;
			d->nbytes = 0;            /* claim it before reposting */

			if (g_rx_packets < 16) hif_bb(BB_RX, idx, nbytes);
			hif_handle_rx(s_rx2[idx], nbytes);

			hif_ce2_repost(idx);
			g_sw2 = (g_sw2 + 1) & (CE2_ENTRIES - 1);
		}
	}

	/* Repost already rings the always-on shadow doorbell for every
	 * completed descriptor.  A periodic CE-body 0x40 store on an idle
	 * engine would be an un-clocked access. */

	/* HTT responses arrive on CE1; drain them on the same 1 ms tick. */
	hif_service_ce1();
	/* and the received-MPDU ring the target fills after RX_RING_CFG. */
	hif_service_htt_ring();
}

/* --------------------------------------------------------------------- init */

/* Stage 2: program the CE ring registers but do not post any receive buffer.
 * ce.c: ath10k_ce_set_dest_ring_base_addr_hi() preserves the non-address bits
 * of dr_base_addr_hi, so use a read-modify-write with CE_DESC_ADDR_HI_MASK.
 *
 * Every one of the 12 copy engines is programmed here, mirroring
 * ath10k_snoc_init_pipes(): the target starts using whichever pipe it pleases
 * the moment it boots, and a reset ring base of 0 makes the CE dma firmware
 * data to garbage addresses under the bypass - which is what killed every
 * build up to 430 within 10 ms of the CE2 doorbell. */
static void
hif_program_bases(void)
{
	uint64_t rri_pa;
	uint32_t ce;

	/* Must precede the CTRL1 writes: bit 19 tells the CE to publish read
	 * indices into this array, so a stale/zero address would send a DMA to
	 * physical 0.  It lives in the same contiguous block as the rings. */
	rri_pa = hif_pa_of(s_rri);
	memset(s_rri, 0, sizeof(s_rri));
	hif_mb();
	*(volatile uint32_t *)(uintptr_t)(g_ce_va + CE_WRAP_RRI_LOW) = (uint32_t)rri_pa;
	hif_mb();
	*(volatile uint32_t *)(uintptr_t)(g_ce_va + CE_WRAP_RRI_HIGH) =
	    (uint32_t)(rri_pa >> 32) & 0x1fu;
	hif_mb();
	HIF_LOG("RRI pa=%llx wrapper=%08x/%08x", (unsigned long long)rri_pa,
	    *(volatile uint32_t *)(uintptr_t)(g_ce_va + CE_WRAP_RRI_LOW),
	    *(volatile uint32_t *)(uintptr_t)(g_ce_va + CE_WRAP_RRI_HIGH));

	for (ce = 0; ce < 12; ce++) {
		const struct hif_ce_shape *sh = &hif_shape[ce];
		uint32_t old = hif_reg_read(ce, CE_CTRL1);
		uint32_t v = old;

		/* ce.c: ath10k_ce_alloc_rri() enables DDR read-index updates on
		 * every CE, not only the ones this driver posts on. */
		v |= CE_CTRL1_RRI_UPD;
		v &= ~(CE_CTRL1_SRC_BSWAP | CE_CTRL1_DST_BSWAP);
		if (sh->src_n > 0) {
			v = (v & ~CE_CTRL1_DMAX_MASK) | sh->dmax;
		}
		hif_reg_write(ce, CE_CTRL1, v);
		HIF_LOG("CE%u ctrl1 %08x -> %08x", ce, old, v);
		IOSleep(30);

		if (sh->src_n > 0) {
			uint64_t pa = g_sr_pa[ce];
			uint32_t wm;

			hif_reg_write(ce, CE_SR_BASE_LO, (uint32_t)pa);
			hif_reg_write(ce, CE_SR_BASE_HI,
			    (uint32_t)(pa >> 32) & 0x1f);
			hif_reg_write(ce, CE_SR_SIZE, sh->src_n);
			/* ce.c: src lowmark 0, highmark nentries. */
			wm = hif_reg_read(ce, CE_SRC_WM);
			hif_reg_write(ce, CE_SRC_WM,
			    (wm & CE_DST_WM_LOW_MASK) |
			    ((uint32_t)sh->src_n & CE_DST_WM_HIGH_MASK));
			HIF_LOG("CE%u src base=%llx n=%u wm=%08x->%08x",
			    ce, (unsigned long long)pa, sh->src_n, wm,
			    hif_reg_read(ce, CE_SRC_WM));
		}
		if (sh->dst_n > 0) {
			uint64_t pa = g_dr_pa[ce];
			uint32_t hi = hif_reg_read(ce, CE_DR_BASE_HI);
			uint32_t wm;

			hif_reg_write(ce, CE_DR_BASE_LO, (uint32_t)pa);
			hi = (hi & ~0x1fu) | ((uint32_t)(pa >> 32) & 0x1fu);
			hif_reg_write(ce, CE_DR_BASE_HI, hi);
			hif_reg_write(ce, CE_DR_SIZE, sh->dst_n);
			/* ce.c: dst low watermark 0, high watermark nentries. */
			wm = hif_reg_read(ce, CE_DST_WM);
			hif_reg_write(ce, CE_DST_WM,
			    (wm & CE_DST_WM_LOW_MASK) |
			    ((uint32_t)sh->dst_n & CE_DST_WM_HIGH_MASK));
			HIF_LOG("CE%u dst base=%llx n=%u wm=%08x->%08x",
			    ce, (unsigned long long)pa, sh->dst_n, wm,
			    hif_reg_read(ce, CE_DST_WM));
		}
		hif_reg_write(ce, CE_HOST_IE, 0);
		hif_reg_write(ce, CE_MISC_IE, 0);
	}

	g_bases_programmed = true;
	hif_dump_regs("stage 2: CE bases programmed, no buffers posted");
	/* SMMUv2 (IHI 0062): with sCR0.USFCFG=0 an unmatched stream uses
	 * S2CR[StreamID] directly.  USFCFG reads 1 here and clearing it is
	 * refused, but some Qualcomm implementations honour the direct index
	 * regardless; S2CR writes are the one register family this SMMU still
	 * accepts.  Write the passthrough bank the console stream uses for both
	 * candidate WLAN CE streams (0x20 and the device-tree's 0xc0), read-only
	 * otherwise: if the implementation ignores the direct index nothing
	 * changes, and the registers are ours to rewrite later. */
	{
		uint32_t rb = 0, ctx = 0;

		/* With the real IOMMU domain installed the route is already owned
		 * by the translated context.  These direct-index pokes predate it
		 * and would only write into a reserved part of the SMMU's global
		 * page (index 0xc0 is beyond the S2CR array); keep them on the
		 * control path only, so the A/B differs in one variable. */
		(void)PE_parse_boot_argn("wlansmmu", &ctx, sizeof(ctx));
		if (ctx == 1) {
			HIF_LOG("stage 2.5: skipped, stream c0 is on a translated context");
		} else {
			picasso_wlan_smmu_s2cr_set(0x20, WLAN_SMMU_PASSTHRU_BANK, &rb);
			HIF_LOG("stage 2.5: S2CR[0x20]=%08x", rb);
			picasso_wlan_smmu_s2cr_set(0xc0, WLAN_SMMU_PASSTHRU_BANK, &rb);
			HIF_LOG("stage 2.5: S2CR[0xc0]=%08x", rb);
		}
	}
	hif_bb(BB_BASES_DONE, 0, 0);
}

/* Post a whole dst ring full of receive buffers and ring that CE's doorbell.
 * snoc.c: ath10k_snoc_rx_post_pipe() fills every slot but one and writes the
 * dest write index once; that write is the doorbell the CE fetches on. */
static void
hif_ce_post_ring(uint32_t ce)
{
	const struct hif_ce_shape *sh = &hif_shape[ce];
	struct wcn_ce_desc_64 *ring = g_dr_desc[ce];
	uint32_t n = sh->dst_n, i;

	/* Fill every descriptor with the buffer address and a zero length.
	 * Linux/CoachZ post exactly this way: the CE writes the transfer
	 * length into nbytes when it completes, and a zero nbytes means
	 * "not done yet" (_ath10k_ce_completed_recv_next_nolock_64). */
	for (i = 0; i < n; i++) {
		ring[i].addr = (g_buf_pa[ce] + (uint64_t)i * sh->buf_sz) &
		    CE_DESC_ADDR_MASK;
		ring[i].nbytes = 0;
		ring[i].flags = 0;
		ring[i].toeplitz = 0;
	}
	/* No cache maintenance: the window is Normal non-cacheable. */
	hif_mb();

	/* Keep one slot free so the ring can never look full to the target. */
	hif_reg_write(ce, CE_DST_WR_INDEX, n - 1);
	if (g_ce_quiet) g_quiet_probe_polls = g_polls + 5000;
	HIF_LOG("CE%u posted %u receive buffers (desc.addr=%llx buf=%u)",
	    ce, n - 1, (unsigned long long)g_buf_pa[ce], sh->buf_sz);
	hif_bb(BB_DOOR_REAL, 0x100u + ce, n - 1);
}

/* Stage 3: hand receive buffers to the copy engines. */
static void
hif_post_buffers(void)
{
	/* Every dst pipe first: the firmware services these rings the moment
	 * it starts consuming CE2 (the 430 family died exactly there), so the
	 * rings must be live before the CE2 doorbell rings. */
	{
		uint32_t ce;

		for (ce = 0; ce < 12; ce++) {
			if (ce == 2) continue;
			if (hif_shape[ce].dst_n > 0) hif_ce_post_ring(ce);
		}
	}

	/* ce.c: the write index is a free-running counter masked with
	 * nentries-1, and __ath10k_ce_rx_post_buf_64() posts on top of
	 * whatever the register already holds.  Keep one slot free so the
	 * ring can never look full to the target. */
	{
		uint32_t cur = hif_reg_read(2, CE_DST_WR_INDEX) & (CE2_ENTRIES - 1);
		uint32_t n = (g_post_count > 0 && g_post_count < CE2_ENTRIES) ?
		    g_post_count : (CE2_ENTRIES - 1);
		uint32_t i;

		g_post2 = (cur + n) & (CE2_ENTRIES - 1);
		/* Fill the CE2 descriptors that are about to be handed over:
		 * the other dst pipes already got complete rings above, and
		 * these must be clean before the fetch. */
		for (i = 0; i < n; i++) {
			uint32_t slot = (cur + i) & (CE2_ENTRIES - 1);

			s_dst2[slot].addr = (g_rx2_pa + (uint64_t)slot * CE_BUF_BYTES) &
			    CE_DESC_ADDR_MASK;
			s_dst2[slot].nbytes = 0;
			s_dst2[slot].flags = 0;
			s_dst2[slot].toeplitz = 0;
		}
		hif_mb();

		/* Bisect the doorbell.  Writing the value that is already there
		 * is a no-op for the CE, so if the SoC dies on that store the
		 * register itself is the trigger. */
		HIF_LOG("stage 3: doorbell cur=%u target=%u dr_base=%08x desc.addr=%llx",
		    cur, g_post2, hif_reg_read(2, CE_DR_BASE_LO),
		    (unsigned long long)s_dst2[0].addr);
		hif_bb(BB_DOOR_NOOP, cur, g_post2);
		IOSleep(30);
		/* Warm the fault-record mapping now: the post-doorbell watch
		 * must not spend its first microseconds in ml_io_map(). */
		{
			uint32_t f[8];

			picasso_wlan_smmu_fault_read(f);
			HIF_LOG("stage 3 warm: gfsr=%08x gfsynr1=%08x scr0=%08x smr4=%08x s2cr4=%08x cb61fsr=%08x",
			    f[0], f[2], f[4], f[5], f[6], f[7]);
		}
		hif_reg_write(2, CE_DST_WR_INDEX, cur);
		HIF_LOG("stage 3: doorbell no-op write ok (reads %u)",
		    hif_reg_read(2, CE_DST_WR_INDEX));
		IOSleep(30);
		/* wcn3990-hif-noopdoor=1 skips the real doorbell: if the machine
		 * then lives through the heartbeat the reset is caused by the
		 * fetch itself (or what follows it). */
		{
			uint32_t noop = 0;

			PE_parse_boot_argn("wcn3990-hif-noopdoor", &noop, sizeof(noop));
			if (noop) {
				HIF_LOG("stage 3: noopdoor - skipping the real doorbell");
			} else {
				hif_bb(BB_DOOR_REAL, g_post2, n);
				hif_reg_write(2, CE_DST_WR_INDEX, g_post2);
				HIF_LOG("stage 3: posted %u receive buffers (desc.addr=%llx wr=%u)",
				    n, (unsigned long long)s_dst2[0].addr, g_post2);
			}
		}
	}

	/* ath10k_snoc_hif_start() services the rings immediately after posting;
	 * CoachZ's wc_poll(1) shows the pure-polling path (no interrupts
	 * attached) is a valid backend.  There is deliberately no sleep here:
	 * a CE completion left unacknowledged for even a few milliseconds is
	 * what froze every previous build at the fifth receive. */
	HIF_LOG("stage 3: CE rings live, poller now servicing every 1 ms");
}

extern "C" void
wcn3990_hif_poll(void);

extern "C" void
wcn3990_hif_init(volatile uint8_t *ce_va, uint64_t ce_pa)
{
	uint32_t v = 0;
	uint32_t i;

	/* wifiauto=1: the unattended daily-use baseline.  Install the exact
	 * gate set proven on hardware (chain-574/575) as DEFAULTS.  Explicit
	 * boot-args parsed below still override every one of them, so single-
	 * variable A/B experiments remain possible on the same build. */
	if (PE_parse_boot_argn("wifiauto", &v, sizeof(v)) && v <= 1) {
		g_wifi_auto = (int)v;
	}
	if (g_wifi_auto) {
		g_autoscan = 1;
		g_chan_list = 1;
		g_htt_connect = 1;
		g_htt_setup = 1;
		g_htt_ring = 7;
		g_scan_fix = 127;
		g_join = 2;
		g_join_sel = 4;
		g_join_skip = 0;
		g_wpa = 1;
		g_net_path = 1;
		g_dhcp = 1;            /* usable by default; dhcp=0 restores the A/B */
		g_init_fix = 3;
		g_rx_fix = 1;
		g_dp_fix = 5;
		/* CE3 copy-complete race (CVE-2022-50700 type): every successful
		 * chain ran with wcn3990-hif-tx3=1.  A daily-use boot carries no
		 * lab flags, so the rotating buffers must be the default too. */
		g_tx3_rotate = 1;
	}

	/* Black box first: print whatever the previous boot managed to write
	 * into IMEM before its reset, then start recording this boot. */
	hif_bb_init();
	hif_bb_dump();
	/* Baseline of the WCSS crash mailbox before any WMI traffic, so a later
	 * change is attributable to the firmware and not to boot-time state. */
	HIF_LOG("smp2p wlan baseline %08x", picasso_wlan_smp2p_bits());

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
	v = 0;
	if (PE_parse_boot_argn("wcn3990-hif-quiet", &v, sizeof(v)) && v <= 1) {
		g_ce_quiet = (int)v;
	}
	if (PE_parse_boot_argn("wcn3990-hif-assign", &v, sizeof(v)) && v <= 3) {
		assign_level = v;
	}
	v = 0;
	if (PE_parse_boot_argn("wcn3990-hif-tx3", &v, sizeof(v)) && v <= 1) {
		g_tx3_rotate = (int)v;
	}
	v = 0;
	if (PE_parse_boot_argn("wcn3990-hif-autoscan", &v, sizeof(v)) && v <= 1) {
		g_autoscan = (int)v;
	}
	v = 0;
	if (PE_parse_boot_argn("hifstop", &v, sizeof(v)) && v <= 6) {
		g_ready_stop = (int)v;
	}
	v = 0;
	if (PE_parse_boot_argn("vdevpre", &v, sizeof(v)) && v <= 1) {
		g_vdev_pre = (int)v;
	}
	v = 0;
	if (PE_parse_boot_argn("wmiinit", &v, sizeof(v)) && v <= 1) {
		g_wmi_init_full = (int)v;
	}
	v = 0;
	if (PE_parse_boot_argn("initarr", &v, sizeof(v)) && v <= 2) {
		g_init_arr = (int)v;
	}
	v = 0;
	if (PE_parse_boot_argn("scanshp", &v, sizeof(v)) && v <= 6) {
		g_scan_shape = (int)v;
	}
	v = 0;
	if (PE_parse_boot_argn("vdstart", &v, sizeof(v)) && v <= 1) {
		g_vdev_start = (int)v;
	}
	v = 0;
	if (PE_parse_boot_argn("msdudesc", &v, sizeof(v)) && v > 0 && v <= 4096) {
		g_msdu_desc = (int)v;
	}
	v = 0;
	if (PE_parse_boot_argn("scanchans", &v, sizeof(v)) && v <= 39) {
		g_scan_chans = (int)v;
	}
	v = 0;
	if (PE_parse_boot_argn("tlvpad", &v, sizeof(v)) && v <= 1) {
		g_tlv_pad = (int)v;
	}
	v = 0;
	if (PE_parse_boot_argn("chanlist", &v, sizeof(v)) && v <= 1) {
		g_chan_list = (int)v;
	}
	v = 0;
	if (PE_parse_boot_argn("vdevup", &v, sizeof(v)) && v <= 1) {
		g_vdev_up = (int)v;
	}
	v = 0;
	if (PE_parse_boot_argn("scanlinux", &v, sizeof(v)) && v <= 1) {
		g_scan_linux = (int)v;
	}
	v = 0;
	if (PE_parse_boot_argn("httconn", &v, sizeof(v)) && v <= 1) {
		g_htt_connect = (int)v;
	}
	v = 0;
	if (PE_parse_boot_argn("httsetup", &v, sizeof(v)) && v <= 2) {
		g_htt_setup = (int)v;
	}
	v = 0;
	if (PE_parse_boot_argn("httring", &v, sizeof(v)) && v <= 63) {
		g_htt_ring = (int)v;
	}
	v = 0;
	if (PE_parse_boot_argn("scanwait", &v, sizeof(v)) && v <= 120) {
		g_scan_wait = (int)v;
	}
	v = 0;
	if (PE_parse_boot_argn("scanfix", &v, sizeof(v)) && v <= 127) {
		g_scan_fix = (int)v;
	}
	v = 0;
	/* bit2 = attention-driven HTT ring drain (chain-571 A/B). */
	if (PE_parse_boot_argn("dpfix", &v, sizeof(v)) && v <= 7) {
		g_dp_fix = (int)v;
	}
	v = 0;
	if (PE_parse_boot_argn("initfix", &v, sizeof(v)) && v <= 7) {
		g_init_fix = (int)v;
	}
	v = 0;
	/* rxfix bit0: use the wlan-wip Ethernet fold offset (frame+18). */
	if (PE_parse_boot_argn("rxfix", &v, sizeof(v)) && v <= 1) {
		g_rx_fix = (int)v;
	}
	v = 0;
	if (PE_parse_boot_argn("authz", &v, sizeof(v)) && v <= 1) {
		g_authz = (int)v;
	}
	/* join=N: after a scan round completes, authenticate and associate
	 * with BSS table entry N (1-based, order of first hearing).  0 or
	 * absent = off, baseline behaviour unchanged. */
	v = 0;
	if (PE_parse_boot_argn("join", &v, sizeof(v)) &&
	    v <= (uint32_t)HIF_JOIN_BSS_MAX) {
		g_join = (int)v;
	}
	/* joinonce=<n>: per-command gates for the wj_begin() once-block
	 * (bit1/2 power, bit4 NSS, bit8 TX_ENCAP_TYPE, bit16 RTS,
	 * bit32/64/128 sta_ps, bit256 ps_mode).  0x1ff = all, 0 = none. */
	v = 0x1ffu;
	if (PE_parse_boot_argn("joinonce", &v, sizeof(v)) && v <= 0x1ffu) {
		g_join_once_mask = (int)v;
	}
	/* joinskip=<n>: bit1 skip PEER_ASSOC, bit2 skip VDEV_UP.  Diagnostic
	 * only: isolates which post-assoc command makes the firmware assert. */
	v = 0;
	if (PE_parse_boot_argn("joinskip", &v, sizeof(v)) && v <= 15) {
		g_join_skip = (int)v;
	}
	/* joinsel=1 selects the first BSS whose SSID starts with "ChinaUnicom",
	 * =2 "1902_WIFI_CLOCK".  join=<n> alone is order-of-hearing dependent
	 * and picked a different access point on nearly every boot. */
	v = 0;
	if (PE_parse_boot_argn("joinsel", &v, sizeof(v)) && v <= 4) {
		g_join_sel = (int)v;
	}
	/* wpa=1 runs the WPA2-PSK four-way handshake after association;
	 * wpass=<passphrase> is the only place the secret enters the driver
	 * (boot-arg, never compiled in). */
	v = 0;
	if (PE_parse_boot_argn("wpa", &v, sizeof(v)) && v <= 1) {
		g_wpa = (int)v;
	}
	(void)PE_parse_boot_argn("wpass", g_wpa_pass, sizeof(g_wpa_pass));
	/* wifiauto: issue the connect request as soon as boot-args are in, so
	 * the radio scans, joins the matching SSID and obtains an address with
	 * no experiment parameters at all.  wifissid= overrides the network. */
	if (g_wifi_auto && g_wpa_pass[0]) {
		char ssid[33] = "ChinaUnicom-33E6TD";
		char wanted[33];

		if (PE_parse_boot_argn("wifissid", wanted, sizeof(wanted))) {
			size_t k = strlen(wanted);

			if (k && k < sizeof(ssid)) {
				memcpy(ssid, wanted, k);
				ssid[k] = 0;
			}
		}
		(void)wcn3990_hif_connect((const uint8_t *)ssid, strlen(ssid),
		    (const uint8_t *)g_wpa_pass, strlen(g_wpa_pass));
		HIF_LOG("wifiauto: unattended connect for '%s'", ssid);
	}
	/* wpadiag=1 dumps the M1/M2 frames plus key fingerprints, so the M2
	 * MIC can be recomputed offline.  Off by default: no key material
	 * (and never the passphrase) reaches the console otherwise. */
	v = 0;
	if (PE_parse_boot_argn("wpadiag", &v, sizeof(v)) && v <= 1) {
		g_wpa_diag = (int)v;
	}
	v = 0;
	if (PE_parse_boot_argn("netpath", &v, sizeof(v)) && v <= 1) {
		g_net_path = (int)v;
	}
	/* txguard defaults to 1 (the CE4 source-ring / TX-slot guards above).
	 * 0 restores the pre-fix behaviour so the same image can A/B the
	 * netpath=1 SError without a rebuild.  Same length as netpath=N. */
	v = 1;
	if (PE_parse_boot_argn("txguard", &v, sizeof(v)) && v <= 1) {
		g_tx_guard = (int)v;
	}
	v = 1;
	if (PE_parse_boot_argn("dhcp", &v, sizeof(v)) && v <= 1) {
		g_dhcp = (int)v;
	}
	v = 0;
	if (PE_parse_boot_argn("dpdiag", &v, sizeof(v)) && v <= 1) {
		g_dp_diag = (int)v;
	}
	v = 0;
	if (PE_parse_boot_argn("vdwait", &v, sizeof(v)) && v <= 5000) {
		g_vdev_wait_ms = (int)v;
	}
	HIF_LOG("CE3 tx buffers: %s; autoscan=%d; hifstop=%d; vdevpre=%d; "
	    "wmiinit=%d; scanchans=%d; tlvpad=%d; chanlist=%d; vdevup=%d; "
	    "scanlinux=%d; httconn=%d; httsetup=%d; httring=%d; scanwait=%d; "
	    "scanfix=%d; join=%d",
	    g_tx3_rotate ? "8 rotating" : "single (legacy)", g_autoscan,
	    g_ready_stop, g_vdev_pre, g_wmi_init_full, g_scan_chans,
	    g_tlv_pad, g_chan_list, g_vdev_up, g_scan_linux, g_htt_connect,
	    g_htt_setup, g_htt_ring, g_scan_wait, g_scan_fix, g_join);
	{
		uint64_t dma_pa = 0;
		g_dma = (struct hif_dma_block *)picasso_wlan_ce_dma_alloc(
		    HIF_DMA_SIZE, &dma_pa, assign_level);
		if (!g_dma || dma_pa == 0) {
			HIF_LOG("CE DMA allocation failed: HIF disabled, no CE registers touched");
			return;
		}
		g_dma_pa = dma_pa;   /* base for every hif_pa_of() below */
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

	g_dst2_pa = hif_pa_of(s_dst2);
	g_rx2_pa = hif_pa_of(s_rx2);
	g_src0_pa = hif_pa_of(s_src0);
	g_src3_pa = hif_pa_of(s_src3);
	if (!g_dst2_pa || !g_rx2_pa || !g_src0_pa || !g_src3_pa) {
		HIF_LOG("cannot resolve ring physical addresses");
		return;
	}

	/* Per-CE ring/buffer table: every pipe the target can wake up gets a
	 * programmed ring with valid descriptors, because a reset ring base
	 * of 0 turned the CE into a scatter machine (430 and friends). */
	{
		uint64_t pa;
		uint32_t ce;

		for (ce = 0; ce < 12; ce++) {
			g_sr_desc[ce] = NULL;
			g_dr_desc[ce] = NULL;
			g_buf_va[ce] = NULL;
			g_sr_pa[ce] = g_dr_pa[ce] = g_buf_pa[ce] = 0;
			if (hif_shape[ce].src_n > 0) {
				g_sr_desc[ce] = (ce == 0 ? s_src0 :
				    ce == 3 ? s_src3 :
				    ce == 4 ? g_dma->src4 : g_dma->src7);
				pa = hif_pa_of(g_sr_desc[ce]);
				g_sr_pa[ce] = pa;
			}
		}
		g_dr_desc[2] = s_dst2;  g_dr_pa[2] = g_dst2_pa;  g_buf_va[2] = &s_rx2[0][0];
		g_buf_pa[2] = g_rx2_pa;
		g_dr_desc[1] = g_dma->dst1;  g_dr_pa[1] = hif_pa_of(g_dma->dst1);
		g_buf_va[1] = &g_dma->rx1[0][0];
		g_dr_desc[5] = g_dma->dst5;  g_dr_pa[5] = hif_pa_of(g_dma->dst5);
		g_buf_va[5] = &g_dma->rx5[0][0];
		g_dr_desc[7] = g_dma->dst7;  g_dr_pa[7] = hif_pa_of(g_dma->dst7);
		g_buf_va[7] = &g_dma->rx7[0][0];
		g_dr_desc[8] = g_dma->dst8;  g_dr_pa[8] = hif_pa_of(g_dma->dst8);
		g_buf_va[8] = &g_dma->rx8[0][0];
		g_dr_desc[9] = g_dma->dst9;  g_dr_pa[9] = hif_pa_of(g_dma->dst9);
		g_buf_va[9] = &g_dma->rx9[0][0];
		g_dr_desc[10] = g_dma->dst10; g_dr_pa[10] = hif_pa_of(g_dma->dst10);
		g_buf_va[10] = &g_dma->rx10[0][0];
		g_dr_desc[11] = g_dma->dst11; g_dr_pa[11] = hif_pa_of(g_dma->dst11);
		g_buf_va[11] = &g_dma->rx11[0][0];
		g_buf_pa[1] = hif_pa_of(g_buf_va[1]);
		g_buf_pa[5] = hif_pa_of(g_buf_va[5]);
		g_buf_pa[7] = hif_pa_of(g_buf_va[7]);
		g_buf_pa[8] = hif_pa_of(g_buf_va[8]);
		g_buf_pa[9] = hif_pa_of(g_buf_va[9]);
		g_buf_pa[10] = hif_pa_of(g_buf_va[10]);
		g_buf_pa[11] = hif_pa_of(g_buf_va[11]);
		for (ce = 0; ce < 12; ce++) {
			if (hif_shape[ce].dst_n > 0 &&
			    (!g_dr_pa[ce] || !g_buf_pa[ce])) {
				HIF_LOG("cannot resolve CE%u ring physical addresses", ce);
				return;
			}
		}
	}
	HIF_LOG("rings: src0=%llx dst2=%llx rx2=%llx src3=%llx mode=%d",
	    (unsigned long long)g_src0_pa, (unsigned long long)g_dst2_pa,
	    (unsigned long long)g_rx2_pa, (unsigned long long)g_src3_pa, g_hif_mode);

	for (i = 0; i < CE2_ENTRIES; i++) {
		s_dst2[i].addr = (g_rx2_pa + (uint64_t)i * CE_BUF_BYTES) & CE_DESC_ADDR_MASK;
		/* Post with a zero length; the CE fills it in on completion. */
		s_dst2[i].nbytes = 0;
		s_dst2[i].flags = 0;
		s_dst2[i].toeplitz = 0;
	}
	hif_mb();

	g_sw2 = 0;
	g_post2 = 0;
	g_wr0 = g_wr3 = 0;
	g_wr4 = 0;
	/* snoc.c ath10k_snoc_rx_post_pipe() leaves one slot free, so the CE's
	 * write index sits at dst_n-1 and the host consumes from 0. */
	g_sw1 = 0;
	g_post1 = (hif_shape[1].dst_n > 0) ? (uint32_t)(hif_shape[1].dst_n - 1) : 0;
	g_htt_rx = 0;
	g_polls = 0;
	g_rx_packets = 0;
	g_beacons = 0;
	g_bases_programmed = false;
	g_stage = (g_hif_mode >= 1 && g_hif_mode <= 3) ? g_hif_mode : 1;
	g_state = HIF_CE_UP;
	g_regs_live = true;

	HIF_LOG("descriptors ready, mode %d stage %d", g_hif_mode, g_stage);
	hif_bb(BB_INIT_ENTRY, (uint32_t)g_hif_mode, (uint32_t)g_post_count);
	if (g_hif_mode == 2) hif_program_bases();
	if (g_hif_mode == 3) {
		hif_program_bases();
		hif_post_buffers();
	}
}

