/* Minimal UFS/GPT partition reader for raphael's firmware loader on picasso.
 *
 * raphael's hakios.image.c reads firmware from its own APFS RAM disk; picasso
 * keeps WCN3990 images in the Android "modem" FAT partition. This module is the
 * partition-reading piece of the proven self-developed path, reused only as a
 * platform I/O helper -- it does not bring back the self-developed WiFi stack.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

extern int native_ufs_raw_read(uint32_t lun, uint64_t lba, uint32_t blocks, void *dst);
extern uint32_t native_ufs_raw_max_blocks(void);
extern void native_ufs_raw_diag(uint32_t *out);

#define GPT_MAX_PARTS 256
#define GPT_NAME_MAX  72

struct gpt_part {
	char     name[GPT_NAME_MAX];
	uint64_t first;
	uint64_t last;
	uint32_t lun;
	uint32_t sector;
};

static struct gpt_part g_parts[GPT_MAX_PARTS];
static int g_scan_talk;
static int g_part_count = -1;
static uint32_t g_ufs_slow_ms;

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

static void
ufs_io_tuning(void)
{
	static bool done;
	uint32_t v = 0;

	if (done) return;
	done = true;
	if (PE_parse_boot_argn("modem_slowio", &v, sizeof(v))) {
		g_ufs_slow_ms = v;
	}
}

static int
read4k(uint32_t lun, uint64_t lba4k, void *dst)
{
	if (native_ufs_raw_max_blocks() == 0) return -1;
	ufs_io_tuning();
	int result = native_ufs_raw_read(lun, lba4k, 1, dst);
	if (g_ufs_slow_ms) delay_for_interval(g_ufs_slow_ms, NSEC_PER_MSEC);
	return result;
}

static uint8_t g_read_block[4096];

static int
read_bytes(uint32_t lun, uint64_t byte, void *dst, uint32_t len)
{
	uint8_t *out = (uint8_t *)dst;

	while (len > 0) {
		const uint64_t blk = byte / 4096u;
		const unsigned off = (unsigned)(byte % 4096u);
		uint32_t n;

		if (read4k(lun, blk, g_read_block) != 0) return -1;
		n = 4096u - off;
		if (n > len) n = len;
		memcpy(out, g_read_block + off, n);
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
	uint32_t entries_count, entry_size;

	if (read4k(lun, 0, block) != 0) return;
	if (memcmp(block + 512, "EFI PART", 8) == 0) {
		memcpy(hdr, block + 512, sizeof(hdr));
		sector = 512;
	} else {
		if (read4k(lun, 1, block) != 0) return;
		if (memcmp(block, "EFI PART", 8) != 0) return;
		memcpy(hdr, block, sizeof(hdr));
		sector = 4096;
	}
	if (le32(hdr + 12) < 92) return;
	entries_byte = le64(hdr + 72) * sector;
	entries_count = le32(hdr + 80);
	entry_size = le32(hdr + 84);
	if (entry_size < 128 || entry_size > sizeof(entry) || entries_count == 0) return;
	if (entries_count > 128) entries_count = 128;

	for (uint32_t i = 0; i < entries_count; i++) {
		int used = 0;
		struct gpt_part *p;

		if (read_bytes(lun, entries_byte + (uint64_t)i * entry_size, entry, entry_size) != 0) return;
		for (int b = 0; b < 16; b++) used |= entry[b];
		if (!used || g_part_count >= GPT_MAX_PARTS) continue;
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
		printf("LX: UFS GPT LUN %u sector=%u entries=%u\n", lun, sector, entries_count);
}

static void
scan_parts(void)
{
	if (g_part_count > 0) return;
	for (unsigned attempt = 0; attempt < 40 && g_part_count <= 0; attempt++) {
		g_part_count = 0;
		g_scan_talk = (attempt == 0);
		for (uint32_t lun = 0; lun < 8; lun++) scan_lun(lun);
		if (g_part_count > 0) break;
		if (attempt == 0) {
			uint32_t diag[5] = { 0, 0, 0, 0, 0 };
			native_ufs_raw_diag(diag);
			printf("LX: UFS GPT not ready (maxblk=%u ready=%u scsi=%x proto=%x ocs=%x cmd=%x)\n",
			    native_ufs_raw_max_blocks(), diag[0], diag[1], diag[2], diag[3], diag[4]);
		}
		delay_for_interval(1000, NSEC_PER_MSEC);
	}
	printf("LX: UFS GPT scan found %d partitions\n", g_part_count);
}

int
picasso_ufs_find_part(const char *name, uint64_t *first, uint64_t *nblocks, uint32_t *part_idx)
{
	scan_parts();
	if (name == NULL) return -1;
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		char wanted[GPT_NAME_MAX];
		int i;

		if (attempt == 0) {
			strlcpy(wanted, name, sizeof(wanted));
		} else {
			snprintf(wanted, sizeof(wanted), "%s_%c", name, attempt == 1 ? 'a' : 'b');
		}
		for (i = 0; i < g_part_count; i++) {
			if (strcmp(g_parts[i].name, wanted) != 0) continue;
			if (first) *first = g_parts[i].first;
			if (nblocks) *nblocks = g_parts[i].last - g_parts[i].first + 1;
			if (part_idx) *part_idx = (uint32_t)i;
			return 0;
		}
	}
	return -1;
}

int
picasso_ufs_read_part(uint32_t part, uint64_t offset, uint8_t *dst, uint64_t len)
{
	static uint8_t block[4096];

	scan_parts();
	if (part >= (uint32_t)g_part_count || dst == NULL) return -1;
	const struct gpt_part *p = &g_parts[part];
	uint64_t part_bytes = (p->last - p->first + 1) * (uint64_t)p->sector;

	if (offset >= part_bytes) return -1;
	if (len > part_bytes - offset) len = part_bytes - offset;
	uint64_t byte = p->first * p->sector + offset;
	uint64_t lba4k = byte / 4096;
	unsigned skip = (unsigned)(byte % 4096);

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
			uint32_t n = len < 4096 ? (uint32_t)len : 4096u;
			if (read4k(p->lun, lba4k, block) != 0) return -1;
			memcpy(dst, block, n);
			dst += n;
			len -= n;
			lba4k++;
			continue;
		}
		if (native_ufs_raw_read(p->lun, lba4k, blocks, dst) != 0) return -1;
		ufs_io_tuning();
		if (g_ufs_slow_ms) delay_for_interval(g_ufs_slow_ms, NSEC_PER_MSEC);
		dst += bytes;
		len -= bytes;
		lba4k += blocks;
	}
	return 0;
}

int
picasso_ufs_read_partition(const char *name, uint64_t offset, uint8_t *dst, uint64_t len, uint64_t *bytes)
{
	uint64_t first = 0, nblocks = 0;
	uint32_t idx = 0;

	if (picasso_ufs_find_part(name, &first, &nblocks, &idx) != 0) return -1;
	uint64_t total = nblocks * (uint64_t)g_parts[idx].sector;
	if (bytes) *bytes = total;
	if (dst == NULL || len == 0) return 0;
	if (offset >= total) return -1;
	if (len > total - offset) len = total - offset;
	return picasso_ufs_read_part(idx, offset, dst, len);
}
