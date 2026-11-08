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

#define HIF_LOG(fmt, ...)   printf("WCN3990-HIF: " fmt "\n", ##__VA_ARGS__)

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
#define HTT_T2H_MSG_TYPE_VERSION_CONF 0
#define HTT_RX_RING_ENTRIES           128
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
	/* wcn3990 keeps the per-CE read indices in DDR (hw_params.rri_on_ddr);
	 * ce.c: ath10k_ce_alloc_rri() points the wrapper at this array. */
	uint32_t rri[16];
	/* HTT RX ring handed to the target by RX_RING_CFG: an array of buffer
	 * physical addresses plus the 4-byte alloc index the firmware bumps.
	 * Without this the target has nowhere to put anything it receives. */
	uint64_t htt_ring[HTT_RX_RING_ENTRIES];
	uint32_t htt_alloc_idx;
	uint8_t htt_buf[HTT_RX_RING_ENTRIES][HTT_RX_BUF_SIZE];
};

#define HIF_DMA_SIZE   0x100000u         /* 1 MiB: rings + CE bufs + HTT RX ring */
/* 8 rotating CE3 buffers and the 256 KiB HTT RX ring must fit; make an
 * overflow a build error. */
typedef char hif_dma_window_check[(sizeof(struct hif_dma_block) <= HIF_DMA_SIZE) ? 1 : -1];
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
static int g_wmi_init_full;   /* wmiinit=1: Linux WCN3990 resource config */
static int g_scan_chans = -1; /* scanchans=N: cap the START_SCAN channel list */
/* wmi-tlv.c ath10k_wmi_tlv_iter() advances by exactly tlv_len, with no
 * roundup.  This driver used to round every TLV up to 4 bytes.  Three of
 * the TLVs we emit are not 4-aligned (VDEV_CREATE 18, scan bssid 6, base
 * MAC 6), so two zero bytes were injected after each: VDEV_CREATE ended
 * with a 2-byte tail that cannot even hold a TLV header.  tlvpad=1 restores
 * the old behaviour as an in-chain negative control. */
