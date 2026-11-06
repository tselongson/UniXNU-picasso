#include <stdarg.h>
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
/*
 * hakios.picasso_wlan.cpp compatibility layer for UniXNU.
 *
 * The WCN3990 driver brought over from hakios-picasso expects three services that
 * hakios provides in its own tree and UniXNU names differently:
 *
 *   hakios_ufs_find_part / hakios_ufs_read_part / hakios_ufs_read_partition
 *       partition lookup by GPT name and raw reads. UniXNU's NativeUfs exports
 *       native_ufs_raw_read() with 4 KiB block units; the GPT is parsed here.
 *   hakios_mpss_rails(level)
 *       RPMh vote for the modem's rails. On this board UEFI/ABL leave them up and
 *       UniXNU's own MPSS path has already loaded the image, so the vote is a no-op
 *       that reports success.
 *   hakios_aop_mailbox_send(text)
 *       QMP "modem load_state" note to the always-on processor. Not needed once the
 *       rails are up; accepted and ignored.
 */
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>

extern "C" int native_ufs_raw_read(uint32_t lun, uint64_t lba, uint32_t blocks, void *dst);
extern "C" uint32_t native_ufs_raw_max_blocks(void);
extern "C" void native_ufs_raw_diag(uint32_t *out);
extern "C" void picasso_wlan_smmu_early(void);

/* LUN 0 alone carries 64 used entries on this device and the firmware LUNs
 * follow it, so the table has to hold several LUNs' worth. */
#define GPT_MAX_PARTS 256
#define GPT_NAME_MAX  72

struct gpt_part {
	char     name[GPT_NAME_MAX];
	uint64_t first;          /* first 512-byte LBA */
	uint64_t last;           /* last 512-byte LBA */
	uint32_t lun;
	uint32_t sector;         /* logical sector size the table was read with */
};

static struct gpt_part g_parts[GPT_MAX_PARTS];
/* Only the first sweep of the first scan explains itself; the retry loop is quiet. */
static int g_scan_talk;
static int g_part_count = -1;

static uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t
le64(const uint8_t *p)
{
	return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static void
gpt_name(const uint8_t *utf16, char out[GPT_NAME_MAX])
{
	unsigned i;
	for (i = 0; i + 1 < GPT_NAME_MAX; i += 2) {
		uint16_t c = le16(utf16 + i);
		if (c == 0) break;
		out[i / 2] = (c < 0x80) ? (char)c : '?';
	}
	out[i / 2] = 0;
}

/* Read one 4 KiB block. The UFS hook works in 4 KiB units. */
/* Delay between 4 KiB reads, in milliseconds.  The Wi-Fi bring-up pulls about
 * 30 MiB through the single-slot UFS host while init is still reading the root
 * volume; that burst was enough to leave a root read with UTSLOT busy until its
 * deadline, which poisons the controller and turns launchd's exec into ENOEXEC.
 * modem_slowio=N paces the firmware reads so APFS keeps getting slots. */
static uint32_t g_ufs_slow_ms;

static void
ufs_io_tuning(void)
{
	static bool done;
	if (done) return;
	done = true;
	uint32_t v = 0;
	if (PE_parse_boot_argn("modem_slowio", &v, sizeof(v))) {
		g_ufs_slow_ms = v;
	}
}

static int
read4k(uint32_t lun, uint64_t lba4k, void *dst)
{
	if (native_ufs_raw_max_blocks() == 0) return -1;
	ufs_io_tuning();
	int r = native_ufs_raw_read(lun, lba4k, 1, dst);
	/* hostRead4k has already dropped gHost.lock by the time we get here. */
	if (g_ufs_slow_ms) IOSleep(g_ufs_slow_ms);
	return r;
}

/* Arbitrary byte reads on top of the 4 KiB hook: the GPT entry array of a
 * 512-byte-sector disk starts at LBA 2 = byte 1024, which is not block aligned. */
static uint8_t g_read_block[4096];

static int
readBytes(uint32_t lun, uint64_t byte, void *dst, uint32_t len)
{
	uint8_t *block = g_read_block;
	uint8_t *out = (uint8_t *)dst;

	while (len > 0) {
		const uint64_t blk = byte / 4096u;
		const unsigned off = (unsigned)(byte % 4096u);
		uint32_t n;
		if (read4k(lun, blk, block) != 0) return -1;
		n = 4096u - off;
		if (n > len) n = len;
		memcpy(out, block + off, n);
		out += n;
		byte += n;
		len -= n;
	}
	return 0;
}

static uint8_t g_gpt_block[4096];

static void
scan_lun(uint32_t lun)
{
	uint8_t *block = g_gpt_block;
	uint8_t hdr[128];
	uint8_t entry[512];
	unsigned sector;
	uint64_t entries_byte;
	uint32_t entries_count, entry_size, i;

	if (read4k(lun, 0, block) != 0) {
		if (g_scan_talk) printf("HAKIOS_WLAN: GPT: LUN %u unreadable\n", lun);
		return;
	}

	/* 512-byte logical sectors put the GPT header at offset 512 of block 0;
	 * 4 KiB logical sectors put it at the start of block 1 (LBA 1 = byte 4096),
	 * so the second case has to read the next block, not look at block 0. */
	if (memcmp(block + 512, "EFI PART", 8) == 0) {
		memcpy(hdr, block + 512, sizeof(hdr));
		sector = 512;
	} else {
		const uint16_t mbr = (uint16_t)(block[510] | (block[511] << 8));
		if (g_scan_talk)
			printf("HAKIOS_WLAN: GPT: LUN %u block0 %02x%02x%02x%02x %02x%02x%02x%02x "
			    "mbr=%04x, trying 4K sectors\n", lun, block[0], block[1], block[2],
			    block[3], block[4], block[5], block[6], block[7], mbr);
		if (read4k(lun, 1, block) != 0) return;
		if (memcmp(block, "EFI PART", 8) != 0) {
			if (g_scan_talk)
				printf("HAKIOS_WLAN: GPT: LUN %u block1 %02x%02x%02x%02x\n", lun,
				    block[0], block[1], block[2], block[3]);
			return;
		}
		memcpy(hdr, block, sizeof(hdr));
		sector = 4096;
	}

	if (le32(hdr + 12) < 92) return;
	entries_byte = le64(hdr + 72) * sector;
	entries_count = le32(hdr + 80);
	entry_size = le32(hdr + 84);
	if (entry_size < 128 || entry_size > sizeof(entry) || entries_count == 0) return;
	if (entries_count > 128) entries_count = 128;

	for (i = 0; i < entries_count; i++) {
		int used = 0, b;
		struct gpt_part *p;

		if (readBytes(lun, entries_byte + (uint64_t)i * entry_size, entry,
		    entry_size) != 0) {
			return;
		}
		for (b = 0; b < 16; b++) used |= entry[b];     /* type GUID: zero means unused */
		if (!used) continue;
		if (g_part_count >= GPT_MAX_PARTS) return;
		p = &g_parts[g_part_count];
		memset(p, 0, sizeof(*p));
		p->first = le64(entry + 32);
		p->last = le64(entry + 40);
		p->lun = lun;
		p->sector = sector;
		gpt_name(entry + 56, p->name);
		g_part_count++;
	}
	if (g_scan_talk)
		printf("HAKIOS_WLAN: GPT: LUN %u sector=%u entries=%u size=%u\n", lun, sector,
		    entries_count, entry_size);
}

static void
scan_parts(void)
{
	if (g_part_count > 0) return;
	/* NativeUfs is started with native-ufs-start-delay=5 and this runs from the
	 * deferred WLAN thread at about the same moment, so the first reads can land
	 * while gHost.ready is still 0. Poll until the controller answers instead of
	 * giving up on the first miss (v308 logged "GPT scan found 0"). */
	for (unsigned attempt = 0; attempt < 40 && g_part_count <= 0; attempt++) {
		g_part_count = 0;
		g_scan_talk = (attempt == 0);
		for (uint32_t lun = 0; lun < 8; lun++) {
			scan_lun(lun);
		}
		if (g_part_count > 0) break;
		if (attempt == 0) {
			uint32_t diag[5] = { 0, 0, 0, 0, 0 };
			native_ufs_raw_diag(diag);
			printf("HAKIOS_WLAN: GPT: no table yet (maxblk=%u ready=%u scsi=%x proto=%x ocs=%x cmd=%x)\n",
			    native_ufs_raw_max_blocks(), diag[0], diag[1], diag[2], diag[3], diag[4]);
		}
		IOSleep(1000);
	}
	printf("HAKIOS_WLAN: GPT scan found %d named partitions\n", g_part_count);
	for (int i = 0; i < g_part_count && i < 8; i++) {
		printf("HAKIOS_WLAN:   %s lun=%u first=%llu blocks=%llu\n",
		    g_parts[i].name, g_parts[i].lun,
		    (unsigned long long)g_parts[i].first,
		    (unsigned long long)(g_parts[i].last - g_parts[i].first + 1));
	}
}

extern "C" int
hakios_ufs_find_part(const char *name, uint64_t *first, uint64_t *nblocks, uint32_t *part_idx)
{
	scan_parts();
	if (!name) return -1;
	/* Android's A/B layout names the firmware slots modem_a / modem_b; try the
	 * plain name first, then the two slots. */
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		char wanted[GPT_NAME_MAX];
		int i;

		if (attempt == 0) {
			strlcpy(wanted, name, sizeof(wanted));
		} else {
			snprintf(wanted, sizeof(wanted), "%s_%c", name,
			    attempt == 1 ? 'a' : 'b');
		}
		for (i = 0; i < g_part_count; i++) {
			if (strcmp(g_parts[i].name, wanted) != 0) continue;
			if (first) *first = g_parts[i].first;
			if (nblocks) *nblocks = g_parts[i].last - g_parts[i].first + 1;
			if (part_idx) *part_idx = (uint32_t)i;
			return 0;
		}
	}
	if (g_scan_talk) {
		printf("HAKIOS_WLAN: GPT: %d partitions, none named %s\n", g_part_count, name);
		for (int i = 0; i < g_part_count && i < GPT_MAX_PARTS; i++)
			printf("HAKIOS_WLAN:   %s lun=%u\n", g_parts[i].name, g_parts[i].lun);
	}
	return -1;
}

