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
#include <kern/kalloc.h>
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

/* ---- FAT12/16 reader for the Android "modem" partition ------------------- */

struct fat_vol {
	uint32_t part, bps, spc, reserved, nfats, fatsz, rootents, rootclus, bits;
	uint64_t fat_sec, root_sec, data_sec;
	uint8_t *fat;
	uint32_t fat_bytes;
};
static struct fat_vol g_fat;
static bool g_fat_ready, g_fat_tried;

static int
fat_mount(struct fat_vol *v)
{
	uint8_t boot[512];
	uint32_t total16, total32;

	if (picasso_ufs_find_part("modem", NULL, NULL, &v->part) != 0) return -1;
	if (picasso_ufs_read_part(v->part, 0, boot, sizeof(boot)) != 0) return -1;
	if (boot[510] != 0x55 || boot[511] != 0xaa) return -1;
	v->bps = le16(boot + 11);
	v->spc = boot[13];
	v->reserved = le16(boot + 14);
	v->nfats = boot[16];
	v->rootents = le16(boot + 17);
	total16 = le16(boot + 19);
	v->fatsz = le16(boot + 22);
	total32 = le32(boot + 32);
	if (v->fatsz == 0) v->fatsz = le32(boot + 36);
	v->rootclus = le32(boot + 44);
	v->bits = v->rootents == 0 ? 32 : 16;
	if (v->bps < 512 || v->bps > 4096 || v->spc == 0 || v->fatsz == 0 ||
	    v->fatsz > 2048 || (total16 == 0 && total32 == 0)) return -1;
	v->fat_sec = v->reserved;
	v->root_sec = v->reserved + (uint64_t)v->nfats * v->fatsz;
	v->data_sec = v->root_sec + ((uint64_t)v->rootents * 32 + v->bps - 1) / v->bps;
	v->fat_bytes = v->fatsz * v->bps;
	if (v->fat_bytes > 4u * 1024u * 1024u) return -1;
	v->fat = kalloc_data(v->fat_bytes, Z_WAITOK | Z_ZERO);
	if (v->fat == NULL) return -1;
	if (picasso_ufs_read_part(v->part, v->fat_sec * v->bps, v->fat, v->fat_bytes) != 0) {
		kfree_data(v->fat, v->fat_bytes);
		v->fat = NULL;
		return -1;
	}
	return 0;
}

static uint32_t
fat_next(const struct fat_vol *v, uint32_t clus)
{
	if (v->bits == 16) return (uint32_t)le16(v->fat + (size_t)clus * 2);
	return le32(v->fat + (size_t)clus * 4) & 0x0fffffffu;
}

static int
fat_read_chain(const struct fat_vol *v, uint32_t clus, uint8_t *dst, uint32_t len)
{
	uint32_t per = v->spc * v->bps;

	for (unsigned guard = 0; guard < 65536 && clus >= 2 && len > 0; guard++) {
		uint64_t byte = (v->data_sec + (uint64_t)(clus - 2) * v->spc) * v->bps;
		uint32_t n = len < per ? len : per;

		if (picasso_ufs_read_part(v->part, byte, dst, n) != 0) return -1;
		dst += n;
		len -= n;
		clus = fat_next(v, clus);
		if (clus >= 0x0ffffff0u || clus == 0) break;
	}
	return len == 0 ? 0 : -1;
}

static void
short_name(const uint8_t entry[11], char out[16])
{
	unsigned n = 0, i;

	for (i = 0; i < 8 && n < 12; i++) {
		if (entry[i] == ' ') break;
		out[n++] = (char)entry[i];
	}
	if (entry[8] != ' ' && n < 13) {
		out[n++] = '.';
		for (i = 8; i < 11 && n < 15; i++) {
			if (entry[i] == ' ') break;
			out[n++] = (char)entry[i];
		}
	}
	out[n] = 0;
}

static int
name_equal(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		char ca = *a, cb = *b;
		if (ca >= 'A' && ca <= 'Z') ca += 32;
		if (cb >= 'A' && cb <= 'Z') cb += 32;
		if (ca != cb) return 0;
	}
	return *a == 0 && *b == 0;
}