static int g_tlv_pad;         /* 0 = Linux-exact TLV lengths */
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

	HIF_LOG("ce%u send: desc %u pa %llx len %u -> wr %u (reg %x shadow %x)",
	    ce, idx, (unsigned long long)pa, len, *wr,
	    hif_reg_read(ce, CE_SR_WR_INDEX),
	    shadow ? hif_shadow_read((uint32_t)(uintptr_t)shadow) : 0);
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
	hif_reg_write(2, CE_DST_WR_INDEX, g_post2);
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
	uint32_t drri = hif_reg_read(1, CE_CURRENT_DRRI) & (n - 1);

	if (g_dr_desc[1] == NULL || g_buf_va[1] == NULL) return;
	while (budget-- > 0 && drri != g_sw1) {
		struct wcn_ce_desc_64 *d = &g_dr_desc[1][g_sw1];
		const uint8_t *b = g_buf_va[1] +
		    (uint64_t)g_sw1 * hif_shape[1].buf_sz;
		uint32_t nbytes = d->nbytes;

		if (nbytes == 0) break;          /* not completed yet */
		if (nbytes > hif_shape[1].buf_sz) nbytes = hif_shape[1].buf_sz;
		d->nbytes = 0;                   /* claim before reposting */
		/* Every CE buffer starts with the 8-byte HTC header; the
		 * htt_resp (and therefore the message type) follows it.  Reading
		 * b[0] here returns the endpoint id, not the message type. */
		if (g_htt_rx < 8) {
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
			g_htt_rx++;
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
			hif_reg_write(1, CE_DST_WR_INDEX, g_post1);
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
#define WMI_SCAN_EVENTID              0x3001
#define WMI_MGMT_RX_EVENTID           0x7001   /* WMI_GRP_MGMT  = 7 */

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
#define WMI_TLV_ARRAY_UINT32          17
#define WMI_TLV_ARRAY_BYTE            18
#define WMI_TLV_ARRAY_STRUCT          19
#define WMI_TLV_ARRAY_FIXED_STRUCT    20
#define WMI_TLV_MGMT_RX_HDR           44
#define WMI_TLV_INIT_CMD              74
#define WMI_TLV_RESOURCE_CONFIG       75
#define WMI_TLV_START_SCAN_CMD        77
#define WMI_TLV_PDEV_SET_REGDOMAIN_CMD 81
#define WMI_TLV_SCAN_CHAN_LIST_CMD    79
#define WMI_TLV_CHANNEL               80
#define WMI_TLV_VDEV_CREATE_CMD       86      /* wmi-tlv.h enum wmi_tlv_tag */
#define WMI_TLV_VDEV_UP_CMD           92
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

static void
hif_service_htt_ring(void)
{
	uint32_t fw_idx, budget = HTT_RX_RING_ENTRIES;

	if (!(g_htt_ring & 1) || !g_htt_ring_done || g_dma == NULL) return;

	fw_idx = g_dma->htt_alloc_idx & (HTT_RX_RING_ENTRIES - 1);
	while (g_htt_ring_rd != fw_idx && budget-- > 0) {
		const uint8_t *b = g_dma->htt_buf[g_htt_ring_rd];

		if (g_htt_ring_seen < 8) {
			/* rx_desc.h: struct rx_attention sits at offset 4 and
			 * rx_hdr_status at 296 for the wcn3990 descriptor. */
			HIF_LOG("htt ring #%u idx=%u attn=%02x%02x%02x%02x hdr0=%02x%02x",
			    g_htt_ring_seen, g_htt_ring_rd,
			    b[7], b[6], b[5], b[4], b[297], b[296]);
			g_htt_ring_seen++;
		}
		g_htt_ring_rd = (g_htt_ring_rd + 1) &
		    (HTT_RX_RING_ENTRIES - 1);
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
	uint32_t i;

	if (!g_dma) return -1;
	ring_pa = hif_pa_of(g_dma->htt_ring);
	idx_pa = hif_pa_of(&g_dma->htt_alloc_idx);
	if (!ring_pa || !idx_pa) return -1;

	/* Hand every buffer to the target before announcing the ring.
	 * htt_rx.c ath10k_htt_set_paddrs_ring_64() stores the plain 64-bit
	 * paddr, no descriptor mask. */
	for (i = 0; i < HTT_RX_RING_ENTRIES; i++) {
		g_dma->htt_ring[i] = hif_pa_of(g_dma->htt_buf[i]);
	}
	g_dma->htt_alloc_idx = 0;
	hif_mb();

	memset(p, 0, sizeof(p));
	p[0] = 1;                              /* num_rings */
	/* p[1..2] = rsvd0 */
	hif_put64(p + 3, idx_pa);              /* fw_idx_shadow_reg_paddr */
	hif_put64(p + 11, ring_pa);            /* rx_ring_base_paddr */
	hif_put16(p + 19, HTT_RX_RING_ENTRIES);/* rx_ring_len (entries) */
	hif_put16(p + 21, HTT_RX_BUF_SIZE);    /* rx_ring_bufsize */
	hif_put16(p + 23, 0xffff);             /* every report type */
	hif_put16(p + 25, 0);                  /* fw_idx_init_val */
	for (i = 0; i < 10; i++) hif_put16(p + 27 + i * 2, g_htt_rx_offsets[i]);

	HIF_LOG("HTT: send RX_RING_CFG ring=%llx idx=%llx n=%u buf=%u",
	    (unsigned long long)ring_pa, (unsigned long long)idx_pa,
	    HTT_RX_RING_ENTRIES, HTT_RX_BUF_SIZE);
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
	/* bit0 = RX_RING_CFG, bit1 = AGGR_CFG.  They are split because the
	 * pair sent together killed the target with a platform error; one
	 * boot per bit tells which command the firmware refuses. */
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
		HIF_LOG("WMI: INIT full Linux cfg vdevs=%u peers=%u tids=%u "
		    "msdu=%u decap=%u max_frag=%u host_capab=0x%x",
		    cfg[0], cfg[1], cfg[5], cfg[28], cfg[13], cfg[29], cfg[43]);
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
	uint8_t tlv[32];
	uint8_t *p = tlv;
	struct {
		uint32_t reg_domain;
		uint32_t reg_domain_2g;
		uint32_t reg_domain_5g;
		uint32_t limit_2g;
		uint32_t limit_5g;
	} __attribute__((packed)) cmd;

	memset(tlv, 0, sizeof(tlv));
	memset(&cmd, 0, sizeof(cmd));
	p = hif_put_tlv(p, WMI_TLV_PDEV_SET_REGDOMAIN_CMD, (uint16_t)sizeof(cmd), &cmd);
	HIF_LOG("WMI: send PDEV_SET_REGDOMAIN (world)");
	return hif_wmi_command(WMI_PDEV_SET_REGDOMAIN_CMDID, tlv, (uint32_t)(p - tlv));
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
		hif_put32(ch + 12, f < 3000 ? 1u : 0u); /* mode: 11G(1) / 11A(0) */
		ch[16] = 0;                            /* min_power */
		ch[17] = 40;                           /* max_power, 0.5 dBm */
		ch[18] = 40;                           /* reg_power, 0.5 dBm */
		ch[19] = 0;                            /* reg_classid */
		ch[20] = 0;                            /* antenna_max */
		ch[21] = 40;                           /* max_tx_power */
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
		uint8_t bssid[6];
	} __attribute__((packed)) cmd;

	memset(tlv, 0, sizeof(tlv));
	memset(&cmd, 0, sizeof(cmd));
	cmd.bssid[0] = 0x02;
	cmd.bssid[5] = 0x01;
	p = hif_put_tlv(p, WMI_TLV_VDEV_UP_CMD, (uint16_t)sizeof(cmd), &cmd);
	HIF_LOG("WMI: send VDEV_UP id=0 (%u TLV bytes)", (unsigned)(p - tlv));
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
		uint8_t mac[6];
	} __attribute__((packed)) cmd;

	memset(tlv, 0, sizeof(tlv));
	memset(&cmd, 0, sizeof(cmd));
	cmd.vdev_id = 0;
	cmd.vdev_type = 2;                     /* WMI_VDEV_TYPE_STA */
	cmd.vdev_subtype = 0;                  /* WMI_VDEV_SUBTYPE_NONE */
	cmd.mac[0] = 0x02;                     /* locally administered */
	cmd.mac[1] = 0x00; cmd.mac[2] = 0x00;
	cmd.mac[3] = 0x00; cmd.mac[4] = 0x00; cmd.mac[5] = 0x01;

	p = hif_put_tlv(p, WMI_TLV_VDEV_CREATE_CMD, (uint16_t)sizeof(cmd), &cmd);
	HIF_LOG("WMI: send VDEV_CREATE id=0 type=STA");
	return hif_wmi_command(WMI_VDEV_CREATE_CMDID, tlv, (uint32_t)(p - tlv));
}

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
	uint8_t bssid[6];

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

	/* ath10k_wmi_start_scan_init() always asks for one broadcast BSSID;
	 * without it the firmware does not send broadcast probe requests. */
	for (i = 0; i < 6; i++) bssid[i] = 0xff;

	for (i = 0; i < nch; i++) hif_put32(chans + i * 4, g_scan_freqs[i]);

	p = hif_put_tlv(p, WMI_TLV_START_SCAN_CMD, (uint16_t)sizeof(cmd), &cmd);
	p = hif_put_tlv(p, WMI_TLV_ARRAY_UINT32, (uint16_t)(nch * 4), chans);
	p = hif_put_tlv(p, WMI_TLV_ARRAY_FIXED_STRUCT, 0, NULL);  /* ssids */
	p = hif_put_tlv(p, WMI_TLV_ARRAY_FIXED_STRUCT, (uint16_t)sizeof(bssid), bssid);
	p = hif_put_tlv(p, WMI_TLV_ARRAY_BYTE, 0, NULL);          /* ie */

	HIF_LOG("WMI: send START_SCAN (scan_id 0x%x, linux=%d, %u channels, %u ms dwell, %u TLV bytes)",
	    cmd.common[0], g_scan_linux, nch, dwell_ms, (unsigned)(p - tlv));
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
		/* Same exact-length walk as ath10k_wmi_tlv_iter(). */
		off += 4 + (g_tlv_pad ? ((tlv_len + 3) & ~3u) : tlv_len);
	}

	if (!frame || !buf_len || buf_len > frame_len) return;
	if (rssi > 0 || rssi < -120) rssi = -60;
	if (g_frame_sink) {
		uint16_t freq = (channel > 1000 && channel < 6000) ? (uint16_t)channel : 2412;
		g_frame_sink(frame, (size_t)buf_len, (int16_t)rssi, freq);
		g_beacons++;
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
	uint8_t mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };

	memset(tlv, 0, sizeof(tlv));
	p = hif_put_tlv(p, WMI_TLV_PDEV_SET_BASE_MACADDR_CMD,
	    (uint16_t)sizeof(mac), mac);
	HIF_LOG("WMI: send PDEV_SET_BASE_MACADDR %02x:%02x:%02x:%02x:%02x:%02x",
	    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return hif_wmi_command(WMI_PDEV_SET_BASE_MACADDR_CMDID, tlv,
	    (uint32_t)(p - tlv));
}