extern "C" int
hakios_ufs_read_part(uint32_t part, uint64_t offset, uint8_t *dst, uint64_t len)
{
	scan_parts();
	if (part >= (uint32_t)g_part_count || dst == NULL) return -1;
	const struct gpt_part *p = &g_parts[part];
	/* Clamp to the partition.  A firmware file whose FAT length disagrees with
	 * the GPT used to run past `last` and read whatever LBA followed, which the
	 * controller reports as a host error for every later command as well. */
	uint64_t part_bytes = (p->last - p->first + 1) * (uint64_t)p->sector;
	if (offset >= part_bytes) return -1;
	if (len > part_bytes - offset) {
		printf("HAKIOS_WLAN: read overruns %s: off %llu len %llu, clamped to %llu\n",
		    p->name, (unsigned long long)offset, (unsigned long long)len,
		    (unsigned long long)(part_bytes - offset));
		len = part_bytes - offset;
	}
	uint64_t byte = p->first * p->sector + offset;
	uint64_t lba4k = byte / 4096;
	unsigned skip = (unsigned)(byte % 4096);
	static uint8_t block[4096];   /* never on a 16 KiB kernel stack */

	if (skip != 0) {
		if (read4k(p->lun, lba4k, block) != 0) return -1;
		uint64_t n = 4096 - skip;
		if (n > len) n = len;
		memcpy(dst, block + skip, (size_t)n);
		dst += n;
		len -= n;
		lba4k++;
	}
	while (len > 0) {
		uint32_t blocks = (uint32_t)((len + 4095) / 4096);
		if (blocks > 32) blocks = 32;
		uint32_t bytes = blocks * 4096;
		if (len < bytes) {
			/* Short tail: the staging buffer is one block, so bounce a
			 * single block at a time. Handing `blocks` to the UFS hook
			 * here used to copy up to 128 KiB into a 4 KiB buffer and
			 * flood the neighbouring .bss with modem-image bytes. */
			uint32_t n = len < 4096 ? (uint32_t)len : 4096u;
			if (read4k(p->lun, lba4k, block) != 0) return -1;
			memcpy(dst, block, n);
			dst += n;
			len -= n;
			lba4k++;
			continue;
		}
		if (native_ufs_raw_read(p->lun, lba4k, blocks, dst) != 0) return -1;
		/* Same pacing as read4k(), for the 128 KiB batch path. */
		ufs_io_tuning();
		if (g_ufs_slow_ms) IOSleep(g_ufs_slow_ms);
		dst += bytes;
		len -= bytes;
		lba4k += blocks;
	}
	return 0;
}

