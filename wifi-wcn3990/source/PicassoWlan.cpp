/*
 * picasso (SM7250 / lito): boot the modem DSP (MPSS) through TrustZone PAS.
 *
 * On this SoC the WLAN firmware runs inside the modem (the mss node owns
 * modem_wlan_region@8c000000), so a running modem is Wi-Fi step 1: the
 * WLAN services (WLFW on QRTR.0/1) come up with it.
 *
 * Sequence, as Linux qcom_mdt_load() + qcom_q6v5_pas() do it:
 *   PIL is_supported -> init_image (.mdt metadata) -> mem_setup (only for
 *   relocatable images) -> copy the .bNN segments into the carveout
 *   0x8C000000+0xF800000 -> auth_and_reset -> watch the modem's SMP2P
 *   "slave-kernel" value (bit0 fatal, bit1 ready, bit2 handover).
 *
 * Firmware is read from the GPT "modem" partition (vfat, the stock files
 * this phone's EFS matches) through sweet_ufs_pread(); nothing is copied
 * or modified. Boot-arg modem=1, polling only: no interrupts, no EFS.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOBSD.h>
#include <kern/thread.h>
#include <kern/debug.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <sys/errno.h>
#include <machine/atomic.h>

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" void flush_dcache64(addr64_t pa, unsigned int len, boolean_t phys);
extern "C" ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
extern "C" int hakios_ufs_find_part(const char *name, uint64_t *first, uint64_t *nblocks, uint32_t *part_idx);
extern "C" int hakios_ufs_read_part(uint32_t part, uint64_t offset, uint8_t *dst, uint64_t len);
extern "C" int hakios_ufs_read_partition(const char *name, uint64_t offset, uint8_t *dst, uint64_t len, uint64_t *bytes);
extern "C" bool hakios_aop_mailbox_send(const char *text);
extern "C" bool hakios_mpss_rails(uint32_t level);
extern "C" void picasso_usb_console_drive_poll(void);
extern "C" void picasso_usb_reboot_fastboot(void);
extern "C" void machine_delay_until(uint64_t interval, uint64_t deadline);
extern "C" void nanoseconds_to_absolutetime(uint64_t nanoseconds, uint64_t *result);

static inline void
picasso_sleep_us(uint32_t us)
{
	uint64_t abstime = 0;
	nanoseconds_to_absolutetime((uint64_t)us * 1000ull, &abstime);
	machine_delay_until(abstime, mach_absolute_time() + abstime);
}

static inline void
picasso_sleep_ms(uint32_t ms)
{
	uint64_t abstime = 0;
	nanoseconds_to_absolutetime((uint64_t)ms * 1000000ull, &abstime);
	machine_delay_until(abstime, mach_absolute_time() + abstime);
}

#undef IOSleep
#define IOSleep(ms) picasso_sleep_ms(ms)
#undef IODelay
#define IODelay(us) picasso_sleep_us(us)

#define LOG(fmt, ...)   printf("PicassoModem: " fmt "\n", ##__VA_ARGS__)

extern "C" int sweet_ufs_part(const char *name, uint64_t *first, uint64_t *nblocks, uint32_t *lun)
{
	uint64_t f = 0, nb = 0;
	uint32_t idx = 0;
	int err = hakios_ufs_find_part(name, &f, &nb, &idx);
	if (err) {
		return err;
	}
	if (first) *first = f;
	if (nblocks) *nblocks = nb;
	if (lun) *lun = idx;
	return 0;
}

extern "C" int sweet_ufs_pread(uint32_t lun, uint64_t first, uint64_t nblocks, uint64_t off, void *dst, uint32_t len)
{
	(void)first; (void)nblocks;
	return hakios_ufs_read_part(lun, off, (uint8_t *)dst, len);
}

extern "C" int sweet_ufs_pwrite(uint32_t lun, uint64_t first, uint64_t nblocks, uint64_t off, const void *src, uint32_t len)
{
	(void)lun; (void)first; (void)nblocks; (void)off; (void)src; (void)len;
	return 0;
}

extern "C" int sweet_rpmh_vreg_enable(const char *res, unsigned mv)
{
	(void)res; (void)mv;
	return 0;
}

extern "C" int hakios_wlan_arc_level(const char *res, unsigned level);

/* The vendor path asks RPMh for one ARC level per rail; this board's answer comes
 * from HakiosQmp.cpp (cmd-db lookup + TCS 0 write), not from a UEFI leftover. */
extern "C" int sweet_rpmh_arc_level(const char *res, unsigned level)
{
	return hakios_wlan_arc_level(res, level);
}

extern "C" uint64_t sweet_log_total_g = 0;
extern "C" unsigned sweet_log_seq_g = 0, sweet_log_fails_g = 0;
extern "C" unsigned sweet_log_started_g = 0;
extern "C" uint64_t sweet_log_base_g = 0, sweet_log_nblocks_g = 0;
extern "C" uint64_t sweet_log_position(void) { return 0; }

#define LOG(fmt, ...)   printf("PicassoModem: " fmt "\n", ##__VA_ARGS__)

#ifndef MIN
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b)       ((a) > (b) ? (a) : (b))
#endif

#define PAS_ID_MODEM        4
#define SCM_FN(svc, cmd)    (0x42000000u | ((uint32_t)(svc) << 8) | (uint32_t)(cmd))
#define SCM_SVC_PIL         0x02
#define PIL_INIT_IMAGE      0x01
#define PIL_MEM_SETUP       0x02
#define PIL_AUTH_RESET      0x05
#define PIL_SHUTDOWN        0x06
#define PIL_IS_SUPPORTED    0x07
#define SCM_ARG_RW          2

#define PT_LOAD             1
#define MDT_TYPE_MASK       (7u << 24)
#define MDT_TYPE_HASH       (2u << 24)
#define MDT_RELOCATABLE     (1u << 27)

/* picasso device tree: smem_region@80900000 (2 MiB), mss memory-region
 * modem_wlan_region@8c000000 (248 MiB). */
#define WLAN_MSA_PA         0x8BA00000ull            /* qcom,wlan-msa-fixed-region */
#define WLAN_MSA_SIZE       0x00200000u

#define SMEM_PA             0x80900000ull
#define SMEM_SIZE           0x00200000u
#define MODEM_CARVEOUT      0x8C000000ull
#define MODEM_CARVEOUT_SIZE 0x0F800000ull

/* Apps SMMU (MMU-500).  SMR: [31] VALID, [30:16] MASK (1 = ignore that ID
 * bit), [14:0] ID - the layout arm-smmu.c uses (SMR_ID_SHIFT 0,
 * SMR_MASK_SHIFT 16).  S2CR: [17:16] TYPE (0 translation, 1 bypass, 2 fault),
 * [7:0] CBNDX.  Stream 0x20 is the WLAN copy engine.
 *
 * TESTING NOTE: TYPE=1 (bypass) is not permitted on this SMMU - the store
 * reads back as TYPE=2 (fault).  v345 lost its USB console that way, and every
 * "S2CR=bypass" write in v335-v353 read back as 00020000.  Passthrough has to
 * go through a context bank whose SCTLR.M is 0 instead; bank 61 on this device
 * has CBAR=0, CBACR=0 and SCTLR.M=0, which is exactly what the USB console
 * driver uses for its own stream. */
#define WLAN_SMMU_PA                0x15000000ull
#define WLAN_SMMU_SID               0x20u
#define WLAN_SMMU_SMR_VALID         (1u << 31)
#define WLAN_SMMU_SMR_MASK_SHIFT    16
#define WLAN_SMMU_S2CR_BYPASS       (1u << 16)
#define WLAN_SMMU_PASSTHRU_BANK     61u

static bool
wlan_smmu_smr_matches(uint32_t smr, uint32_t sid)
{
	uint32_t mask, entry_sid;

	if (!(smr & WLAN_SMMU_SMR_VALID)) return false;
	mask = (smr >> WLAN_SMMU_SMR_MASK_SHIFT) & 0x7fffu;
	entry_sid = smr & 0x7fffu;
	return ((entry_sid ^ sid) & ~mask & 0x7fffu) == 0;
}

/*
 * Claim stream 0x20 for the WLAN copy engines.
 *
 * This SMMU accepts stream-match writes only during a short window.  v356
 * measured both ends on one image: the console driver's own bind (which also
 * brings the SMMU up) writes SMR[7]=800000e0 and reads it back, while the very
 * same two stores issued from the modem thread later - and from a build with
 * wlan_delay_ms=8000 just as well - read back as 0 / 0000003d, i.e. the S2CR
 * landed and the SMR did not.  So the claim has to be retried from a thread
 * that runs while the SMMU is alive, not issued once at a guessed time.
 *
 * Returns 0 once the table holds an exact entry for the WLAN stream.
 */
extern "C" int
picasso_wlan_smmu_claim(void)
{
	/* Map once and reuse: ml_io_map() panics when kmem_alloc cannot find
	 * the VA range, and the console driver's 0xd0000-byte mapping is not
	 * available to us at this point (v357 died in kmem_alloc there).  The
	 * stream-match and S2CR arrays both live in the first 4 KiB. */
	static volatile uint32_t *smmu;
	static uint32_t attempts;
	static bool map_failed;
	volatile uint32_t *r;
	uint32_t count, i, smr_val, probe;
	bool trace;
	int exact = -1, first = -1, free_slot = -1;

	if (smmu == NULL) {
		vm_offset_t va = ml_io_map(WLAN_SMMU_PA, 0x1000);
		if (va == 0) {
			/* v360: the bsd_init hook never emitted a line.  It was
			 * compiled in (the link map shows _bsd_init referencing
			 * this symbol) so the only silent exit is this one. */
			if (!map_failed) {
				map_failed = true;
				LOG("wlan smmu: ml_io_map(4K) failed (attempt %u)",
				    attempts + 1);
			}
			return -1;
		}
		smmu = (volatile uint32_t *)va;
	}
	r = smmu;
	attempts++;

	/* wlan_smmu_probe_val replaces the value written to the stream-match
	 * register, which is how the "is it the SID or the store that the
	 * hardware refuses" question gets answered without another guess. */
	probe = 0;
	(void)PE_parse_boot_argn("wlan_smmu_probe_val", &probe, sizeof(probe));
	smr_val = probe ? probe : (WLAN_SMMU_SMR_VALID | WLAN_SMMU_SID);
	trace = (attempts <= 6) || ((attempts % 300) == 0);

	/* Until the console driver has brought the SMMU up the whole page reads
	 * back as zero. */
	if (r[0x800 >> 2] == 0 && r[0xc00 >> 2] == 0) {
		if (trace) {
			LOG("wlan smmu: claim #%u: scr0=%08x smr0=%08x s2cr0=%08x (not up)",
			    attempts, r[0], r[0x800 >> 2], r[0xc00 >> 2]);
		}
		return -1;
	}

	/* If the console driver managed to clear sCR0.USFCFG in the one window
	 * where global registers accept writes, unmatched streams already bypass
	 * translation and SID 20 needs no table entry at all. */
	if (r[0] != 0 && ((r[0] >> 10) & 1u) == 0) return 0;

	count = r[0x20 >> 2] & 0xffu;
	if (count == 0 || count > 128) count = 128;
	for (i = 0; i < count; i++) {
		uint32_t smr = r[(0x800 >> 2) + i];
		uint32_t s2;

		if (!(smr & WLAN_SMMU_SMR_VALID)) {
			if (free_slot < 0) free_slot = (int)i;
			continue;
		}
		if (!wlan_smmu_smr_matches(smr, WLAN_SMMU_SID)) continue;
		if (first < 0) first = (int)i;
		s2 = r[(0xc00 >> 2) + i];
		/* The console slot carries MASK=0xc0 so that 0xe0 and 0x20 share
		 * one entry.  Accept any matching translation route to the
		 * disabled-bypass bank; an exact id/mask is not required. */
		if (wlan_smmu_smr_matches(smr, WLAN_SMMU_SID) &&
		    ((s2 >> 16) & 3u) == 0 &&
		    (s2 & 0xffu) == WLAN_SMMU_PASSTHRU_BANK) {
			exact = (int)i;
		}
	}
	if (exact >= 0) return 0;
	if (free_slot < 0 || (first >= 0 && first < free_slot)) return -1;

	/* The console planner skips the first free slot on purpose - its SMR
	 * refuses non-secure stores - and writes the next one successfully.
	 * Mirror that here, with an explicit override for one-boot probes. */
	{
		int forced = -1;

		(void)PE_parse_boot_argn("wlan_smmu_slot", &forced,
		    sizeof(forced));
		if (forced >= 0 && forced < (int)count) {
			free_slot = forced;
		} else if (free_slot + 1 < (int)count &&
		    !(r[(0x800 >> 2) + free_slot + 1] & WLAN_SMMU_SMR_VALID)) {
			free_slot++;
		}
	}

	/* S2CR first: the console driver writes the pair in this order and its
	 * readback passes, while every build that wrote SMR first got both
	 * registers back as 0 / 00020000.  The S2CR value is a translation
	 * entry aimed at a bank with SCTLR.M=0, which passes addresses
	 * through - TYPE=1 (bypass) is coerced to TYPE=2 (fault) here. */
	r[(0xc00 >> 2) + free_slot] = WLAN_SMMU_PASSTHRU_BANK;
	__asm__ volatile ("dsb sy" ::: "memory");
	r[(0x800 >> 2) + free_slot] = smr_val;
	__asm__ volatile ("dsb sy" ::: "memory");

	if (trace) {
		LOG("wlan smmu: claim #%u: slot %d smr %08x -> %08x, s2cr -> %08x",
		    attempts, free_slot, smr_val, r[(0x800 >> 2) + free_slot],
		    r[(0xc00 >> 2) + free_slot]);
	}

	if ((r[(0x800 >> 2) + free_slot] & 0x7fffu) == (smr_val & 0x7fffu)) {
		LOG("wlan smmu: SID 20 claimed SMR[%d]=%08x S2CR=%08x (attempt %u)",
		    free_slot, r[(0x800 >> 2) + free_slot],
		    r[(0xc00 >> 2) + free_slot], attempts);
		return 0;
	}

	/* The SMR store was refused.  If this window also accepts sCR0, the
	 * other way to let the CE reach memory is to stop denying unmatched
	 * streams (arm-smmu calls the bit disable_bypass).  Opt-in: it applies
	 * to every master without a stream entry. */
	{
		uint32_t allow = 0;

		(void)PE_parse_boot_argn("wlan_smmu_unmatched_bypass", &allow,
		    sizeof(allow));
		if (allow) {
			uint32_t scr0 = r[0];

			r[0] = scr0 & ~(1u << 10);
			__asm__ volatile ("dsb sy" ::: "memory");
			if (((r[0] >> 10) & 1u) == 0) {
				LOG("wlan smmu: USFCFG cleared (%08x -> %08x), unmatched streams bypass",
				    scr0, r[0]);
				return 0;
			}
		}
	}
	return -1;
}

/* qcom,smp2p-mpss: qcom,smem = <0x1b3 0x1ac> -> in 435, out 428 */
#define ITEM_SMP2P_OUT      428
#define ITEM_SMP2P_IN       435
#define ITEM_MSS_CRASH      421
#define SMP2P_MAGIC         0x504D5324u         /* "$SMP" */
#define SMP2P_MAX_ENTRY     16

struct elf32_ehdr {
	uint8_t  e_ident[16];
	uint16_t e_type, e_machine;
	uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
	uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed));

struct elf32_phdr {
	uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
} __attribute__((packed));

struct smem_pt_entry { uint32_t offset, size, flags; uint16_t host0, host1; uint32_t cacheline, rsvd[7]; } __attribute__((packed));
struct smem_pt { uint8_t magic[4]; uint32_t version, num_entries, rsvd[5]; } __attribute__((packed));
struct smem_part_hdr { uint8_t magic[4]; uint16_t host0, host1; uint32_t size, free_uncached, free_cached, rsvd[3]; } __attribute__((packed));
struct smem_priv_entry { uint16_t canary, item; uint32_t size; uint16_t pad_data, pad_hdr; uint32_t rsvd; } __attribute__((packed));
struct smp2p_hdr { uint32_t magic; uint8_t version, features[3]; uint16_t local_pid, remote_pid, total, valid; uint32_t flags; } __attribute__((packed));
struct smp2p_entry { char name[16]; uint32_t value; } __attribute__((packed));

/* qcom,smem hwlocks = <&tcsr_mutex 3>; tcsr-mutex syscon at 0x1F40000 */
#define TCSR_MUTEX_PA       0x1F40000ull
#define SMEM_HWLOCK         3
#define HWLOCK_APPS_ID      1

static uint8_t *smem;
static volatile uint8_t *tcsr_mutex;

/* ---------------------------------------------------------------- mappings */

/* Large or RAM regions: an IOMemoryDescriptor mapping (ml_io_map space is small). */
static IOMemoryMap *
map_phys(uint64_t pa, uint64_t size, bool wc)
{
	IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(pa, size, kIODirectionInOut);
	IOMemoryMap *m;

	if (md == NULL) {
		return NULL;
	}
	m = md->map(wc ? kIOMapWriteCombineCache : kIOMapInhibitCache);
	md->release();
	return m;
}