static void
hif_finish_vdev(void)
{
	if (g_vdev_pre) (void)hif_send_base_macaddr();
	if (hif_send_vdev_create() == 0) {
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
		/* mac.c ath10k_update_channel_list() runs before regd_update():
		 * the target gets its channel/power table first. */
		if (g_chan_list) (void)hif_send_scan_chan_list();
		/* hifstop=1/3: skip PDEV_SET_REGDOMAIN.  The WCN3990 firmware
		 * has a documented deterministic crash shortly after vdev_create;
		 * bisecting which host command precedes it tells us whether the
		 * firmware is dying on our input or on its own. */
		if (g_ready_stop == 1 || g_ready_stop == 3) {
			HIF_LOG("WMI: READY stop=%u, skipping PDEV_SET_REGDOMAIN",
			    g_ready_stop);
		} else {
			hif_send_pdev_regdomain();
		}
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

	/* Heartbeat every ~1000 polls, and advance the staged ladder.  This is
	 * deliberately cheap: no cache flushes and no CE register access until
	 * the corresponding stage is entered. */
	if ((g_polls % 1000) == 0) {
		HIF_LOG("heartbeat polls=%u stage=%d state=%d rx=%u beacons=%u desc0.nbytes=%u drri=%x",
		    g_polls, g_stage, (int)g_state, g_rx_packets, g_beacons,
		    (unsigned)s_dst2[0].nbytes,
		    g_bases_programmed ? hif_reg_read(2, CE_CURRENT_DRRI) : 0);
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
		g_autoscan_sent = 1;
		HIF_LOG("autoscan: WMI ready, issuing START_SCAN");
		(void)wcn3990_hif_start_scan(40);
	}
	if (g_scan_active) g_scan_polls++;

	if (g_ce_quiet) {
		/* The console dies at the doorbell, so anything learned here has to
		 * reach the next boot through the IMEM box: one probe every ~5 s,
		 * each recorded as two records (BB_SMMU_CB a1=probe# a2=DRRI,
		 * BB_CE_ACCESS a1=0x5150|probe# a2=rx bytes).  A DRRI that moves
		 * while the driver never touches the engines means the target is
		 * consuming buffers and the wlan is further along than the console
		 * ever showed. */
		if (g_quiet_probe_polls != 0 && g_polls >= g_quiet_probe_polls) {
			uint32_t drri = hif_reg_read(2, CE_CURRENT_DRRI);
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

	/* ath10k_ce_per_engine_service() / CoachZ wc_service() both acknowledge
	 * the copy-complete, error and watermark status bits (W1C) before they
	 * look at the ring.  Leaving them set is what stops the CE after a few
	 * completions - every build so far froze on the fifth receive. */
	hif_reg_write(2, CE_STATUS, 0x1u | 0x1eu);

	/* The block is Normal non-cacheable, so the CE's write to the DDR read
	 * index and to each descriptor's nbytes is visible immediately: no
	 * invalidate, no clean, and no risk of writing back a stale line. */
	/* ath10k_ce_dest_ring_read_index_get(): a pipe without CE_ATTR_DIS_INTR
	 * (CE2 is one) reads the CE's CURRENT_DRRI register.  The DDR RRI word
	 * packs src in [15:0] and dst in [31:16] and is only used for the
	 * DIS_INTR pipes, so indexing s_rri[2] directly was reading the wrong
	 * half of the wrong source. */
	{
		uint32_t drri = hif_reg_read(2, CE_CURRENT_DRRI) & (CE2_ENTRIES - 1);

		while (budget-- > 0 && drri != g_sw2) {
			struct wcn_ce_desc_64 *d = &s_dst2[g_sw2];
			uint32_t idx = g_sw2, nbytes;

			/* ath10k: a zero length is a descriptor the CE has not
			 * completed yet, not a full-size frame. */
			nbytes = d->nbytes;
			if (nbytes == 0) break;   /* CE has not completed this one yet */
			if (nbytes > CE_BUF_BYTES) nbytes = CE_BUF_BYTES;
			d->nbytes = 0;            /* claim it before reposting */

			if (g_rx_packets < 16) hif_bb(BB_RX, idx, nbytes);
			hif_handle_rx(s_rx2[idx], nbytes);

			hif_ce2_repost(idx);
			g_sw2 = (g_sw2 + 1) & (CE2_ENTRIES - 1);
		}
	}

	if (g_state == HIF_CE_UP && (g_polls % 2000) == 0) {
		hif_reg_write(2, CE_DST_WR_INDEX, g_post2);
	}

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

	/* Black box first: print whatever the previous boot managed to write
	 * into IMEM before its reset, then start recording this boot. */
	hif_bb_init();
	hif_bb_dump();

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
	HIF_LOG("CE3 tx buffers: %s; autoscan=%d; hifstop=%d; vdevpre=%d; "
	    "wmiinit=%d; scanchans=%d; tlvpad=%d; chanlist=%d; vdevup=%d; "
	    "scanlinux=%d; httconn=%d; httsetup=%d; httring=%d",
	    g_tx3_rotate ? "8 rotating" : "single (legacy)", g_autoscan,
	    g_ready_stop, g_vdev_pre, g_wmi_init_full, g_scan_chans,
	    g_tlv_pad, g_chan_list, g_vdev_up, g_scan_linux, g_htt_connect,
	    g_htt_setup, g_htt_ring);
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

