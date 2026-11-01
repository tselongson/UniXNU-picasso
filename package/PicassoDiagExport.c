// SPDX-License-Identifier: MIT
// Drive BootDiagExport's stream into the UFS export region.
//
// BootDiagExport.c walks the userspace log, the crash reports and backboardd's own cache directory
// and hands them out through one callback. In the sheng port that callback is inside BootLog.c,
// which is 1454 lines of kernel code that assumes the kernel build's full flag set and does not
// compile as a standalone unit. Only the callback is needed here, so it is written on its own:
// the stream goes into the export region NativeUfs already provides, in 4 KiB blocks, behind the
// header the host-side reader expects.
//
// Boot argument: sheng-diag-export=<seconds>, the delay before the stream is taken. Absent or 0
// leaves it off, so this costs nothing until asked for.
#include <sys/param.h>
#include <sys/systm.h>
#include <pexpert/pexpert.h>

#define DIAG_BLOCK 4096u
#define DIAG_HEADER 12u          // "SHEXR001", u32 stream bytes, u32 0

extern uint64_t native_ufs_export_blocks(void) __attribute__((weak_import));
extern int native_ufs_export_write(uint64_t rel, uint32_t blocks, const void *src)
    __attribute__((weak_import));
extern int native_ufs_log_open(uint64_t *firstLba, uint64_t *blocks, const char **why)
    __attribute__((weak_import));
extern int boot_diag_stream(int (*emit)(const void *, uint32_t, void *), void *arg)
    __attribute__((weak_import));

static uint8_t diag_block[DIAG_BLOCK];
static uint32_t diag_fill;          // bytes pending in diag_block
static uint64_t diag_block_index;   // next block to write
static uint32_t diag_written;       // bytes handed to the region, header included
static int diag_result = -1;
static int diag_done;

volatile uint32_t picasso_diag_bytes, picasso_diag_files, picasso_diag_failed;

static int flush_block(void)
{
	if (!diag_fill) {
		return 0;
	}
	memset(diag_block + diag_fill, 0, DIAG_BLOCK - diag_fill);
	int error = native_ufs_export_write(diag_block_index, 1, diag_block);
	if (error) {
		++picasso_diag_failed;
		return error;
	}
	++diag_block_index;
	diag_fill = 0;
	return 0;
}

static int diag_emit(const void *data, uint32_t length, void *arg)
{
	(void)arg;
	const uint8_t *p = data;
	diag_written += length;
	picasso_diag_bytes = diag_written;
	while (length) {
		uint32_t room = DIAG_BLOCK - diag_fill;
		uint32_t take = length < room ? length : room;
		memcpy(diag_block + diag_fill, p, take);
		diag_fill += take;
		p += take;
		length -= take;
		if (diag_fill == DIAG_BLOCK) {
			int error = flush_block();
			if (error) {
				return error;
			}
			++picasso_diag_files;
		}
	}
	return 0;
}

static void diag_export(void)
{
	// The export region only exists once the log window is open, and opening it is what BootLog.c
	// did in the sheng port; this driver does it directly so the 1454-line file is not needed.
	if (native_ufs_log_open) {
		uint64_t first = 0, blocks = 0;
		const char *why = NULL;
		int opened = native_ufs_log_open(&first, &blocks, &why);
		if (opened) {
			printf("PICASSO_DIAG: log window did not open: %d %s\n", opened, why ? why : "");
		}
	}
	uint64_t blocks = native_ufs_export_blocks();
	if (!blocks) {
		printf("PICASSO_DIAG: no export region; the log window must open first\n");
		diag_result = -2;
		return;
	}
	if (!boot_diag_stream) {
		printf("PICASSO_DIAG: BootDiagExport is not linked in\n");
		diag_result = -3;
		return;
	}
	// The reader expects the header first, with the stream length filled in afterwards; it is
	// written once with a zero length and patched at the end, because the length is only known
	// after the walk.
	uint8_t header[DIAG_HEADER];
	memcpy(header, "SHEXR001", 8);
	memset(header + 8, 0, 4);
	diag_fill = 0;
	memcpy(diag_block, header, DIAG_HEADER);
	diag_fill = DIAG_HEADER;
	diag_written = DIAG_HEADER;

	int error = boot_diag_stream(diag_emit, NULL);
	if (!error) {
		error = flush_block();
	}
	if (!error) {
		uint32_t stream = diag_written - DIAG_HEADER;
		memset(diag_block, 0, DIAG_BLOCK);
		memcpy(diag_block, "SHEXR001", 8);
		memcpy(diag_block + 8, &stream, 4);
		error = native_ufs_export_write(0, 1, diag_block);
	}
	diag_result = error;
	printf("PICASSO_DIAG: export done error=%d bytes=%u blocks=%llu files=%u failed=%u\n",
	       error, diag_written, blocks, picasso_diag_files, picasso_diag_failed);
}

// Called from the display refresh tick. The delay lets the system reach the point where the
// userspace log has something worth reading; a stalled compositor still ticks, which is exactly
// the case this exists to diagnose.
void picasso_diag_tick(unsigned now)
{
	if (diag_done) {
		return;
	}
	unsigned delay = 0;
	if (!PE_parse_boot_argn("sheng-diag-export", &delay, sizeof(delay)) || !delay) {
		diag_done = 1;
		return;
	}
	if (now < delay) {
		return;
	}
	diag_done = 1;
	diag_export();
}
