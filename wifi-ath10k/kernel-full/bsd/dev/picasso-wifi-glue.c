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
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
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
#include <vm/pmap.h>

extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
/* Without this the compiler takes kvtophys() to return int, and an address above 0x80000000
 * comes back sign-extended: 0xccecf000 arrived as 0xffffffffccecf000 and every check on it
 * failed while the memory was in fact a perfectly good 3.2 GB page. */
extern uint64_t kvtophys(vm_offset_t va);
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
extern int picasso_ufs_find_part(const char *name, uint64_t *first, uint64_t *nblocks, uint32_t *part_idx);
extern int picasso_ufs_read_part(uint32_t part, uint64_t offset, uint8_t *dst, uint64_t len);
extern int picasso_ufs_read_partition(const char *name, uint64_t offset, uint8_t *dst, uint64_t len, uint64_t *bytes);
extern const uint8_t *picasso_fw_get(const char *name, uint64_t *size);

int
hakios_ufs_find_part(const char *name, uint64_t *first, uint64_t *nblocks, uint32_t *part_idx)
{
	return picasso_ufs_find_part(name, first, nblocks, part_idx);
}

int
hakios_ufs_read_part(uint32_t part, uint64_t offset, uint8_t *dst, uint64_t len)
{
	return picasso_ufs_read_part(part, offset, dst, len);
}