static void
lfn_prepend(const uint8_t entry[32], char *buf, size_t max)
{
	static const unsigned at[] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28 };
	char chunk[16];
	unsigned n = 0;

	for (unsigned i = 0; i < 14 && n < sizeof(chunk) - 1; i++) {
		uint16_t c = le16(entry + at[i]);
		if (c == 0 || c == 0xffff) break;
		if (c < 0x80) chunk[n++] = (char)c;
	}
	chunk[n] = 0;
	size_t used = strlen(buf), add = strlen(chunk);
	if (used + add >= max) return;
	memmove(buf + add, buf, used + 1);
	memcpy(buf, chunk, add);
}

static int
fat_lookup(const struct fat_vol *v, const char *name, uint32_t *clus, uint32_t *size)
{
	uint8_t *dir = NULL;
	uint32_t dir_bytes = 0;
	uint64_t dir_byte = 0;
	char pending[128] = "";
	int result = -1;

	if (v->rootents != 0) {
		dir_bytes = v->rootents * 32;
		dir_byte = v->root_sec * v->bps;
	} else {
		uint32_t c = v->rootclus, clusters = 0;
		while (c >= 2 && c < 0x0ffffff0u && clusters < 4096) {
			clusters++;
			c = fat_next(v, c);
		}
		if (clusters == 0) return -1;
		dir_bytes = clusters * v->spc * v->bps;
	}
	if (dir_bytes == 0 || dir_bytes > 8u * 1024u * 1024u) return -1;
	dir = kalloc_data(dir_bytes, Z_WAITOK | Z_ZERO);
	if (dir == NULL) return -1;
	if (v->rootents != 0) {
		if (picasso_ufs_read_part(v->part, dir_byte, dir, dir_bytes) != 0) goto out;
	} else if (fat_read_chain(v, v->rootclus, dir, dir_bytes) != 0) {
		goto out;
	}
	for (uint32_t off = 0; off + 32 <= dir_bytes; off += 32) {
		const uint8_t *entry = dir + off;
		char effective[128];
		uint32_t entry_clus;
		uint8_t attr;

		if (entry[0] == 0) break;
		if (entry[0] == 0xe5) { pending[0] = 0; continue; }
		attr = entry[11];
		if (attr == 0x0f) {
			if ((entry[0] & 0x40) != 0) pending[0] = 0;
			lfn_prepend(entry, pending, sizeof(pending));
			continue;
		}
		if ((attr & 0x08) != 0) { pending[0] = 0; continue; }
		if (pending[0] != 0) {
			strlcpy(effective, pending, sizeof(effective));
		} else {
			short_name(entry, effective);
		}
		pending[0] = 0;
		entry_clus = le16(entry + 26);
		if (v->bits == 32) entry_clus |= (uint32_t)le16(entry + 20) << 16;
		if (name_equal(effective, name)) {
			*clus = entry_clus;
			*size = le32(entry + 28);
			result = 0;
			break;
		}
	}
out:
	kfree_data(dir, dir_bytes);
	return result;
}

const uint8_t *
picasso_fw_get(const char *name, uint64_t *size)
{
	uint32_t clus = 0, bytes = 0;
	uint8_t *data;

	if (name == NULL || size == NULL) return NULL;
	if (!g_fat_ready) {
		if (g_fat_tried) return NULL;
		g_fat_tried = true;
		if (fat_mount(&g_fat) != 0) {
			printf("LX: firmware FAT: cannot mount modem partition\n");
			return NULL;
		}
		g_fat_ready = true;
	}
	if (fat_lookup(&g_fat, name, &clus, &bytes) != 0 || bytes == 0 || bytes > 64u * 1024u * 1024u) {
		printf("LX: firmware FAT: %s not found\n", name);
		return NULL;
	}
	data = kalloc_data(bytes, Z_WAITOK | Z_ZERO);
	if (data == NULL) return NULL;
	if (fat_read_chain(&g_fat, clus, data, bytes) != 0) {
		kfree_data(data, bytes);
		printf("LX: firmware FAT: %s read failed\n", name);
		return NULL;
	}
	*size = bytes;
	printf("LX: firmware FAT: %s %u bytes\n", name, bytes);
	return data;
}
