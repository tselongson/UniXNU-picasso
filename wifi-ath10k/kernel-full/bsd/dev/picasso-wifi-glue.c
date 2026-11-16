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
