/* picasso platform glue for raphael's kernel-side WiFi stack (route 2).
 *
 * raphael's hakios.input.c provided two globals used by the WiFi files:
 *   hakios_is_raphael()  - choose the SM8150 SNOC/ath10k branch
 *   hakios_tl()          - traced log line
 * We do not compile hakios.input.c here (it also owns touchscreen init, which
 * would collide with picasso's Novatek touch driver). Instead this file gives
 * picasso equivalents:
 *   - SM7250 is the same WCN3990 SNOC family as SM8150 raphael, so the WiFi
 *     stack must take the raphael (ath10k/polled/APCS) branch, not the redwood
 *     (WCN6750 ath11k) branch. Return true.
 *   - Route trace lines to the kernel printf (loader_log_line is raphael-only).
 *
 * Platform constants (cmd-db/MSA/SMEM/BDF/rail/IRQ) are patched in-place in
 * the raphael sources to picasso's values (self-developed stack was the
 * reference); see the raphael-port patch applied to wifi-ath10k/kernel-full.
 */
#include <stdarg.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <kern/locks.h>
#include <vm/vm_param.h>

int
hakios_is_raphael(void)
{
	return 1;  /* picasso SM7250: take the WCN3990 SNOC / ath10k branch */
}

void
hakios_tl(const char *format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	printf("LX: %s\n", line);
}

/* raphael's patched XNU supplied these globals/functions to its hakios.*.c
 * files; picasso's XNU does not. Provide safe equivalents so the raphael WiFi
 * stack links. The locks only serialize raphael's own accesses; the loader /
 * memdev helpers are trace/fallback paths not used by the main data path. */
static LCK_GRP_DECLARE(lx_glue_lock_group, "raphael-wifi-glue");
LCK_MTX_DECLARE(hakios_rsc_lock, &lx_glue_lock_group);
LCK_MTX_DECLARE(hakios_aop_mailbox_lock, &lx_glue_lock_group);
LCK_MTX_DECLARE(hakios_smem_alloc_lock, &lx_glue_lock_group);
LCK_MTX_DECLARE(hakios_smmu_lock, &lx_glue_lock_group);

void loader_log_putc(char c) { printf("%c", c); }
void loader_return(void) { }
dev_t mdevlookup(int devid) { (void)devid; return (dev_t)-1; }
int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size)
{
	(void)dev; (void)base; (void)size; return -1;
}