extern "C" int
hakios_ufs_read_partition(const char *name, uint64_t offset, uint8_t *dst, uint64_t len, uint64_t *bytes)
{
	uint64_t first = 0, nblocks = 0;
	uint32_t idx = 0;
	if (hakios_ufs_find_part(name, &first, &nblocks, &idx) != 0) return -1;
	uint32_t sector = g_parts[idx].sector;
	uint64_t total = nblocks * sector;
	if (bytes) *bytes = total;
	if (dst == NULL || len == 0) return 0;
	if (offset >= total) return -1;
	if (len > total - offset) len = total - offset;
	return hakios_ufs_read_part(idx, offset, dst, len);
}

/* hakios_mpss_rails() and hakios_aop_mailbox_send() live in HakiosQmp.cpp:
 * the real RPMh votes and the AOP QMP mailbox. */

/*
 * picasso_usb.c (the XNU console patch) keeps its poller static, but the WLAN
 * driver's long PAS/QRTR waits call it to keep the CDC ring drained. Weak empty
 * definitions keep the static archive link whole; the XNU patch's own copy is
 * used inside that translation unit, and this one is only the external name.
 */
extern "C" __attribute__((weak)) void
picasso_usb_console_drive_poll(void)
{
}

extern "C" __attribute__((weak)) void
picasso_usb_reboot_fastboot(void)
{
}

/* hakios.net.c initialises the virtio link it knows; this tree has its own. */
extern "C" void hakios_net_init(void);
extern "C" void picasso_modem_init(void);
extern "C" int picasso_wlan_smmu_claim(void);

static void
picasso_wlan_boot_thread(void *arg, wait_result_t wr)
{
	(void)arg;
	(void)wr;
	uint32_t tries;

	/* Claim stream 0x20 while the Apps SMMU still accepts stream-match
	 * writes.  The window opens when the console driver brings the SMMU up
	 * a few hundred milliseconds into boot and is closed again long before
	 * the modem comes up, so retry instead of guessing a time. */
	for (tries = 0; tries < 240; tries++) {
		if (picasso_wlan_smmu_claim() == 0) break;
		IOSleep(50);
	}
	if (tries == 240) {
		printf("PICASSO_WLAN: wlan smmu: SID 20 never accepted a claim\n");
	}

	/* Leave bsd_init alone: ifnet_allocate_extended needs the network stack
	 * settled, and a blocking WCN3990 bring-up here would keep the CDC console
	 * from ever starting. Five seconds is past the root publication.
	 * wlan_delay_ms moves the whole bring-up later, which keeps the ~30 MiB
	 * firmware pull off the UFS host while init is still exec'ing launchd. */
	uint32_t delay_ms = 70000;
	PE_parse_boot_argn("wlan_delay_ms", &delay_ms, sizeof(delay_ms));
	if (delay_ms < 1000) delay_ms = 1000;
	IOSleep(delay_ms);
	printf("PICASSO_WLAN: deferred init thread: starting en0 and WCN3990\n");
	hakios_net_init();
	picasso_modem_init();
	printf("PICASSO_WLAN: deferred init thread: returned\n");
	thread_terminate(current_thread());
}

extern "C" void
picasso_wlan_deferred_start(void)
{
	/* Must happen before the deferred thread: the stream-match rewrite is only
	 * safe while no other master is doing DMA, i.e. at the top of bsd_init. */
	picasso_wlan_smmu_early();

	thread_t thread;
	if (kernel_thread_start(picasso_wlan_boot_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("PICASSO_WLAN: cannot start the deferred init thread\n");
	}
}


extern "C" void
hakios_virtio_net_init(void)
{
}

/* hakios.sta.c's "say it even with wifi=1" channel. */
extern "C" int
hakios_wifi_tell(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	vprintf(format, ap);
	va_end(ap);
	return 0;
}
