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
#include <mach/mach_types.h>
#include <kern/kalloc.h>
#include <sys/errno.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/vfs_context.h>
#include <kern/locks.h>
#include <mach/vm_param.h>
#include <sys/hakios.ipa.h>
#include <sys/hakios.nas.h>

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
 * stack links. The four hakios_* locks are already defined in hakios.wifi.c
 * (the archive's own object), and mdevlookup comes from XNU's bsd/memdev.o --
 * defining them here again would be duplicate symbols. Only the truly missing
 * trace/loader helpers stay here. */
void loader_log_putc(char c) { printf("%c", c); }
void loader_return(void) { }
int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size)
{
	(void)dev; (void)base; (void)size; return -1;
}

/* More raphael-patch symbols referenced by the WiFi files. */
void hakios_spmi_lock(void) { }
void hakios_spmi_unlock(void) { }
int hakios_ufs_read_partition(const char *name, uint64_t offset, uint8_t *dst,
                              uint64_t len, uint64_t *bytes)
{
	(void)name; (void)offset; (void)dst; (void)len;
	if (bytes) *bytes = 0;
	return -1;
}
void hakios_virtio_net_init(void) { }  /* QEMU-only, not on picasso */

/* The parent sysctl node _kern_hakios that raphael's patched XNU provided.
 * mpss.c registers cellular_* sysctls under it via SYSCTL_INT. */
SYSCTL_DECL(_kern_hakios);
SYSCTL_NODE(_kern, OID_AUTO, hakios, CTLFLAG_RD, 0, "hakios");

/* bsd_init.c's existing hook calls this to start the WiFi stack. In the
 * raphael build it starts raphael's self-contained wifi=1 bring-up instead of
 * the self-developed stack. The display module also probes WiFi presence. */
extern void hakios_wifi_init(void);
void picasso_wlan_deferred_start(void) { hakios_wifi_init(); }
void native_wlan_probe(void) { }

/* raphael's hakios.image.c reads firmware from an APFS payload RAM disk through
 * mdevlookup()/mdevgetkernelrange(); picasso has no such RAM disk at link time.
 * Use the mounted root instead. The order matches where Android keeps the
 * WCN3990 images; /fw is the raphael layout and is tried first. */
const uint8_t *
hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out)
{
	static const char * const prefixes[] = {
		"/fw/%s",
		"/readonly/vendor/firmware_mnt/image/%s",
		"/readonly/firmware/image/%s",
		"/readonly/vendor/firmware/%s",
	};
	vfs_context_t context = vfs_context_kernel();
	char path[256];

	(void)dir;
	if (name == NULL || size == NULL) {
		return NULL;
	}
	for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		vnode_t vnode = NULLVP;
		struct vnode_attr attributes;
		uint8_t *data;
		int left = 0, error;

		snprintf(path, sizeof(path), prefixes[i], name);
		if (vnode_lookup(path, 0, &vnode, context) != 0) {
			continue;
		}
		VATTR_INIT(&attributes);
		VATTR_WANTED(&attributes, va_data_size);
		if (vnode_getattr(vnode, &attributes, context) != 0 || attributes.va_data_size == 0 ||
		    attributes.va_data_size > 0x4000000) {
			vnode_put(vnode);
			continue;
		}
		data = kalloc_data((size_t)attributes.va_data_size, Z_WAITOK | Z_ZERO);
		if (data == NULL) {
			vnode_put(vnode);
			continue;
		}
		error = vn_rdwr(UIO_READ, vnode, (caddr_t)data, (int)attributes.va_data_size, 0, UIO_SYSSPACE, 0,
		    vfs_context_ucred(context), &left, vfs_context_proc(context));
		vnode_put(vnode);
		if (error != 0 || left != 0) {
			printf("LX: image_file %s: read error %d, %d bytes left\n", path, error, left);
			kfree_data(data, (size_t)attributes.va_data_size);
			continue;
		}
		*size = attributes.va_data_size;
		if (disk_out != NULL) {
			*disk_out = -1;   /* root filesystem, not one of raphael's RAM disks */
		}
		printf("LX: image_file %s: %llu bytes\n", path, (unsigned long long)*size);
		return data;
	}
	printf("LX: image_file: %s not found on the root\n", name);
	return NULL;
}

/* raphael's cellular/IPA/NAS layer belongs to its full handset build. Picasso's
 * WiFi path only needs the modem to reach WLFW; the data network is not carried
 * over these stubs. Returning false from IPA setup keeps the cellular attach
 * path disabled while letting mpss.c finish its bring-up. */
bool
hakios_ipa_data_setup(void)
{
	printf("LX: ipa data setup skipped on picasso (WiFi-only)\n");
	return false;
}

void
hakios_ipa_modem_endpoints(uint32_t *to_modem, uint32_t *from_modem)
{
	(void)to_modem;
	(void)from_modem;
}

void
hakios_cell_session(bool up, uint32_t address, uint32_t mask, uint32_t gateway, uint32_t dns1, uint32_t dns2, uint32_t mtu)
{
	(void)up; (void)address; (void)mask; (void)gateway; (void)dns1; (void)dns2; (void)mtu;
}

int
hakios_nas_client_open(struct hakios_nas_client *client, const struct hakios_qrtr_ops *transport,
    void (*changed)(void *, const struct hakios_nas_state *), void *context)
{
	(void)transport; (void)changed; (void)context;
	if (client != NULL) {
		client->connected = false;
	}
	printf("LX: nas client not started on picasso (WiFi-only)\n");
	return 0;   /* mpss.c's step_nas must complete even without cellular */
}

int
hakios_nas_client_poll(struct hakios_nas_client *client)
{
	(void)client;
	return 0;
}

void
hakios_nas_client_close(struct hakios_nas_client *client)
{
	(void)client;
}

void
hakios_cell_attach(void)
{
	printf("LX: cellular attach skipped on picasso (WiFi-only)\n");
}

/* raphael's GICv3 driver lives in its patched XNU, not in the stock picasso
 * kernel. These stubs let LinuxKPI's irq layer link; picasso's WiFi path still
 * has to be checked on hardware for whether CE completions arrive. They are
 * deliberately loud so a missing interrupt is visible in the serial log. */
kern_return_t
gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context)
{
	(void)edge; (void)handler; (void)context;
	printf("LX: GIC SPI %u attach stubbed (no picasso interrupt handler)\n", intid);
	return KERN_SUCCESS;
}

void
gic_v3_spi_mask(uint32_t intid)
{
	(void)intid;
}

void
gic_v3_spi_unmask(uint32_t intid)
{
	(void)intid;
}

void
gic_v3_spi_detach(uint32_t intid)
{
	(void)intid;
}

uint64_t
gic_v3_spi_count(uint32_t intid)
{
	(void)intid;
	return 0;
}
