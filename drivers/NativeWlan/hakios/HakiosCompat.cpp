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

extern "C" int native_ufs_raw_read(uint32_t lun, uint64_t lba, uint32_t blocks, void *dst);
extern "C" uint32_t native_ufs_raw_max_blocks(void);

#define GPT_MAX_PARTS 64
#define GPT_NAME_MAX  72

struct gpt_part {
	char     name[GPT_NAME_MAX];
	uint64_t first;          /* first 512-byte LBA */
	uint64_t last;           /* last 512-byte LBA */
	uint32_t lun;
	uint32_t sector;         /* logical sector size the table was read with */
};

static struct gpt_part g_parts[GPT_MAX_PARTS];
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
static int
read4k(uint32_t lun, uint64_t lba4k, void *dst)
{
	if (native_ufs_raw_max_blocks() == 0) return -1;
	return native_ufs_raw_read(lun, lba4k, 1, dst);
}

static void
scan_lun(uint32_t lun)
{
	uint8_t block[4096];
	uint8_t entries[4096];
	uint64_t entries_lba;
	uint32_t entries_count, entry_size, i;
	unsigned sector;

	if (read4k(lun, 0, block) != 0) return;

	/* 512-byte logical sectors put the GPT header at offset 512 of block 0;
	 * 4 KiB logical sectors put it at the start of block 1. */
	if (memcmp(block + 512, "EFI PART", 8) == 0) {
		sector = 512;
	} else if (memcmp(block, "EFI PART", 8) == 0) {
		sector = 4096;
	} else {
		return;
	}

	const uint8_t *hdr = (sector == 512) ? block + 512 : block;
	if (le32(hdr + 12) < 92) return;
	entries_lba = le64(hdr + 72);
	entries_count = le32(hdr + 80);
	entry_size = le32(hdr + 84);
	if (entry_size < 128 || entries_count == 0 || entries_count > 256) return;

	/* Entries are addressed in logical sectors. Convert to the 4 KiB hook. */
	uint64_t entries_byte = entries_lba * sector;
	uint64_t entries_4k = entries_byte / 4096;
	unsigned within = (unsigned)(entries_byte % 4096);
	if (within != 0) return;                        /* tables are 4 KiB aligned here */

	uint32_t per_block = 4096 / entry_size;
	uint32_t done = 0;
	while (done < entries_count && done < 128) {
		uint32_t batch = entries_count - done;
		if (batch > per_block) batch = per_block;
		if (read4k(lun, entries_4k + done / per_block, entries) != 0) return;
		for (i = 0; i < batch; i++) {
			const uint8_t *e = entries + (size_t)i * entry_size;
			int used = 0;
			for (int b = 0; b < 16; b++) used |= e[b];
			if (!used) continue;
			if (g_part_count >= GPT_MAX_PARTS) return;
			struct gpt_part *p = &g_parts[g_part_count];
			memset(p, 0, sizeof(*p));
			p->first = le64(e + 32);
			p->last = le64(e + 40);
			p->lun = lun;
			p->sector = sector;
			gpt_name(e + 56, p->name);
			g_part_count++;
		}
		done += batch;
		entries_4k++;
	}
}

static void
scan_parts(void)
{
	if (g_part_count >= 0) return;
	g_part_count = 0;
	for (uint32_t lun = 0; lun < 6; lun++) {
		scan_lun(lun);
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
	for (int i = 0; i < g_part_count; i++) {
		if (strcmp(g_parts[i].name, name) != 0) continue;
		if (first) *first = g_parts[i].first;
		if (nblocks) *nblocks = g_parts[i].last - g_parts[i].first + 1;
		if (part_idx) *part_idx = (uint32_t)i;
		return 0;
	}
	return -1;
}

extern "C" int
hakios_ufs_read_part(uint32_t part, uint64_t offset, uint8_t *dst, uint64_t len)
{
	scan_parts();
	if (part >= (uint32_t)g_part_count || dst == NULL) return -1;
	const struct gpt_part *p = &g_parts[part];
	uint64_t byte = p->first * p->sector + offset;
	uint64_t lba4k = byte / 4096;
	unsigned skip = (unsigned)(byte % 4096);
	uint8_t block[4096];

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
		uint8_t *target = dst;
		if (len < bytes) {
			if (native_ufs_raw_read(p->lun, lba4k, blocks, block) != 0) return -1;
			memcpy(dst, block, (size_t)len);
			return 0;
		}
		if (native_ufs_raw_read(p->lun, lba4k, blocks, target) != 0) return -1;
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

extern "C" bool
hakios_mpss_rails(uint32_t level)
{
	(void)level;
	return true;
}

extern "C" bool
hakios_aop_mailbox_send(const char *text)
{
	(void)text;
	return true;
}