int
hakios_ufs_read_partition(const char *name, uint64_t offset, uint8_t *dst,
                          uint64_t len, uint64_t *bytes)
{
	return picasso_ufs_read_partition(name, offset, dst, len, bytes);
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

/* bsd_init must not block on the WiFi bring-up: the CDC console and launchd
 * start afterwards, and the self-developed stack learned this the hard way
 * (a synchronous start left no serial port at all). Delay onto a kernel
 * thread; wlan_delay_ms overrides the 150 s default for experiments. */
static void
picasso_wlan_boot(void *argument, wait_result_t result)
{
	uint32_t delay_ms = 150000;

	(void)argument;
	(void)result;
	PE_parse_boot_argn("wlan_delay_ms", &delay_ms, sizeof(delay_ms));
	if (delay_ms < 1000) delay_ms = 1000;
	delay_for_interval(delay_ms, NSEC_PER_MSEC);
	printf("LX: raphael WiFi deferred init starting after %u ms\n", delay_ms);
	hakios_wifi_init();
	printf("LX: raphael WiFi deferred init returned\n");
	thread_terminate(current_thread());
}

extern int hakios_lx_prepare(void);

/* The modem's rmtfs window: 3 MiB of physical memory below 4 GiB that it reads and writes
 * through 32-bit addresses. Taken here, at bsd_init, for the same reason the radio's 44 MB
 * is: by the time the deferred thread runs (150 s in) the kernel's memory is in pieces and no
 * 3 MiB run exists any more ("no memory in one piece below 4 GiB for 3145728 bytes"). */
#define HAKIOS_RMTFS_BYTES      0x300000u
static vm_offset_t hakios_rmtfs_buffer;
static uint64_t hakios_rmtfs_physical;

void *
hakios_rmtfs_memory(uint64_t *physical)
{
	*physical = hakios_rmtfs_physical;
	return (void *)hakios_rmtfs_buffer;
}

static void
picasso_rmtfs_reserve(void)
{
	if (hakios_rmtfs_buffer != 0) {
		return;
	}
	/* max_pnum is the fifth argument in this kernel: 0x100000 pages = 4 GiB. KMA_KOBJECT
	 * is what makes kvtophys() answer for it at all -- lx_os_radio_memory() in hakios.lx.c
	 * takes the radio's 44 MB the same way, and a plain KMA_DATA mapping lives outside the
	 * direct-mapped window where kvtophys() returns 0. */
	if (kmem_alloc_contig(kernel_map, &hakios_rmtfs_buffer, HAKIOS_RMTFS_BYTES, PAGE_MASK, (ppnum_t)0x100000, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("LX: no 3 MB in one piece below 4 GiB for the modem's file system\n");
		hakios_rmtfs_buffer = 0;
		return;
	}
	hakios_rmtfs_physical = kvtophys(hakios_rmtfs_buffer);
	if (hakios_rmtfs_physical == 0) {
		/* A kmem_alloc_contig mapping is not in the kernel's direct-mapped window, so
		 * kvtophys() answers 0 for it; the page table still knows the page. */
		ppnum_t page = pmap_find_phys(kernel_pmap, (addr64_t)hakios_rmtfs_buffer);

		hakios_rmtfs_physical = page != 0 ? ptoa_64(page) : 0;
	}
	if (hakios_rmtfs_physical == 0) {
		printf("LX: the modem's file system memory at %#llx has no physical address\n",
		    (uint64_t)hakios_rmtfs_buffer);
		hakios_rmtfs_buffer = 0;
		return;
	}
	printf("LX: modem's file system memory: %u bytes at %#llx (physical %#llx)\n", HAKIOS_RMTFS_BYTES,
	    (uint64_t)hakios_rmtfs_buffer, hakios_rmtfs_physical);
}

void
picasso_wlan_deferred_start(void)
{
	thread_t thread;

	/* raphael's ath10k shows the radio 44 MB of memory in one physical piece
	 * (lx_pool_init -> kmem_alloc_contig). Raphael took it while its system
	 * was still empty; picasso reaches this hook at bsd_init and then waits
	 * 150 s, by which time SpringBoard owns the memory and a 44 MB
	 * contiguous alloc fails ("NO MEMORY FOR THE RADIO in one piece").
	 * Reserve the pool here, before anything else runs; the deferred thread
	 * finds pool.memory already set and reuses it. */
	if (hakios_lx_prepare() != 0) {
		printf("LX: could not reserve the radio memory at bsd_init\n");
	} else {
		printf("LX: radio memory reserved at bsd_init\n");
	}
	picasso_rmtfs_reserve();

	if (kernel_thread_start(picasso_wlan_boot, NULL, &thread) != KERN_SUCCESS) {
		printf("LX: cannot start raphael WiFi deferred init thread\n");
		return;
	}
	thread_deallocate(thread);
}
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
		/* ath10k's WCN3990 host metadata (firmware-5.bin) rides in the Data seed
		 * (skip-setup --inject-fw), which mounts at /private/var. */
		"/private/var/picasso/fw/%s",
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
	/* The Android modem FAT partition is where the working picasso stack has
	 * always found bdwlan.bin / firmware-5.bin / wlanmdsp.mbn. Try it before
	 * the root filesystem; the latter is only a fallback. */
	{
		const uint8_t *firmware = picasso_fw_get(name, size);

		if (firmware != NULL) {
			printf("LX: image_file %s: %llu bytes from modem FAT\n", name,
			    (unsigned long long)*size);
			if (disk_out != NULL) *disk_out = -1;
			return firmware;
		}
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
 * kernel. Instead of dead stubs (which leave ath10k with no CE completions),
 * emulate the SPI line with a 1 ms polling thread: the handler raphael gives
 * us is lx_irq_arrived(), which only has to set a pending bit and wake the
 * existing lx_irq_runs thread. This keeps the whole LinuxKPI IRQ path intact
 * without touching picasso's live interrupt controller. */
#define GIC_POLL_SLOTS 64
struct gic_poll_slot {
	uint32_t intid;
	void   (*handler)(void *context, uint32_t intid);
	void    *context;
	volatile bool enabled;
	uint64_t count;
};
static struct gic_poll_slot gic_poll_slots[GIC_POLL_SLOTS];
static thread_t gic_poll_thread;

static void
gic_poll_runs(__unused void *argument, __unused wait_result_t result)
{
	for (;;) {
		for (unsigned i = 0; i < GIC_POLL_SLOTS; i++) {
			struct gic_poll_slot *slot = &gic_poll_slots[i];

			if (slot->enabled && slot->handler != NULL) {
				slot->count++;
				slot->handler(slot->context, slot->intid);
			}
		}
		delay_for_interval(1000, NSEC_PER_USEC);
	}
}

static struct gic_poll_slot *
gic_poll_find(uint32_t intid)
{
	for (unsigned i = 0; i < GIC_POLL_SLOTS; i++) {
		if (gic_poll_slots[i].intid == intid) {
			return &gic_poll_slots[i];
		}
	}
	return NULL;
}

kern_return_t
gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context)
{
	struct gic_poll_slot *slot;

	(void)edge;
	if (handler == NULL) return KERN_INVALID_ARGUMENT;
	if ((slot = gic_poll_find(intid)) == NULL) {
		for (unsigned i = 0; i < GIC_POLL_SLOTS; i++) {
			if (gic_poll_slots[i].intid == 0) {
				slot = &gic_poll_slots[i];
				break;
			}
		}
	}
	if (slot == NULL) return KERN_RESOURCE_SHORTAGE;
	slot->intid = intid;
	slot->handler = handler;
	slot->context = context;
	slot->count = 0;
	slot->enabled = true;
	if (gic_poll_thread == THREAD_NULL) {
		if (kernel_thread_start(gic_poll_runs, NULL, &gic_poll_thread) != KERN_SUCCESS) {
			gic_poll_thread = THREAD_NULL;
			return KERN_FAILURE;
		}
		thread_set_thread_name(gic_poll_thread, "wifi-gic-poll");
		thread_deallocate(gic_poll_thread);
	}
	printf("LX: GIC SPI %u attached to 1 ms poller\n", intid);
	return KERN_SUCCESS;
}

void
gic_v3_spi_mask(uint32_t intid)
{
	struct gic_poll_slot *slot = gic_poll_find(intid);
	if (slot != NULL) slot->enabled = false;
}

void
gic_v3_spi_unmask(uint32_t intid)
{
	struct gic_poll_slot *slot = gic_poll_find(intid);
	if (slot != NULL) slot->enabled = true;
}

void
gic_v3_spi_detach(uint32_t intid)
{
	struct gic_poll_slot *slot = gic_poll_find(intid);
	if (slot != NULL) {
		slot->enabled = false;
		slot->handler = NULL;
		slot->intid = 0;
	}
}

uint64_t
gic_v3_spi_count(uint32_t intid)
{
	struct gic_poll_slot *slot = gic_poll_find(intid);
	return slot != NULL ? slot->count : 0;
}