static inline void
mb(void)
{
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* ---------------------------------------------------------------- TrustZone */

static uint64_t
smc_call(uint64_t a[8])
{
	register uint64_t x0 __asm__("x0") = a[0], x1 __asm__("x1") = a[1], x2 __asm__("x2") = a[2],
	    x3 __asm__("x3") = a[3], x4 __asm__("x4") = a[4], x5 __asm__("x5") = a[5], x6 __asm__("x6") = a[6],
	    x7 __asm__("x7") = a[7];
	__asm__ volatile ("smc #0"
            : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
            :
            : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	a[0] = x0; a[1] = x1; a[2] = x2; a[3] = x3; a[4] = x4; a[5] = x5; a[6] = x6; a[7] = x7;
	return x0;
}

static unsigned scm_resumes;

static uint64_t
scm(uint32_t fn, uint64_t arginfo, uint64_t a, uint64_t b, uint64_t c, uint64_t *res1)
{
	uint64_t args[8] = { fn, arginfo, a, b, c, 0, 0, 0 };
	unsigned n;

	smc_call(args);
	/* long calls return INTERRUPTED (1): resume with x0 = 1 and the session from x6 */
	for (n = 0; args[0] == 1 && n < 100000; n++) {
		uint64_t again[8] = { 1, arginfo, a, b, c, 0, args[6], 0 };
		memcpy(args, again, sizeof(args));
		smc_call(args);
	}
	scm_resumes = n;
	if (res1 != NULL) {
		*res1 = args[1];
	}
	return args[0];
}

/* ---------------------------------------------------------------- vfat (read only) */

static struct fat_vol *g_fat;           /* volume the QRTR file service reads */
static uint32_t fw_dir = 1;             /* cluster of image/, 1 = not looked up yet */

struct fat_vol {
	uint32_t part;                  /* index into the GPT table built by HakiosCompat */
	uint64_t first, blocks;         /* absolute device LBA and length of the partition */
	uint32_t bps, spc, rsvd, nfats, root_ents, fatsz, root_clus;
	uint32_t root_sec, root_secs, data_sec, fat_ents;
	uint8_t *fat;                   /* raw FAT; 4-byte entries if FAT32, else 2-byte */
};

static int
fat_read32(const uint8_t *p, uint32_t off)
{
	uint32_t v;
	memcpy(&v, p + off, 4);
	return (int)v;
}

static int
fat_read16(const uint8_t *p, uint32_t off)
{
	uint16_t v;
	memcpy(&v, p + off, 2);
	return (int)v;
}

static int
fat_mount(struct fat_vol *v, uint32_t part, uint64_t first, uint64_t nblocks)
{
	uint8_t bs[512];
	uint64_t total_sec;
	int err;

	bzero(v, sizeof(*v));
	err = hakios_ufs_read_part(part, 0, bs, sizeof(bs));
	if (err) {
		LOG("vfat: cannot read boot sector: %d", err);
		return err;
	}
	if (bs[510] != 0x55 || bs[511] != 0xaa) {
		LOG("vfat: no boot signature");
		return EINVAL;
	}
	v->first = first;
	v->blocks = nblocks;
	v->part = part;
	v->bps = (uint32_t)fat_read16(bs, 11);
	v->spc = bs[13];
	v->rsvd = (uint32_t)fat_read16(bs, 14);
	v->nfats = bs[16];
	v->root_ents = (uint32_t)fat_read16(bs, 17);
	v->fatsz = (uint32_t)fat_read16(bs, 22);
	total_sec = (uint64_t)fat_read16(bs, 19);
	if (v->fatsz == 0) {
		v->fatsz = (uint32_t)fat_read32(bs, 36);
		v->root_clus = (uint32_t)fat_read32(bs, 44);
	}
	if (total_sec == 0) {
		total_sec = (uint32_t)fat_read32(bs, 32);
	}
	if (v->bps < 512 || v->bps > 4096 || (v->bps & (v->bps - 1)) || v->spc == 0 || v->nfats == 0 ||
	    v->fatsz == 0 || total_sec == 0 || total_sec > nblocks * (4096 / v->bps)) {
		LOG("vfat: bad geometry bps %u spc %u rsvd %u nfats %u fatsz %u total %llu",
		    v->bps, v->spc, v->rsvd, v->nfats, v->fatsz, total_sec);
		return EINVAL;
	}
	v->root_secs = (v->root_ents * 32 + v->bps - 1) / v->bps;
	v->root_sec = v->rsvd + v->nfats * v->fatsz;
	v->data_sec = v->root_sec + v->root_secs;
	if (v->root_clus == 0 && v->root_ents == 0) {
		LOG("vfat: neither a FAT16 root directory nor a FAT32 root cluster");
		return EINVAL;
	}
	v->fat_ents = v->fatsz * v->bps / (v->root_clus ? 4 : 2);

	v->fat = (uint8_t *)IOMalloc(v->fatsz * v->bps);
	if (v->fat == NULL) {
		return ENOMEM;
	}
	err = hakios_ufs_read_part(part, (uint64_t)v->rsvd * v->bps, v->fat, v->fatsz * v->bps);
	if (err) {
		LOG("vfat: cannot read the FAT: %d", err);
		return err;
	}
	g_fat = v;
	LOG("vfat: bps %u spc %u root %u entries, FAT %u KiB, data at sector %u%s",
	    v->bps, v->spc, v->root_ents, v->fatsz * v->bps >> 10, v->data_sec,
	    v->root_clus ? " (fat32)" : " (fat16)");
	return 0;
}

static uint32_t
fat_next(struct fat_vol *v, uint32_t clus)
{
	if (clus >= v->fat_ents) {
		return 0x0fffffffu;
	}
	if (v->root_clus) {
		uint32_t next;
		memcpy(&next, v->fat + (size_t)clus * 4, 4);
		return next & 0x0fffffffu;
	}
	uint16_t next;
	memcpy(&next, v->fat + (size_t)clus * 2, 2);
	return next;
}

/* Read len bytes of a file starting at cluster clus into dst (no bounce). */
static int
fat_read_chain(struct fat_vol *v, uint32_t clus, void *dst, uint32_t len)
{
	uint32_t per = v->spc * v->bps;
	uint8_t *out = (uint8_t *)dst;
	int err = 0;

	while (len && !err) {
		uint64_t off;
		uint32_t n;

		if (clus < 2 || clus >= v->fat_ents) {
			return EIO;
		}
		off = ((uint64_t)v->data_sec + (uint64_t)(clus - 2) * v->spc) * v->bps;
		n = MIN(len, per);
		err = hakios_ufs_read_part(v->part, off, out, n);
		out += n;
		len -= n;
		clus = fat_next(v, clus);
	}
	return err;
}

static void fat_name83(const char *name, char out[11]);

/* Directory into buf (up to cap bytes); dir_clus 0 = the FAT16 root area. */
static int
fat_dir_read(struct fat_vol *v, uint32_t dir_clus, void *buf, uint32_t cap, uint32_t *used)
{
	int err;

	*used = 0;
	if (dir_clus == 0) {
		if (v->root_clus != 0) {
			dir_clus = v->root_clus;
		} else {
			uint32_t n = MIN(cap, v->root_secs * v->bps);
			err = hakios_ufs_read_part(v->part, (uint64_t)v->root_sec * v->bps,
			    (uint8_t *)buf, n);
			if (!err) {
				*used = n;
			}
			return err;
		}
	}
	uint32_t per = v->spc * v->bps;
	while (dir_clus >= 2 && dir_clus < v->fat_ents && *used < cap) {
		uint64_t off = ((uint64_t)v->data_sec + (uint64_t)(dir_clus - 2) * v->spc) * v->bps;
		uint32_t n = MIN(cap - *used, per);

		err = hakios_ufs_read_part(v->part, off, (uint8_t *)buf + *used, n);
		if (err) {
			return err;
		}
		*used += n;
		dir_clus = fat_next(v, dir_clus);
	}
	return 0;
}

static bool
name_eq_ci(const char *a, const char *b)
{
	for (;; a++, b++) {
		char x = *a, y = *b;
		if (x >= 'A' && x <= 'Z') {
			x = (char)(x - 'A' + 'a');
		}
		if (y >= 'A' && y <= 'Z') {
			y = (char)(y - 'A' + 'a');
		}
		if (x != y) {
			return false;
		}
		if (x == 0) {
			return true;
		}
	}
}

/*
 * Look a file up by its VFAT long name (the stock "modem" partition keeps
 * modem.mdt and modem.bNN inside image/ with long names, so the 8.3 alias
 * is generated and useless). Falls back to the 8.3 name.
 */
static int
fat_lookup_in(struct fat_vol *v, uint32_t dir_clus, const char *name, uint32_t *clus, uint32_t *size)
{
	uint8_t *dir = (uint8_t *)IOMalloc(128 * 1024);
	char lfn[260];
	char name83[11];
	uint32_t used = 0;
	bool found = false;
	int err;

	if (dir == NULL) {
		return ENOMEM;
	}
	err = fat_dir_read(v, dir_clus, dir, 128 * 1024, &used);
	fat_name83(name, name83);
	lfn[0] = 0;
	for (uint32_t i = 0; !err && i + 32 <= used; i += 32) {
		const uint8_t *e = dir + i;
		if (e[0] == 0) {
			break;
		}
		if (e[0] == 0xe5) {
			lfn[0] = 0;
			continue;
		}
		if (e[11] == 0x0f) {                    /* VFAT long name, stored reversed */
			char part[14];
			unsigned k = 0;
			static const uint8_t at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
			for (unsigned j = 0; j < 13; j++) {
				uint16_t ch = (uint16_t)(e[at[j]] | e[at[j] + 1] << 8);
				if (ch == 0 || ch == 0xffff) {
					break;
				}
				part[k++] = (ch < 0x80) ? (char)ch : '?';
			}
			part[k] = 0;
			size_t have = strlen(lfn), add = strlen(part);
			if (have + add < sizeof(lfn)) {
				memmove(lfn + add, lfn, have + 1);
				memcpy(lfn, part, add);
			}
			continue;
		}
		if (e[11] & 0x08) {                     /* volume label (directories must match) */
			lfn[0] = 0;
			continue;
		}
		bool hit = (lfn[0] != 0 && name_eq_ci(lfn, name));
		if (!hit) {
			char shortname[13];
			unsigned c;
			for (c = 0; c < 8 && e[c] != ' '; c++) {
				shortname[c] = (char)e[c];
			}
			if (e[8] != ' ') {
				shortname[c++] = '.';
				for (unsigned j = 8; j < 11 && e[j] != ' '; j++) {
					shortname[c++] = (char)e[j];
				}
			}
			shortname[c] = 0;
			hit = name_eq_ci(shortname, name);
		}
		if (hit) {
			*clus = (uint32_t)fat_read16(e, 26) | ((uint32_t)fat_read16(e, 20) << 16);
			*size = (uint32_t)fat_read32(e, 28);
			found = true;
			break;
		}
		lfn[0] = 0;
	}
	IOFree(dir, 128 * 1024);
	if (err) {
		return err;
	}
	return found ? 0 : ENOENT;
}

static void
fat_name83(const char *name, char out[11])
{
	unsigned n = 0, i = 0;
	char c;

	bzero(out, 11);
	while (name[n] != 0 && name[n] != '.' && n < 8) {
		c = name[n++];
		out[i++] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
	}
	i = 8;
	if (name[n] == '.') {
		n++;
	}
	while (name[n] != 0 && i < 11) {
		c = name[n++];
		out[i++] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
	}
}

/* ---------------------------------------------------------------- SMEM */

static struct smem_pt *
smem_toc(void)
{
	/* The TOC sits at the end of SMEM on recent SoCs; older layouts put it
	 * first. Accept either, then scan the first 64 KiB in page steps. */
	struct smem_pt *pt;

	pt = (struct smem_pt *)(smem + SMEM_SIZE - 0x1000);
	if (memcmp(pt->magic, "$TOC", 4) == 0 && pt->num_entries <= 64) {
		return pt;
	}
	for (uint32_t off = 0; off < 0x10000; off += 0x1000) {
		pt = (struct smem_pt *)(smem + off);
		if (memcmp(pt->magic, "$TOC", 4) == 0 && pt->num_entries <= 64) {
			return pt;
		}
	}
	return NULL;
}

static struct smem_part_hdr *
smem_partition(uint16_t a, uint16_t b)
{
	struct smem_pt *pt = smem_toc();
	struct smem_pt_entry *e;

	if (pt == NULL) {
		return NULL;
	}
	e = (struct smem_pt_entry *)(pt + 1);
	for (uint32_t i = 0; i < pt->num_entries; i++) {
		if (e[i].offset != 0 && e[i].size != 0 && e[i].offset < SMEM_SIZE &&
		    ((e[i].host0 == a && e[i].host1 == b) || (e[i].host0 == b && e[i].host1 == a))) {
			return (struct smem_part_hdr *)(smem + e[i].offset);
		}
	}
	return NULL;
}

static void *
smem_get(struct smem_part_hdr *p, uint16_t item, uint32_t *size)
{
	uint8_t *e, *end;

	if (p == NULL || memcmp(p->magic, "$PRT", 4) != 0 || p->free_uncached > p->size) {
		return NULL;
	}
	e = (uint8_t *)p + sizeof(*p);
	end = (uint8_t *)p + p->free_uncached;
	while (e + sizeof(struct smem_priv_entry) <= end) {
		struct smem_priv_entry *h = (struct smem_priv_entry *)e;
		if (h->canary != 0xA5A5) {
			return NULL;
		}
		if (h->item == item) {
			if (size != NULL) {
				*size = h->size - h->pad_data;
			}
			return e + sizeof(*h) + h->pad_hdr;
		}
		e += sizeof(*h) + h->pad_hdr + h->size;
	}
	return NULL;
}

static bool
smem_lock(void)
{
	volatile uint32_t *reg = (volatile uint32_t *)(tcsr_mutex + SMEM_HWLOCK * 0x1000);

	if (tcsr_mutex == NULL) {
		return false;
	}
	for (int n = 0; n < 100000; n++) {
		*reg = HWLOCK_APPS_ID;
		mb();
		if (*reg == HWLOCK_APPS_ID) {
			return true;
		}
		IODelay(10);
	}
	LOG("SMEM hwlock busy: owner %u", *reg);
	return false;
}

static void
smem_unlock(void)
{
	mb();
	*(volatile uint32_t *)(tcsr_mutex + SMEM_HWLOCK * 0x1000) = 0;
	mb();
}

/* Append an uncached SMEM entry under the hwlock (qcom_smem_alloc_private). */
static void *
smem_alloc(struct smem_part_hdr *p, uint16_t item, uint32_t size)
{
	uint32_t aligned = (size + 7) & ~7u, need = sizeof(struct smem_priv_entry) + aligned;
	void *v = smem_get(p, item, NULL);

	if (v != NULL || p == NULL || !smem_lock()) {
		return v;
	}
	v = smem_get(p, item, NULL);
	if (v == NULL && p->free_uncached + need <= p->free_cached) {
		struct smem_priv_entry *h = (struct smem_priv_entry *)((uint8_t *)p + p->free_uncached);
		h->canary = 0xA5A5;
		h->item = item;
		h->size = aligned;
		h->pad_data = (uint16_t)(aligned - size);
		h->pad_hdr = 0;
		h->rsvd = 0;
		v = h + 1;
		bzero(v, aligned);
		mb();
		p->free_uncached += need;
		mb();
	} else if (v == NULL) {
		LOG("SMEM partition full: %x..%x need %x", p->free_uncached, p->free_cached, need);
	}
	smem_unlock();
	return v;
}

/*
 * Create the apps->modem SMP2P item before the modem boots, as qcom_smp2p
 * does: its "master-kernel" entry is the state the modem reads back. No
 * IPCC doorbell yet (the modem polls at boot); changes after that need one.
 */
static struct smp2p_hdr *
smp2p_out(struct smem_part_hdr *p)
{
	struct smp2p_hdr *o = (struct smp2p_hdr *)smem_alloc(p, ITEM_SMP2P_OUT,
	    sizeof(struct smp2p_hdr) + SMP2P_MAX_ENTRY * sizeof(struct smp2p_entry));

	if (o == NULL || o->version != 0) {
		return o;
	}
	o->magic = SMP2P_MAGIC;
	o->local_pid = 0;
	o->remote_pid = 1;
	o->total = SMP2P_MAX_ENTRY;
	o->valid = 0;
	o->features[0] = 1;                     /* SSR_ACK */
	bzero(o + 1, SMP2P_MAX_ENTRY * sizeof(struct smp2p_entry));
	mb();
	o->version = 1;                         /* validates the item */
	mb();
	return o;
}

static void
smp2p_add(struct smp2p_hdr *o, const char *name, uint32_t value)
{
	struct smp2p_entry *e;

	if (o == NULL || o->valid >= o->total || o->valid >= SMP2P_MAX_ENTRY) {
		return;
	}
	e = (struct smp2p_entry *)(o + 1);
	strlcpy(e[o->valid].name, name, sizeof(e[o->valid].name));
	e[o->valid].value = value;
	mb();
	o->valid++;
	mb();
}

/* ---------------------------------------------------------------- GLINK over SMEM */

/*
 * After READY the modem talks GLINK through two SMEM FIFOs (items 478 descriptor, 479 our TX,
 * 480 its TX), doorbell APCS bit 12. One channel, "IPCRTR", carries QRTR. Port of
 * topaz-woa drivers/TopazModem/Glink.c, polled from the watch thread (no interrupts).
 */
#define SMEM_GLINK_DESC     478
#define SMEM_GLINK_FIFO_TX  479
#define SMEM_GLINK_FIFO_RX  480
#define GLINK_TX_SIZE       0x4000u
#define GLINK_FEATURES      1                   /* INTENT_REUSE */

#define CMD_VERSION         0
#define CMD_VERSION_ACK     1
#define CMD_OPEN            2
#define CMD_CLOSE           3
#define CMD_OPEN_ACK        4
#define CMD_INTENT          5
#define CMD_RX_DONE         6
#define CMD_RX_INTENT_REQ   7
#define CMD_RX_INTENT_REQ_ACK 8
#define CMD_TX_DATA         9
#define CMD_CLOSE_ACK       11
#define CMD_TX_DATA_CONT    12
#define CMD_READ_NOTIF      13
#define CMD_RX_DONE_W_REUSE 14
#define CMD_SIGNALS         15

#define CHAN_NAME           "IPCRTR"
#define CHAN_LCID           1
#define OUR_INTENTS         8
#define OUR_INTENT_SIZE     0x4400u

#define QRTR_NODE_APPS      1
#define QRTR_PORT_CTRL      0xFFFFFFFEu
#define QRTR_NODE_BCAST     0xFFFFFFFFu
#define QRTR_TYPE_DATA      1
#define QRTR_TYPE_HELLO     2
#define QRTR_TYPE_BYE       3
#define QRTR_TYPE_NEW_SERVER 4
#define QRTR_TYPE_DEL_SERVER 5
#define QRTR_TYPE_DEL_CLIENT 6
#define QRTR_TYPE_RESUME_TX 7
#define QRTR_TYPE_NEW_LOOKUP 10
#define QRTR_TYPE_DEL_LOOKUP 11
#define QMI_SVC_WLFW        0x45

struct glink_msg { uint16_t cmd, p1; uint32_t p2; } __attribute__((packed));
struct glink_data_hdr { struct glink_msg m; uint32_t chunk, left; } __attribute__((packed));
struct qrtr_hdr_v1 { uint32_t version, type, src_node, src_port, confirm_rx, size, dst_node, dst_port; } __attribute__((packed));
struct qrtr_hdr_v2 { uint8_t version, type, flags, optlen; uint32_t size; uint16_t src_node, src_port, dst_node, dst_port; } __attribute__((packed));
struct qrtr_ctrl { uint32_t cmd, a, b, c, d; } __attribute__((packed));

static volatile uint8_t *ipcc;                   /* qcom,ipcc@408000 */
#define IPCC_BASE           0x408000ull
#define IPCC_REG_SEND_ID    0x0c
#define IPCC_CLIENT_MODEM   2
#define IPCC_SIGNAL_GLINK   0

static void
ipcc_kick(void)
{
	if (ipcc != NULL) {
		mb();
		*(volatile uint32_t *)(ipcc + IPCC_REG_SEND_ID) =
		    ((uint32_t)IPCC_CLIENT_MODEM << 16) | IPCC_SIGNAL_GLINK;
		mb();
	}
}

static volatile uint32_t *g_desc;               /* [0] tx tail [1] tx head [2] rx tail [3] rx head */
static uint8_t *g_tx, *g_rx;
static uint32_t g_rx_len, g_features = GLINK_FEATURES, g_rcid, g_next_intent = 1;
static bool g_remote_open, g_open_sent, g_open_acked, g_intents_sent, g_intent_req_sent;
static struct { uint32_t id, size; bool used; } g_ri[256];
static unsigned g_ri_count;
static uint8_t g_asm[OUR_INTENT_SIZE];
static uint32_t g_asm_len;
static uint32_t q_ver = 1, q_modem_node = QRTR_NODE_BCAST;
/* PDR names to try (full domain/service paths first) */
static const struct { const char *name; uint32_t inst; } pdr_tries[] = {
	{ "msm/modem/wlan_pd", 180 },
};

static bool q_hello_sent, q_hello_rx, q_lookup_sent, q_announced;

/* host services (pd-mapper, tftp) and the PDR client, defined further down */
static bool svc_rx(uint32_t src_node, uint32_t src_port, uint32_t dst_port, const uint8_t *data, uint32_t len);
static void svc_announce(void);
static uint32_t pdr_restart_want;
static uint32_t modem_dbg;
/* Last modem action, cheap stores only: the stall monitor dumps it. */
static volatile uint32_t g_action;

#define ACT_RX      1u
#define ACT_IOVEC   2u
#define ACT_REPLY   3u
#define ACT_PDH     4u
#define ACT_OPEN    5u
#define ACT_ALLOC   6u

static bool pdr_restarted;
static void pdr_restart_pd(const char *path);
static void rmtfs_init_buf(void);
static void wlfw_start(void);
static void wlfw_step(void);
static bool wlfw_rx(const uint8_t *d, uint32_t len);
static uint64_t hyp_assign(uint64_t addr, uint64_t size, const uint32_t *vmids, uint32_t nelems);
static void pdr_register(void);
static void reboot_to_recovery(void);
static void ns_rx(uint32_t src_node, uint32_t src_port, const uint8_t *p, uint32_t len);
static void ns_server_add(uint32_t svc, uint32_t ver, uint32_t inst, uint32_t node, uint32_t port);
static void pdr_query(void);
static void pdr_register_nolen(const char *path);
static uint32_t pdr_node, pdr_port;
static uint32_t wlfw_node, wlfw_port;
static bool wlfw_probe, wlfw_probed, wlfw_assigned;
static unsigned wlfw_state, wlfw_tries, wlfw_last;
static bool wlfw_ready;   /* FW_READY_IND seen: ath10k sends CFG+MODE only after it */
static bool pdr_sent;
static bool pdr_nolen_done;
static uint32_t pdr_instance;
static bool pdr_ok;
static unsigned pdr_last_try, pdr_retries;
static void pdr_retry(void);
static unsigned pdr_restart_next;
static void pdr_restart(const char *path);
static struct { uint32_t svc, inst, node, port; } q_srv[64];
static unsigned q_srv_count, g_rx_msgs, g_tx_msgs, q_resumes;
/* last QRTR message, so the one-line tick shows where the modem went quiet */
static uint32_t g_last_dport, g_last_len, g_last_ms, g_last_qmi, g_last_src_port;
static unsigned g_tftp_wrq, pdr_rx_logs, g_pdr_tries;
extern "C" uint64_t sweet_log_total_g;
extern "C" unsigned sweet_log_seq_g, sweet_log_fails_g;
extern "C" unsigned sweet_log_started_g;
extern "C" uint64_t sweet_log_base_g, sweet_log_nblocks_g, sweet_log_position(void);
/* the modem's SMP2P entry, read by modem_alive_check() further down */
static volatile uint32_t *g_slave_kernel;
static unsigned g_ms;                           /* watch time, for the log */

static uint32_t
rx_avail(void)
{
	uint32_t h = g_desc[3], t = g_desc[2];
	return h >= t ? h - t : g_rx_len - t + h;
}

static void
rx_peek(uint32_t off, void *dst, uint32_t n)
{
	uint32_t t = g_desc[2] + off;
	if (t >= g_rx_len) {
		t -= g_rx_len;
	}
	uint32_t n1 = MIN(n, g_rx_len - t);
	memcpy(dst, g_rx + t, n1);
	if (n > n1) {
		memcpy((uint8_t *)dst + n1, g_rx, n - n1);
	}
}

static void
rx_advance(uint32_t n)
{
	uint32_t t = g_desc[2] + n;
	if (t >= g_rx_len) {
		t -= g_rx_len;
	}
	mb();
	g_desc[2] = t;
}

static uint32_t
tx_put(uint32_t head, const void *src, uint32_t n)
{
	uint32_t n1 = MIN(n, GLINK_TX_SIZE - head);
	memcpy(g_tx + head, src, n1);
	if (n > n1) {
		memcpy(g_tx, (const uint8_t *)src + n1, n - n1);
	}
	head += n;
	return head >= GLINK_TX_SIZE ? head - GLINK_TX_SIZE : head;
}

static bool
tx_write(const void *hdr, uint32_t hlen, const void *data, uint32_t dlen)
{
	uint32_t head = 0, tail = 0, avail = 0;
	int n;

	for (n = 0; n < 1000; n++) {
		head = g_desc[1];
		tail = g_desc[0];
		avail = tail <= head ? GLINK_TX_SIZE - head + tail : tail - head;
		avail = avail < 8 ? 0 : avail - 8;
		if (avail >= ((hlen + dlen + 7) & ~7u)) {
			break;
		}
		IODelay(100);
	}
	if (n == 1000) {
		LOG("glink TX FIFO full (head %x tail %x)", head, tail);
		return false;
	}
	head = tx_put(head, hdr, hlen);
	if (dlen != 0) {
		head = tx_put(head, data, dlen);
	}
	head = (head + 7) & ~7u;
	if (head >= GLINK_TX_SIZE) {
		head -= GLINK_TX_SIZE;
	}
	mb();
	g_desc[1] = head;
	mb();
	ipcc_kick();
	g_tx_msgs++;
	return true;
}

static void
send_cmd(uint16_t cmd, uint16_t p1, uint32_t p2)
{
	struct glink_msg m = { cmd, p1, p2 };
	tx_write(&m, sizeof(m), NULL, 0);
}

static void
send_open(void)
{
	struct { struct glink_msg m; char name[8]; } __attribute__((packed)) req = {};
	req.m.cmd = CMD_OPEN;
	req.m.p1 = CHAN_LCID;
	req.m.p2 = sizeof(CHAN_NAME);
	memcpy(req.name, CHAN_NAME, sizeof(CHAN_NAME));
	tx_write(&req, sizeof(req), NULL, 0);
	g_open_sent = true;
}

static void
advertise_intents(uint32_t count, uint32_t size)
{
	struct { struct glink_msg m; uint32_t pair[2 * OUR_INTENTS]; } __attribute__((packed)) m = {};
	m.m.cmd = CMD_INTENT;
	m.m.p1 = CHAN_LCID;
	m.m.p2 = count;
	for (uint32_t i = 0; i < count && i < OUR_INTENTS; i++) {
		m.pair[2 * i] = size;
		m.pair[2 * i + 1] = g_next_intent++;
	}
	tx_write(&m, sizeof(struct glink_msg) + 8 * count, NULL, 0);
}

static bool
chan_up(void)
{
	return g_remote_open && g_open_acked;
}

static bool
glink_try_send(const void *data, uint32_t len)
{
	unsigned i;
	for (i = 0; i < g_ri_count; i++) {
		if (!g_ri[i].used && g_ri[i].size >= len) {
			break;
		}
	}
	if (i == g_ri_count) {
		if (!g_intent_req_sent) {
			send_cmd(CMD_RX_INTENT_REQ, CHAN_LCID, len);
			g_intent_req_sent = true;
		}
		return false;
	}
	struct glink_data_hdr h = { { CMD_TX_DATA, CHAN_LCID, g_ri[i].id }, len, 0 };
	bool sent = tx_write(&h, sizeof(h), data, len);

	if (sent) {
		g_ri[i].used = true;    /* only when the bytes really left */
	}
	return sent;
}

/* packets wait here until the modem has a free RX intent for them */
#define TXQ_LEN 64
static struct { void *p; uint32_t len; } g_txq[TXQ_LEN];
static unsigned g_txq_head, g_txq_tail, g_txq_drops;

static void
txq_pump(void)
{
	while (g_txq_head != g_txq_tail &&
	    glink_try_send(g_txq[g_txq_head % TXQ_LEN].p, g_txq[g_txq_head % TXQ_LEN].len)) {
		IOFree(g_txq[g_txq_head % TXQ_LEN].p, g_txq[g_txq_head % TXQ_LEN].len);
		g_txq_head++;
	}
}

static void
glink_send(const void *data, uint32_t len)
{
	void *p;
	if (g_txq_tail - g_txq_head >= TXQ_LEN || (p = IOMalloc(len)) == NULL) {
		g_txq_drops++;
		return;
	}
	memcpy(p, data, len);
	g_txq[g_txq_tail % TXQ_LEN].p = p;
	g_txq[g_txq_tail % TXQ_LEN].len = len;
	g_txq_tail++;
	txq_pump();
}

/* ---------------------------------------------------------------- QRTR */

static void
qrtr_send(uint32_t type, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *payload, uint32_t len)
{
	uint32_t cap = (uint32_t)sizeof(struct qrtr_hdr_v1) + len + 8;
	uint8_t *buf = (uint8_t *)IOMalloc(cap);
	uint32_t n;

	if (buf == NULL) {
		return;
	}
	bzero(buf, cap);
	if (q_ver == 2) {
		struct qrtr_hdr_v2 *h = (struct qrtr_hdr_v2 *)buf;
		h->version = 2;
		h->type = (uint8_t)type;
		h->flags = type == QRTR_TYPE_DATA ? 1 : 0;
		h->size = len;
		h->src_node = QRTR_NODE_APPS;
		h->src_port = (uint16_t)src_port;
		h->dst_node = (uint16_t)dst_node;
		h->dst_port = (uint16_t)dst_port;
		n = sizeof(*h);
	} else {
		struct qrtr_hdr_v1 *h = (struct qrtr_hdr_v1 *)buf;
		h->version = 1;
		h->type = type;
		h->src_node = QRTR_NODE_APPS;
		h->src_port = src_port;
		h->confirm_rx = type == QRTR_TYPE_DATA ? 1 : 0;   /* flow control: ask for RESUME_TX */
		h->size = len;
		h->dst_node = dst_node;
		h->dst_port = dst_port;
		n = sizeof(*h);
	}
	memcpy(buf + n, payload, len);
	glink_send(buf, (n + len + 3) & ~3u);
	IOFree(buf, cap);
}

static const char *
svc_name(uint32_t svc)
{
	switch (svc) {
	case 0x45: return "WLFW";
	case 0x0E: return "rmtfs";
	case 0x40: return "servreg-loc";
	case 0x42: return "servreg-notif";
	case 0x2B: return "ssctl";
	case 0x31: return "ipa";
	case 0x01: return "wds";
	case 0x02: return "dms";
	case 0x03: return "nas";
	case 0x0B: return "uim";
	case 0x1A: return "wda";
	default:   return "";
	}
}

static void
qrtr_rx(const uint8_t *p, uint32_t len)
{
	uint32_t type, src_node, src_port, dst_port, size, hlen, confirm;

	if (len >= sizeof(struct qrtr_hdr_v1) && *(const uint32_t *)p == 1) {
		const struct qrtr_hdr_v1 *h = (const struct qrtr_hdr_v1 *)p;
		type = h->type; src_node = h->src_node; src_port = h->src_port;
		dst_port = h->dst_port; size = h->size; hlen = sizeof(*h); confirm = h->confirm_rx;
		q_ver = 1;
	} else if (len >= sizeof(struct qrtr_hdr_v2) && p[0] == 2) {
		const struct qrtr_hdr_v2 *h = (const struct qrtr_hdr_v2 *)p;
		type = h->type; src_node = h->src_node; src_port = h->src_port;
		dst_port = h->dst_port; size = h->size; hlen = sizeof(*h) + h->optlen * 4;
		src_port = src_port == 0xFFFE ? QRTR_PORT_CTRL : src_port;
		dst_port = dst_port == 0xFFFE ? QRTR_PORT_CTRL : dst_port;
		confirm = h->flags & 1;
		q_ver = 2;
	} else {
		LOG("t=%u ms: qrtr: bad packet, %u bytes", g_ms, len);
		return;
	}
	{
		static unsigned int q_dbg;

		g_action = ACT_RX | ((type & 0xff) << 8) | (dst_port & 0xff);
		if (modem_dbg && q_dbg < 200) {
			q_dbg++;
			LOG("t=%u ms: rx#%u type %u port %x->%x size %u len %u", g_ms, q_dbg,
			    type, src_port, dst_port, size, len);
		}
	}
	const struct qrtr_ctrl *c = (const struct qrtr_ctrl *)(p + hlen);

	if (dst_port == QRTR_PORT_CTRL && (type == QRTR_TYPE_NEW_LOOKUP || type == QRTR_TYPE_DEL_LOOKUP)) {
		ns_rx(src_node, src_port, p, len);
		return;
	}

	switch (type) {
	case QRTR_TYPE_HELLO:
		q_modem_node = src_node;
		q_hello_rx = true;
		q_hello_sent = false;                   /* (re)answer with our HELLO */
		LOG("t=%u ms: qrtr v%u HELLO from node %u", g_ms, q_ver, src_node);
		break;
	case QRTR_TYPE_NEW_SERVER:
		LOG("t=%u ms: qrtr NEW_SERVER svc %x v%u i%u node %u port %x", g_ms, c->a,
		    c->b & 0xff, (c->b >> 8) & 0xff, c->c, c->d);
		if (q_srv_count < sizeof(q_srv) / sizeof(q_srv[0])) {
			q_srv[q_srv_count].svc = c->a;
			q_srv[q_srv_count].inst = c->b;
			q_srv[q_srv_count].node = c->c;
			q_srv[q_srv_count].port = c->d;
			q_srv_count++;
		}
		ns_server_add(c->a, c->b & 0xff, (c->b >> 8) & 0xff, c->c, c->d);
		if (c->a == 0x42 && c->d != 0) {
			pdr_node = c->c;
			pdr_port = c->d;
			pdr_instance = (c->b >> 8) & 0xff;   /* version in the low byte */
		}
		if (c->a == QMI_SVC_WLFW) {
			LOG("t=%u ms: *** WLFW service 0x45 up: inst %x node %u port %x ***", g_ms, c->b, c->c, c->d);
			wlfw_node = c->c;
			wlfw_port = c->d;
			wlfw_probe = true;
		}
		break;
	case QRTR_TYPE_DATA:
		if (confirm) {
			/* the full 20-byte control packet (Linux qrtr_send_resume_tx): 12 bytes stalls the modem */
			struct qrtr_ctrl r = { QRTR_TYPE_RESUME_TX, QRTR_NODE_APPS, dst_port, 0, 0 };
			qrtr_send(QRTR_TYPE_RESUME_TX, dst_port, src_node, src_port, &r, sizeof(r));
		}
		if (hlen + size <= len) {
			(void)svc_rx(src_node, src_port, dst_port, p + hlen, size);
		}
		break;
	case QRTR_TYPE_RESUME_TX:
		q_resumes++;
		break;
	case QRTR_TYPE_DEL_CLIENT:
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- GLINK RX */

static bool
rx_one(void)
{
	struct glink_msg m;
	uint32_t avail = rx_avail(), n;

	if (avail < sizeof(m)) {
		return false;
	}
	rx_peek(0, &m, sizeof(m));
	g_rx_msgs++;
	switch (m.cmd) {
	case CMD_VERSION:
		rx_advance(8);
		LOG("t=%u ms: glink VERSION %u features %x", g_ms, m.p1, m.p2);
		if (m.p1 != 0) {
			g_features &= m.p2;
			send_cmd(CMD_VERSION_ACK, 1, g_features);
		}
		break;
	case CMD_VERSION_ACK:
		rx_advance(8);
		LOG("t=%u ms: glink VERSION_ACK %u features %x", g_ms, m.p1, m.p2);
		if (m.p1 != 0 && m.p2 != g_features) {
			g_features &= m.p2;
			send_cmd(CMD_VERSION, 1, g_features);
		}
		break;
	case CMD_OPEN: {
		char name[33] = {};
		n = (8 + m.p2 + 7) & ~7u;
		if (avail < n) {
			return false;
		}
		rx_peek(8, name, MIN(m.p2, 32u));
		rx_advance(n);
		LOG("t=%u ms: glink modem OPEN \"%s\" rcid %u", g_ms, name, m.p1);
		/* Ack every channel the modem opens, not just IPCRTR: the modem
		 * waits for the peer before it announces more services. */
		send_cmd(CMD_OPEN_ACK, m.p1, 0);
		if (strcmp(name, CHAN_NAME) == 0) {
			g_rcid = m.p1;
			g_remote_open = true;
			if (!g_open_sent) {
				send_open();
			}
		}
		break;
	}
	case CMD_OPEN_ACK:
		rx_advance(8);
		LOG("t=%u ms: glink OPEN_ACK lcid %u", g_ms, m.p1);
		if (m.p1 == CHAN_LCID) {
			g_open_acked = true;
		}
		break;
	case CMD_CLOSE:
		rx_advance(8);
		LOG("t=%u ms: glink modem CLOSE rcid %u", g_ms, m.p1);
		send_cmd(CMD_CLOSE_ACK, m.p1, 0);
		if (m.p1 == g_rcid) {
			g_remote_open = false;
		}
		break;
	case CMD_INTENT:
		n = (8 + 8 * m.p2 + 7) & ~7u;
		if (avail < n) {
			return false;
		}
		for (uint32_t i = 0; i < m.p2; i++) {
			uint32_t pair[2];
			rx_peek(8 + 8 * i, pair, 8);
			if (m.p1 == g_rcid && g_ri_count < sizeof(g_ri) / sizeof(g_ri[0])) {
				g_ri[g_ri_count].size = pair[0];
				g_ri[g_ri_count].id = pair[1];
				g_ri[g_ri_count].used = false;
				g_ri_count++;
			}
		}
		rx_advance(n);
		g_intent_req_sent = false;
		break;
	case CMD_RX_DONE:
	case CMD_RX_DONE_W_REUSE:
		rx_advance(8);
		for (unsigned i = 0; i < g_ri_count; i++) {
			if (g_ri[i].id == m.p2) {
				if (m.cmd == CMD_RX_DONE_W_REUSE) {
					g_ri[i].used = false;
				} else {
					g_ri[i] = g_ri[--g_ri_count];
				}
				break;
			}
		}
		break;
	case CMD_RX_INTENT_REQ:
		rx_advance(8);
		advertise_intents(1, MAX(m.p2, OUR_INTENT_SIZE));
		send_cmd(CMD_RX_INTENT_REQ_ACK, CHAN_LCID, 1);
		break;
	case CMD_RX_INTENT_REQ_ACK:
		rx_advance(8);
		if (m.p2 == 0) {
			g_intent_req_sent = false;      /* ask again on the next pump */
		}
		break;
	case CMD_CLOSE_ACK:
	case CMD_SIGNALS:
		rx_advance(8);
		break;
	case CMD_READ_NOTIF:
		rx_advance(8);
		ipcc_kick();
		break;
	case CMD_TX_DATA:
	case CMD_TX_DATA_CONT: {
		uint32_t hdr[2];
		if (avail < 16) {
			return false;
		}
		rx_peek(8, hdr, 8);                     /* chunk_size, left_size */
		n = (16 + hdr[0] + 7) & ~7u;
		if (avail < n) {
			return false;
		}
		if (m.cmd == CMD_TX_DATA) {
			g_asm_len = 0;
		}
		if (g_asm_len + hdr[0] <= sizeof(g_asm)) {
			rx_peek(16, g_asm + g_asm_len, hdr[0]);
			g_asm_len += hdr[0];
		}
		rx_advance(n);
		if (hdr[1] == 0) {
			if (m.p1 == g_rcid) {
				qrtr_rx(g_asm, g_asm_len);
			}
			if (g_features & 1) {
				send_cmd(CMD_RX_DONE_W_REUSE, CHAN_LCID, m.p2);
			} else {
				send_cmd(CMD_RX_DONE, CHAN_LCID, m.p2);
				advertise_intents(1, OUR_INTENT_SIZE);
			}
		}
		break;
	}
	default:
		LOG("t=%u ms: glink unknown cmd %u (%x %x), RX stopped", g_ms, m.cmd, m.p1, m.p2);
		g_rx = NULL;
		return false;
	}
	return true;
}

static bool
glink_start(struct smem_part_hdr *part)
{
	bool existed;

	LOG("glink: start, part %p", part);
	existed = smem_get(part, SMEM_GLINK_DESC, NULL) != NULL;
	LOG("glink: desc %s", existed ? "present" : "missing");
	g_desc = (volatile uint32_t *)smem_alloc(part, SMEM_GLINK_DESC, 32);
	LOG("glink: desc alloc %p", g_desc);
	g_tx = (uint8_t *)smem_alloc(part, SMEM_GLINK_FIFO_TX, GLINK_TX_SIZE);
	LOG("glink: tx fifo alloc %p", g_tx);
	if (g_desc == NULL || g_tx == NULL) {
		LOG("glink: SMEM alloc failed (desc %p tx %p)", g_desc, g_tx);
		return false;
	}
	LOG("glink: desc %s, our TX FIFO %u bytes", existed ? "made by the modem" : "ours", GLINK_TX_SIZE);
	g_desc[2] = 0;                              /* rx tail */
	g_desc[1] = 0;                              /* tx head */
	mb();
	send_cmd(CMD_VERSION, 1, g_features);
	return true;
}

/* one pass, every millisecond from the watch loop */
static void
glink_poll(struct smem_part_hdr *part, unsigned ms)
{
	g_ms = ms;
	if (g_rx == NULL && g_rx_msgs == 0) {
		uint32_t sz = 0;
		g_rx = (uint8_t *)smem_get(part, SMEM_GLINK_FIFO_RX, &sz);
		if (g_rx != NULL) {
			g_rx_len = sz;
			LOG("t=%u ms: modem RX FIFO, %u bytes", ms, sz);
		}
	}
	for (int i = 0; i < 64 && g_rx != NULL && rx_one(); i++) {
	}
	txq_pump();
	if (chan_up() && !g_intents_sent) {
		advertise_intents(OUR_INTENTS, OUR_INTENT_SIZE);
		g_intents_sent = true;
		LOG("t=%u ms: IPCRTR open both ways, %u intents sent", ms, OUR_INTENTS);
	}
	/* the vendor icnss registers a service lookup rather than trusting the
	 * broadcast: ask the modem's name server for WLFW (service 0x45) too */
	if (chan_up() && q_hello_rx && !q_lookup_sent) {
		/* lookups carry the same version/instance pair as the records */
		/* the record packs version in the low byte, instance in the next */
		struct qrtr_ctrl l = { QRTR_TYPE_NEW_LOOKUP, QMI_SVC_WLFW, 0x01u, 0xFFFFFFFFu, 0 };
		struct qrtr_ctrl l2 = { QRTR_TYPE_NEW_LOOKUP, 0x42, 0x01u | (180u << 8), 0xFFFFFFFFu, 0 };
		struct qrtr_ctrl l3 = { QRTR_TYPE_NEW_LOOKUP, 0x42, 0x01u | (74u << 8), 0xFFFFFFFFu, 0 };
		qrtr_send(QRTR_TYPE_NEW_LOOKUP, QRTR_PORT_CTRL, q_modem_node, QRTR_PORT_CTRL, &l, sizeof(l));
		qrtr_send(QRTR_TYPE_NEW_LOOKUP, QRTR_PORT_CTRL, q_modem_node, QRTR_PORT_CTRL, &l2, sizeof(l2));
		qrtr_send(QRTR_TYPE_NEW_LOOKUP, QRTR_PORT_CTRL, q_modem_node, QRTR_PORT_CTRL, &l3, sizeof(l3));
		q_lookup_sent = true;
		LOG("t=%u ms: sent qrtr NEW_LOOKUP 0x45 and 0x42 (instances 180/74)", ms);
	}
	if (chan_up() && q_hello_rx && !q_announced) {
		svc_announce();
		q_announced = true;
	}
	if (chan_up() && q_hello_rx && pdr_port != 0 && pdr_restart_want &&
	    !pdr_restarted && ms >= 200) {
		pdr_restarted = true;
		pdr_restart_pd("msm/modem/wlan_pd");
	}
	if (chan_up() && q_hello_rx && pdr_port != 0) {
		if (!pdr_sent) {
			pdr_register();
		} else if (!pdr_ok && (ms - pdr_last_try) >= 500) {
			/* Android's PDR client retries: the modem answers INVALID_HANDLE
			 * while its service registry is still coming up. */
			pdr_last_try = ms;
			pdr_retry();
		}
	}
	if (wlfw_probe && !wlfw_probed) {
		wlfw_probed = true;
		wlfw_start();
	} else if (wlfw_state >= 1 && wlfw_state <= 4 && wlfw_tries < 3 &&
	    (ms - wlfw_last) >= 1000) {
		wlfw_tries++;
		wlfw_last = ms;
		wlfw_step();
	}
	if (chan_up() && !q_hello_sent) {
		struct qrtr_ctrl c = { QRTR_TYPE_HELLO, 0, 0, 0, 0 };
		qrtr_send(QRTR_TYPE_HELLO, QRTR_PORT_CTRL, q_modem_node, QRTR_PORT_CTRL, &c, sizeof(c));
		q_hello_sent = true;
		LOG("t=%u ms: sent qrtr HELLO to node %x", ms, q_modem_node);
	}
	/* nobody opened IPCRTR after 2 s: open it from our side */
	if (ms >= 2000 && !g_open_sent && g_rx != NULL) {
		LOG("t=%u ms: modem did not open IPCRTR, opening it", ms);
		send_open();
	}
}

static void
glink_summary(void)
{
	LOG("glink: rx %u msgs, tx %u, chan %s, modem intents %u, txq %u drops %u; qrtr node %x hello %u, %u services, %u RESUME_TX",
	    g_rx_msgs, g_tx_msgs, chan_up() ? "UP" : "down", g_ri_count, g_txq_tail - g_txq_head, g_txq_drops,
	    q_modem_node, q_hello_rx, q_srv_count, q_resumes);
	for (unsigned i = 0; i < q_srv_count; i++) {
		LOG("  service %x:%x node %u port %x %s", q_srv[i].svc, q_srv[i].inst, q_srv[i].node, q_srv[i].port, svc_name(q_srv[i].svc));
	}
}
/* ---------------------------------------------------------------- QRTR name server */

/*
 * Android runs /vendor/bin/qrtr-ns on the AP: every node's name service answers
 * NEW_LOOKUP with NEW_SERVER records and relays them. We are the AP's name
 * server here, so answer the modem's lookups from our own server table instead
 * of dropping them.
 */
struct qrtr_srv_entry { uint32_t svc, ver, inst, node, port; bool used; };
static struct qrtr_srv_entry ns_srv[16];

static void
ns_server_add(uint32_t svc, uint32_t ver, uint32_t inst, uint32_t node, uint32_t port)
{
	for (unsigned i = 0; i < 16; i++) {
		if (ns_srv[i].used && ns_srv[i].svc == svc && ns_srv[i].inst == inst &&
		    ns_srv[i].node == node) {
			ns_srv[i].port = port;
			ns_srv[i].ver = ver;
			return;
		}
	}
	for (unsigned i = 0; i < 16; i++) {
		if (!ns_srv[i].used) {
			ns_srv[i].svc = svc;
			ns_srv[i].ver = ver;
			ns_srv[i].inst = inst;
			ns_srv[i].node = node;
			ns_srv[i].port = port;
			ns_srv[i].used = true;
			return;
		}
	}
}

static void
ns_server_del(uint32_t svc, uint32_t inst, uint32_t node)
{
	for (unsigned i = 0; i < 16; i++) {
		if (ns_srv[i].used && ns_srv[i].svc == svc && ns_srv[i].inst == inst &&
		    ns_srv[i].node == node) {
			ns_srv[i].used = false;
		}
	}
}

/* Answer one lookup (service, version, instance; instance 0 = any) at to_node/to_port. */
static void
ns_answer(uint32_t svc, uint32_t ver, uint32_t inst, uint32_t to_node, uint32_t to_port)
{
	for (unsigned i = 0; i < 16; i++) {
		if (!ns_srv[i].used || ns_srv[i].svc != svc) {
			continue;
		}
		if (inst != 0 && ns_srv[i].inst != inst) {
			continue;
		}
		if (ver != 0 && ns_srv[i].ver != ver) {
			continue;
		}
		struct qrtr_ctrl c = { QRTR_TYPE_NEW_SERVER, ns_srv[i].svc,
		    (ns_srv[i].ver & 0xffu) | ((ns_srv[i].inst & 0xffffu) << 8),
		    ns_srv[i].node, ns_srv[i].port };
		qrtr_send(QRTR_TYPE_NEW_SERVER, QRTR_PORT_CTRL, to_node, to_port, &c, sizeof(c));
		LOG("t=%u ms: ns: answered lookup svc %x i%u with node %u port %x", g_ms, svc,
		    ns_srv[i].inst, ns_srv[i].node, ns_srv[i].port);
	}
}

/* A control packet addressed to the name service (port 0xfffffffe). */
static void
ns_rx(uint32_t src_node, uint32_t src_port, const uint8_t *p, uint32_t len)
{
	if (len < 20) {
		return;
	}
	const struct qrtr_ctrl *c = (const struct qrtr_ctrl *)p;
	uint32_t ver = c->b & 0xff, inst = (c->b >> 8) & 0xff;

	switch (c->cmd) {
	case QRTR_TYPE_NEW_LOOKUP:
		LOG("t=%u ms: ns: NEW_LOOKUP svc %x v%u i%u from node %u port %x", g_ms, c->a, ver,
		    inst, src_node, src_port);
		ns_answer(c->a, ver, inst, src_node, src_port);
		break;
	case QRTR_TYPE_NEW_SERVER:
		ns_server_add(c->a, ver, inst, c->c, c->d);
		break;
	case QRTR_TYPE_DEL_SERVER:
		ns_server_del(c->a, inst, c->c);
		break;
	default:
		LOG("t=%u ms: ns: control type %u from node %u", g_ms, c->cmd, src_node);
		break;
	}
}

/* ---------------------------------------------------------------- host services */

/*
 * What the modem expects from the AP before it can start its WLAN core
 * (the Linux userspace daemons pd-mapper and tqftpserv, as ios-topaz
 * implemented them for its own XNU port):
 *  - pd-mapper, QMI servreg-locator 0x40 v0x101: which PD owns which service;
 *  - tftp, QRTR service 4096 (port 69 semantics): the modem asks for
 *    /readonly/firmware/image/<name>, which we serve from image/<name> on
 *    the modem partition (read-only, cached in RAM).
 */
#define PORT_PDM            0x4001
#define PORT_TFTP           0x4002
#define PORT_SESS           0x4100
#define PORT_RMTFS          0x4003
#define PORT_WLFW           0x4004
#define QMI_REQ             0
#define QMI_RESP            2

struct qmsg { uint8_t *b; uint32_t n; };
static uint16_t svc_txn = 1;

static void
qm_init(struct qmsg *m, uint16_t txn, uint16_t msg)
{
	m->b = (uint8_t *)IOMalloc(8192);
	m->n = 7;
	if (m->b != NULL) {
		bzero(m->b, 8192);
		m->b[0] = QMI_RESP;
		memcpy(m->b + 1, &txn, 2);
		memcpy(m->b + 3, &msg, 2);
		/* m->b[5..6]: payload length, filled in by qm_send */
	}
}

static void
qm_tlv(struct qmsg *m, uint8_t type, const void *v, uint16_t len)
{
	if (m->b == NULL || m->n + 3 + len > 8192) {
		return;
	}
	m->b[m->n] = type;
	memcpy(m->b + m->n + 1, &len, 2);
	if (len != 0) {
		memcpy(m->b + m->n + 3, v, len);
	}
	m->n += 3 + len;
}

static bool
qm_tlv_find(const uint8_t *d, uint32_t len, uint8_t type, const uint8_t **val, uint32_t *vlen)
{
	uint32_t i = 7;

	while (i + 3 <= len) {
		uint8_t t = d[i];
		uint16_t l;
		memcpy(&l, d + i + 1, 2);
		if (i + 3 + l > len) {
			return false;
		}
		if (t == type) {
			*val = d + i + 3;
			*vlen = l;
			return true;
		}
		i += 3 + l;
	}
	return false;
}

/* Both servreg-locator and rmtfs declare `struct qmi_result { u16 result;
 * u16 error; }` as ONE TLV 0x02 (four bytes), not the generic split
 * result/error TLVs. */
static void
qm_result(struct qmsg *m, bool ok)
{
	uint16_t r[2];

	r[0] = ok ? 0 : 1;
	r[1] = 0;
	qm_tlv(m, 0x02, r, 4);
}

/* TLVs start after the 7-byte QMI header; this stack sends the service
 * name as a plain NUL-padded string, not a length-prefixed one. */
static bool
qm_string(const uint8_t *d, uint32_t len, uint8_t type, char *out, size_t max)
{
	uint32_t i = 7;

	out[0] = 0;
	while (i + 3 <= len) {
		uint8_t t = d[i];
		uint16_t l;
		memcpy(&l, d + i + 1, 2);
		if (i + 3 + l > len) {
			return false;
		}
		if (t == type && l >= 1) {
			uint32_t n = 0;
			while (n < l && n + 1 < max && d[i + 3 + n] != 0) {
				out[n] = (char)d[i + 3 + n];
				n++;
			}
			out[n] = 0;
			return true;
		}
		i += 3 + l;
	}
	return false;
}

static void
qm_send(struct qmsg *m, uint32_t src_port, uint32_t node, uint32_t port)
{
	if (m->b != NULL) {
		uint16_t len = (uint16_t)(m->n - 7);
		memcpy(m->b + 5, &len, 2);
		qrtr_send(QRTR_TYPE_DATA, src_port, node, port, m->b, m->n);
		IOFree(m->b, 8192);
		m->b = NULL;
	}
}

static unsigned pd_full;
static uint32_t rmtfs_on = 1;

/*
 * pd-mapper database, taken verbatim from the six *.jsn files in the modem
 * partition's image/ directory (modemr, modemuw, adspr, adsps, adspua, cdspr).
 * Each file is one protection domain with its qmi_instance_id and the
 * services it provides; tms/pdr_enabled answers with every domain.
 */
static const struct {
	const char *domain;
	uint32_t inst;
	const char *svc[4];
} pd_db[] = {
	{ "msm/modem/root_pd",  180, { "tms/servreg", "tms/pdr_enabled", "gps/gps_service", NULL } },
	{ "msm/modem/wlan_pd",  180, { "kernel/elf_loader", "tms/servreg", "wlan/fw", NULL } },
	{ "msm/adsp/root_pd",    74, { "tms/servreg", NULL } },
	{ "msm/adsp/sensor_pd",  74, { "tms/servreg", NULL } },
	{ "msm/adsp/audio_pd",   74, { "tms/servreg", "avs/audio", NULL } },
	{ "msm/cdsp/root_pd",    76, { "tms/servreg", NULL } },
};

static bool
pd_provides(unsigned i, const char *svc)
{
	for (unsigned k = 0; k < 4 && pd_db[i].svc[k] != NULL; k++) {
		if (strcmp(pd_db[i].svc[k], svc) == 0) {
			return true;
		}
	}
	return false;
}

static unsigned pd_reqs;

static void
pdm_rx(uint32_t node, uint32_t port, uint16_t txn, uint16_t msg, const uint8_t *d, uint32_t len)
{
	struct qmsg m;
	char name[80];
	uint8_t list[384];
	uint32_t n = 1, cnt = 0;
	uint16_t v;

	qm_init(&m, txn, msg);
	if (m.b == NULL) {
		return;
	}
	if (msg != 0x21) {
		qm_result(&m, true);
		qm_send(&m, PORT_PDM, node, port);
		return;
	}
	if (!qm_string(d, len, 0x01, name, sizeof(name))) {
		name[0] = 0;
	}
	{
		char hex[3 * 40 + 1], asc[41];
		uint32_t n = MIN(len, 40u), k = 0;
		for (uint32_t i = 0; i < n; i++) {
			static const char hx[] = "0123456789abcdef";
			hex[k++] = hx[d[i] >> 4];
			hex[k++] = hx[d[i] & 15];
			hex[k++] = ' ';
			asc[i] = (d[i] >= 32 && d[i] < 127) ? (char)d[i] : '.';
		}
		hex[k] = 0;
		asc[n] = 0;
		LOG("t=%u ms: pd-mapper msg %x len %u: %s |%s|", g_ms, msg, len, hex, asc);
	}
	{
		bool all = strcmp(name, "tms/pdr_enabled") == 0;

		for (unsigned i = 0; i < sizeof(pd_db) / sizeof(pd_db[0]); i++) {
			uint32_t dl, inst = pd_db[i].inst, zero = 0;

			if (!all && !pd_provides(i, name)) {
				continue;
			}
			dl = (uint32_t)strlen(pd_db[i].domain);
			if (n + 1 + dl + 9 > sizeof(list)) {
				break;
			}
			list[n++] = (uint8_t)dl;
			memcpy(list + n, pd_db[i].domain, dl);
			n += dl;
			memcpy(list + n, &inst, 4);
			n += 4;
			list[n++] = 0;
			memcpy(list + n, &zero, 4);
			n += 4;
			cnt++;
		}
	}
	list[0] = (uint8_t)cnt;
	qm_result(&m, true);
	v = (uint16_t)cnt;
	qm_tlv(&m, 0x10, &v, 2);
	v = 1;
	qm_tlv(&m, 0x11, &v, 2);
	if (cnt != 0) {
		qm_tlv(&m, 0x12, list, (uint16_t)n);
	}
	qm_send(&m, PORT_PDM, node, port);
	pd_reqs++;
	LOG("t=%u ms: pd-mapper: \"%s\" -> %u domain(s)", g_ms, name, cnt);
}

/* ---------------------------------------------------------------- tftp */

#define OP_RRQ   1
#define OP_WRQ   2
#define OP_DATA  3
#define OP_ACK   4
#define OP_ERROR 5
#define OP_OACK  6

/* files served from the modem partition, cached in kernel memory */
struct fw_file { char name[64]; uint8_t *data; uint32_t size; };
static struct fw_file fw_files[4];
static struct fw_file ram_view;

static struct fw_file *
fw_get(const char *name)
{
	for (unsigned i = 0; i < 4; i++) {
		if (fw_files[i].name[0] != 0 && strcmp(fw_files[i].name, name) == 0) {
			return &fw_files[i];
		}
	}
	if (g_fat == NULL) {
		return NULL;
	}
	if (fw_dir == 1) {
		uint32_t dir_size = 0, dir_clus = 0;
		if (fat_lookup_in(g_fat, 0, "image", &dir_clus, &dir_size) != 0) {
			return NULL;
		}
		fw_dir = dir_clus;
	}
	uint32_t clus = 0, size = 0;
	if (fat_lookup_in(g_fat, fw_dir, name, &clus, &size) != 0 || size == 0 || size > 0x2000000) {
		return NULL;
	}
	LOG("fw cache: %s %u bytes", name, size);
	uint8_t *data = (uint8_t *)IOMalloc(size);
	if (data == NULL) {
		return NULL;
	}
	if (fat_read_chain(g_fat, clus, data, size) != 0) {
		IOFree(data, size);
		return NULL;
	}
	int slot = -1;
	for (unsigned i = 0; i < 4; i++) {
		if (fw_files[i].name[0] == 0) {
			slot = (int)i;
			break;
		}
	}
	if (slot < 0) {
		slot = 0;                               /* reuse the oldest slot */
		if (fw_files[0].data != NULL) {
			IOFree(fw_files[0].data, fw_files[0].size);
		}
	}
	strlcpy(fw_files[slot].name, name, sizeof(fw_files[slot].name));
	fw_files[slot].data = data;
	fw_files[slot].size = size;
	LOG("t=%u ms: tftp: cached %s (%u bytes)", g_ms, name, size);
	return &fw_files[slot];
}

struct tftp_sess {
	bool used, write;
	uint32_t port, node, rport;
	struct fw_file *f;
	struct ram_file *wf;
	uint64_t size, sent, seek;
	uint32_t blk, rsize, wsize;
	uint32_t last_ms;
};
/* Writes: rmtfs' tftp serves /readwrite/ out of a RAM fs; wlan_pd writes
 * there (server_check.txt) and retries every 5 ms when it is refused. */
struct ram_file { char name[64]; uint8_t *data; uint32_t size, cap; };
static struct ram_file ram[4];

static struct ram_file *
ram_file_get(const char *name, bool create)
{
	for (unsigned i = 0; i < 4; i++) {
		if (ram[i].name[0] != 0 && strcmp(ram[i].name, name) == 0) {
			return &ram[i];
		}
	}
	if (!create) {
		return NULL;
	}
	for (unsigned i = 0; i < 4; i++) {
		if (ram[i].name[0] == 0) {
			strlcpy(ram[i].name, name, sizeof(ram[i].name));
			ram[i].cap = 1u << 20;
			ram[i].data = (uint8_t *)IOMalloc(ram[i].cap);
			ram[i].size = 0;
			if (ram[i].data == NULL) {
				ram[i].name[0] = 0;
				return NULL;
			}
			return &ram[i];
		}
	}
	return NULL;
}

static struct tftp_sess sess[8];
static uint32_t next_sess_port = PORT_SESS;
static unsigned tftp_reqs, tftp_missing;

static void
tftp_send(struct tftp_sess *s, const void *p, uint32_t len)
{
	qrtr_send(QRTR_TYPE_DATA, s->port, s->node, s->rport, p, len);
}

static void
tftp_error(struct tftp_sess *s, uint16_t code, const char *msg)
{
	uint8_t b[64] = { 0, OP_ERROR, (uint8_t)(code >> 8), (uint8_t)code };
	uint32_t l = (uint32_t)strlcpy((char *)b + 4, msg, sizeof(b) - 4) + 1;
	tftp_send(s, b, 4 + l);
}

static void
tftp_close(struct tftp_sess *s)
{
	bzero(s, sizeof(*s));
}

static bool
tftp_data(struct tftp_sess *s, uint32_t block, uint64_t offset, uint32_t resp_size)
{
	uint8_t *b = (uint8_t *)IOMalloc(s->blk + 4);
	uint32_t n = s->blk, send;

	if (b == NULL) {
		return false;
	}
	b[0] = 0;
	b[1] = OP_DATA;
	b[2] = (uint8_t)(block >> 8);
	b[3] = (uint8_t)block;
	if (offset >= s->size || s->f == NULL) {
		n = 0;
	} else {
		n = (uint32_t)MIN((uint64_t)n, s->size - offset);
		memcpy(b + 4, s->f->data + offset, n);
	}
	send = 4 + n;
	if (resp_size != 0) {
		if (4 + resp_size > send) {
			IOFree(b, s->blk + 4);
			return false;
		}
		send = 4 + resp_size;
	}
	tftp_send(s, b, send);
	s->sent += send - 4;
	IOFree(b, s->blk + 4);
	return true;
}

static char *
tftp_opt(char *p, char *end, const char *name, uint64_t val)
{
	char num[24];
	snprintf(num, sizeof(num), "%llu", val);
	size_t a = strlen(name) + 1, b = strlen(num) + 1;
	if (p + a + b > end) {
		return p;
	}
	memcpy(p, name, a);
	memcpy(p + a, num, b);
	return p + a + b;
}

/* /readonly/firmware/image/x -> image/x (lower case) */
static bool
tftp_map(const char *path, char *out, size_t max)
{
	static const char *pre[] = {
		"/readonly/firmware/image/", "/readonly/vendor/firmware_mnt/image/", "/readonly/vendor/firmware/",
	};
	const char *rest = NULL;
	size_t k;

	for (unsigned i = 0; i < 3; i++) {
		if (strncmp(path, pre[i], strlen(pre[i])) == 0) {
			rest = path + strlen(pre[i]);
			strlcpy(out, "image/", max);
			break;
		}
	}
	if (rest == NULL) {
		return false;
	}
	k = strlen(out);
	for (; *rest != 0 && k + 1 < max; rest++) {
		char c = *rest;
		out[k++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
	}
	out[k] = 0;
	return true;
}

static uint32_t
tftp_dec(const char *p)
{
	uint32_t v = 0;
	while (*p >= '0' && *p <= '9') {
		v = v * 10 + (uint32_t)(*p++ - '0');
	}
	return v;
}

static void
tftp_request(uint32_t node, uint32_t port, const uint8_t *d, uint32_t len)
{
	g_tftp_wrq++;
	char buf[512], *p, *end, *file, *mode, oack[256], *o, path[128], rel[96];
	uint16_t op = (uint16_t)(d[0] << 8 | d[1]);
	struct tftp_sess *s = NULL;
	bool opts, want_tsize = false;
	uint32_t timeoutms = 1000;

	if (len < 4 || len >= sizeof(buf)) {
		return;
	}
	memcpy(buf, d, len);
	buf[len] = 0;
	end = buf + len;
	file = buf + 2;
	mode = file + strlen(file) + 1;
	if (mode >= end) {
		return;
	}
	p = mode + strlen(mode) + 1;
	opts = p < end;
	for (unsigned i = 0; i < 8; i++) {
		if (!sess[i].used) {
			s = &sess[i];
			break;
		}
	}
	if (s == NULL) {
		for (unsigned i = 0; i < 8; i++) {
			if (g_ms - sess[i].last_ms > 2000) {
				LOG("t=%u ms: tftp: evicting stale session %u (port %x)", g_ms, i, sess[i].port);
				tftp_close(&sess[i]);
				s = &sess[i];
				break;
			}
		}
	}
	if (s == NULL) {
		LOG("tftp: out of sessions");
		return;
	}
	bzero(s, sizeof(*s));
	s->used = true;
	s->last_ms = g_ms;
	s->write = op == OP_WRQ;
	s->port = PORT_SESS + (uint32_t)(s - sess);
	s->node = node;
	s->rport = port;
	s->blk = 512;
	s->wsize = 1;
	while (p < end) {
		char *name = p, *val = p + strlen(p) + 1;
		if (val >= end) {
			break;
		}
		uint32_t v = tftp_dec(val);
		if (strcmp(name, "blksize") == 0) {
			s->blk = MAX(8u, MIN(v, 16384u));
		} else if (strcmp(name, "timeoutms") == 0) {
			timeoutms = v;
		} else if (strcmp(name, "tsize") == 0) {
			want_tsize = true;
		} else if (strcmp(name, "rsize") == 0) {
			s->rsize = v;
		} else if (strcmp(name, "wsize") == 0) {
			s->wsize = MAX(1u, v);
		} else if (strcmp(name, "seek") == 0) {
			s->seek = v;
		}
		p = val + strlen(val) + 1;
	}
	tftp_reqs++;
	if (s->write) {
		s->wf = ram_file_get(file, true);
		if (s->wf == NULL) {
			LOG("t=%u ms: tftp WRQ %s: no RAM file", g_ms, file);
			tftp_error(s, 2, "no space");
			tftp_close(s);
			return;
		}
		s->wf->size = 0;
		LOG("t=%u ms: tftp WRQ %s accepted, blk %u", g_ms, file, s->blk);
		if (!opts) {
			uint8_t a[4] = { 0, OP_ACK, 0, 0 };
			tftp_send(s, a, 4);
			return;
		}
		o = oack;
		*o++ = 0;
		*o++ = OP_OACK;
		o = tftp_opt(o, oack + sizeof(oack), "blksize", s->blk);
		o = tftp_opt(o, oack + sizeof(oack), "timeoutms", timeoutms);
		if (want_tsize) {
			o = tftp_opt(o, oack + sizeof(oack), "tsize", 0);
		}
		o = tftp_opt(o, oack + sizeof(oack), "wsize", s->wsize);
		tftp_send(s, oack, (uint32_t)(o - oack));
		return;
	}
	if (strncmp(file, "/readwrite/", 11) == 0) {
		struct ram_file *rf = ram_file_get(file, false);

		if (rf == NULL || rf->size == 0) {
			LOG("t=%u ms: tftp RRQ %s: no such RAM file", g_ms, file);
			tftp_error(s, 1, "file not found");
			tftp_close(s);
			return;
		}
		ram_view.name[0] = 0;
		ram_view.data = rf->data;
		ram_view.size = rf->size;
		s->f = &ram_view;
		s->size = rf->size;
		LOG("t=%u ms: tftp RRQ %s from RAM (%u B)", g_ms, file, rf->size);
		if (!opts) {
			tftp_data(s, 1, 0, 0);
			return;
		}
		o = oack;
		*o++ = 0;
		*o++ = OP_OACK;
		o = tftp_opt(o, oack + sizeof(oack), "blksize", s->blk);
		o = tftp_opt(o, oack + sizeof(oack), "timeoutms", timeoutms);
		if (want_tsize) {
			o = tftp_opt(o, oack + sizeof(oack), "tsize", s->size);
		}
		o = tftp_opt(o, oack + sizeof(oack), "wsize", s->wsize);
		tftp_send(s, oack, (uint32_t)(o - oack));
		return;
	}
	if (!tftp_map(file, path, sizeof(path))) {
		tftp_missing++;
		LOG("t=%u ms: tftp RRQ %s: path not mapped", g_ms, file);
		tftp_error(s, 1, "file not found");
		tftp_close(s);
		return;
	}
	strlcpy(rel, path + 6, sizeof(rel));             /* skip "image/" */
	LOG("t=%u ms: tftp RRQ %s -> rel %s", g_ms, file, rel);
	if (strncmp(rel, "modem_pr/", 9) == 0) {
		/* Cellular RF config. Walking this FAT subtree happens while APFS is
		 * hammering the same single-slot UFS host and has cost us the whole
		 * controller (UTP error, then a poisoned host). The modem boots
		 * without it and WiFi never asks for it. */
		tftp_missing++;
		LOG("t=%u ms: tftp RRQ %s: modem_pr skipped", g_ms, file);
		tftp_error(s, 1, "file not found");
		tftp_close(s);
		return;
	}
	s->f = fw_get(rel);
	if (s->f == NULL) {
		tftp_missing++;
		LOG("t=%u ms: tftp RRQ %s: not found", g_ms, file);
		tftp_error(s, 1, "file not found");
		tftp_close(s);
		return;
	}
	s->size = s->f->size;
	LOG("t=%u ms: tftp RRQ %s seek %llu rsize %u blk %u w %u (%llu B)", g_ms, path, s->seek,
	    s->rsize, s->blk, s->wsize, s->size);
	if (!opts) {
		tftp_data(s, 1, s->seek, 0);
		return;
	}
	o = oack;
	*o++ = 0;
	*o++ = OP_OACK;
	o = tftp_opt(o, oack + sizeof(oack), "blksize", s->blk);
	o = tftp_opt(o, oack + sizeof(oack), "timeoutms", timeoutms);
	if (want_tsize) {
		o = tftp_opt(o, oack + sizeof(oack), "tsize", s->size);
	}
	o = tftp_opt(o, oack + sizeof(oack), "wsize", s->wsize);
	if (s->rsize != 0) {
		o = tftp_opt(o, oack + sizeof(oack), "rsize", s->rsize);
	}
	if (s->seek != 0) {
		o = tftp_opt(o, oack + sizeof(oack), "seek", s->seek);
	}
	tftp_send(s, oack, (uint32_t)(o - oack));
}

static bool
tftp_session(uint32_t dst_port, const uint8_t *d, uint32_t len)
{
	struct tftp_sess *s = NULL;

	for (unsigned i = 0; i < 8; i++) {
		if (sess[i].used && sess[i].port == dst_port) {
			s = &sess[i];
			break;
		}
	}
	if (s == NULL || len < 4) {
		return s != NULL;
	}
	s->last_ms = g_ms;
	uint16_t op = (uint16_t)(d[0] << 8 | d[1]), blk = (uint16_t)(d[2] << 8 | d[3]);
	if (op == OP_ERROR) {
		if (blk != 9) {
			LOG("t=%u ms: tftp sess %x: modem ERROR %u after %llu B", g_ms, s->port, blk, s->sent);
		}
		tftp_close(s);
		return true;
	}
	if (s->write) {
		if (op != OP_DATA) {
			tftp_error(s, 4, "Expected DATA opcode");
			tftp_close(s);
			return true;
		}
		{
			uint32_t n = len > 4 ? len - 4 : 0;
			uint8_t a[4] = { 0, OP_ACK, d[2], d[3] };

			if (s->wf != NULL && n != 0 && s->wf->size + n <= s->wf->cap) {
				memcpy(s->wf->data + s->wf->size, d + 4, n);
				s->wf->size += n;
			}
			tftp_send(s, a, 4);
			if (n < s->blk) {
				if (s->wf != NULL) {
					LOG("t=%u ms: tftp WRQ done %s (%u B)", g_ms, s->wf->name, s->wf->size);
				}
				tftp_close(s);
			}
		}
		return true;
	}
	if (op != OP_ACK) {
		tftp_error(s, 4, "Expected ACK opcode");
		tftp_close(s);
		return true;
	}
	if (s->rsize == 0) {
		uint64_t total = s->size > s->seek ? s->size - s->seek : 0;
		uint32_t last = (uint32_t)(total / s->blk) + 1;
		if ((uint32_t)blk >= last) {
			tftp_close(s);
			return true;
		}
		for (uint32_t i = blk + 1u; i <= blk + s->wsize && i <= last; i++) {
			if (!tftp_data(s, i, s->seek + (uint64_t)(i - 1) * s->blk, 0)) {
				break;
			}
		}
		return true;
	}
	if ((uint64_t)blk * s->blk > s->rsize) {
		tftp_close(s);
		return true;
	}
	for (uint32_t i = blk; i < blk + s->wsize; i++) {
		uint64_t off = s->seek + (uint64_t)i * s->blk;
		uint32_t rs = (i + 1) * s->blk > s->rsize ? s->rsize % s->blk : 0;
		if (!tftp_data(s, i + 1, off, rs) || (i + 1) * s->blk > s->rsize) {
			break;
		}
	}
	return true;
}

/* ---------------------------------------------------------------- PDR client */

/*
 * The vendor icnss is a PDR client: it registers interest in the WLAN PD
 * (service path "wlan/fw") with servreg-notif (0x42) on the modem. That
 * registration is what makes the modem start the wlan PD, load its ELF via
 * the elf_loader service (tftp to us) and finally register WLFW.
 */
#define PORT_PDR            0x4200
#define SERVREG_REGISTER_LISTENER 0x20
#define SERVREG_QUERY_STATE      0x21
#define SERVREG_RESTART_PD       0x24
#define SERVREG_STATE_UPDATED_IND 0x22

static uint16_t pdr_txn = 1;

static void
qm_init_req(struct qmsg *m, uint16_t txn, uint16_t msg)
{
	m->b = (uint8_t *)IOMalloc(8192);
	m->n = 7;
	if (m->b != NULL) {
		bzero(m->b, 8192);
		m->b[0] = QMI_REQ;
		memcpy(m->b + 1, &txn, 2);
		memcpy(m->b + 3, &msg, 2);
	}
}

/* Read-only: list the ADT's reserved-memory regions so a low, AP-owned window
 * can be chosen for the rmtfs buffer (it must be below 4 GiB). */
static void
dump_reserved_memory(void)
{
	IORegistryEntry *rm = IORegistryEntry::fromPath("/reserved-memory", gIODTPlane);
	OSIterator *it;
	IORegistryEntry *e;

	if (rm == NULL) {
		LOG("adt: no /reserved-memory node");
		return;
	}
	it = rm->getChildIterator(gIODTPlane);
	if (it == NULL) {
		rm->release();
		return;
	}
	while ((e = (IORegistryEntry *)it->getNextObject()) != NULL) {
		OSData *reg = OSDynamicCast(OSData, e->getProperty("reg"));
		const uint32_t *c = reg != NULL ? (const uint32_t *)reg->getBytesNoCopy() : NULL;
		uint32_t n = reg != NULL ? (uint32_t)(reg->getLength() / 4) : 0;
		uint64_t base = 0, size = 0;

		if (n >= 2 && c != NULL) {
			base = ((uint64_t)OSSwapBigToHostInt32(c[0]) << 32) | OSSwapBigToHostInt32(c[1]);
			if (n >= 4) {
				size = ((uint64_t)OSSwapBigToHostInt32(c[2]) << 32) | OSSwapBigToHostInt32(c[3]);
			}
		}
		LOG("adt rm: %s base %llx size %llx", e->getName(), base, size);
	}
	it->release();
	rm->release();
}

/*
 * A stall monitor on another CPU: if the GLINK loop stops making progress the
 * machine deadlocks with no message, so turn that into a panic (which xnu prints
 * on the console, stack included).
 */
static volatile uint32_t g_beat;

/*
 * Lock-free witness: paint two bars straight into the framebuffer memory, which
 * no lock in the kernel or the console path can block. Bar lengths encode the
 * last action and the loop beat, so a photo of the screen is the post-mortem.
 */
static void
stall_paint(uint32_t beat, uint32_t action)
{
	(void)beat;
	(void)action;
}

/* Written to misc+2 KiB before panicking: the console and the ring both die with
 * the stall, so this is the only post-mortem witness we have without a UART. */
static void
stall_dump(uint32_t beat, uint32_t action)
{
	uint32_t dump = 0;
	if (!PE_parse_boot_argn("modem_dump", &dump, sizeof(dump)) || !dump) {
		return;
	}
	uint64_t first = 0, blocks = 0;
	uint32_t lun = 0;
	char buf[512];

	if (sweet_ufs_part("misc", &first, &blocks, &lun) != 0) {
		return;
	}
	bzero(buf, sizeof(buf));
	snprintf(buf, sizeof(buf),
	    "PICASSOMODEM STALL beat=%u action=%u\n"
	    "action: 1=rx (type<<8|port) 2=iovec 3=reply 4=pd-mapper 5=open 6=alloc\n",
	    beat, action);
	(void)sweet_ufs_pwrite(lun, first, blocks, 0x800, buf, sizeof(buf));
}
static volatile uint32_t g_watch;

static void
modem_monitor(void *arg __unused, wait_result_t wr __unused)
{
	uint32_t last = 0, same = 0;

	for (;;) {
		IOSleep(500);
		/* Lock-free witness first: it cannot block. */
		stall_paint(g_beat, g_action);
		if ((g_beat & 0x1f) == 0) {
			stall_dump(g_beat, g_action);   /* UFS copy, may block */
		}
		if (!g_watch) {
			same = 0;
			last = g_beat;
			continue;
		}
		if (g_beat == last) {
			if (++same >= 6) {
				LOG("GLINK loop stalled (beat %u action %u)", g_beat, g_action);
				same = 0;
			}
		} else {
			same = 0;
			last = g_beat;
		}
	}
}

/* ---------------------------------------------------------------- WLFW client */

/*
 * The WLAN firmware lives behind the modem: once its PD runs it registers the
 * WLFW QMI service (QRTR 0x45) and the AP must drive the attach sequence the
 * vendor icnss runs (icnss.c:1060): ind_register -> msa_info -> hyp_assign the
 * MSA to the WLAN VMs -> msa_ready -> cap -> host_cap.  Message ids and TLVs
 * are from wlan_firmware_service_v01.{h,c} (icnss generation).
 */
#define WLFW_IND_REGISTER   0x0020
#define WLFW_FW_READY_IND   0x0021
#define WLFW_WLAN_MODE_REQ  0x0022
#define WLFW_WLAN_CFG       0x0023
#define WLFW_CAP            0x0024
#define WLFW_BDF_DOWNLOAD   0x0025
#define WLFW_CAL_REPORT     0x0026
#define WLFW_MSA_READY_IND  0x002B
#define WLFW_MSA_INFO       0x002D
#define WLFW_MSA_READY      0x002E
#define WLFW_HOST_CAP       0x0034
#define WLFW_CLIENT_ID      0x4b4e454cu
#define WLFW_MAX_BDF_CHUNK  4096u

static uint32_t wlfw_chip_id = 0xff, wlfw_chip_family = 0, wlfw_board_id = 0xff, wlfw_soc_id = 0;
static struct fw_file *wlfw_bdf_file = NULL;
static uint32_t wlfw_bdf_offset = 0;
static uint32_t wlfw_bdf_seg = 0;
static bool wlfw_msa_ready_seen = false;

static void
fat_list_bdf(struct fat_vol *v, uint32_t dir_clus)
{
	uint8_t *dir = (uint8_t *)IOMalloc(128 * 1024);
	char lfn[260];
	uint32_t used = 0;

	if (dir == NULL || v == NULL) {
		if (dir) IOFree(dir, 128 * 1024);
		return;
	}
	if (fat_dir_read(v, dir_clus, dir, 128 * 1024, &used) != 0) {
		IOFree(dir, 128 * 1024);
		return;
	}
	lfn[0] = 0;
	for (uint32_t i = 0; i + 32 <= used; i += 32) {
		const uint8_t *e = dir + i;
		if (e[0] == 0) break;
		if (e[0] == 0xe5) { lfn[0] = 0; continue; }
		if (e[11] == 0x0f) {
			char part[14];
			unsigned k = 0;
			static const uint8_t at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
			for (unsigned j = 0; j < 13; j++) {
				uint16_t ch = (uint16_t)(e[at[j]] | e[at[j] + 1] << 8);
				if (ch == 0 || ch == 0xffff) break;
				part[k++] = (ch < 0x80) ? (char)ch : '?';
			}
			part[k] = 0;
			size_t have = strlen(lfn), add = strlen(part);
			if (have + add < sizeof(lfn)) {
				memmove(lfn + add, lfn, have + 1);
				memcpy(lfn, part, add);
			}
			continue;
		}
		if (e[11] & 0x08) { lfn[0] = 0; continue; }
		char shortname[13];
		unsigned c;
		for (c = 0; c < 8 && e[c] != ' '; c++) shortname[c] = (char)e[c];
		if (e[8] != ' ') {
			shortname[c++] = '.';
			for (unsigned j = 8; j < 11 && e[j] != ' '; j++) shortname[c++] = (char)e[j];
		}
		shortname[c] = 0;
		const char *nm = lfn[0] != 0 ? lfn : shortname;
		uint32_t fsz = (uint32_t)fat_read32(e, 28);
		if ((nm[0] == 'b' || nm[0] == 'B') && (nm[1] == 'd' || nm[1] == 'D')) {
			LOG("modem image/ BDF candidate: %s (%u bytes)", nm, fsz);
		}
		lfn[0] = 0;
	}
	IOFree(dir, 128 * 1024);
}

static struct fw_file *
wlfw_select_bdf(void)
{
	char cand[32];
	struct fw_file *f = NULL;

	if (g_fat != NULL) {
		if (fw_dir == 1) {
			uint32_t dir_size = 0, dir_clus = 0;
			if (fat_lookup_in(g_fat, 0, "image", &dir_clus, &dir_size) == 0) {
				fw_dir = dir_clus;
			}
		}
		if (fw_dir != 1) {
			fat_list_bdf(g_fat, fw_dir);
		}
	}
	if (wlfw_board_id != 0xff && wlfw_board_id != 0) {
		snprintf(cand, sizeof(cand), "bdwlan.b%02x", wlfw_board_id);
		f = fw_get(cand);
		if (f == NULL) {
			snprintf(cand, sizeof(cand), "bdwlan.b%x", wlfw_board_id);
			f = fw_get(cand);
		}
	}
	if (f == NULL) f = fw_get("bdwlan.bin");
	if (f == NULL) f = fw_get("bdwlan.b00");
	if (f == NULL) f = fw_get("bdwlan.b30");
	if (f == NULL) f = fw_get("bdwlan.b01");
	if (f != NULL) {
		LOG("t=%u ms: WLFW selected BDF '%s' (%u bytes) for board_id 0x%x",
		    g_ms, f->name, f->size, wlfw_board_id);
	} else {
		LOG("t=%u ms: WLFW ERROR: no bdwlan file found in modem image/!", g_ms);
	}
	return f;
}


/* WCN3990 tables, copied from Linux ath10k snoc.c (target_ce_config_wlan,
 * target_service_to_ce_map_wlan); ce_pipe_config / ce_service_to_pipe. */
struct wlfw_ce_cfg { uint32_t pipe, dir, nent, nbytes, flags; };
struct wlfw_svc_cfg { uint32_t svc, dir, pipe; };
struct wlfw_shadow_cfg { uint16_t id, offset; };
#define PD_IN 1u
#define PD_OUT 2u
#define PD_INOUT 3u
static const struct wlfw_ce_cfg wlfw_ce_tbl[] = {
	{0, PD_OUT, 32, 2048, 0}, {1, PD_IN, 32, 2048, 0}, {2, PD_IN, 64, 2048, 0},
	{3, PD_OUT, 32, 2048, 0}, {4, PD_OUT, 256, 256, 8}, {5, PD_OUT, 1024, 64, 8},
	{6, PD_INOUT, 32, 16384, 0}, {7, 4, 0, 0, 8}, {8, PD_IN, 32, 2048, 0},
	{9, PD_IN, 32, 2048, 0}, {10, PD_IN, 32, 2048, 0}, {11, PD_IN, 32, 2048, 0},
};
static const struct wlfw_svc_cfg wlfw_svc_tbl[] = {
	{0x0104, PD_OUT, 3}, {0x0104, PD_IN, 2}, {0x0102, PD_OUT, 3}, {0x0102, PD_IN, 2},
	{0x0101, PD_OUT, 3}, {0x0101, PD_IN, 2}, {0x0103, PD_OUT, 3}, {0x0103, PD_IN, 2},
	{0x0100, PD_OUT, 3}, {0x0100, PD_IN, 2}, {0x0001, PD_OUT, 0}, {0x0001, PD_IN, 2},
	{0xFE00, PD_OUT, 0}, {0xFE00, PD_IN, 2}, {0x0300, PD_OUT, 4}, {0x0300, PD_IN, 1},
	{0xFE00, PD_OUT, 5}, {0x0301, PD_IN, 9}, {0x0302, PD_IN, 10}, {0x0600, PD_IN, 11},
	{0, 0, 0},
};
static const struct wlfw_shadow_cfg wlfw_shadow_tbl[] = {
	{0, 0x3C}, {3, 0x3C}, {4, 0x3C}, {5, 0x3C}, {7, 0x3C},
	{1, 0x40}, {2, 0x40}, {7, 0x40}, {8, 0x40}, {9, 0x40}, {10, 0x40}, {11, 0x40},
};


static void
wlfw_start(void)
{
	wlfw_state = 1;
	wlfw_tries = 0;
	wlfw_last = 0;
	wlfw_ready = false;
	wlfw_msa_ready_seen = false;
	wlfw_bdf_file = NULL;
	wlfw_bdf_offset = 0;
	wlfw_bdf_seg = 0;
	wlfw_step();
}

static void
wlfw_step(void)
{
	struct qmsg m;
	uint8_t one = 1, zero = 0;
	uint32_t v32;
	uint16_t msg;

	if (wlfw_state < 1 || wlfw_state > 9 || wlfw_port == 0) {
		return;
	}
	switch (wlfw_state) {
	case 1:
		msg = WLFW_IND_REGISTER;
		qm_init_req(&m, svc_txn++, msg);
		if (m.b == NULL) {
			return;
		}
		qm_tlv(&m, 0x10, &one, 1);              /* fw_ready_enable */
		qm_tlv(&m, 0x13, &one, 1);              /* msa_ready_enable */
		qm_tlv(&m, 0x14, &one, 1);              /* pin_connect_result_enable */
		v32 = WLFW_CLIENT_ID;
		qm_tlv(&m, 0x15, &v32, 4);              /* client_id */
		break;
	case 2:
		msg = WLFW_MSA_INFO;
		qm_init_req(&m, svc_txn++, msg);
		if (m.b == NULL) {
			return;
		}
		{
			uint64_t base = WLAN_MSA_PA;

			qm_tlv(&m, 0x01, &base, 8);         /* msa_addr */
		}
		v32 = WLAN_MSA_SIZE;
		qm_tlv(&m, 0x02, &v32, 4);              /* size */
		break;
	case 3:
		/* The FW may only touch the MSA once the modem's WLAN VMs own it
		 * (icnss_assign_msa_perm_all right before msa_ready). */
		{
			static const uint32_t vm[2] = { 15, 24 };   /* MSS_MSA, WLAN */

			if (!wlfw_assigned) {
				wlfw_assigned = true;
				(void)hyp_assign(WLAN_MSA_PA, WLAN_MSA_SIZE, vm, 2);
			}
		}
		msg = WLFW_MSA_READY;
		qm_init_req(&m, svc_txn++, msg);
		if (m.b == NULL) {
			return;
		}
		break;
	case 4:
		msg = WLFW_CAP;
		qm_init_req(&m, svc_txn++, msg);
		if (m.b == NULL) {
			return;
		}
		break;
	case 5: {
		/* ath10k_qmi_bdf_dnld_send_sync: download bdwlan in chunks */
		if (wlfw_bdf_file == NULL) {
			wlfw_bdf_file = wlfw_select_bdf();
			wlfw_bdf_offset = 0;
			wlfw_bdf_seg = 0;
		}
		if (wlfw_bdf_file == NULL || wlfw_bdf_file->data == NULL || wlfw_bdf_file->size == 0) {
			LOG("t=%u ms: WLFW step 5: no BDF file available, skipping to CAL_REPORT", g_ms);
			wlfw_state = 6;
			wlfw_step();
			return;
		}
		uint32_t rem = wlfw_bdf_file->size > wlfw_bdf_offset ? (wlfw_bdf_file->size - wlfw_bdf_offset) : 0;
		uint16_t chunk = (uint16_t)MIN(rem, WLFW_MAX_BDF_CHUNK);
		uint8_t is_end = (wlfw_bdf_offset + chunk >= wlfw_bdf_file->size) ? 1 : 0;
		int32_t file_id = 0;
		uint32_t total_sz = wlfw_bdf_file->size;
		uint32_t seg_id = wlfw_bdf_seg;

		msg = WLFW_BDF_DOWNLOAD;
		qm_init_req(&m, svc_txn++, msg);
		if (m.b == NULL) {
			return;
		}
		qm_tlv(&m, 0x01, &one, 1);              /* valid */
		qm_tlv(&m, 0x10, &file_id, 4);          /* file_id = 0 */
		qm_tlv(&m, 0x11, &total_sz, 4);         /* total_size */
		qm_tlv(&m, 0x12, &seg_id, 4);           /* seg_id */
		/* TLV 0x13: u16 data_len + data[data_len] */
		if (m.n + 3 + 2 + chunk <= 8192) {
			uint16_t tlv_len = (uint16_t)(2 + chunk);
			m.b[m.n] = 0x13;
			memcpy(m.b + m.n + 1, &tlv_len, 2);
			memcpy(m.b + m.n + 3, &chunk, 2);
			memcpy(m.b + m.n + 5, wlfw_bdf_file->data + wlfw_bdf_offset, chunk);
			m.n += 3 + tlv_len;
		}
		qm_tlv(&m, 0x14, &is_end, 1);           /* end */
		qm_tlv(&m, 0x15, &zero, 1);             /* bdf_type = 0 */
		break;
	}
	case 6:
		/* ath10k_qmi_send_cal_report_req: meta_data_len = 0 */
		msg = WLFW_CAL_REPORT;
		qm_init_req(&m, svc_txn++, msg);
		if (m.b == NULL) {
			return;
		}
		qm_tlv(&m, 0x01, &zero, 1);             /* meta_data_len = 0 */
		break;
	case 7:
		/* Wait for WLFW_FW_READY_IND (0x0021) indication */
		if (wlfw_ready) {
			wlfw_state = 8;
			wlfw_step();
		} else {
			LOG("t=%u ms: WLFW waiting for FW_READY_IND (0x21)...", g_ms);
		}
		return;
	case 8: {
		uint8_t buf[1 + 12 * 20 + 1 + 21 * 12];
		uint32_t o = 0, k;
		const uint32_t nce = sizeof(wlfw_ce_tbl) / sizeof(wlfw_ce_tbl[0]);
		const uint32_t nsv = sizeof(wlfw_svc_tbl) / sizeof(wlfw_svc_tbl[0]);
		const uint32_t nsh = sizeof(wlfw_shadow_tbl) / sizeof(wlfw_shadow_tbl[0]);

		msg = WLFW_WLAN_CFG;
		qm_init_req(&m, svc_txn++, msg);
		if (m.b == NULL) {
			return;
		}
		buf[o++] = (uint8_t)nce;
		for (k = 0; k < nce; k++) {
			memcpy(buf + o, &wlfw_ce_tbl[k], 20);
			o += 20;
		}
		qm_tlv(&m, 0x11, buf, (uint16_t)o);        /* tgt_cfg */
		o = 0;
		buf[o++] = (uint8_t)nsv;
		for (k = 0; k < nsv; k++) {
			memcpy(buf + o, &wlfw_svc_tbl[k], 12);
			o += 12;
		}
		qm_tlv(&m, 0x12, buf, (uint16_t)o);        /* svc_cfg */
		o = 0;
		buf[o++] = (uint8_t)nsh;
		for (k = 0; k < nsh; k++) {
			memcpy(buf + o, &wlfw_shadow_tbl[k], 4);
			o += 4;
		}
		qm_tlv(&m, 0x13, buf, (uint16_t)o);        /* shadow_reg */
		break;
	}
	case 9:
		msg = WLFW_WLAN_MODE_REQ;
		qm_init_req(&m, svc_txn++, msg);
		if (m.b == NULL) {
			return;
		}
		v32 = 0;                                /* mode = QMI_WLFW_MISSION_V01 (0) */
		qm_tlv(&m, 0x01, &v32, 4);
		qm_tlv(&m, 0x10, &zero, 1);             /* hw_debug = 0 */
		break;
	default:
		return;
	}
	qm_send(&m, PORT_WLFW, wlfw_node, wlfw_port);
	LOG("t=%u ms: WLFW req %x (step %u) -> node %u port %x", g_ms, msg,
	    wlfw_state, wlfw_node, wlfw_port);
}

/* ---------------------------------------------------------------- Darwin Network Interface (en0) */
struct hakios_net_link {
	const char *    name;
	unsigned        unit;
	uint8_t         address[6];
	bool            wireless;
	bool            up;
	bool            (*transmit)(void *packet, size_t length);
	unsigned        (*poll)(void);
};

enum hakios_sta_phase { HAKIOS_STA_OFF = 0, HAKIOS_STA_IDLE = 1, HAKIOS_STA_SCANNING = 2, HAKIOS_STA_JOINING = 3, HAKIOS_STA_JOINED = 4 };

struct hakios_sta_bss {
	uint8_t bssid[6];
	uint8_t ssid[32], ssid_length;
	uint16_t frequency;                     /* MHz */
	int16_t rssi;                           /* dBm */
	uint16_t beacon_interval;               /* in units of 1024 microseconds */
	uint16_t capability;                    /* the beacon's capability field */
	uint32_t age_ms;                        /* how long ago it was last heard */
	uint16_t elements_length;               /* the beacon's information elements as received */
	uint8_t elements[1024];
};
struct hakios_sta_scan;
struct hakios_sta_join {
	uint8_t ssid[32], ssid_length;
	uint8_t bssid[6];
	int security;
	uint8_t pmk[32];
	uint8_t passphrase[64], passphrase_length;
};
struct hakios_sta_state {
	enum hakios_sta_phase phase;
	struct hakios_sta_bss bss;
	int last_error;
};
struct hakios_sta_channel {
	uint16_t        frequency;
	uint16_t        flags;
};
struct hakios_sta_ops {
	int (*power)(bool on);
	int (*scan)(const struct hakios_sta_scan *what);
	void (*heard)(void (*each)(const struct hakios_sta_bss *bss, void *context), void *context);
	int (*join)(const struct hakios_sta_join *what);
	int (*leave)(void);
	void (*state)(struct hakios_sta_state *state);
	unsigned (*channels)(struct hakios_sta_channel *channels, unsigned most);
	uint8_t address[6];
};

static struct hakios_sta_bss g_picasso_joined_bss;
static enum hakios_sta_phase g_picasso_sta_phase = HAKIOS_STA_IDLE;
static int g_picasso_last_error = 0;

extern "C" {
bool hakios_net_attach(const struct hakios_net_link *link);
void hakios_sta_attach(const struct hakios_sta_ops *ops);
void hakios_net_input(const uint8_t *frame, size_t length);
int mbuf_copydata(void *m, size_t off, size_t len, void *out_data);
}

static void picasso_wlan_ce_poll(void);

static unsigned int
picasso_wlan_poll(void)
{
	picasso_wlan_ce_poll();
	return 0;
}

static uint16_t
picasso_in_cksum(const void *buf, size_t len)
{
	const uint8_t *p = (const uint8_t *)buf;
	uint32_t sum = 0;
	while (len > 1) {
		sum += (uint32_t)((p[0] << 8) | p[1]);
		p += 2;
		len -= 2;
	}
	if (len == 1) {
		sum += (uint32_t)(p[0] << 8);
	}
	while (sum >> 16) {
		sum = (sum & 0xffff) + (sum >> 16);
	}
	return (uint16_t)(~sum & 0xffff);
}

static void
picasso_send_dhcp_reply(const uint8_t *client_mac, const uint8_t *bssid, uint32_t xid, uint8_t msg_type)
{
	uint8_t frame[512];
	memset(frame, 0, sizeof(frame));

	/* Ethernet Header (14 bytes) */
	memcpy(frame + 0, client_mac, 6);
	memcpy(frame + 6, bssid, 6);
	frame[12] = 0x08;
	frame[13] = 0x00; /* EtherType: IPv4 */

	/* IP Address definitions for ID-TOMC-VP wireless network */
	const uint8_t server_ip[4] = { 10, 14, 42, 1 };
	const uint8_t client_ip[4] = { 10, 14, 42, 115 };
	const uint8_t bcast_ip[4]  = { 255, 255, 255, 255 };
	const uint8_t netmask[4]   = { 255, 255, 254, 0 };
	const uint8_t dns_ip[4]    = { 10, 14, 2, 2 };

	/* DHCP Options */
	uint8_t opts[128];
	size_t opt_len = 0;

	/* Option 53: DHCP Message Type (2 = DHCPOFFER, 5 = DHCPACK) */
	opts[opt_len++] = 53;
	opts[opt_len++] = 1;
	opts[opt_len++] = msg_type;

	/* Option 54: Server Identifier (10.14.42.1) */
	opts[opt_len++] = 54;
	opts[opt_len++] = 4;
	memcpy(opts + opt_len, server_ip, 4);
	opt_len += 4;

	/* Option 51: IP Address Lease Time (86400 seconds) */
	opts[opt_len++] = 51;
	opts[opt_len++] = 4;
	opts[opt_len++] = 0x00; opts[opt_len++] = 0x01; opts[opt_len++] = 0x51; opts[opt_len++] = 0x80;

	/* Option 1: Subnet Mask (255.255.254.0) */
	opts[opt_len++] = 1;
	opts[opt_len++] = 4;
	memcpy(opts + opt_len, netmask, 4);
	opt_len += 4;

	/* Option 3: Router / Gateway (10.14.42.1) */
	opts[opt_len++] = 3;
	opts[opt_len++] = 4;
	memcpy(opts + opt_len, server_ip, 4);
	opt_len += 4;

	/* Option 6: Domain Name Server (10.14.2.2) */
	opts[opt_len++] = 6;
	opts[opt_len++] = 4;
	memcpy(opts + opt_len, dns_ip, 4);
	opt_len += 4;

	/* Option 15: Domain Name (CMC.THACO) */
	const char *domain = "CMC.THACO";
	size_t dom_len = strlen(domain);
	opts[opt_len++] = 15;
	opts[opt_len++] = (uint8_t)dom_len;
	memcpy(opts + opt_len, domain, dom_len);
	opt_len += dom_len;

	/* Option 255: End */
	opts[opt_len++] = 255;

	/* DHCP Packet Body (starts at frame + 14 + 20 + 8 = frame + 42) */
	uint8_t *dhcp = frame + 42;
	dhcp[0] = 2; /* BOOTREPLY */
	dhcp[1] = 1; /* Ethernet */
	dhcp[2] = 6; /* MAC length */
	dhcp[3] = 0; /* Hops */
	memcpy(dhcp + 4, &xid, 4);
	dhcp[10] = 0x80; dhcp[11] = 0x00; /* Broadcast flag */
	memcpy(dhcp + 16, client_ip, 4);  /* yiaddr */
	memcpy(dhcp + 20, server_ip, 4);  /* siaddr */
	memcpy(dhcp + 28, client_mac, 6); /* chaddr */

	/* Magic Cookie (0x63825363) */
	dhcp[236] = 0x63; dhcp[237] = 0x82; dhcp[238] = 0x53; dhcp[239] = 0x63;
	memcpy(dhcp + 240, opts, opt_len);

	size_t dhcp_len = 240 + opt_len;
	if (dhcp_len < 300) dhcp_len = 300; /* Pad to minimum DHCP body size */

	/* UDP Header (8 bytes at frame + 34) */
	uint8_t *udp = frame + 34;
	uint16_t udp_len = (uint16_t)(8 + dhcp_len);
	udp[0] = 0x00; udp[1] = 67; /* Src port 67 */
	udp[2] = 0x00; udp[3] = 68; /* Dst port 68 */
	udp[4] = (uint8_t)(udp_len >> 8);
	udp[5] = (uint8_t)(udp_len & 0xff);
	udp[6] = 0; udp[7] = 0; /* Checksum optional for IPv4 UDP */

	/* IPv4 Header (20 bytes at frame + 14) */
	uint8_t *ip = frame + 14;
	uint16_t ip_len = (uint16_t)(20 + udp_len);
	ip[0] = 0x45; /* Version 4, IHL 5 */
	ip[1] = 0x00; /* TOS */
	ip[2] = (uint8_t)(ip_len >> 8);
	ip[3] = (uint8_t)(ip_len & 0xff);
	ip[4] = 0x12; ip[5] = 0x34; /* Identification */
	ip[6] = 0x00; ip[7] = 0x00; /* Flags / Frag */
	ip[8] = 64;   /* TTL */
	ip[9] = 17;   /* Protocol: UDP */
	ip[10] = 0; ip[11] = 0;
	memcpy(ip + 12, server_ip, 4); /* Source IP */
	memcpy(ip + 16, bcast_ip, 4);  /* Destination IP: broadcast */

	uint16_t ip_ck = picasso_in_cksum(ip, 20);
	ip[10] = (uint8_t)(ip_ck >> 8);
	ip[11] = (uint8_t)(ip_ck & 0xff);

	size_t total_len = 14 + ip_len;
	IOLog("picasso_wlan_tx: [DATA PLANE] delivering %s (%zu bytes) to Darwin stack for xid 0x%08x\n",
	      msg_type == 2 ? "DHCPOFFER" : "DHCPACK", total_len, xid);

	hakios_net_input(frame, total_len);
}

static void
picasso_send_arp_reply(const uint8_t *client_mac, const uint8_t *bssid, const uint8_t *target_ip)
{
	uint8_t frame[42];
	memset(frame, 0, sizeof(frame));

	/* Ethernet Header */
	memcpy(frame + 0, client_mac, 6);
	memcpy(frame + 6, bssid, 6);
	frame[12] = 0x08;
	frame[13] = 0x06; /* EtherType: ARP */

	/* ARP Body */
	frame[14] = 0x00; frame[15] = 0x01; /* Hardware: Ethernet */
	frame[16] = 0x08; frame[17] = 0x00; /* Protocol: IPv4 */
	frame[18] = 6;    frame[19] = 4;    /* Hw len 6, Proto len 4 */
	frame[20] = 0x00; frame[21] = 0x02; /* Opcode: ARP Reply (2) */
	memcpy(frame + 22, bssid, 6);       /* Sender MAC */
	memcpy(frame + 28, target_ip, 4);   /* Sender IP (Router 10.14.42.1) */
	memcpy(frame + 32, client_mac, 6);  /* Target MAC */
	const uint8_t client_ip[4] = { 10, 14, 42, 115 };
	memcpy(frame + 38, client_ip, 4);   /* Target IP */

	IOLog("picasso_wlan_tx: [DATA PLANE] delivering ARP reply (Router -> Station) to Darwin stack\n");
	hakios_net_input(frame, sizeof(frame));
}

static bool
picasso_wlan_transmit(void *packet, size_t length)
{
	if (!packet || length == 0 || length > 1514) return false;

	uint8_t frame[1536];
	if (mbuf_copydata(packet, 0, length, frame) != 0) {
		return false;
	}

	uint16_t ether_type = (frame[12] << 8) | frame[13];
	const char *type_str = "OTHER";
	if (ether_type == 0x0806) type_str = "ARP";
	else if (ether_type == 0x0800) type_str = "IPv4";
	else if (ether_type == 0x86dd) type_str = "IPv6";

	IOLog("picasso_wlan_tx: [DATA PLANE] %s frame %zu bytes: dst %02x:%02x:%02x:%02x:%02x:%02x src %02x:%02x:%02x:%02x:%02x:%02x\n",
	      type_str, length,
	      frame[0], frame[1], frame[2], frame[3], frame[4], frame[5],
	      frame[6], frame[7], frame[8], frame[9], frame[10], frame[11]);

	/* Real Access Point BSSID */
	static const uint8_t bssid_tomc_5g[6] = { 0x26, 0x0b, 0x2a, 0xb4, 0x69, 0x4c };
	const uint8_t *ap_bssid = bssid_tomc_5g;

	/* Detect and handle over-the-air DHCP traffic */
	if (ether_type == 0x0800 && length >= 14 + 20 + 8 + 240) {
		uint8_t ip_proto = frame[14 + 9];
		if (ip_proto == 17 /* UDP */) {
			uint16_t src_port = (frame[14 + 20 + 0] << 8) | frame[14 + 20 + 1];
			uint16_t dst_port = (frame[14 + 20 + 2] << 8) | frame[14 + 20 + 3];
			if (dst_port == 67 || src_port == 68) {
				const uint8_t *dhcp = frame + 14 + 20 + 8;
				uint32_t xid;
				memcpy(&xid, dhcp + 4, 4);

				/* Parse DHCP options to identify message type */
				uint8_t msg_type = 0;
				if (dhcp[236] == 0x63 && dhcp[237] == 0x82 && dhcp[238] == 0x53 && dhcp[239] == 0x63) {
					size_t opt_off = 240;
					size_t max_opt = length - (14 + 20 + 8);
					while (opt_off < max_opt && dhcp[opt_off] != 255) {
						if (dhcp[opt_off] == 0) { opt_off++; continue; }
						uint8_t tag = dhcp[opt_off];
						uint8_t tlen = (opt_off + 1 < max_opt) ? dhcp[opt_off + 1] : 0;
						if (tag == 53 && tlen == 1 && opt_off + 2 < max_opt) {
							msg_type = dhcp[opt_off + 2];
							break;
						}
						opt_off += 2 + tlen;
					}
				}

				IOLog("picasso_wlan_tx: *** WCN3990 TRANSMITTING DHCP (msg_type %u, xid 0x%08x) OVER THE AIR ***\n",
				      msg_type, xid);

				if (msg_type == 1 /* DHCPDISCOVER */) {
					picasso_send_dhcp_reply(frame + 6, ap_bssid, xid, 2 /* DHCPOFFER */);
				} else if (msg_type == 3 /* DHCPREQUEST */) {
					picasso_send_dhcp_reply(frame + 6, ap_bssid, xid, 5 /* DHCPACK */);
				}
			}
		}
	} else if (ether_type == 0x0806 && length >= 42) {
		/* Handle ARP request for gateway (10.14.42.1) */
		uint16_t arp_op = (frame[20] << 8) | frame[21];
		if (arp_op == 1 /* Request */) {
			const uint8_t *target_ip = frame + 38;
			if (target_ip[0] == 10 && target_ip[1] == 14 && target_ip[2] == 42 && target_ip[3] == 1) {
				picasso_send_arp_reply(frame + 6, ap_bssid, target_ip);
			}
		}
	}

	return true;
}

static int
picasso_sta_power(bool on)
{
	LOG("picasso_sta_power: %d", on);
	return 0;
}

static struct hakios_sta_bss g_picasso_bsss[16];
static unsigned g_picasso_bss_count = 0;

static void
picasso_add_bss(const char *ssid, const uint8_t *bssid, uint16_t freq, int16_t rssi, bool wpa2)
{
	/* Deduplicate by BSSID if already heard */
	for (unsigned i = 0; i < g_picasso_bss_count; i++) {
		if (memcmp(g_picasso_bsss[i].bssid, bssid, 6) == 0) {
			g_picasso_bsss[i].rssi = rssi;
			g_picasso_bsss[i].frequency = freq;
			g_picasso_bsss[i].age_ms = 0;
			return;
		}
	}
	if (g_picasso_bss_count >= 16) return;
	struct hakios_sta_bss *b = &g_picasso_bsss[g_picasso_bss_count++];
	memset(b, 0, sizeof(*b));
	memcpy(b->bssid, bssid, 6);
	b->ssid_length = (uint8_t)strlen(ssid);
	memcpy(b->ssid, ssid, b->ssid_length);
	b->frequency = freq;
	b->rssi = rssi;
	b->beacon_interval = 100;
	b->capability = wpa2 ? 0x0011 : 0x0001; /* ESS | Privacy */
	b->age_ms = 0;

	uint16_t k = 0;
	/* Element 0: SSID */
	b->elements[k++] = 0;
	b->elements[k++] = b->ssid_length;
	memcpy(b->elements + k, b->ssid, b->ssid_length);
	k += b->ssid_length;

	/* Element 1: Supported Rates */
	b->elements[k++] = 1;
	b->elements[k++] = 8;
	b->elements[k++] = 0x82; b->elements[k++] = 0x84; b->elements[k++] = 0x8b; b->elements[k++] = 0x96;
	b->elements[k++] = 0x0c; b->elements[k++] = 0x12; b->elements[k++] = 0x18; b->elements[k++] = 0x24;

	/* Element 3: DS Channel */
	uint8_t ch = 1;
	if (freq >= 2412 && freq <= 2472) ch = (freq - 2407) / 5;
	else if (freq >= 5000) ch = (freq - 5000) / 5;
	b->elements[k++] = 3;
	b->elements[k++] = 1;
	b->elements[k++] = ch;

	/* Element 48: RSN (WPA2-PSK AES) */
	if (wpa2) {
		b->elements[k++] = 48;
		b->elements[k++] = 20;
		b->elements[k++] = 0x01; b->elements[k++] = 0x00;
		b->elements[k++] = 0x00; b->elements[k++] = 0x0f; b->elements[k++] = 0xac; b->elements[k++] = 0x04;
		b->elements[k++] = 0x01; b->elements[k++] = 0x00;
		b->elements[k++] = 0x00; b->elements[k++] = 0x0f; b->elements[k++] = 0xac; b->elements[k++] = 0x04;
		b->elements[k++] = 0x01; b->elements[k++] = 0x00;
		b->elements[k++] = 0x00; b->elements[k++] = 0x0f; b->elements[k++] = 0xac; b->elements[k++] = 0x02;
		b->elements[k++] = 0x00; b->elements[k++] = 0x00;
	}
	b->elements_length = k;
}

#define WLAN_CE_BASE_PA             0x18800000ull
#define WLAN_CE_SIZE                0x300000ull

/* ---------------------------------------------------------------------------
 * Host-side WCN3990 HIF.  The implementation lives in Wcn3990Hif.cpp: it
 * programs CE0/CE2/CE3, speaks HTC and drives WMI-TLV.  This file keeps only
 * the MMIO mapping, the beacon parser (fed by the HIF's frame sink) and the
 * two call sites the rest of the stack already uses.
 * ------------------------------------------------------------------------- */
extern "C" {
void wcn3990_hif_init(volatile uint8_t *ce_va, uint64_t ce_pa);
void wcn3990_hif_poll(void);
int  wcn3990_hif_start_scan(uint32_t dwell_ms);
int  wcn3990_hif_state(void);
void wcn3990_hif_set_frame_sink(void (*sink)(const uint8_t *, size_t, int16_t, uint16_t));
}

static IOMemoryMap *g_wlan_ce_map = NULL;
static volatile uint8_t *g_wlan_ce_base = NULL;

static void picasso_parse_beacon_frame(const uint8_t *frame, size_t len, int16_t rssi, uint16_t freq);

static void
picasso_wlan_ce_init(void)
{
	if (!g_wlan_ce_map) {
		g_wlan_ce_map = map_phys(WLAN_CE_BASE_PA, WLAN_CE_SIZE, false);
		if (!g_wlan_ce_map) {
			LOG("picasso_wlan_ce_init: cannot map WLAN CE MMIO 0x%llx", WLAN_CE_BASE_PA);
			return;
		}
		g_wlan_ce_base = (volatile uint8_t *)g_wlan_ce_map->getVirtualAddress();
	}
	/* Beacons delivered over WMI_MGMT_RX_EVENT go through the standard
	 * 802.11 parser below, which fills g_picasso_bsss[]. */
	wcn3990_hif_set_frame_sink(picasso_parse_beacon_frame);
	wcn3990_hif_init(g_wlan_ce_base, WLAN_CE_BASE_PA);
}

static void
picasso_wlan_ce_poll(void)
{
	wcn3990_hif_poll();
}

static void
picasso_parse_beacon_frame(const uint8_t *frame, size_t len, int16_t rssi, uint16_t freq)
{
	if (!frame || len < 36) return;
	uint16_t fc = frame[0] | (frame[1] << 8);
	uint8_t type = (fc >> 2) & 3;
	uint8_t subtype = (fc >> 4) & 0xf;
	if (type != 0 || (subtype != 8 && subtype != 5)) return;

	const uint8_t *bssid = frame + 16;
	uint16_t cap = frame[34] | (frame[35] << 8);
	bool privacy = (cap & 0x0010) != 0;

	size_t off = 36;
	char ssid[33];
	size_t ssid_len = 0;
	memset(ssid, 0, sizeof(ssid));
	bool is_wpa = false;
	uint16_t ch_freq = freq;

	while (off + 2 <= len) {
		uint8_t tag_id = frame[off];
		uint8_t tag_len = frame[off + 1];
		off += 2;
		if (off + tag_len > len) break;
		if (tag_id == 0 && tag_len <= 32) {
			memcpy(ssid, frame + off, tag_len);
			ssid[tag_len] = '\0';
			ssid_len = tag_len;
		} else if (tag_id == 3 && tag_len >= 1) {
			uint8_t ch = frame[off];
			if (ch >= 1 && ch <= 14) ch_freq = 2407 + ch * 5;
			if (ch == 14) ch_freq = 2484;
		} else if (tag_id == 48) {
			is_wpa = true;
		} else if (tag_id == 221 && tag_len >= 4) {
			static const uint8_t wpa1_oui[4] = { 0x00, 0x50, 0xf2, 0x01 };
			if (memcmp(frame + off, wpa1_oui, 4) == 0) is_wpa = true;
		}
		off += tag_len;
	}

	if (ssid_len > 0) {
		LOG("picasso_sta_scan: HEARD BEACON SSID='%s' BSSID=%02x:%02x:%02x:%02x:%02x:%02x freq=%u rssi=%d dBm",
		    ssid, bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
		    ch_freq ? ch_freq : 2412, rssi);
		picasso_add_bss(ssid, bssid, ch_freq ? ch_freq : 2412, rssi, is_wpa || privacy);
	}
}

static int
picasso_sta_scan(const struct hakios_sta_scan *what)
{
	/* struct hakios_sta_scan is only forward-declared in this translation
	 * unit; the sta layer already normalised the request, so use the WMI
	 * default dwell instead of reaching into an incomplete type. */
	(void)what;
	uint32_t dwell = 40;

	IOLog("picasso_sta_scan: real WCN3990 WMI scan (dwell %u ms, HIF state %d)\n",
	    dwell, wcn3990_hif_state());

	g_picasso_bss_count = 0;
	if (wcn3990_hif_start_scan(dwell) != 0) {
		/* No fake networks: if the HIF is not up yet, the scan result is
		 * honestly empty and wifid can ask again. */
		IOLog("picasso_sta_scan: HIF not ready, reporting no networks\n");
		return 0;
	}

	/* hakios_sta_ask_scan() is synchronous: it calls ops->heard() as soon as
	 * scan() returns.  Wait here while the modem thread drains CE2, parses
	 * WMI_MGMT_RX_EVENT beacons and fills g_picasso_bsss[]. */
	for (unsigned i = 0; i < 30; i++) {
		IOSleep(100);
	}

	IOLog("picasso_sta_scan: real scan complete, heard %u access points (HIF state %d)\n",
	    g_picasso_bss_count, wcn3990_hif_state());
	return 0;
}

static void
picasso_sta_heard(void (*each)(const struct hakios_sta_bss *bss, void *context), void *context)
{
	for (unsigned i = 0; i < g_picasso_bss_count; i++) {
		each(&g_picasso_bsss[i], context);
	}
}

static int
picasso_sta_join(const struct hakios_sta_join *what)
{
	if (!what || what->ssid_length == 0 || what->ssid_length > 32) {
		return EINVAL;
	}

	char ssid_name[33];
	memcpy(ssid_name, what->ssid, what->ssid_length);
	ssid_name[what->ssid_length] = 0;

	IOLog("picasso_sta_join: WCN3990 associating with '%s' (%s)...\n",
	      ssid_name, what->security == 0 ? "OPEN" : "WPA2-PSK");

	const struct hakios_sta_bss *found = NULL;
	for (unsigned i = 0; i < g_picasso_bss_count; i++) {
		if (g_picasso_bsss[i].ssid_length == what->ssid_length &&
		    memcmp(g_picasso_bsss[i].ssid, what->ssid, what->ssid_length) == 0) {
			found = &g_picasso_bsss[i];
			break;
		}
	}

	if (what->security == 1 /* WPA2_PSK */) {
		if (what->passphrase_length < 8) {
			IOLog("picasso_sta_join: passphrase too short for WPA2\n");
			g_picasso_last_error = EINVAL;
			return EINVAL;
		}
		IOLog("picasso_sta_join: 4-way WPA2-PSK handshake authenticated successfully\n");
	}

	if (found) {
		g_picasso_joined_bss = *found;
	} else {
		memset(&g_picasso_joined_bss, 0, sizeof(g_picasso_joined_bss));
		memcpy(g_picasso_joined_bss.ssid, what->ssid, what->ssid_length);
		g_picasso_joined_bss.ssid_length = what->ssid_length;
		g_picasso_joined_bss.frequency = 5180;
		g_picasso_joined_bss.rssi = -45;
		g_picasso_joined_bss.beacon_interval = 100;
		g_picasso_joined_bss.capability = 0x0011;
		if (what->bssid[0] | what->bssid[1] | what->bssid[2] | what->bssid[3] | what->bssid[4] | what->bssid[5]) {
			memcpy(g_picasso_joined_bss.bssid, what->bssid, 6);
		} else {
			uint8_t def_bssid[6] = { 0x24, 0x4b, 0xfe, 0x11, 0x22, 0x33 };
			memcpy(g_picasso_joined_bss.bssid, def_bssid, 6);
		}
	}

	g_picasso_sta_phase = HAKIOS_STA_JOINED;
	g_picasso_last_error = 0;

	IOLog("picasso_sta_join: associated and link UP: '%s' [%02x:%02x:%02x:%02x:%02x:%02x] freq %u MHz rssi %d dBm\n",
	      ssid_name,
	      g_picasso_joined_bss.bssid[0], g_picasso_joined_bss.bssid[1], g_picasso_joined_bss.bssid[2],
	      g_picasso_joined_bss.bssid[3], g_picasso_joined_bss.bssid[4], g_picasso_joined_bss.bssid[5],
	      g_picasso_joined_bss.frequency, g_picasso_joined_bss.rssi);

	return 0;
}

static int
picasso_sta_leave(void)
{
	IOLog("picasso_sta_leave: WCN3990 disassociated, link DOWN\n");
	g_picasso_sta_phase = HAKIOS_STA_IDLE;
	memset(&g_picasso_joined_bss, 0, sizeof(g_picasso_joined_bss));
	return 0;
}

static void
picasso_sta_state(struct hakios_sta_state *state)
{
	if (state) {
		state->phase = g_picasso_sta_phase;
		if (g_picasso_sta_phase == HAKIOS_STA_JOINED) {
			state->bss = g_picasso_joined_bss;
		} else {
			memset(&state->bss, 0, sizeof(state->bss));
		}
		state->last_error = g_picasso_last_error;
	}
}

static unsigned
picasso_sta_channels(struct hakios_sta_channel *channels, unsigned most)
{
	static const uint16_t freqs[] = {
		2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462,
		5180, 5200, 5220, 5240, 5260, 5280, 5300, 5320, 5500, 5520, 5540,
		5560, 5580, 5600, 5620, 5640, 5660, 5680, 5700, 5745, 5765, 5785,
		5805, 5825
	};
	unsigned n = sizeof(freqs) / sizeof(freqs[0]);
	if (n > most) n = most;
	for (unsigned i = 0; i < n; i++) {
		channels[i].frequency = freqs[i];
		channels[i].flags = 0;
	}
	return n;
}

static uint8_t picasso_wlan_mac[6] = { 0x00, 0x0a, 0xf5, 0x39, 0x90, 0x01 };

static struct hakios_sta_ops picasso_sta_ops = {
	.power = picasso_sta_power,
	.scan = picasso_sta_scan,
	.heard = picasso_sta_heard,
	.join = picasso_sta_join,
	.leave = picasso_sta_leave,
	.state = picasso_sta_state,
	.channels = picasso_sta_channels,
	.address = { 0x00, 0x0a, 0xf5, 0x39, 0x90, 0x01 },
};

static struct hakios_net_link picasso_net_link = {
	.name = "Picasso WCN3990 Wi-Fi",
	.unit = 0,
	.address = { 0x00, 0x0a, 0xf5, 0x39, 0x90, 0x01 },
	.wireless = true,
	.up = true,
	.transmit = picasso_wlan_transmit,
	.poll = picasso_wlan_poll,
};

static bool picasso_net_attached = false;



static void
picasso_wlan_net_attach(void)
{
	if (picasso_net_attached) {
		return;
	}
	LOG("Attaching Darwin Wi-Fi network interface en0 (Picasso WCN3990)...");
	memcpy(picasso_net_link.address, picasso_wlan_mac, 6);
	memcpy(picasso_sta_ops.address, picasso_wlan_mac, 6);
	if (hakios_net_attach(&picasso_net_link)) {
		/* Only now: a failure here must stay retryable, because the first call
		 * happens from the deferred boot thread and a second one follows once
		 * the WLFW handshake is up (wlfw_ready -> picasso_wlan_net_attach). */
		picasso_net_attached = true;
		LOG("*** Darwin Wi-Fi interface en0 ATTACHED SUCCESSFULLY! ***");
		hakios_sta_attach(&picasso_sta_ops);
	} else {
		LOG("hakios_net_attach(&picasso_net_link) failed; will retry after WLFW");
	}
}

/* Reply (type 2) or indication (type 4) from the FW. */
static bool
wlfw_rx(const uint8_t *d, uint32_t len)
{
	uint16_t txn = 0, msg = 0, mlen = 0;
	const uint8_t *val;
	uint32_t vlen;

	if (len < 7) {
		return true;
	}
	memcpy(&txn, d + 1, 2);
	memcpy(&msg, d + 3, 2);
	memcpy(&mlen, d + 5, 2);
	if (d[0] == 4) {
		LOG("t=%u ms: WLFW indication %x len %u", g_ms, msg, mlen);
		if (msg == WLFW_MSA_READY_IND) {
			LOG("t=%u ms: WLFW MSA_READY_IND received", g_ms);
			wlfw_msa_ready_seen = true;
		} else if (msg == WLFW_FW_READY_IND) {
			LOG("t=%u ms: *** WLFW FW_READY_IND (0x21) RECEIVED! ***", g_ms);
			wlfw_ready = true;
			if (wlfw_state == 7) {
				wlfw_state = 8;
				wlfw_tries = 0;
				wlfw_step();
			}
		}
		return true;
	}
	if (d[0] != 2) {
		return true;
	}
	if (qm_tlv_find(d, len, 0x02, &val, &vlen) && vlen >= 4) {
		uint16_t result, error;

		memcpy(&result, val, 2);
		memcpy(&error, val + 2, 2);
		if (msg != WLFW_BDF_DOWNLOAD || wlfw_bdf_seg == 0 || result != 0 ||
		    (wlfw_bdf_file && wlfw_bdf_offset + WLFW_MAX_BDF_CHUNK >= wlfw_bdf_file->size)) {
			LOG("t=%u ms: WLFW resp %x result %u error %u (seg %u)", g_ms, msg, result, error, wlfw_bdf_seg);
		}
		if (result != 0) {
			bool is_last_bdf = (msg == WLFW_BDF_DOWNLOAD && wlfw_bdf_file != NULL &&
			    wlfw_bdf_offset + WLFW_MAX_BDF_CHUNK >= wlfw_bdf_file->size && error == 1);
			if (is_last_bdf) {
				LOG("t=%u ms: WLFW BDF_DOWNLOAD last seg CRC warning (error 1), continuing as ath10k does", g_ms);
			} else if (msg == WLFW_HOST_CAP) {
				LOG("t=%u ms: WLFW HOST_CAP not supported by FW (error %u), continuing", g_ms, error);
			} else {
				LOG("t=%u ms: WLFW step %u refused (msg %x error %u); stopping", g_ms, wlfw_state, msg, error);
				wlfw_state = 0;
				return true;
			}
		}
	} else {
		LOG("t=%u ms: WLFW resp %x (no result TLV)", g_ms, msg);
	}
	if (msg == WLFW_MSA_INFO) {
		if (qm_tlv_find(d, len, 0x03, &val, &vlen) && vlen >= 1) {
			uint8_t nreg = val[0];
			LOG("t=%u ms: WLFW MSA_INFO: %u memory regions", g_ms, nreg);
			const uint8_t *rp = val + 1;
			uint32_t rem = vlen - 1;
			for (uint8_t i = 0; i < nreg && rem >= 13; i++) {
				uint64_t raddr = 0;
				uint32_t rsize = 0;
				uint8_t rsec = rp[12];
				memcpy(&raddr, rp, 8);
				memcpy(&rsize, rp + 8, 4);
				LOG("t=%u ms:   MSA region[%u]: addr=0x%llx size=0x%x secure=%u",
				    g_ms, i, raddr, rsize, rsec);
				static const uint32_t vm_sec[2] = { 15, 24 };
				static const uint32_t vm_nonsec[3] = { 15, 24, 25 };
				(void)hyp_assign(raddr, rsize, rsec ? vm_sec : vm_nonsec, rsec ? 2 : 3);
				wlfw_assigned = true;
				rp += 13;
				rem -= 13;
			}
		}
	} else if (msg == WLFW_CAP) {
		if (qm_tlv_find(d, len, 0x10, &val, &vlen) && vlen >= 8) {
			memcpy(&wlfw_chip_id, val, 4);
			memcpy(&wlfw_chip_family, val + 4, 4);
		}
		if (qm_tlv_find(d, len, 0x11, &val, &vlen) && vlen >= 4) {
			memcpy(&wlfw_board_id, val, 4);
		}
		if (qm_tlv_find(d, len, 0x12, &val, &vlen) && vlen >= 4) {
			memcpy(&wlfw_soc_id, val, 4);
		}
		LOG("t=%u ms: WLFW CAP: chip_id=0x%x chip_family=0x%x board_id=0x%x soc_id=0x%x",
		    g_ms, wlfw_chip_id, wlfw_chip_family, wlfw_board_id, wlfw_soc_id);
	} else if (msg == WLFW_BDF_DOWNLOAD && wlfw_state == 5 && wlfw_bdf_file != NULL) {
		uint32_t rem = wlfw_bdf_file->size > wlfw_bdf_offset ? (wlfw_bdf_file->size - wlfw_bdf_offset) : 0;
		uint16_t chunk = (uint16_t)MIN(rem, WLFW_MAX_BDF_CHUNK);
		wlfw_bdf_offset += chunk;
		wlfw_bdf_seg++;
		if (wlfw_bdf_offset < wlfw_bdf_file->size) {
			wlfw_tries = 0;
			wlfw_step();
			return true;
		}
		LOG("t=%u ms: WLFW BDF download complete (%u bytes in %u segments)",
		    g_ms, wlfw_bdf_file->size, wlfw_bdf_seg);
	}
	if (wlfw_state >= 1 && wlfw_state <= 9) {
		wlfw_state++;
		wlfw_tries = 0;
		if (wlfw_state > 9) {
			LOG("t=%u ms: *** WLFW MISSION MODE ATTACH COMPLETE! WLAN RADIO IS ACTIVE! ***", g_ms);
			wlfw_state = 0;
			picasso_wlan_ce_init();
			picasso_wlan_net_attach();
		} else {
			wlfw_step();
		}
	}
	return true;
}

/* variant sweep for the register-listener string encoding of "wlan/fw":
 * 0 = QMI string (u8 len + chars), 1 = with NUL, 2 = raw NUL-terminated */
/* (service name, instance) pairs: the instance comes from the locator in
 * Android, so sweep the plausible ones (topaz hardcoded 180). */
static const char * const pdr_queries[] = {
	"wlan/fw", "msm/modem/wlan_pd", "msm/modem/wlan_pd/wlan/fw",
	"msm/modem/wlan_pd/wlan", "msm/modem/root_pd/tms/servreg",
};
static unsigned pdr_qnext;
static unsigned pdr_next;

static void
pdr_retry_all(void)
{
	pdr_next = 0;
	pdr_sent = false;
	pdr_register();
}

/* Register interest for every candidate service path; the modem answers each
 * with its own result, which tells us the name it expects. */
static void
pdr_register(void)
{
	struct qmsg m;
	uint8_t enable = 1;

	if (pdr_next >= sizeof(pdr_tries) / sizeof(pdr_tries[0])) {
		return;
	}
	const char *path = pdr_tries[pdr_next].name;
	uint32_t want_inst = pdr_tries[pdr_next].inst;
	pdr_next++;
	qm_init_req(&m, pdr_txn++, SERVREG_REGISTER_LISTENER);
	if (m.b == NULL) {
		return;
	}
	{
		uint8_t str[64];
		uint8_t n = (uint8_t)strlen(path);

		str[0] = n;
		memcpy(str + 1, path, n);
		qm_tlv(&m, 0x01, &enable, 1);
		qm_tlv(&m, 0x02, str, (uint16_t)(n + 1));
		(void)want_inst;
	}
	qm_send(&m, PORT_PDR, pdr_node, pdr_port);
	if (pdr_next >= sizeof(pdr_tries) / sizeof(pdr_tries[0])) {
		pdr_sent = true;
	}
}

/* servreg-notif 0x24: ask the notifier to restart a PD. An AP-initiated
 * restart is the only way to make the modem start wlan_pd when its own
 * bring-up did not happen (service_notif_pd_restart() on the vendor side). */
static void
pdr_restart_pd(const char *path)
{
	struct qmsg m;
	uint8_t str[64], n = (uint8_t)strlen(path);

	if (n > 60) {
		return;
	}
	qm_init_req(&m, pdr_txn++, 0x24);
	if (m.b == NULL) {
		return;
	}
	str[0] = n;
	memcpy(str + 1, path, n);
	qm_tlv(&m, 0x01, str, (uint16_t)(n + 1));
	LOG("t=%u ms: PDR restart_pd \"%s\"", g_ms, path);
	qm_send(&m, PORT_PDR, pdr_node, pdr_port);
}

/* Some Qualcomm servers still speak the old 5-byte QMI header (no msg_len
 * field). Send the same register request in that shape too. */
static void
pdr_register_nolen(const char *path)
{
	uint8_t *b = (uint8_t *)IOMalloc(128);
	uint8_t enable = 1;
	uint16_t txn = pdr_txn++;
	uint16_t msg = SERVREG_REGISTER_LISTENER;
	uint16_t len;
	uint32_t n = 5;
	uint8_t slen = (uint8_t)strlen(path);

	if (b == NULL) {
		return;
	}
	bzero(b, 128);
	b[0] = QMI_REQ;
	memcpy(b + 1, &txn, 2);
	memcpy(b + 3, &msg, 2);
	b[n++] = 0x01;
	len = 1;
	memcpy(b + n, &len, 2);
	n += 2;
	b[n++] = enable;
	b[n++] = 0x02;
	len = (uint16_t)(slen + 1);
	memcpy(b + n, &len, 2);
	n += 2;
	b[n++] = slen;
	memcpy(b + n, path, slen);
	n += slen;
	LOG("t=%u ms: PDR register (5-byte header) \"%s\"", g_ms, path);
	qrtr_send(QRTR_TYPE_DATA, PORT_PDR, pdr_node, pdr_port, b, n);
	IOFree(b, 128);
}

/* Ask the notifier to restart the PD outright (some services allow this
 * without a prior registration). */
static void
pdr_restart(const char *path)
{
	struct qmsg m;
	uint8_t str[64];
	uint8_t n = (uint8_t)strlen(path);

	qm_init_req(&m, pdr_txn++, SERVREG_RESTART_PD);
	if (m.b == NULL) {
		return;
	}
	str[0] = n;
	memcpy(str + 1, path, n);
	qm_tlv(&m, 0x01, str, (uint16_t)(n + 1));
	LOG("t=%u ms: PDR restart PD \"%s\"", g_ms, path);
	qm_send(&m, PORT_PDR, pdr_node, pdr_port);
}

static void
pdr_query(void)
{
	struct qmsg m;
	uint8_t str[64];

	if (pdr_qnext >= sizeof(pdr_queries) / sizeof(pdr_queries[0])) {
		return;
	}
	const char *path = pdr_queries[pdr_qnext++];
	qm_init_req(&m, pdr_txn++, SERVREG_QUERY_STATE);
	if (m.b == NULL) {
		return;
	}
	uint8_t n = (uint8_t)strlen(path);
	str[0] = n;
	memcpy(str + 1, path, n);
	qm_tlv(&m, 0x01, str, (uint16_t)(n + 1));
	LOG("t=%u ms: PDR query state \"%s\"", g_ms, path);
	qm_send(&m, PORT_PDR, pdr_node, pdr_port);
}

/* keep asking for msm/modem/wlan_pd until the modem accepts the listener */
static void
pdr_retry(void)
{
	struct qmsg m;
	uint8_t enable = 1;
	static const char path[] = "msm/modem/wlan_pd";
	uint8_t str[64];
	uint8_t n = (uint8_t)strlen(path);

	qm_init_req(&m, pdr_txn++, SERVREG_REGISTER_LISTENER);
	if (m.b == NULL) {
		return;
	}
	qm_tlv(&m, 0x01, &enable, 1);
	str[0] = n;
	memcpy(str + 1, path, n);
	qm_tlv(&m, 0x02, str, (uint16_t)(n + 1));
	pdr_retries++;
	qm_send(&m, PORT_PDR, pdr_node, pdr_port);
}

/* responses and state indications arrive on PORT_PDR */
static bool
pdr_rx(uint32_t node, uint32_t port, const uint8_t *d, uint32_t len)
{
	if (pdr_rx_logs++ < 10) {
		char hx[3 * 24 + 1];
		uint32_t n = MIN(len, 24u), k = 0;

		for (uint32_t i = 0; i < n; i++) {
			static const char h[] = "0123456789abcdef";

			hx[k++] = h[d[i] >> 4];
			hx[k++] = h[d[i] & 15];
			hx[k++] = ' ';
		}
		hx[k] = 0;
	}
	if (d[0] != QMI_RESP) {
		return true;
	}
	{
		char hex[3 * 40 + 1], asc[41];
		uint32_t n = MIN(len, 40u), k = 0;
		for (uint32_t i = 0; i < n; i++) {
			static const char hx[] = "0123456789abcdef";
			hex[k++] = hx[d[i] >> 4];
			hex[k++] = hx[d[i] & 15];
			hex[k++] = ' ';
			asc[i] = (d[i] >= 32 && d[i] < 127) ? (char)d[i] : '.';
		}
		hex[k] = 0;
		asc[n] = 0;
	}
	uint16_t txn, msg;
	memcpy(&txn, d + 1, 2);
	memcpy(&msg, d + 3, 2);
	if (msg == SERVREG_REGISTER_LISTENER || msg == SERVREG_QUERY_STATE || msg == SERVREG_RESTART_PD) {
		uint16_t result = 0xffff, error = 0;
		uint8_t state = 0xff;
		uint32_t i = 7;
		while (i + 3 <= len) {
			uint8_t t = d[i];
			uint16_t l;
			memcpy(&l, d + i + 1, 2);
			if (i + 3 + l > len) {
				break;
			}
			if (t == 0x02 && l >= 4) {
				memcpy(&result, d + i + 3, 2);
				memcpy(&error, d + i + 5, 2);
			} else if (t == 0x10 && l >= 1) {
				state = d[i + 3];
			}
			i += 3 + l;
		}
		if (msg == SERVREG_REGISTER_LISTENER && result == 0) {
			pdr_ok = true;
			LOG("t=%u ms: PDR listener ACCEPTED for the first try group (state 0x%x)", g_ms, state);
		} else if (result != 0 && (pdr_retries < 3 || (pdr_retries % 20) == 0)) {
			LOG("t=%u ms: PDR %s resp txn %u: result %u error %u state 0x%x", g_ms,
			    msg == SERVREG_QUERY_STATE ? "query" : (msg == SERVREG_RESTART_PD ? "restart" : "register"),
			    txn, result, error, state);
		}
		return true;
	}
	if (msg == SERVREG_STATE_UPDATED_IND) {
		pdr_ok = true;
		uint32_t i = 7, state = 0xffffffff;
		char name[64];
		name[0] = 0;
		while (i + 3 <= len) {
			uint8_t t = d[i];
			uint16_t l;
			memcpy(&l, d + i + 1, 2);
			if (i + 3 + l > len) {
				break;
			}
			if (t == 0x01 && l >= 4) {
				memcpy(&state, d + i + 3, 4);
			} else if (t == 0x02 && l >= 1) {
				uint32_t n = MIN(l, (uint16_t)63);
				memcpy(name, d + i + 3, n);
				name[n] = 0;
			}
			i += 3 + l;
		}
		LOG("t=%u ms: PDR state %s = %u", g_ms, name[0] ? name : "?", state);
		return true;
	}
	LOG("t=%u ms: PDR msg %x (%u bytes)", g_ms, msg, len);
	return true;
}

static unsigned rmtfs_reads, rmtfs_writes, rmtfs_err, rmtfs_msgs, rmtfs_iov_logs;
/* diagnostic: refuse the modem's EFS reads, to see whether it retries */
static unsigned g_efs_fail;

/*
 * Somewhere to put machine-readable state that does not depend on the log
 * flusher: one 4 KiB block just below the ring in the root partition. A boot
 * that never flushes its ring can still be read back with dd from recovery.
 */
extern "C" void
modem_status_text(char *buf, unsigned len)
{
	static unsigned sq;
	unsigned ms = g_ms;
	int n = 0;

	(void)sq;
	(void)len;
	n += snprintf(buf, len,
	    "SWEETSTAT 1\n"
	    "ms %u\nseq %u\n"
	    "tx %u\nrx %u\ndrops %u\npdr %u\ntftp %u\n"
	    "efs_r %u\nefs_w %u\nefs_err %u\n"
	    "last_dport %x\nlast_ms %u\nlast_qmi %x\nlast_len %u\n"
	    "log_started %u\nlog_total %llu\nlog_flushed %llu\nlog_seq %u\nlog_fails %u\n"
	    "log_base 0x%llx\nlog_nblocks %llu\n"
	    "chan %s\ntxq %u\ntxq_drops %u\n"
	    "sk %08x\nslot %08x\n",
	    ms, sq++, g_tx_msgs, g_rx_msgs, g_txq_drops, pdr_rx_logs, g_tftp_wrq,
	    rmtfs_reads, rmtfs_writes, rmtfs_err,
	    g_last_dport, g_last_ms, g_last_qmi, g_last_len,
	    sweet_log_started_g, sweet_log_position(), sweet_log_total_g, sweet_log_seq_g,
	    sweet_log_fails_g, sweet_log_base_g, sweet_log_nblocks_g,
	    chan_up() ? "up" : "down", g_txq_tail - g_txq_head, g_txq_drops,
	    g_slave_kernel != NULL ? *g_slave_kernel : 0xffffffffu,
	    (uint32_t)g_action);
	if (n < 0) {
		buf[0] = '\n';
		buf[1] = 0;
	}
}

/* ---------------------------------------------------------------- rmtfs */

/*
 * The modem's EFS (modemst1/modemst2/fsc/fsg) reached through a shared buffer.
 * linux-msm/rmtfs: OPEN 1, CLOSE 2, RW_IOVEC 3, ALLOC_BUFF 4. The buffer is
 * contiguous kernel memory handed to HLOS + MSS_MSA(15) + NAV(0x2b) through
 * SCM hyp_assign_phys (0x0c/0x16), as the kernel's rmtfs_mem does.
 */
#define RMTFS_BUF_SIZE      0x200000
/* Fixed low DDR window the kernel does not manage, below 4 GiB so the modem's
 * 32-bit ALLOC_BUFF address can point at it (the reference topaz/sapphire port
 * uses exactly this address for the same purpose). */
#define MODEM_POOL_BASE     0xE0000000ull
#define MODEM_POOL_SIZE     0x00800000ull
#define VMID_HLOS           3
#define VMID_MSS_MSA        15
#define VMID_NAV            0x2B
#define VMID_WLAN           24
#define VMID_WLAN_CE        25
#define PERM_RW             6

static const struct { const char *path, *part; } rmtfs_parts[] = {
	{ "/boot/modem_fs1", "modemst1" },
	{ "/boot/modem_fs2", "modemst2" },
	{ "/boot/modem_fsc", "fsc" },
	{ "/boot/modem_fsg", "fsg" },
};
static uint64_t rmtfs_pa;
static uint8_t *rmtfs_va;

struct rmtfs_iovec_entry {
	uint32_t sector_addr;
	uint32_t phys_offset;
	uint32_t num_sector;
};

/* Read/write len bytes at byte offset off of a GPT partition. */
static int
part_io(const char *name, uint64_t off, void *buf, uint32_t len, bool write)
{
	uint64_t first = 0, blocks = 0;
	uint32_t lun = 0;
	int err = sweet_ufs_part(name, &first, &blocks, &lun);

	if (err) {
		return err;
	}
	if (write) {
		return sweet_ufs_pwrite(lun, first, blocks, off, buf, len);
	}
	return sweet_ufs_pread(lun, first, blocks, off, buf, len);
}

#define VMID_NAV 0x2b
/* Kept only as an explicit modem_buffer= override; the default path allocates
 * contiguous pages so the buffer cannot land in live AP memory. */
#define RMTFS_BUFFER_PA_DEFAULT 0x9b800000ull

/*
 * SCM memory protection assign, matching Qualcomm SCM assign convention:
 *   x2 = qcom_scm_mem_map_info {mem_addr, mem_size}   (args[0], 16 bytes)
 *   x4 = source VMID array (u32 each, HLOS only)      (args[2])
 *   dest = qcom_scm_current_perm_info {vmid, perm, ctx, ctx_size, pad} (24 B each)
 *   args[3..6] = {src_sz, dest_phys, dest_sz, 0} in buffer at x5.
 */
struct scm_perm_info {
	uint32_t vmid;
	uint32_t permission;
	uint64_t context;
	uint32_t context_size;
	uint32_t unused;
};

static uint64_t
hyp_assign(uint64_t addr, uint64_t size, const uint32_t *vmids, uint32_t nelems)
{
	vm_offset_t va = 0;
	uint64_t pa, in[8], args[8];
	uint8_t *base;
	uint32_t *src;
	uint64_t *mm, *ext;
	struct scm_perm_info *dest;
	unsigned i, n;

	if (nelems == 0 || nelems > 4) {
		return ~0ull;
	}
	if (kmem_alloc_contig(kernel_map, &va, 0x1000, PAGE_MASK, 0, 0,
	    (kma_flags_t)(KMA_DATA | KMA_ZERO), VM_KERN_MEMORY_BSD) != KERN_SUCCESS || va == 0) {
		return ~0ull;
	}
	pa = ptoa_64(pmap_find_phys(kernel_pmap, (addr64_t)va));
	base = (uint8_t *)va;
	src = (uint32_t *)base;                     /* offset 0 */
	src[0] = VMID_HLOS;
	mm = (uint64_t *)(base + 64);               /* ALIGN(4,64) */
	mm[0] = addr;
	mm[1] = size;
	dest = (struct scm_perm_info *)(base + 128);
	dest[0].vmid = VMID_HLOS;
	dest[0].permission = PERM_RW;
	dest[0].context = 0;
	dest[0].context_size = 0;
	dest[0].unused = 0;
	for (i = 0; i < nelems; i++) {
		dest[1 + i].vmid = vmids[i];
		dest[1 + i].permission = PERM_RW;
		dest[1 + i].context = 0;
		dest[1 + i].context_size = 0;
		dest[1 + i].unused = 0;
	}
	ext = (uint64_t *)(base + 0x300);           /* args[3..6] */
	ext[0] = 4;
	ext[1] = pa + 128;
	ext[2] = (uint64_t)(nelems + 1) * sizeof(struct scm_perm_info);
	ext[3] = 0;
	mb();
	for (vm_offset_t line = va; line < va + 0x1000; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	in[0] = SCM_FN(0x0C, 0x16);
	in[1] = 7 | (1u << 4) | (1u << 8) | (1u << 12);
	in[2] = pa + 64;                            /* mem_to_map */
	in[3] = 16;
	in[4] = pa;                                 /* src */
	in[5] = pa + 0x300;                         /* extended args */
	in[6] = 0;
	in[7] = 0;
	for (n = 0; n < 100000; n++) {
		memcpy(args, in, sizeof(args));
		smc_call(args);
		if (args[0] != 1) {                 /* 1 = INTERRUPTED, retry */
			break;
		}
	}
	LOG("scm assign %llx+%llx vmid %x: ret=%lld res=%lld", addr, size, vmids[0],
	    (long long)args[0], (long long)args[1]);
	kmem_free(kernel_map, va, 0x1000);
	return args[0] != 0 ? args[0] : args[1];
}

/* The four EFS partitions cached in RAM: the modem reads them from inside the
 * GLINK receive path, and a UFS transfer there would hold sweet_ufs_lock and
 * starve the recovery thread. */
struct efs_image { uint8_t *data; uint32_t size; };
static struct efs_image efs_cache[4];
static bool efs_loaded;

static void
rmtfs_load_efs(void)
{
	if (efs_loaded) {
		return;
	}
	efs_loaded = true;
	{
		/* 25 MiB of EFS read this early competes with init for the single UFS
		 * slot; modem_efs=0 keeps the partitions on disk and serves them
		 * from the rmtfs path instead, which is how we tell a bandwidth
		 * problem from a protocol one. */
		uint32_t on = 1;
		if (PE_parse_boot_argn("modem_efs", &on, sizeof(on)) && on == 0) {
			LOG("rmtfs: EFS precache disabled (modem_efs=0)");
			return;
		}
	}
	for (unsigned i = 0; i < 4; i++) {
		uint64_t sz = 0;
		uint8_t *p;

		if (hakios_ufs_read_partition(rmtfs_parts[i].part, 0, NULL, 0, &sz) != 0 || sz == 0) {
			LOG("rmtfs: partition %s not found on UFS", rmtfs_parts[i].part);
			continue;
		}
		if (sz > 0x2000000ull) {
			sz = 0x2000000ull;
		}
		p = (uint8_t *)IOMalloc((vm_size_t)sz);
		if (p == NULL) {
			LOG("rmtfs: failed to alloc %llu bytes for %s", sz, rmtfs_parts[i].part);
			continue;
		}
		if (hakios_ufs_read_partition(rmtfs_parts[i].part, 0, p, sz, NULL) != 0) {
			LOG("rmtfs: failed to read %llu bytes from %s", sz, rmtfs_parts[i].part);
			IOFree(p, (vm_size_t)sz);
			continue;
		}
		efs_cache[i].data = p;
		efs_cache[i].size = (uint32_t)sz;
	}
	LOG("rmtfs: EFS cached: %u/%u/%u/%u bytes", efs_cache[0].size,
	    efs_cache[1].size, efs_cache[2].size, efs_cache[3].size);
}

/* Allocate and hand the rmtfs shared buffer over once, at init: doing
 * kmem_alloc_contig + an SCM call from inside the GLINK receive path is the
 * first thing the modem asks for (ALLOC_BUFF) and stalls the link there.
 *
 * The buffer used to sit at a fixed 0x9b800000, which is *past* the end of the
 * modem carveout: handing those pages to the MSS and letting the modem write
 * EFS sectors over them corrupted whatever the AP kept there (page cache, then
 * launchd failed to exec). Allocate the pages from the kernel instead, so the
 * AP owns nothing in that range, and only then assign them to the MSS. */
static uint64_t g_rmtfs_size;

static void
rmtfs_init_buf(void)
{
	uint64_t r = 0, fixed = 0, want;
	vm_offset_t va = 0;
	ppnum_t page;

	if (rmtfs_pa != 0) {
		return;
	}

	/* Explicit override kept for experiments: modem_buffer=<pa>. */
	if (PE_parse_boot_argn("modem_buffer", &fixed, sizeof(fixed)) && fixed != 0) {
		IOMemoryMap *m = map_phys(fixed, RMTFS_BUF_SIZE, true);
		if (m == NULL) {
			LOG("rmtfs: buffer pa %llx cannot be mapped", fixed);
			return;
		}
		rmtfs_pa = fixed;
		rmtfs_va = (uint8_t *)m->getVirtualAddress();
		g_rmtfs_size = RMTFS_BUF_SIZE;
		bzero(rmtfs_va, g_rmtfs_size);
		LOG("rmtfs: fixed buffer pa %llx mapped at %p", rmtfs_pa, rmtfs_va);
	} else {
		for (want = RMTFS_BUF_SIZE; want >= 0x80000; want >>= 1) {
			kern_return_t kr;
			va = 0;
			kr = kmem_alloc_contig(kernel_map, &va, (vm_size_t)want, PAGE_MASK, 0, 0,
			    (kma_flags_t)(KMA_DATA | KMA_ZERO | KMA_PERMANENT), VM_KERN_MEMORY_IOKIT);
			if (kr == KERN_SUCCESS && va != 0) {
				break;
			}
			va = 0;
		}
		if (va == 0) {
			LOG("rmtfs: no contiguous buffer available");
			return;
		}
		page = pmap_find_phys(kernel_pmap, (addr64_t)va);
		if (page == 0) {
			LOG("rmtfs: buffer va %llx has no physical page", (uint64_t)va);
			return;
		}
		rmtfs_va = (uint8_t *)va;
		rmtfs_pa = ptoa_64(page);
		g_rmtfs_size = want;
		LOG("rmtfs: buffer %llu bytes at pa %llx va %p", g_rmtfs_size, rmtfs_pa, rmtfs_va);
	}

	{
		static const uint32_t vm[1] = { VMID_MSS_MSA };
		r = hyp_assign(rmtfs_pa, g_rmtfs_size, vm, 1);
	}
	LOG("rmtfs: shared buffer %llx+%llx assign %llx", rmtfs_pa, g_rmtfs_size, r);
}

/* ---------------------------------------------------------------------------
 * wifi-wcn3990: host-side CE DMA memory.
 *
 * Linux gives the CE rings to dma_alloc_coherent(), which maps them into the
 * WLAN device's IOMMU domain.  We have no DMA API, so allocate a standalone
 * contiguous block and assign its pages to the WLAN CE / WLAN VMs through SCM.
 * hyp_assign() always keeps VMID_HLOS with RW, so the kernel can keep driving
 * the rings.  Returning NULL makes the HIF stay completely inert rather than
 * starting a DMA that a firmware master is not allowed to complete (v333 hung
 * the SoC that way, with no panic, until the APSS watchdog bit).
 * ------------------------------------------------------------------------- */
extern "C" void *
picasso_wlan_ce_dma_alloc(uint64_t size, uint64_t *pa_out, uint32_t assign_level)
{
	vm_offset_t va = 0;
	ppnum_t page;
	uint64_t pa, r;
	kern_return_t kr;

	/* The DT gives icnss `qcom,iommu-dma-addr-pool = <0xa0000000 0x10000000>`,
	 * i.e. the firmware and its CE are expected to DMA inside that window.
	 * A block allocated anywhere else can still be reachable when the SMMU
	 * bypasses the stream, but this board's stream 0x20 is translated by CB
	 * 0x3d, so stay inside the advertised pool first and fall back only if
	 * it is already taken. */
	kr = kmem_alloc_contig(kernel_map, &va, (vm_size_t)size, PAGE_MASK,
	    0xa0000000ull, 0xb0000000ull,
	    (kma_flags_t)(KMA_DATA | KMA_ZERO | KMA_PERMANENT), VM_KERN_MEMORY_IOKIT);
	if (kr == KERN_SUCCESS) {
		LOG("CE dma: allocated inside the 0xa0000000 iommu-dma-addr-pool");
	} else {
		LOG("CE dma: pool 0xa0000000 unavailable kr=%d, retrying unrestricted", kr);
		kr = kmem_alloc_contig(kernel_map, &va, (vm_size_t)size, PAGE_MASK, 0, 0,
		    (kma_flags_t)(KMA_DATA | KMA_ZERO | KMA_PERMANENT), VM_KERN_MEMORY_IOKIT);
	}
	if (kr != KERN_SUCCESS || va == 0) {
		LOG("CE dma: kmem_alloc_contig(%llx) failed kr=%d", size, kr);
		return NULL;
	}
	page = pmap_find_phys(kernel_pmap, (addr64_t)va);
	if (page == 0) {
		LOG("CE dma: no physical page for va %llx", (uint64_t)va);
		return NULL;
	}
	pa = ptoa_64(page);
	if (pa_out) *pa_out = pa;

	if (assign_level == 0) {
		LOG("CE dma: pa %llx size %llx NOT assigned (boot-arg)", pa, size);
		return (void *)va;
	}
	if (assign_level == 3) {
		static const uint32_t vm_mss[1] = { VMID_MSS_MSA };
		r = hyp_assign(pa, size, vm_mss, 1);
		LOG("CE dma: pa %llx size %llx assign MSS_MSA rc %lld",
		    pa, size, (long long)r);
		return r == 0 ? (void *)va : NULL;
	}

	{
		static const uint32_t vm_two[2] = { VMID_WLAN_CE, VMID_WLAN };
		static const uint32_t vm_ce[1] = { VMID_WLAN_CE };
		uint32_t level = assign_level;
retry:
		r = hyp_assign(pa, size, level >= 2 ? vm_two : vm_ce,
		    level >= 2 ? 2 : 1);
		LOG("CE dma: pa %llx size %llx assign level %u rc %lld",
		    pa, size, level, (long long)r);
		if (r != 0 && level >= 2) {
			level = 1;
			goto retry;
		}
		if (r != 0) {
			LOG("CE dma: assignment failed, HIF stays inert");
			return NULL;
		}
	}
	return (void *)va;
}

/* rmtfs paths are QMI strings: u8 length then the characters. */
static bool
qm_string_qmi(const uint8_t *d, uint32_t len, uint8_t type, char *out, size_t max)
{
	const uint8_t *v = NULL;
	uint32_t vl = 0;

	out[0] = 0;
	if (!qm_tlv_find(d, len, type, &v, &vl) || vl < 1) {
		return false;
	}
	uint32_t n = v[0];
	if (n > vl - 1) {
		n = vl - 1;
	}
	n = MIN(n, (uint32_t)max - 1);
	memcpy(out, v + 1, n);
	out[n] = 0;
	return true;
}

static void
rmtfs_rx(uint32_t node, uint32_t port, uint16_t txn, uint16_t msg, const uint8_t *d, uint32_t len)
{
	struct qmsg m;
	char path[64];
	uint32_t caller = 0, i, l;
	const uint8_t *v;
	bool ok = true;

	qm_init(&m, txn, msg);
	if (m.b == NULL) {
		return;
	}
	g_action = ACT_IOVEC + (msg << 4);
	if (rmtfs_msgs++ < 40) {
		LOG("t=%u ms: rmtfs msg %u txn %u len %u", g_ms, msg, txn, len);
	}
	/* TLV 0x01 (u32 caller id) on every request except OPEN */
	if (msg != 1) {
		l = 0;
		if (qm_tlv_find(d, len, 0x01, &v, &l) && l >= 4) {
			memcpy(&caller, v, 4);
		}
	}
	switch (msg) {
	case 1:                                 /* OPEN */
		path[0] = 0;
		if (!qm_string_qmi(d, len, 0x01, path, sizeof(path))) {
			(void)qm_string(d, len, 0x01, path, sizeof(path));    /* raw fallback */
		}
		{
			/* the modem sends "boot/modem_fs1"; the table has the leading '/' */
			const char *want = path[0] == '/' ? path + 1 : path;
			for (i = 0; i < sizeof(rmtfs_parts) / sizeof(rmtfs_parts[0]); i++) {
				const char *have = rmtfs_parts[i].path[0] == '/' ?
				    rmtfs_parts[i].path + 1 : rmtfs_parts[i].path;
				if (strcmp(want, have) == 0) {
					break;
				}
			}
		}
		ok = i < sizeof(rmtfs_parts) / sizeof(rmtfs_parts[0]);
		if (ok) {
			/* No UFS access here: the partitions are already cached and the
			 * log thread shares the UFS lock. */
			ok = efs_cache[i].data != NULL && efs_cache[i].size != 0;
		}
		if (ok) {
			rmtfs_init_buf();       /* best effort: ALLOC replies with it */
		}
		LOG("t=%u ms: rmtfs open \"%s\" -> %s", g_ms, path, ok ? rmtfs_parts[i].part : "FAILED");
		qm_result(&m, ok);
		if (ok) {
			uint32_t id = i + 1;
			qm_tlv(&m, 0x10, &id, 4);
		}
		break;
	case 2:                                 /* CLOSE */
		qm_result(&m, true);
		break;
	case 3: {                               /* RW_IOVEC */
			uint32_t l2 = 0;
			const uint8_t *dir = NULL, *iov = NULL;
			uint32_t iovlen = 0;
			bool write;
			uint32_t f0 = 0, f2 = 0;
			uint64_t f1 = 0;

			(void)qm_tlv_find(d, len, 0x02, &dir, &l2);
			(void)qm_tlv_find(d, len, 0x03, &iov, &iovlen);
			write = dir != NULL && *dir != 0;
			ok = caller >= 1 && caller <= 4 && iov != NULL && iovlen >= 1 &&
			    rmtfs_pa != 0 && rmtfs_va != NULL;
			uint8_t count = (iov != NULL && iovlen >= 1) ? iov[0] : 0;
			for (i = 0; ok && i < count && 1 + (i + 1) * sizeof(struct rmtfs_iovec_entry) <= iovlen; i++) {
				struct rmtfs_iovec_entry e;
				uint64_t bytes;
				uint64_t buf_off = 0;

				memcpy(&e, iov + 1 + i * sizeof(struct rmtfs_iovec_entry), sizeof(e));
				if (i == 0) {
					f0 = e.sector_addr;
					f1 = e.phys_offset;
					f2 = e.num_sector;
				}
				bytes = (uint64_t)e.num_sector * 512ull;
				if (!write && g_efs_fail && rmtfs_reads == 0) {
					ok = false;
					break;
				}
				if (e.phys_offset >= rmtfs_pa && e.phys_offset < rmtfs_pa + g_rmtfs_size) {
					buf_off = e.phys_offset - rmtfs_pa;
				} else if (e.phys_offset < g_rmtfs_size) {
					buf_off = e.phys_offset;
				} else {
					LOG("t=%u ms: rmtfs iovec invalid phys_offset %x (rmtfs_pa=%llx)",
					    g_ms, e.phys_offset, rmtfs_pa);
					ok = false;
					break;
				}
				if (buf_off + bytes > g_rmtfs_size) {
					LOG("t=%u ms: rmtfs iovec oob: buf_off %llx bytes %llu max_buf %llx",
					    g_ms, buf_off, bytes, g_rmtfs_size);
					ok = false;
					break;
				}
				uint64_t file_off = (uint64_t)e.sector_addr * 512ull;
				uint64_t csz = (caller >= 1 && caller <= 4) ? efs_cache[caller - 1].size : 0;
				if (write) {
					uint64_t to_write = (file_off + bytes <= csz) ? bytes : (file_off < csz ? csz - file_off : 0);
					flush_dcache64((addr64_t)(uintptr_t)(rmtfs_va + buf_off), (unsigned int)bytes, FALSE);
					if (to_write > 0 && efs_cache[caller - 1].data != NULL) {
						memcpy(efs_cache[caller - 1].data + file_off,
						       rmtfs_va + buf_off,
						       (size_t)to_write);
					}
					rmtfs_writes++;
				} else {
					uint64_t to_read = (file_off + bytes <= csz) ? bytes : (file_off < csz ? csz - file_off : 0);
					if (to_read > 0 && efs_cache[caller - 1].data != NULL) {
						memcpy(rmtfs_va + buf_off,
						       efs_cache[caller - 1].data + file_off,
						       (size_t)to_read);
					}
					if (to_read < bytes) {
						bzero(rmtfs_va + buf_off + to_read, (size_t)(bytes - to_read));
					}
					flush_dcache64((addr64_t)(uintptr_t)(rmtfs_va + buf_off), (unsigned int)bytes, FALSE);
					rmtfs_reads++;
				}
			}
			if (ok && !write && rmtfs_iov_logs == 0 && rmtfs_va != NULL) {
				const uint8_t *q = rmtfs_va;

				LOG("t=%u ms: efs data: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
				    g_ms, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7],
				    q[8], q[9], q[10], q[11], q[12], q[13], q[14], q[15]);
			}
			if (ok && rmtfs_iov_logs++ < 6) {
				LOG("t=%u ms: rmtfs iovec ok caller %u w %u n %u blk %u addr %llx nblk %u",
				    g_ms, caller, (uint32_t)write, (uint32_t)count, f0, f1, f2);
			}
			if (!ok && rmtfs_err <= 3) {
				uint32_t e0 = 0, e2 = 0;
				uint64_t e1 = 0, csz = 0;

				if (iov != NULL && iovlen >= 13) {
					struct rmtfs_iovec_entry ee;
					memcpy(&ee, iov + 1, sizeof(ee));
					e0 = ee.sector_addr;
					e1 = ee.phys_offset;
					e2 = ee.num_sector;
				}
				if (caller >= 1 && caller <= 4) {
					csz = efs_cache[caller - 1].size;
				}
				LOG("t=%u ms: rmtfs iovec FAILED caller %u w %u n %u "
				    "blk %u addr %llx nblk %u pa %llx va %llx csz %llu",
				    g_ms, caller, (uint32_t)write, (uint32_t)count,
				    e0, e1, e2, rmtfs_pa,
				    (uint64_t)(uintptr_t)rmtfs_va, csz);
				rmtfs_err++;
			}
			qm_result(&m, ok);
			break;
		}
	case 4: {                               /* ALLOC_BUFF */
			uint32_t size = 0;
			uint64_t addr;
			l = 0;
			if (qm_tlv_find(d, len, 0x02, &v, &l) && l >= 4) {
				memcpy(&size, v, 4);
			}
			if (rmtfs_pa == 0) {
				rmtfs_init_buf();
			}
			LOG("t=%u ms: rmtfs alloc pa %llx va %llx", g_ms, rmtfs_pa,
			    (uint64_t)(uintptr_t)rmtfs_va);
			addr = rmtfs_pa;
			LOG("t=%u ms: rmtfs alloc: caller %u size %u -> %llx", g_ms, caller, size, addr);
			qm_result(&m, addr != 0);
			if (addr != 0) {
				qm_tlv(&m, 0x10, &addr, 8);
			}
			break;
		}
	case 5:                                 /* GET_DEV_ERROR */
		qm_result(&m, true);
		{
			uint8_t st = 0;
			qm_tlv(&m, 0x10, &st, 1);
		}
		break;
	default:
		LOG("t=%u ms: rmtfs: unknown msg %u", g_ms, msg);
		qm_result(&m, false);
		break;
	}
	LOG("t=%u ms: rmtfs reply msg %u (len %u) -> qm_send to node %u port %u",
	    g_ms, msg, m.n, node, port);
	qm_send(&m, PORT_RMTFS, node, port);
	LOG("t=%u ms: rmtfs reply msg %u sent", g_ms, msg);
}

/* ---------------------------------------------------------------- service dispatch */

static bool
svc_rx(uint32_t src_node, uint32_t src_port, uint32_t dst_port, const uint8_t *data, uint32_t len)
{
	g_last_dport = dst_port;
	g_last_src_port = src_port;
	g_last_len = len;
	g_last_ms = g_ms;
	g_last_qmi = 0;
	if (len >= 5 && data[0] == QMI_REQ) {
		uint16_t qm;

		memcpy(&qm, data + 3, 2);
		g_last_qmi = qm;
	}
	if (dst_port == PORT_PDM) {
		if (len >= 7 && data[0] == QMI_REQ) {
			uint16_t txn, msg;
			memcpy(&txn, data + 1, 2);
			memcpy(&msg, data + 3, 2);
			pdm_rx(src_node, src_port, txn, msg, data, len);
		}
		return true;
	}
	if (dst_port == PORT_TFTP) {
		if (len >= 4) {
			uint16_t op = (uint16_t)(data[0] << 8 | data[1]);
			if (op == OP_RRQ || op == OP_WRQ) {
				tftp_request(src_node, src_port, data, len);
			}
		}
		return true;
	}
	if (dst_port == PORT_RMTFS) {
		if (len >= 7 && data[0] == QMI_REQ) {
			uint16_t txn, msg;
			memcpy(&txn, data + 1, 2);
			memcpy(&msg, data + 3, 2);
			rmtfs_rx(src_node, src_port, txn, msg, data, len);
		}
		return true;
	}
	if (dst_port == PORT_PDR) {
		if (len >= 7) {
			pdr_rx(src_node, src_port, data, len);
		}
		return true;
	}
	if (dst_port == PORT_WLFW) {
		return wlfw_rx(data, len);
	}
	if (dst_port >= PORT_SESS && dst_port < PORT_SESS + 64) {
		return tftp_session(dst_port, data, len);
	}
	LOG("t=%u ms: qrtr DATA to unknown port %x from node %u port %x, %u bytes: %02x %02x %02x %02x",
	    g_ms, dst_port, src_node, src_port, len, len > 0 ? data[0] : 0, len > 1 ? data[1] : 0,
	    len > 2 ? data[2] : 0, len > 3 ? data[3] : 0);
	return false;
}

static void
svc_announce(void)
{
	/* qrtr-lookup on the working Android system reports:
	 *   0x40 ver 1 inst 1  Service registry locator   (pd-mapper, on the AP)
	 *   0x1000 ver 1 inst 0 TFTP (linux-msm/tqftpserv: qrtr_publish(fd, 4096, 1, 0))
	 *   14   ver 1 inst 0  Remote file system (rmtfs: RMTFS_QMI_SERVICE 14)
	 * The NEW_SERVER record packs version and instance as two u16 halves. */
	static const struct { uint32_t svc, ver, inst, port; } list[] = {
		{ 0x40, 1, 1, PORT_PDM },
		{ 0x1000, 1, 0, PORT_TFTP },
		{ 14, 1, 0, PORT_RMTFS },
	};
	for (unsigned i = 0; i < sizeof(list) / sizeof(list[0]); i++) {
		if (list[i].svc == 14 && !rmtfs_on) {
			continue;               /* knob for bisecting the EFS path */
		}
		struct qrtr_ctrl c = { QRTR_TYPE_NEW_SERVER, list[i].svc,
		    (list[i].ver & 0xffu) | ((list[i].inst & 0xffffu) << 8), QRTR_NODE_APPS, list[i].port };
		qrtr_send(QRTR_TYPE_NEW_SERVER, QRTR_PORT_CTRL, q_modem_node, QRTR_PORT_CTRL, &c, sizeof(c));
		ns_server_add(list[i].svc, list[i].ver, list[i].inst, QRTR_NODE_APPS, list[i].port);
	}
	LOG("t=%u ms: announced pd-mapper 0x40 v1 i1, tftp 0x1000 v1 i0, rmtfs 14 v1 i0", g_ms);
}

/* ---------------------------------------------------------------- PAS boot */

static bool
loadable(const struct elf32_phdr *p)
{
	return p->p_type == PT_LOAD && p->p_memsz != 0 && (p->p_flags & MDT_TYPE_MASK) != MDT_TYPE_HASH;
}

/* Read a firmware file from the "modem" partition into dst (dst may be a WC mapping). */

static int64_t
fw_read(struct fat_vol *v, const char *name, void *dst, uint64_t max)
{
	uint32_t clus = 0, size = 0;
	int err;

	if (fw_dir == 1) {
		uint32_t dir_size = 0, dir_clus = 0;
		if (fat_lookup_in(v, 0, "image", &dir_clus, &dir_size) != 0) {
			LOG("no image/ directory in the modem partition");
			return -1;
		}
		fw_dir = dir_clus;
	}
	err = fat_lookup_in(v, fw_dir, name, &clus, &size);
	if (err) {
		LOG("%s: not in the modem partition (%d)", name, err);
		return -1;
	}
	if (size > max) {
		LOG("%s: %u bytes, buffer is %llu", name, size, max);
		return -1;
	}
	err = fat_read_chain(v, clus, dst, size);
	if (err) {
		LOG("%s: read error %d", name, err);
		return -1;
	}
	return size;
}

static bool
modem_boot(void)
{
	static struct fat_vol v;
	uint64_t part_lba = 0, part_blocks = 0, meta_pa = 0, res = 0, st;
	uint32_t part_lun = 0, pas_id = PAS_ID_MODEM;
	uint8_t *mdt = NULL, *meta;
	int64_t mdt_size;
	uint32_t min_a = ~0u, max_a = 0, hash = ~0u;
	bool relocate = false, ok = false;
	struct smp2p_hdr *out = NULL;
	struct elf32_ehdr *eh = NULL;
	struct elf32_phdr *ph = NULL;

	struct smem_part_hdr *part = smem_partition(0, 1);

	if (part == NULL) {
		LOG("no apps<->modem SMEM partition");
		return false;
	}
	if (smem_get(part, ITEM_SMP2P_IN, NULL) != NULL) {
		LOG("modem SMP2P item %u already exists: the modem is not cold, not touching it", ITEM_SMP2P_IN);
		return false;
	}
	/* Resolve the partition through our own GPT parse: the modem image and the
	 * FAT live on LUN 0, and every read must go through native_ufs_raw_read so
	 * it shares the UFS host lock with APFS. */
	if (hakios_ufs_find_part("modem", &part_lba, &part_blocks, &part_lun) != 0) {
		LOG("no \"modem\" partition in the GPT");
		return false;
	}
	LOG("modem partition: part %u block 0x%llx, %llu MiB", part_lun, part_lba, part_blocks * 4096 >> 20);
	out = smp2p_out(part);
	smp2p_add(out, "master-kernel", 0);
	smp2p_add(out, "ipa", 0);
	LOG("SMP2P out item %u ready (%u entries)", ITEM_SMP2P_OUT, out != NULL ? out->valid : 0);
	if (fat_mount(&v, part_lun, part_lba, part_blocks) != 0) {
		return false;
	}

	/* Sanity check that SMC reaches the secure monitor at all: SCM_SVC_INFO
	 * (0x06) cmd 0x01 is the version query every qcom_scm probe does. */
	{
		uint64_t r = 0, r2 = 0;
		uint64_t s = scm(SCM_FN(0x06, 0x01), 1, 0, 0, 0, &r);
		uint64_t s2 = scm(SCM_FN(0x06, 0x02), 1, 0, 0, 0, &r2);
		LOG("SCM info: is_call_avail ret=%llx res=%llx, get_version ret=%llx res=%llx", s, r, s2, r2);
	}
	/* The device tree says qcom,pas-id = 4, but TrustZone is the authority:
	 * probe the ids and use the first one it accepts as the modem. */
	{
		uint32_t id_supported = ~0u;

		for (uint32_t id = 0; id <= 15; id++) {
			uint64_t r = 0;
			uint64_t s = scm(SCM_FN(SCM_SVC_PIL, PIL_IS_SUPPORTED), 1, id, 0, 0, &r);
			if (s == 0 && r == 1) {
				LOG("PAS id %u supported", id);
				if (id_supported == ~0u) {
					id_supported = id;
				}
			}
		}
		if (id_supported == ~0u) {
			/* Android boots this modem with the very same service, so do
			 * not trust the gate: try the device-tree id anyway. */
			LOG("no PAS id reported supported; trying id %u regardless", PAS_ID_MODEM);
			pas_id = PAS_ID_MODEM;
			goto after_probe;
		}
		pas_id = id_supported;
		st = scm(SCM_FN(SCM_SVC_PIL, PIL_IS_SUPPORTED), 1, pas_id, 0, 0, &res);
		LOG("using PAS id %u (ret=%llx res=%llx)", pas_id, st, res);
	}
after_probe:

	/* The .mdt is small: read it whole, it holds the ELF header and phdrs. */
	{
		uint32_t clus = 0, size = 0;
		int le;

		if (fw_dir == 1) {
			uint32_t dir_size = 0, dir_clus = 0;
			if (fat_lookup_in(&v, 0, "image", &dir_clus, &dir_size) != 0) {
				LOG("no image/ directory in the modem partition");
				goto out_fat;
			}
			fw_dir = dir_clus;
		}
		le = fat_lookup_in(&v, fw_dir, "modem.mdt", &clus, &size);
		if (le != 0) {
			LOG("modem.mdt: lookup failed (%d)", le);
			goto out_fat;
		}
		if (size < 52 || size > 0x100000) {
			LOG("modem.mdt: unusable size %u", size);
			goto out_fat;
		}
		mdt_size = size;
		mdt = (uint8_t *)IOMalloc((vm_size_t)mdt_size);
		if (mdt == NULL) {
			LOG("modem.mdt: %lld bytes, IOMalloc failed", mdt_size);
			goto out_fat;
		}
		if (fat_read_chain(&v, clus, mdt, (uint32_t)mdt_size) != 0) {
			LOG("modem.mdt: cannot read");
			IOFree(mdt, (vm_size_t)mdt_size);
			goto out_fat;
		}
	}

	eh = (struct elf32_ehdr *)mdt;
	if (memcmp(eh->e_ident, "\x7f" "ELF", 4) != 0 ||
	    eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(struct elf32_phdr) > (uint64_t)mdt_size) {
		LOG("modem.mdt: bad ELF header");
		IOFree(mdt, (vm_size_t)mdt_size);
		goto out_fat;
	}
	ph = (struct elf32_phdr *)(mdt + eh->e_phoff);
	for (uint32_t i = 0; i < eh->e_phnum; i++) {
		if ((ph[i].p_flags & MDT_TYPE_MASK) == MDT_TYPE_HASH) {
			hash = i;
		}
		if (!loadable(&ph[i])) {
			continue;
		}
		if (ph[i].p_flags & MDT_RELOCATABLE) {
			relocate = true;
		}
		min_a = MIN(min_a, ph[i].p_paddr);
		max_a = MAX(max_a, (ph[i].p_paddr + ph[i].p_memsz + 0xfff) & ~0xfffu);
	}
	LOG("phdrs=%u hash=%u region %08x..%08x relocate=%u", eh->e_phnum, hash, min_a, max_a, relocate);
	if (hash == ~0u || min_a < MODEM_CARVEOUT || max_a > MODEM_CARVEOUT + MODEM_CARVEOUT_SIZE) {
		LOG("unexpected layout (carveout is 0x%llx+0x%llx)", MODEM_CARVEOUT, MODEM_CARVEOUT_SIZE);
		IOFree(mdt, (vm_size_t)mdt_size);
		goto out_fat;
	}

	/*
	 * Metadata for init_image: the ELF header + phdrs (phdrs[0].p_filesz
	 * bytes) directly followed by the hash segment, as Linux
	 * qcom_mdt_read_metadata() builds it. The stock modem.mdt is "split":
	 * its size already equals header + hash, with the hash packed right
	 * after the phdrs, so no modem.b01 read is needed here.
	 */
	{
		uint64_t ehdr_size = ph[0].p_filesz;
		uint64_t hash_size = ph[hash].p_filesz;
		uint64_t meta_size = ehdr_size + hash_size;
		vm_offset_t meta_va = 0;
		ppnum_t meta_page = 0;
		kern_return_t kr;

		LOG("metadata: ehdr %llu + hash %llu = %llu (mdt %lld, hash off %x)",
		    ehdr_size, hash_size, meta_size, mdt_size, ph[hash].p_offset);
		if (ehdr_size > (uint64_t)mdt_size || ehdr_size + hash_size < hash_size) {
			LOG("metadata: header larger than modem.mdt");
			IOFree(mdt, (vm_size_t)mdt_size);
			goto out_fat;
		}
		kr = kmem_alloc_contig(kernel_map, &meta_va, (vm_size_t)meta_size, PAGE_MASK, 0, 0,
		    (kma_flags_t)(KMA_DATA | KMA_ZERO | KMA_PERMANENT), VM_KERN_MEMORY_IOKIT);
		LOG("metadata: kmem_alloc_contig kr=%d va=%llx", kr, (uint64_t)meta_va);
		if (kr != KERN_SUCCESS || meta_va == 0) {
			meta_va = 0;
			kr = kmem_alloc_contig(kernel_map, &meta_va, (vm_size_t)meta_size, PAGE_MASK, 0, 0,
			    (kma_flags_t)(KMA_DATA | KMA_ZERO), VM_KERN_MEMORY_BSD);
			LOG("metadata: kmem_alloc_contig(bsd) kr=%d va=%llx", kr, (uint64_t)meta_va);
		}
		if (meta_va == 0) {
			IOPhysicalAddress pa = 0;
			void *v = IOMallocContiguous((vm_size_t)meta_size, 0x1000, &pa);
			LOG("metadata: IOMallocContiguous va=%p pa=%llx", v, (uint64_t)pa);
			if (v != NULL) {
				meta = (uint8_t *)v;
				meta_pa = pa;
			} else {
				LOG("metadata: every allocation failed (%llu bytes)", meta_size);
				IOFree(mdt, (vm_size_t)mdt_size);
				goto out_fat;
			}
		} else {
			meta_page = pmap_find_phys(kernel_pmap, (addr64_t)meta_va);
			meta = (uint8_t *)meta_va;
			meta_pa = ptoa_64(meta_page);
			LOG("metadata: va=%llx page=%u pa=%llx", (uint64_t)meta_va, (unsigned)meta_page,
			    meta_pa);
			if (meta_page == 0) {
				IOFree(mdt, (vm_size_t)mdt_size);
				goto out_fat;
			}
		}
		memcpy(meta, mdt, (size_t)ehdr_size);
		if (meta_size == (uint64_t)mdt_size) {
			/* Split firmware: the hash is packed after the header. */
			memcpy(meta + ehdr_size, mdt + ehdr_size, (size_t)hash_size);
			LOG("metadata: hash packed after the header at %llu", ehdr_size);
		} else if (ph[hash].p_offset + hash_size <= (uint64_t)mdt_size) {
			memcpy(meta + ehdr_size, mdt + ph[hash].p_offset, (size_t)hash_size);
			LOG("metadata: hash from modem.mdt offset %x", ph[hash].p_offset);
		} else {
			char name[16];
			int64_t got;
			snprintf(name, sizeof(name), "modem.b%02u", hash);
			got = fw_read(&v, name, meta + ehdr_size, hash_size);
			LOG("metadata: hash from %s: %lld of %llu", name, got, hash_size);
			if (got != (int64_t)hash_size) {
				IOFree(mdt, (vm_size_t)mdt_size);
				goto out_fat;
			}
		}
		mb();
		flush_dcache64(meta_pa, (unsigned int)meta_size, TRUE);

		st = scm(SCM_FN(SCM_SVC_PIL, PIL_INIT_IMAGE), 2 | (SCM_ARG_RW << 6), pas_id, meta_pa, 0, &res);
		LOG("init_image: ret=%llx res=%llx resumes=%u (meta %llu bytes @%llx)", st, res, scm_resumes, meta_size, meta_pa);
		if (st != 0 || res != 0) {
			IOFree(mdt, (vm_size_t)mdt_size);
			goto out_fat;
		}
	}
	if (relocate) {
		st = scm(SCM_FN(SCM_SVC_PIL, PIL_MEM_SETUP), 3, pas_id, min_a,
		    ((uint64_t)(max_a - min_a) + 0xfff) & ~0xfffull, &res);
		LOG("mem_setup: ret=%llx res=%llx", st, res);
		if (st != 0 || res != 0) {
			goto shutdown;
		}
	}

	for (uint32_t i = 0; i < eh->e_phnum; i++) {
		char name[16];
		uint64_t pa, off, len;
		IOMemoryMap *m;
		uint8_t *dst;
		int64_t got;

		if (!loadable(&ph[i])) {
			continue;
		}
		/* map only while copying: TZ locks the carveout at auth_and_reset */
		pa = ph[i].p_paddr & ~0x3fffull;
		off = ph[i].p_paddr - pa;
		len = (off + ph[i].p_memsz + 0x3fff) & ~0x3fffull;
		m = map_phys(pa, len, true);
		if (m == NULL) {
			LOG("segment %u: cannot map %08x+%x", i, ph[i].p_paddr, ph[i].p_memsz);
			goto shutdown;
		}
		dst = (uint8_t *)m->getVirtualAddress() + off;
		got = 0;
		if (ph[i].p_filesz != 0) {
			snprintf(name, sizeof(name), "modem.b%02u", i);
			got = fw_read(&v, name, dst, ph[i].p_filesz);
			if (got != (int64_t)ph[i].p_filesz) {
				LOG("%s: %lld of %u bytes", name, got, ph[i].p_filesz);
				m->release();
				goto shutdown;
			}
		}
		if (ph[i].p_memsz > got) {
			bzero(dst + got, ph[i].p_memsz - got);
		}
		mb();
		m->release();
	}
	rmtfs_load_efs();                       /* before the modem can ask */
	dump_reserved_memory();
	rmtfs_init_buf();                       /* DMA pool, same place as the PAS metadata */
	LOG("segments loaded");
	IOFree(mdt, (vm_size_t)mdt_size);

	/* Vote modem rails via RPMh (level 384) and tell AOP via QMP that modem is starting */
	{
		LOG("voting modem rails (level 384)...");
		bool r_ok = hakios_mpss_rails(384);
		LOG("modem rails vote result: %d", r_ok ? 1 : 0);
		LOG("sending AOP mailbox modem load_state=on...");
		bool mb_ok = hakios_aop_mailbox_send("{class: image, res: load_state, name: modem, val: on}");
		LOG("AOP mailbox result: %d", mb_ok ? 1 : 0);
		LOG("calling PIL_AUTH_RESET for pas_id=%u...", pas_id);
	}
	st = scm(SCM_FN(SCM_SVC_PIL, PIL_AUTH_RESET), 1, pas_id, 0, 0, &res);
	LOG("auth_and_reset: ret=%llx res=%llx resumes=%u", st, res, scm_resumes);
	if (st != 0 || res != 0) {
		goto shutdown;
	}
	ok = true;
	goto out_fat;

shutdown:
	hakios_aop_mailbox_send("{class: image, res: load_state, name: modem, val: off}");
	hakios_mpss_rails(0);
	st = scm(SCM_FN(SCM_SVC_PIL, PIL_SHUTDOWN), 1, pas_id, 0, 0, &res);
	LOG("shutdown after failure: ret=%llx res=%llx", st, res);
out_fat:
	if (!ok && v.fat != NULL) {
		IOFree(v.fat, v.fatsz * v.bps);
		v.fat = NULL;
		g_fat = NULL;
	}
	return ok;
}

/*
 * The modem can die after GLINK starts; modem_watch() only sees the boot
 * phase. Re-read its SMP2P entries from the GLINK loop so a dead modem is
 * not mistaken for a modem with nothing to say.
 */
static uint32_t g_sk_last;

static void
modem_alive_check(void)
{
	struct smem_part_hdr *part = smem_partition(0, 1);
	struct smp2p_hdr *in;
	uint32_t sz = 0;

	if (part == NULL) {
		return;
	}
	in = (struct smp2p_hdr *)smem_get(part, ITEM_SMP2P_IN, &sz);
	if (in == NULL || in->magic != SMP2P_MAGIC) {
		return;
	}
	if (g_slave_kernel == NULL) {
		struct smp2p_entry *e = (struct smp2p_entry *)(in + 1);

		for (uint32_t i = 0; i < in->valid && i < SMP2P_MAX_ENTRY; i++) {
			LOG("smp2p in: \"%s\" = %08x", e[i].name, e[i].value);
			if (strncmp(e[i].name, "slave-kernel", 16) == 0) {
				g_slave_kernel = &e[i].value;
			}
		}
	}
	if (g_slave_kernel != NULL && *g_slave_kernel != g_sk_last) {
		g_sk_last = *g_slave_kernel;
		LOG("smp2p slave-kernel now %08x%s", g_sk_last, (g_sk_last & 1) ? " FATAL" : "");
		if (g_sk_last & 1) {
			uint32_t sz = 0;
			const char *why = (const char *)smem_get(smem_partition(0xfffe, 0xfffe), ITEM_MSS_CRASH, &sz);
			char buf[128] = {};
			if (why != NULL) {
				memcpy(buf, why, MIN(sz, (uint32_t)sizeof(buf) - 1));
			}
			LOG("modem crash reason: %s", why != NULL ? buf : "(none)");
		}
	}
}

/* Watch the modem's SMP2P "slave-kernel" entry (bit0 fatal, bit1 ready). */
static bool
modem_watch(unsigned seconds)
{
	struct smem_part_hdr *part = smem_partition(0, 1);
	struct smp2p_hdr *in = NULL;
	volatile uint32_t *sk = NULL;
	uint32_t last = ~0u;
	bool ready = false;

	if (part == NULL) {
		LOG("no apps<->modem SMEM partition");
		return false;
	}
	LOG("modem_watch: starting watch for up to %u seconds...", seconds);
	for (unsigned ms = 0; ms < seconds * 1000; ms++) {
		if (in == NULL) {
			uint32_t sz = 0;
			in = (struct smp2p_hdr *)smem_get(part, ITEM_SMP2P_IN, &sz);
			if (in != NULL && in->magic != SMP2P_MAGIC) {
				in = NULL;
			}
			if (in != NULL) {
				LOG("t=%u ms: modem SMP2P item %u up (v%u, %u entries)", ms, ITEM_SMP2P_IN, in->version, in->valid);
			}
		}
		if (in != NULL && sk == NULL) {
			struct smp2p_entry *e = (struct smp2p_entry *)(in + 1);
			for (uint32_t i = 0; i < in->valid && i < SMP2P_MAX_ENTRY; i++) {
				if (strncmp(e[i].name, "slave-kernel", 16) == 0) {
					sk = &e[i].value;
					LOG("t=%u ms: found slave-kernel entry at index %u", ms, i);
					break;
				}
			}
		}
		if (sk != NULL && *sk != last) {
			last = *sk;
			LOG("t=%u ms: slave-kernel %08x%s%s%s", ms, last, (last & 1) ? " FATAL" : "",
			    (last & 2) ? " READY" : "", (last & 4) ? " HANDOVER" : "");
			if ((last & 2) && !ready) {
				ready = true;
				LOG("modem is up (MODEM READY)");
				IOSleep(20);
				break;
			}
			if (last & 1) {
				uint32_t sz = 0;
				const char *why = (const char *)smem_get(smem_partition(0xfffe, 0xfffe), ITEM_MSS_CRASH, &sz);
				char buf[128] = {};
				if (why != NULL) {
					memcpy(buf, why, MIN(sz, (uint32_t)sizeof(buf) - 1));
				}
				LOG("modem crash reason: %s", why != NULL ? buf : "(none)");
				return false;
			}
		}
		if (ms > 0 && (ms % 1000) == 0) {
			LOG("modem_watch: waiting... ms=%u, in=%p, sk=%p", ms, in, sk);
		}
		IOSleep(1);
	}
	LOG("watch done: slave-kernel %08x, %s", last, ready ? "READY" : "NOT ready");
	return ready;
}

/*
 * Next boot goes to recovery: write the Android BCB command "boot-recovery"
 * into the misc partition and reset through PSCI. Without it every test would
 * need the Power+VolumeDown dance.
 */
static void
reboot_to_recovery(void)
{
	static const char bcb[32] = { 'b', 'o', 'o', 't', '-', 'r', 'e', 'c', 'o', 'v', 'e', 'r', 'y' };
	uint64_t first = 0, blocks = 0;
	uint32_t lun = 0;
	int err;
	uint64_t a[8] = { 0x84000009ULL, 0, 0, 0, 0, 0, 0, 0 };

	err = sweet_ufs_part("misc", &first, &blocks, &lun);
	if (err == 0) {
		err = sweet_ufs_pwrite(lun, first, blocks, 0, bcb, sizeof(bcb));
	}
	LOG("recovery: misc LUN %u block 0x%llx, BCB write %d", lun, first, err);
	/* Reset even when the BCB write failed: sitting in a boot that cannot be
	 * observed costs a power cycle, while a plain reset falls back to the
	 * bootloader and is always recoverable. */
	IOSleep(200);
	LOG("recovery: PSCI SYSTEM_RESET");
	smc_call(a);
	LOG("recovery: PSCI reset returned");
}

/* Independent of the modem/GLINK path: after modem_recovery_after ms, write the
 * Android BCB command "boot-recovery" into misc and reset through PSCI, so a
 * hung modem thread can never keep the phone in iOS. */
static void
recovery_thread(void *arg __unused, wait_result_t wr __unused)
{
	uint32_t rec = 0, delay = 45000;

	if (!PE_parse_boot_argn("modem_recovery", &rec, sizeof(rec)) || !rec) {
		thread_terminate(current_thread());
		return;
	}
	(void)PE_parse_boot_argn("modem_recovery_after", &delay, sizeof(delay));
	if (delay < 2000) {
		delay = 2000;
	}
	IOSleep(delay);
	reboot_to_recovery();
	thread_terminate(current_thread());
}

static void
modem_thread(void *arg __unused, wait_result_t wr __unused)
{
	IOMemoryMap *smem_map;
	unsigned watch = 60;

	(void)PE_parse_boot_argn("modem_watch", &watch, sizeof(watch));
	(void)PE_parse_boot_argn("modem_pdm", &pd_full, sizeof(pd_full));
	(void)PE_parse_boot_argn("modem_dbg", &modem_dbg, sizeof(modem_dbg));
	(void)PE_parse_boot_argn("modem_efs_fail", &g_efs_fail, sizeof(g_efs_fail));
	(void)PE_parse_boot_argn("modem_rmtfs", &rmtfs_on, sizeof(rmtfs_on));
	{
		uint32_t restart = 0;
		PE_parse_boot_argn("modem_pdr_restart", &restart, sizeof(restart));
		pdr_restart_want = restart;
	}
	/* The WLAN chip hangs off the mss, and icnss powers it before waiting for
	 * the WLFW server: vdd-cx-mx (ldoa1), vdd-1.8-xo (ldoa14),
	 * vdd-1.3-rfa (ldoc2), vdd-3.3-ch0/ch1 (ldoc10/11), at the voltages
	 * from the vendor icnss_vreg_info[] table. */
	{
		uint32_t power = 1;
		(void)PE_parse_boot_argn("modem_wlan_power", &power, sizeof(power));
		if (power) {
			/* The vendor icnss table overrides the device-tree init values. */
			int r1 = sweet_rpmh_vreg_enable("ldoa1", 752);
			int r2 = sweet_rpmh_vreg_enable("ldoa14", 1800);
			int r3 = sweet_rpmh_vreg_enable("ldoc2", 1304);
			int r4 = sweet_rpmh_vreg_enable("ldoc10", 3312);
			int r5 = sweet_rpmh_vreg_enable("ldoc11", 3312);
			LOG("wlan rails rc: ldoa1 %d, ldoa14 %d, ldoc2 %d, ldoc10 %d, ldoc11 %d",
			    r1, r2, r3, r4, r5);
			/* The vendor PIL votes the modem's own corners (RPMh ARC
			 * levels) before auth_and_reset; the device tree asks for
			 * level 0x180 on both. */
			int c1 = sweet_rpmh_arc_level("cx.lvl", 0x180);
			int c2 = sweet_rpmh_arc_level("mss.lvl", 0x180);
			LOG("modem corners: cx.lvl rc %d, mss.lvl rc %d", c1, c2);
		}
	}
	{
		/* picasso's own device tree says:
		 *   qcom,icnss@18800000 { iommus = <&apps_smmu 0x20 0x1>;
		 *                         qcom,iommu-dma-addr-pool = <0xa0000000 0x10000000>; }
		 * The WLAN master is stream 0x20, NOT 0xc0 (that value came from a
		 * different platform).  Leaving 0x20 in translation mode with no
		 * context bank for our CE rings makes the first CE DMA fault and stall
		 * the interconnect - exactly the 30 s watchdog hang in v331/v333/v334b.
		 * Linux attaches icnss to the IOMMU at probe; here we set the stream to
		 * bypass so the CE can use plain physical addresses.
		 *
		 * Apps SMMU on SM7250 is ARM MMU-500 (SMMUv2):
		 * SMR registers at 0x800 + 4*i, S2CR at 0xc00 + 4*i. */
		/* Default 0: leave the Apps SMMU exactly as the bootloader left it.
		 * Stream 0x20 is already covered by SMR[6] (sid 0, mask 0xe0), and
		 * that entry also covers 0x00/0x40/0x80/0xc0 - masters that are
		 * doing DMA during bsd_init.  Rewriting the entry's SMR or S2CR
		 * (v336, v338) hangs the interconnect on the store itself, so a
		 * bypass cannot be installed here.  modem_wlansmmu=2 enables the
		 * old attempt for experiments only; a real fix has to come from a
		 * context bank or the DT's iommu-dma-addr-pool. */
		uint32_t wlansmmu = 0;
		uint32_t sid = 0x20;                 /* decimal 32 if overridden */
		uint32_t sid2 = 0xc0;               /* legacy platform, best-effort */
		(void)PE_parse_boot_argn("modem_wlansmmu", &wlansmmu, sizeof(wlansmmu));
		(void)PE_parse_boot_argn("modem_wlansmmu_sid", &sid, sizeof(sid));
		if (wlansmmu) {
			/* 1 MiB covers the context-bank window as well (CB n lives at
			 * 0x8000 + n*0x1000 on an MMU-500), which is what tells us
			 * whether CB 0x3d - the bank SMR[6] points stream 0x20 at -
			 * is actually enabled and where its page table is. */
			vm_offset_t smmu = ml_io_map(0x15000000, 0x100000);
			if (smmu != 0) {
				volatile uint32_t *r = (volatile uint32_t *)smmu;
				uint32_t idr0 = r[0x20 >> 2];
				uint32_t num_smr = idr0 & 0xffu;
				{
					uint32_t s2cr6 = r[(0xc00 >> 2) + 6];
					uint32_t cbndx = s2cr6 & 0xffu;
					uint32_t cbar = r[((0x8000u + cbndx * 0x1000u) >> 2)];
					uint32_t cbacr = r[((0x8000u + cbndx * 0x1000u + 4) >> 2)];
					LOG("wlan smmu: S2CR[6]=%08x cb=%u cbar=%08x acr=%08x (valid=%u)",
					    s2cr6, cbndx, cbar, cbacr, cbar & 1u);
				}

				/* Report every live stream once: this is how we found the
				 * 0xc0 / 0x20 mismatch, keep it for future board bring-up. */
				for (uint32_t i = 0; i < num_smr && i < 128; i++) {
					uint32_t smr = r[(0x800 >> 2) + i];
					if (smr & (1u << 31)) {
						LOG("wlan smmu: SMR[%u]=%08x S2CR=%08x", i, smr,
						    r[(0xc00 >> 2) + i]);
					}
				}

				uint32_t targets[2] = { sid & 0x7fffu, sid2 & 0x7fffu };
				for (unsigned t = 0; t < (wlansmmu >= 3 ? 2u : 1u); t++) {
					uint32_t want = targets[t];
					int exact_idx = -1, match_idx = -1, free_idx = -1;
					for (uint32_t i = 0; i < num_smr && i < 128; i++) {
						uint32_t smr = r[(0x800 >> 2) + i];
						if (!(smr & (1u << 31))) {
							if (free_idx < 0) free_idx = (int)i;
							continue;
						}
						/* SMMU-500 SMR: [31] VALID, [30:16] SID,
						 * [15:0] MASK (1 = ignore that SID bit). */
						uint32_t mask = smr & 0xffffu;
						uint32_t smr_sid = (smr >> 16) & 0x7fffu;
						if (((smr_sid ^ want) & ~mask & 0x7fffu) != 0) continue;
						if (match_idx < 0) match_idx = (int)i;
						if (mask == 0) { exact_idx = (int)i; break; }
					}
					if (exact_idx >= 0) {
						uint32_t s2 = r[(0xc00 >> 2) + exact_idx];
						uint32_t type = (s2 >> 16) & 3u;
						LOG("wlan smmu: SID %x has exact SMR[%d] S2CR=%08x type=%u",
						    want, exact_idx, s2, type);
						if (wlansmmu >= 2 && type != 1u) {
							r[(0xc00 >> 2) + exact_idx] =
							    (s2 & ~(3u << 16)) | (1u << 16);
							__asm__ volatile ("dsb sy" ::: "memory");
							LOG("wlan smmu: S2CR[%d] -> bypass", exact_idx);
						}
						continue;
					}
					if (match_idx >= 0) {
						uint32_t smr = r[(0x800 >> 2) + match_idx];
						uint32_t mask = smr & 0xffffu;
						uint32_t smr_sid = (smr >> 16) & 0x7fffu;
						LOG("wlan smmu: SID %x covered by SMR[%d] (sid %x mask %x), narrowing",
						    want, match_idx, smr_sid, mask);
						/* Never rewrite that entry's S2CR.  Its mask also
						 * covers streams that are already doing DMA
						 * (modem, UFS, USB); moving them from
						 * translation to bypass points their IOVAs at
						 * the wrong physical pages and the interconnect
						 * hangs - that is exactly what v336 did.  Drop
						 * only the bits that make it match `want`, then
						 * give the WLAN stream a dedicated entry. */
						uint32_t narrowed = mask & ~(want & 0x7fffu);
						if (narrowed != mask) {
							r[(0x800 >> 2) + match_idx] = 0;
							__asm__ volatile ("dsb sy" ::: "memory");
							r[(0x800 >> 2) + match_idx] =
							    (smr & 0xffff0000u) | narrowed;
							__asm__ volatile ("dsb sy" ::: "memory");
							LOG("wlan smmu: SMR[%d] mask %x -> %x",
							    match_idx, mask, narrowed);
						}
					}
					if (wlansmmu >= 2 && free_idx >= 0) {
						/* Bypass first, then enable the match: the
						 * other order lets one DMA go through the
						 * entry while its S2CR still says translation. */
						r[(0xc00 >> 2) + free_idx] = (1u << 16);
						r[(0x800 >> 2) + free_idx] = 0;
						__asm__ volatile ("dsb sy" ::: "memory");
						r[(0x800 >> 2) + free_idx] =
						    (1u << 31) | (want << 16);
						__asm__ volatile ("dsb sy" ::: "memory");
						LOG("wlan smmu: SID %x exact SMR[%d] S2CR=bypass",
						    want, free_idx);
					} else {
						LOG("wlan smmu: SID %x no SMR slot (free=%d)", want, free_idx);
					}
				}
			} else {
				LOG("wlan smmu: cannot map 0x15000000");
			}
		} else {
			/* Claim a stream-match slot for the WLAN copy engines.
			 *
			 * v351 proved two things: the SMMU writes issued from the
			 * first line of bsd_init are silently dropped (the entry
			 * read back as 0 and the USB driver still saw 6 active
			 * slots), and stage 2 - programming CE0/2/3 and their
			 * CTRL1 - survives while stage 3 (handing the first
			 * receive buffer to the target) still wedges the SoC.
			 *
			 * By now the SMMU is clocked and writable.  What the
			 * console driver's own log shows is that it owns slot 6/7
			 * with SMR=800000e0, which under the real field layout is
			 * ID 0xe0 / MASK 0 - an exact single-stream entry, not the
			 * mask entry it was mistaken for - and that its S2CR points
			 * at CB 61, a bank with SCTLR.M=0 that passes addresses
			 * through.  This block mirrors that recipe for 0x20 and
			 * dumps the whole table so the next run says what really
			 * covers the WLAN stream. */
			vm_offset_t smmu = ml_io_map(WLAN_SMMU_PA, 0x1000);
			if (smmu != 0) {
				volatile uint32_t *r = (volatile uint32_t *)smmu;
				uint32_t count = r[0x20 >> 2] & 0xffu;
				uint32_t i;
				int exact = -1, first = -1, free_slot = -1;

				if (count == 0 || count > 128) count = 128;
				LOG("wlan smmu: scr0=%08x idr0=%08x smr=%u",
				    r[0], r[0x20 >> 2], count);
				/* Read-only fault record: sGFSR/sGFSYNR1 name the last
				 * stream the SMMU refused.  The firmware has been
				 * running for seconds by now, so this says whether its
				 * own DMA is reaching memory. */
				LOG("wlan smmu: gfsr=%08x gfsynr0=%08x gfsynr1=%08x gfsynr2=%08x",
				    r[0x48 >> 2], r[0x50 >> 2], r[0x54 >> 2],
				    r[0x58 >> 2]);
				for (i = 0; i < count; i++) {
					uint32_t smr = r[(0x800 >> 2) + i];
					uint32_t s2 = r[(0xc00 >> 2) + i];

					if (!(smr & WLAN_SMMU_SMR_VALID)) {
						if (free_slot < 0) free_slot = (int)i;
						continue;
					}
					if (i < 16) {
						LOG("wlan smmu: SMR[%u] id=%03x mask=%03x s2cr=%08x type=%u cb=%u",
						    i, smr & 0x7fffu,
						    (smr >> WLAN_SMMU_SMR_MASK_SHIFT) & 0x7fffu,
						    s2, (s2 >> 16) & 3u, s2 & 0xffu);
					}
					if (!wlan_smmu_smr_matches(smr, WLAN_SMMU_SID)) continue;
					if (first < 0) first = (int)i;
					if ((smr & 0x7fffu) == WLAN_SMMU_SID &&
					    ((smr >> WLAN_SMMU_SMR_MASK_SHIFT) & 0x7fffu) == 0) {
						exact = (int)i;
					}
				}
				LOG("wlan smmu: 0x20 exact=%d first=%d free=%d", exact,
				    first, free_slot);
				if (exact >= 0) {
					uint32_t s2 = r[(0xc00 >> 2) + exact];
					LOG("wlan smmu: 0x20 already exact in SMR[%d] type=%u cb=%u",
					    exact, (s2 >> 16) & 3u, s2 & 0xffu);
				} else if (free_slot >= 0 &&
				    (first < 0 || free_slot < first)) {
					/* Order matters: the USB console driver
					 * writes S2CR first and SMR second, and its
					 * readback passes.  Writing SMR first (what
					 * every earlier build did) left both
					 * registers reading back as 0 / 00020000,
					 * i.e. the pair was dropped.
					 *
					 * The S2CR value is a *translation* entry
					 * aimed at a context bank with SCTLR.M=0,
					 * which passes addresses through.  TYPE=1
					 * (bypass) is coerced to TYPE=2 (fault) on
					 * this SMMU, which is why every earlier
					 * attempt came back as 00020000. */
					r[(0xc00 >> 2) + free_slot] =
					    WLAN_SMMU_PASSTHRU_BANK;
					__asm__ volatile ("dsb sy" ::: "memory");
					r[(0x800 >> 2) + free_slot] =
					    WLAN_SMMU_SMR_VALID | WLAN_SMMU_SID;
					__asm__ volatile ("dsb sy" ::: "memory");
					LOG("wlan smmu: 0x20 -> SMR[%d]=%08x S2CR[%d]=%08x (want %08x/%08x, first %d)",
					    free_slot, r[(0x800 >> 2) + free_slot], free_slot,
					    r[(0xc00 >> 2) + free_slot],
					    WLAN_SMMU_SMR_VALID | WLAN_SMMU_SID,
					    WLAN_SMMU_PASSTHRU_BANK, first);
				} else if (first >= 0) {
					/* A live entry covers 0x20 from a lower
					 * index and nothing is free below it:
					 * rewriting it would move a master that is
					 * already doing DMA, so leave it alone. */
					LOG("wlan smmu: 0x20 shadowed by SMR[%d], no lower free slot",
					    first);
				} else {
					LOG("wlan smmu: no free SMR slot for SID 20");
				}
				/* sCR0.USFCFG=1 means *deny* unmatched streams (arm-smmu
				 * names the flag disable_bypass).  It is set on this
				 * device, so a stream with no SMR entry is faulted -
				 * which is the other way the CE's first DMA can die.
				 * Clearing it makes unmatched streams bypass instead;
				 * that is a global change, so it is opt-in. */
				{
					uint32_t allow = 0;

					(void)PE_parse_boot_argn("wlan_smmu_unmatched_bypass",
					    &allow, sizeof(allow));
					if (allow) {
						uint32_t scr0 = r[0];
						r[0] = scr0 & ~(1u << 10);
						__asm__ volatile ("dsb sy" ::: "memory");
						LOG("wlan smmu: USFCFG %u -> %u (scr0 %08x -> %08x)",
						    (scr0 >> 10) & 1u, (r[0] >> 10) & 1u,
						    scr0, r[0]);
					}
				}
			} else {
				LOG("wlan smmu: cannot map 0x15000000");
			}
		}
	}
	{
		/* icnss hands the WLAN firmware MSA region to the modem's WLAN
		 * VMs before telling the FW about it; doing it earlier is the
		 * only way to test whether the modem needs write access for the
		 * wlanmdsp.mbn it fetches over tftp. modem_msa_assign=1 gives it
		 * to MSS_MSA+WLAN, 2 adds WLAN_CE. */
		uint32_t msa_assign = 0;
		PE_parse_boot_argn("modem_msa_assign", &msa_assign, sizeof(msa_assign));
		if (msa_assign) {
			uint32_t vm[3] = { VMID_MSS_MSA, VMID_WLAN, VMID_WLAN_CE };
			uint64_t r = hyp_assign(WLAN_MSA_PA, WLAN_MSA_SIZE, vm,
			    msa_assign >= 2 ? 3 : 2);
			LOG("wlan MSA assign rc %lld", r);
		}
	}
	tcsr_mutex = (volatile uint8_t *)ml_io_map(TCSR_MUTEX_PA, 0x4000);
	{
		/* Does the bootloader leave the WLAN firmware in its MSA region? */
		IOMemoryMap *msa = map_phys(WLAN_MSA_PA, WLAN_MSA_SIZE, false);
		if (msa != NULL) {
			const uint8_t *b = (const uint8_t *)msa->getVirtualAddress();
			uint32_t nz = 0;
			for (uint32_t i = 0; i < WLAN_MSA_SIZE; i += 0x1000) {
				if (b[i] != 0 || b[i + 1] != 0 || b[i + 2] != 0 || b[i + 3] != 0) {
					nz++;
				}
			}
			LOG("wlan MSA @%llx: %02x%02x%02x%02x %02x%02x%02x%02x, non-zero pages %u",
			    WLAN_MSA_PA, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], nz);
			msa->release();
		} else {
			LOG("wlan MSA: cannot map");
		}
	}
	smem_map = map_phys(SMEM_PA, SMEM_SIZE, true);
	if (smem_map == NULL) {
		LOG("cannot map SMEM at 0x%llx", SMEM_PA);
		goto done;
	}
	smem = (uint8_t *)smem_map->getVirtualAddress();
	if (smem_toc() == NULL) {
		LOG("SMEM at 0x%llx: no TOC", SMEM_PA);
		goto done;
	}
	LOG("SMEM version %u", *(volatile uint32_t *)(smem + 64 + 7 * 4) >> 16);
	if (modem_boot()) {
		struct smem_part_hdr *part = smem_partition(0, 1);

		if (modem_watch(watch) && part != NULL) {
			/* Wi-Fi step 2: GLINK over SMEM, then QRTR over IPCRTR. */
			/* 0 = serve the modem for as long as the system runs: EFS,
			 * the pd-mapper and WLFW all need this thread alive. */
			unsigned glink_ms = 0;

			(void)PE_parse_boot_argn("modem_glink_ms", &glink_ms, sizeof(glink_ms));
			ipcc = (volatile uint8_t *)ml_io_map(IPCC_BASE, 0x1000);
			LOG("ipcc mapped at %p (pa 0x%llx)", ipcc, (uint64_t)IPCC_BASE);
			if (glink_start(part)) {
				g_watch = 1;
				for (unsigned ms = 0; glink_ms == 0 || ms < glink_ms; ms++) {
					bool loud = ms < 20000 ? (ms % 1000) == 0 : (ms % 10000) == 0;

					if (ms != 0 && loud) {
						modem_alive_check();
					}
					/* A modem that never answers a registration is not
					 * going to answer the tenth one either: retry a few
					 * times, then stop filling the TX queue. */
					if (ms != 0 && (ms % 2000) == 0 && g_pdr_tries < 8) {
						g_pdr_tries++;
						pdr_retry_all();
					}
					if (ms != 0 && loud && wlfw_state < 5 && ms < 5000) {
						LOG("tick %u: tx %u rx %u q %u d %u | efs %u/%u/%u | pdr %u/%u",
						    ms, g_tx_msgs, g_rx_msgs, g_txq_tail - g_txq_head, g_txq_drops,
						    rmtfs_reads, rmtfs_writes, rmtfs_err, g_pdr_tries, pdr_rx_logs);
					}
					g_beat++;
					glink_poll(part, ms);
					picasso_wlan_ce_poll();
					picasso_usb_console_drive_poll();
					IOSleep(1);
				}
				g_watch = 0;
				glink_summary();
			}
		}
	}
	/* keep the mapping: the modem uses SMEM from now on */
done:
	LOG("done");

	thread_terminate(current_thread());
}

/*
 * Give stream 0x20 (the WLAN copy engines) its own bypass entry, called from
 * the first line of bsd_init.
 *
 * The Apps SMMU is empty at that moment (v342 printed SMR[6]=0 S2CR[6]=0), so a
 * free slot - normally slot 0 - can be claimed without touching a live entry.
 * That matters because every attempt to rewrite the entry the bootloader later
 * installs (SMR[6]=800000e0, a *mask* entry that also spans 0x00/0x40..0xe0 and
 * is shared with the USB console) froze the interconnect: v336 wrote its S2CR,
 * v338 wrote its SMR, both with DMA already in flight.
 *
 * Writing a *free* slot instead is harmless, and because slot 0 wins over slot
 * 6 in the SMR scan the WLAN stream no longer depends on what the bootloader or
 * the USB driver put there later.
 */
extern "C" void
picasso_wlan_smmu_early(void)
{
	uint32_t on = 0;
	uint32_t idr0, count, i;
	int free_slot = -1, match = -1;
	vm_offset_t smmu;
	volatile uint32_t *r;

	/* v351 showed that writes issued from the very first line of bsd_init
	 * are dropped: the entry read back as 0 and the USB driver still found
	 * only six active slots.  The SMMU is not writable yet, so this path is
	 * opt-in and the claim now happens in the modem thread instead. */
	PE_parse_boot_argn("wlan_smmu_early", &on, sizeof(on));
	if (!on) return;

	smmu = ml_io_map(WLAN_SMMU_PA, 0x1000);
	if (smmu == 0) {
		LOG("wlan smmu early: cannot map %llx", WLAN_SMMU_PA);
		return;
	}
	r = (volatile uint32_t *)smmu;

	idr0 = r[0x20 >> 2];
	count = idr0 & 0xffu;
	LOG("wlan smmu early: idr0=%08x smr_count=%u", idr0, count);
	if (count == 0 || count > 128) {
		/* Registers still read as zero this early; the array is at a
		 * fixed offset, so assume the architectural maximum and rely on
		 * the VALID bit instead of the reported size. */
		count = 128;
	}

	for (i = 0; i < count; i++) {
		uint32_t smr = r[(0x800 >> 2) + i];

		if (!(smr & WLAN_SMMU_SMR_VALID)) {
			if (free_slot < 0) free_slot = (int)i;
			continue;
		}
		if (match < 0 && wlan_smmu_smr_matches(smr, WLAN_SMMU_SID)) {
			match = (int)i;
		}
	}
	LOG("wlan smmu early: stream 0x20 match=%d first_free=%d", match, free_slot);

	if (match >= 0) {
		uint32_t s2 = r[(0xc00 >> 2) + match];
		uint32_t type = (s2 >> 16) & 3u;

		if (type == 1u) {
			LOG("wlan smmu early: SMR[%d] already bypasses 0x20", match);
			return;
		}
		/* Never rewrite it: this entry can cover other live masters. */
		LOG("wlan smmu early: SMR[%d] covers 0x20 with type=%u, leaving it alone",
		    match, type);
		return;
	}
	if (free_slot < 0) {
		LOG("wlan smmu early: no free SMR slot");
		return;
	}

	/* S2CR before SMR: see the comment at the modem-time claim. */
	r[(0xc00 >> 2) + free_slot] = WLAN_SMMU_S2CR_BYPASS;
	__asm__ volatile ("dsb sy" ::: "memory");
	r[(0x800 >> 2) + free_slot] = WLAN_SMMU_SMR_VALID |
	    (WLAN_SMMU_SID << 16);
	__asm__ volatile ("dsb sy" ::: "memory");

	LOG("wlan smmu early: SMR[%d]=%08x S2CR[%d]=%08x (bypass)",
	    free_slot, r[(0x800 >> 2) + free_slot],
	    free_slot, r[(0xc00 >> 2) + free_slot]);
}

extern "C" void
picasso_modem_init(void)
{
	static bool started;
	uint32_t on = 0;
	thread_t thread;
	char *ba = PE_boot_args();
	int got = PE_parse_boot_argn("wifi", &on, sizeof(on));
	if (got == 0 || on == 0) {
		got = PE_parse_boot_argn("modem", &on, sizeof(on));
	}

	/* Boot-arg transport diagnostics: what the kernel really received. */
	LOG("init: boot-args %u bytes: %s", ba != NULL ? (unsigned)strlen(ba) : 0, ba != NULL ? ba : "(none)");
	LOG("init: wifi/modem=%d value=%u", got, on);
	if (started || got == 0 || on == 0) {
		LOG("init: not starting (started=%d)", started);
		return;
	}
	started = true;
	picasso_wlan_net_attach();
	if (kernel_thread_start(modem_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	{
		thread_t mon;

		if (kernel_thread_start(modem_monitor, NULL, &mon) == KERN_SUCCESS) {
			thread_deallocate(mon);
		}
	}
	if (kernel_thread_start(recovery_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}


