/*
 * hakios.dhcp.c -- a tiny in-kernel DHCPv4 client for en0.
 *
 * Why this exists
 * ---------------
 * Everything below the IP layer is already working on this port: WCN3990
 * firmware, CE/HTC/WMI/HTT, scan, auth, assoc, VDEV_UP, WPA2 four-way
 * handshake, PTK/GTK, PEER_AUTHORIZE and real Ethernet frames into
 * ifnet_input(). What never happens is configd creating an IPv4 service:
 * SIOCSIFADDR / DHCP / inet have been 0 in every boot since chain-580.
 *
 * The user's requirement is "WiFi must work, by any means". Waiting for
 * configd's InterfaceNamer -> IOKit quiet -> PreferencesMonitor ->
 * IPConfiguration chain means reverse-engineering a closed-source 20H392
 * daemon whose source we only have in a different version. This file takes
 * the other route: run DHCP ourselves in the kernel, over the driver's own
 * working TX/RX data plane, and install the address with the same KPI XNU's
 * own netboot client uses (bsd/kern/netboot.c:558 inet_aifaddr()).
 *
 * That bypasses configd, InterfaceNamer, IONetworkStack, preferences.plist
 * and IOKit quiet entirely. The address is NOT published in
 * SystemConfiguration, so Settings/Status bar will not show Wi-Fi as
 * connected -- that is a follow-up. Being able to send/receive IP is the
 * checkpoint this file targets.
 *
 * Gate: dhcp=1 in boot-args (default off, exact single-variable A/B).
 * Checkpoint text: "DHCP: bound inet a.b.c.d mask ... router ... addr=0 rt=0"
 *
 * Wire references: RFC 2131/2132, XNU bsd/kern/netboot.c (inet_aifaddr,
 * default_route_add), bsd/netinet/dhcp_options.h. No userspace dependency.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/socket.h>
#include <sys/proc.h>
#include <sys/sockio.h>
#include <pexpert/pexpert.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <libkern/libkern.h>
#include <sys/attr.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/mount.h>
#include <sys/signal.h>
#include <sys/uio.h>
#include <sys/vnode.h>

/* Kernel-internal process helpers (bsd/sys/proc.h).  Declared here the same
 * way patches/picasso-ipc-census-xnu.patch does, so no private header is
 * needed. */
extern void proc_name(int pid, char *buf, int size);

/* Kernel-internal entry points. Declared here rather than pulling in
 * private headers whose availability differs across XNU builds; every one
 * of these is present in kernel.macho (ifioctl 39, socreate 26,
 * rtrequest_scoped 2, current_proc 8 references in run 43162658105). */
struct socket;
extern int socreate(int dom, struct socket **aso, int type, int proto);
extern int ifioctl(struct socket *so, u_long cmd, caddr_t data, proc_t p);
extern int rtrequest_scoped(int req, struct sockaddr *dst,
    struct sockaddr *gateway, struct sockaddr *netmask, int flags,
    struct rtentry **ret_nrt, unsigned int ifscope);
extern proc_t current_proc(void);

/* bsd/sys/sockio_private.h is not exported to drivers.  This is its exact
 * definition; Apple's own tests/net_test_lib.c:ifnet_attach_ip() calls
 * ioctl(s, SIOCPROTOATTACH, &ifr) to plumb PF_INET on a fresh interface.
 * XNU does NOT plumb from ifnet_attach(): proto_plumb(PF_INET) is reached
 * only from in_domifattach() (bsd/netinet/in.c:285) via SIOCAIFADDR or this
 * ioctl.  Without it, ether_demux() has no ETHERTYPE_ARP/IP descriptor and
 * DLIL drops every inbound frame ("packets not matching an attached
 * protocol are dropped"). */
#ifndef SIOCPROTOATTACH
#define SIOCPROTOATTACH _IOWR('i', 80, struct ifreq)
#endif

/* The C++ side owns the radio. It wraps wcn3990_hif_net_tx() so this C file
 * can put a complete Ethernet frame on the air. */
extern int picasso_dhcp_send(const uint8_t *eth, size_t len);

/* Defined below, called from dhcp_install() on the driver thread. */
static void dhcp_publish_dns(void);

#define DHCP_TICK_RETRY   500u    /* polls between retransmits (~0.5-2 s) */
#define DHCP_MAX_RETRY    12u
#define DHCP_XID          0x50494341u   /* "PICA" */
#define DHCP_BOOTP_LEN    300u    /* 236 fixed + 4 cookie + options + padding */
#define DHCP_CLIENT_PORT  68
#define DHCP_SERVER_PORT  67

enum dhcp_state {
	DHCP_IDLE = 0,
	DHCP_SELECT,
	DHCP_REQUEST,
	DHCP_BOUND
};

static enum dhcp_state dhcp_st;
static uint32_t dhcp_next;
static unsigned dhcp_retry;
static uint8_t dhcp_mac[6];
static uint8_t dhcp_yiaddr[4];
static uint8_t dhcp_server[4];
static uint8_t dhcp_mask[4];
static uint8_t dhcp_router[4];
static uint8_t dhcp_dns[4];
static uint8_t dhcp_dns2[4];
static unsigned dhcp_dns_count;
static unsigned dhcp_opt_logged;
static int dhcp_have_server;

/* ------------------------------------------------------------ checksums */

static uint16_t
dhcp_checksum(const uint8_t *p, size_t n, uint32_t sum)
{
	while (n > 1) {
		sum += ((uint32_t)p[0] << 8) | p[1];
		p += 2;
		n -= 2;
	}
	if (n) {
		sum += (uint32_t)p[0] << 8;
	}
	while (sum >> 16) {
		sum = (sum & 0xffffu) + (sum >> 16);
	}
	return (uint16_t)(~sum & 0xffffu);
}

static void
dhcp_put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void
dhcp_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* ------------------------------------------------------------ transmit */

static void
dhcp_send(uint8_t msgtype)
{
	uint8_t frame[14 + 20 + 8 + DHCP_BOOTP_LEN];
	uint8_t *ip = frame + 14;
	uint8_t *udp = ip + 20;
	uint8_t *bootp = udp + 8;
	uint8_t *opt;
	size_t bootp_len = DHCP_BOOTP_LEN;
	size_t udp_len = 8 + bootp_len;
	size_t total = 14 + 20 + udp_len;
	uint32_t pseudo;
	uint16_t sum;
	unsigned i;

	bzero(frame, sizeof(frame));

	/* Ethernet: broadcast, our own MAC, IPv4 */
	for (i = 0; i < 6; i++) {
		frame[i] = 0xff;
		frame[6 + i] = dhcp_mac[i];
	}
	frame[12] = 0x08;
	frame[13] = 0x00;

	/* IPv4, no fragmentation, all-ones destination (limited broadcast) */
	ip[0] = 0x45;
	ip[1] = 0;
	dhcp_put16(ip + 2, (uint16_t)(20 + udp_len));
	dhcp_put16(ip + 4, 0x1234);
	dhcp_put16(ip + 6, 0);
	ip[8] = 64;
	ip[9] = 17;                 /* IPPROTO_UDP */
	ip[12] = ip[13] = ip[14] = ip[15] = 0;
	ip[16] = ip[17] = ip[18] = ip[19] = 0xff;
	dhcp_put16(ip + 10, dhcp_checksum(ip, 20, 0));

	/* UDP 68 -> 67 */
	dhcp_put16(udp + 0, DHCP_CLIENT_PORT);
	dhcp_put16(udp + 2, DHCP_SERVER_PORT);
	dhcp_put16(udp + 4, (uint16_t)udp_len);

	/* BOOTP / DHCP fixed part */
	bootp[0] = 1;               /* BOOTREQUEST */
	bootp[1] = 1;               /* Ethernet */
	bootp[2] = 6;               /* MAC length */
	bootp[3] = 0;
	dhcp_put32(bootp + 4, DHCP_XID);
	dhcp_put16(bootp + 8, 0);  /* secs */
	dhcp_put16(bootp + 10, 0x8000); /* ask for a broadcast reply */
	for (i = 0; i < 6; i++) {
		bootp[28 + i] = dhcp_mac[i];
	}
	bootp[236] = 0x63;
	bootp[237] = 0x82;
	bootp[238] = 0x53;
	bootp[239] = 0x63;

	opt = bootp + 240;
	*opt++ = 53; *opt++ = 1; *opt++ = msgtype;                 /* message type */
	*opt++ = 61; *opt++ = 7; *opt++ = 1;                       /* client id */
	for (i = 0; i < 6; i++) {
		*opt++ = dhcp_mac[i];
	}
	*opt++ = 55; *opt++ = 5;                                   /* parameter list */
	*opt++ = 1;  *opt++ = 3;  *opt++ = 6; *opt++ = 51; *opt++ = 54;
	if (msgtype == 3) {                                         /* REQUEST */
		*opt++ = 50; *opt++ = 4;
		for (i = 0; i < 4; i++) {
			*opt++ = dhcp_yiaddr[i];
		}
		if (dhcp_have_server) {
			*opt++ = 54; *opt++ = 4;
			for (i = 0; i < 4; i++) {
				*opt++ = dhcp_server[i];
			}
		}
	}
	*opt++ = 255;                                               /* end */

	/* UDP checksum with the IPv4 pseudo header */
	pseudo = 0;
	pseudo += 0x0000 + 0x0000 + 0xffff + 0xffff;  /* src 0.0.0.0, dst 255.255.255.255 */
	pseudo += 17u + (uint32_t)udp_len;
	sum = dhcp_checksum(udp, udp_len, pseudo);
	if (sum == 0) {
		sum = 0xffff;
	}
	dhcp_put16(udp + 6, sum);

	if (picasso_dhcp_send(frame, total) != 0) {
		printf("DHCP: send failed (msgtype=%u state=%d)\n", msgtype, dhcp_st);
	}
}

static void
dhcp_reset(void)
{
	dhcp_st = DHCP_IDLE;
	dhcp_retry = 0;
	dhcp_have_server = 0;
	dhcp_next = 0;
	bzero(dhcp_yiaddr, sizeof(dhcp_yiaddr));
	bzero(dhcp_server, sizeof(dhcp_server));
	bzero(dhcp_mask, sizeof(dhcp_mask));
	bzero(dhcp_router, sizeof(dhcp_router));
}

/* ------------------------------------------------------------ DNS publish */

/* Publish the DHCP-learned resolvers in Apple's plain-text resolver format
 * ("nameserver <IPv4>" per line, mDNSPosix ParseDNSServers) and ask
 * mDNSResponder to re-read its configuration.  Every step prints its result:
 * this boot is also the measurement that tells us whether the iOS platform
 * layer uses a file at all.  Runs ONLY from dhcp_install() on the driver
 * thread (v17 charter: no work on launchd's exec path). */
static void
dhcp_publish_dns(void)
{
	static const char *const paths[] = {
	    "/private/var/run/resolv.conf",
	    "/private/etc/resolv.conf",
	    "/etc/resolv.conf",
	};
	char text[128];
	char name[32];
	int n, i, pid, found = -1, published = 0;
	vfs_context_t ctx;
	vnode_t vp;
	int error, resid, close_error;

	if (dhcp_dns_count == 0) {
		printf("DHCP: dns option 6 absent, no resolver to publish\n");
		return;
	}
	n = snprintf(text, sizeof(text),
	    "# published by the in-kernel DHCP client (option 6)\n"
	    "nameserver %u.%u.%u.%u\n",
	    dhcp_dns[0], dhcp_dns[1], dhcp_dns[2], dhcp_dns[3]);
	if (dhcp_dns_count > 1 && n > 0 && (size_t)n < sizeof(text)) {
		n += snprintf(text + n, sizeof(text) - (size_t)n,
		    "nameserver %u.%u.%u.%u\n",
		    dhcp_dns2[0], dhcp_dns2[1], dhcp_dns2[2], dhcp_dns2[3]);
	}
	printf("DHCP: dns %u.%u.%u.%u count=%u\n",
	    dhcp_dns[0], dhcp_dns[1], dhcp_dns[2], dhcp_dns[3],
	    dhcp_dns_count);
	if (dhcp_dns_count > 1) {
		printf("DHCP: dns2 %u.%u.%u.%u\n",
		    dhcp_dns2[0], dhcp_dns2[1], dhcp_dns2[2], dhcp_dns2[3]);
	}
	if (n <= 0 || (size_t)n >= sizeof(text)) {
		printf("DHCP: dns text too long, not publishing\n");
		return;
	}

	/* Apple's own file-writing sequence, verbatim from
	 * patches/picasso-ca-prefs-xnu.patch:39-67.  vfs_context_current() takes
	 * NO arguments (bsd/sys/vnode.h:1597). */
	ctx = vfs_context_current();
	for (i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++) {
		vp = NULLVP;
		error = vnode_open(paths[i], O_CREAT | O_TRUNC | FWRITE | O_NOFOLLOW,
		    0644, 0, &vp, ctx);
		printf("DHCP: dns open %s error=%d\n", paths[i], error);
		if (error) {
			continue;
		}
		resid = 0;
		error = vn_rdwr(UIO_WRITE, vp, (caddr_t)text, n, 0, UIO_SYSSPACE,
		    IO_NOAUTH | IO_UNIT, vfs_context_ucred(ctx), &resid,
		    vfs_context_proc(ctx));
		if (!error && resid) {
			error = EIO;
		}
		if (!error) {
			error = VNOP_FSYNC(vp, MNT_WAIT, ctx);
		}
		close_error = vnode_close(vp, FWRITE, ctx);
		printf("DHCP: dns write %s error=%d close=%d bytes=%d\n",
		    paths[i], error, close_error, n);
		if (!error && !close_error) {
			published = 1;
			printf("DHCP: dns published via %s\n", paths[i]);
			/* Do NOT stop at the first success: pong's
			 * userland/package-network.c also publishes /etc/resolv.conf
			 * (an alias of the run file).  Try every candidate and print
			 * each errno, so the device tells us which paths really exist
			 * and which need a symlink. */
		}
	}
	printf("DHCP: dns published=%d\n", published);

	/* Is mDNSResponder actually alive?  /dev/console drops lines, so the
	 * absence of a launchd exec line is NOT evidence (see the reference
	 * port's notes.md).  Ask the kernel's process list instead. */
	for (pid = 1; pid < 4096; pid++) {
		name[0] = 0;
		proc_name(pid, name, (int)sizeof(name));
		if (strncmp(name, "mDNSResponder", 13) == 0) {
			found = pid;
			break;
		}
	}
	printf("DHCP: dns resolver pid=%d\n", found);
	if (found > 0) {
		proc_signal(found, SIGHUP);
		printf("DHCP: dns reload signalled pid=%d\n", found);
	}
}

/* ------------------------------------------------------------ install */

static void
dhcp_install(void)
{
	struct ifaliasreq ifra;
	struct sockaddr_in *sin;
	struct sockaddr_in dst, gw, mask;
	struct socket *so = NULL;
	uint32_t ip4, mask4, bcast;
	uint32_t wifix = 0;
	int err, rterr = 0;
	unsigned i;

	(void)PE_parse_boot_argn("wifix", &wifix, sizeof(wifix));

	ip4 = ((uint32_t)dhcp_yiaddr[0] << 24) | ((uint32_t)dhcp_yiaddr[1] << 16) |
	    ((uint32_t)dhcp_yiaddr[2] << 8) | dhcp_yiaddr[3];
	mask4 = ((uint32_t)dhcp_mask[0] << 24) | ((uint32_t)dhcp_mask[1] << 16) |
	    ((uint32_t)dhcp_mask[2] << 8) | dhcp_mask[3];
	if (mask4 == 0) {
		mask4 = 0x00ffffffu;     /* assume /24 if the server omitted option 1 */
		dhcp_mask[0] = 255; dhcp_mask[1] = 255; dhcp_mask[2] = 255;
	}
	bcast = ip4 | ~mask4;

	err = socreate(AF_INET, &so, SOCK_DGRAM, 0);
	if (err) {
		printf("DHCP: socreate failed %d\n", err);
		return;
	}

	bzero(&ifra, sizeof(ifra));
	strlcpy(ifra.ifra_name, "en0", sizeof(ifra.ifra_name));
	sin = (struct sockaddr_in *)(void *)&ifra.ifra_addr;
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	/* s_addr is in network byte order. Fill it byte-wise so no byte-order
	 * macro (whose availability varies across XNU headers) is needed. */
	{
		uint8_t *a = (uint8_t *)&sin->sin_addr.s_addr;
		for (i = 0; i < 4; i++) {
			a[i] = dhcp_yiaddr[i];
		}
	}
	sin = (struct sockaddr_in *)(void *)&ifra.ifra_mask;
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	{
		uint8_t *a = (uint8_t *)&sin->sin_addr.s_addr;
		a[0] = (uint8_t)(mask4 >> 24); a[1] = (uint8_t)(mask4 >> 16);
		a[2] = (uint8_t)(mask4 >> 8);  a[3] = (uint8_t)mask4;
	}
	sin = (struct sockaddr_in *)(void *)&ifra.ifra_broadaddr;
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	{
		uint8_t *a = (uint8_t *)&sin->sin_addr.s_addr;
		a[0] = (uint8_t)(bcast >> 24); a[1] = (uint8_t)(bcast >> 16);
		a[2] = (uint8_t)(bcast >> 8);  a[3] = (uint8_t)bcast;
	}

	err = ifioctl(so, SIOCAIFADDR, (caddr_t)&ifra, current_proc());

	/* wifix=2: make protocol registration explicit and observable.
	 * SIOCAIFADDR also reaches in_domifattach(), but its return is
	 * discarded inside in_control() ("(void) in_domifattach(ifp)") and a
	 * failed proto_plumb() only reaches log(LOG_ERR), not the console.
	 * This ioctl returns the real errno: 0/EEXIST=in plumbed, ENXIO=no
	 * plumber registered, EPERM=not privileged.  It is idempotent. */
	if (wifix >= 2) {
		struct ifreq ifr;
		int plumb;

		bzero(&ifr, sizeof(ifr));
		strlcpy(ifr.ifr_name, "en0", sizeof(ifr.ifr_name));
		plumb = ifioctl(so, SIOCPROTOATTACH, (caddr_t)&ifr, current_proc());
		printf("DHCP: plumb SIOCPROTOATTACH rc=%d (0/17=attached, 6=no plumber, 1=EPERM)\n",
		    plumb);
	}

	if (err == 0 && dhcp_router[0] != 0) {
		bzero(&dst, sizeof(dst));
		bzero(&gw, sizeof(gw));
		bzero(&mask, sizeof(mask));
		dst.sin_len = gw.sin_len = mask.sin_len = sizeof(dst);
		dst.sin_family = gw.sin_family = mask.sin_family = AF_INET;
		dst.sin_addr.s_addr = 0;
		mask.sin_addr.s_addr = 0;
		{
			uint8_t *a = (uint8_t *)&gw.sin_addr.s_addr;
			for (i = 0; i < 4; i++) {
				a[i] = dhcp_router[i];
			}
		}
		rterr = rtrequest_scoped(RTM_ADD, (struct sockaddr *)&dst,
		    (struct sockaddr *)&gw, (struct sockaddr *)&mask,
		    RTF_UP | RTF_STATIC | RTF_GATEWAY, NULL, 0);
	}

	printf("DHCP: bound inet %u.%u.%u.%u mask %u.%u.%u.%u router %u.%u.%u.%u "
	    "addr=%d rt=%d\n",
	    dhcp_yiaddr[0], dhcp_yiaddr[1], dhcp_yiaddr[2], dhcp_yiaddr[3],
	    dhcp_mask[0], dhcp_mask[1], dhcp_mask[2], dhcp_mask[3],
	    dhcp_router[0], dhcp_router[1], dhcp_router[2], dhcp_router[3],
	    err, rterr);
	if (err == 0) {
		dhcp_publish_dns();
	}
}

/* ------------------------------------------------------------ receive */

static void
dhcp_option(const uint8_t *p, size_t n, uint8_t *msgtype)
{
	size_t at = 240;
	char tags[160] = { 0 };
	size_t tl = 0;
	if (n < 244 || p[236] != 0x63 || p[237] != 0x82 ||
	    p[238] != 0x53 || p[239] != 0x63) {
		return;
	}
	while (at < n) {
		uint8_t tag = p[at++];
		uint8_t len;
		if (tag == 0) {
			continue;
		}
		if (tag == 255) {
			break;
		}
		if (at >= n) {
			break;
		}
		len = p[at++];
		if (at + len > n) {
			break;
		}
		/* Print the FULL option list of the first two DHCP replies.  This is
		 * the missing evidence: v19 inferred "the AP offered DNS" from our
		 * own parameter-request list, never from the OFFER bytes. */
		if (tl + 8 < sizeof(tags)) {
			tl += (size_t)snprintf(tags + tl, sizeof(tags) - tl,
			    "%s%u", tl ? "," : "", tag);
		}
		switch (tag) {
		case 1:                                  /* subnet mask */
			if (len >= 4) {
				memcpy(dhcp_mask, p + at, 4);
			}
			break;
		case 3:                                  /* router */
			if (len >= 4) {
				memcpy(dhcp_router, p + at, 4);
			}
			break;
		case 6:                                  /* DNS servers, RFC 2132 */
			if (len >= 4) {
				memcpy(dhcp_dns, p + at, 4);
				dhcp_dns_count = 1;
			}
			if (len >= 8) {
				memcpy(dhcp_dns2, p + at + 4, 4);
				dhcp_dns_count = 2;
			}
			break;
		case 53:                                 /* DHCP message type */
			if (len >= 1) {
				*msgtype = p[at];
			}
			break;
		case 54:                                 /* server identifier */
			if (len >= 4) {
				memcpy(dhcp_server, p + at, 4);
				dhcp_have_server = 1;
			}
			break;
		default:
			break;
		}
		at += len;
	}
	if (dhcp_opt_logged++ < 2) {
		printf("DHCP: options %s dns=%u\n", tags, dhcp_dns_count);
	}
}

void
hakios_dhcp_input(const uint8_t *eth, size_t len)
{
	const uint8_t *ip, *udp, *bootp;
	uint16_t ip_total, udp_len;
	size_t ip_hlen, bootp_len;
	uint32_t xid;
	uint8_t msgtype = 0;
	unsigned i;

	if (dhcp_st == DHCP_BOUND || len < 14 + 20 + 8 + 240) {
		return;
	}
	if (eth[12] != 0x08 || eth[13] != 0x00) {
		return;                 /* IPv4 only */
	}
	ip = eth + 14;
	if ((ip[0] >> 4) != 4 || ip[9] != 17) {
		return;                 /* not IPv4 / not UDP */
	}
	ip_hlen = (size_t)(ip[0] & 0x0f) * 4;
	if (ip_hlen < 20 || len < 14 + ip_hlen + 8 + 240) {
		return;
	}
	ip_total = (uint16_t)(((uint16_t)ip[2] << 8) | ip[3]);
	if (ip_total < ip_hlen + 8 || 14 + ip_total > len) {
		return;
	}
	udp = ip + ip_hlen;
	udp_len = (uint16_t)(((uint16_t)udp[4] << 8) | udp[5]);
	if (udp_len < 8 + 240 || udp_len > ip_total - ip_hlen) {
		return;
	}
	if ((((uint16_t)udp[0] << 8) | udp[1]) != DHCP_SERVER_PORT ||
	    (((uint16_t)udp[2] << 8) | udp[3]) != DHCP_CLIENT_PORT) {
		return;                 /* 67 -> 68 only */
	}
	bootp = udp + 8;
	bootp_len = udp_len - 8;
	if (bootp[0] != 2 || bootp[1] != 1) {
		return;                 /* BOOTREPLY / Ethernet */
	}
	xid = ((uint32_t)bootp[4] << 24) | ((uint32_t)bootp[5] << 16) |
	    ((uint32_t)bootp[6] << 8) | bootp[7];
	if (xid != DHCP_XID) {
		return;
	}
	dhcp_option(bootp, bootp_len, &msgtype);

	if (msgtype == 2 && (dhcp_st == DHCP_IDLE || dhcp_st == DHCP_SELECT)) {
		/* OFFER */
		for (i = 0; i < 4; i++) {
			dhcp_yiaddr[i] = bootp[16 + i];
		}
		printf("DHCP: OFFER %u.%u.%u.%u server %u.%u.%u.%u\n",
		    dhcp_yiaddr[0], dhcp_yiaddr[1], dhcp_yiaddr[2], dhcp_yiaddr[3],
		    dhcp_server[0], dhcp_server[1], dhcp_server[2], dhcp_server[3]);
		dhcp_st = DHCP_REQUEST;
		dhcp_retry = 0;
		dhcp_next = 0;         /* send REQUEST on the next tick */
		return;
	}
	if (msgtype == 5 && dhcp_st == DHCP_REQUEST) {
		/* ACK */
		printf("DHCP: ACK from %u.%u.%u.%u\n",
		    dhcp_server[0], dhcp_server[1], dhcp_server[2], dhcp_server[3]);
		dhcp_st = DHCP_BOUND;
		dhcp_install();
		return;
	}
	if (msgtype == 6) {
		printf("DHCP: NAK, restarting\n");
		dhcp_reset();
	}
}

/* ------------------------------------------------------------ tick */

void
hakios_dhcp_tick(uint32_t polls, int link_up, const uint8_t mac[6])
{
	unsigned i;
	if (!link_up) {
		if (dhcp_st != DHCP_IDLE) {
			printf("DHCP: link down, reset\n");
			dhcp_reset();
		}
		return;
	}
	for (i = 0; i < 6; i++) {
		dhcp_mac[i] = mac[i];
	}
	if (dhcp_st == DHCP_BOUND) {
		return;
	}
	if (dhcp_next != 0 && (int32_t)(polls - dhcp_next) < 0) {
		return;
	}
	if (dhcp_retry >= DHCP_MAX_RETRY) {
		printf("DHCP: giving up after %u tries (state=%d)\n", dhcp_retry, dhcp_st);
		dhcp_reset();
		dhcp_next = polls + 2000u;
		return;
	}
	dhcp_retry++;
	if (dhcp_st == DHCP_IDLE) {
		printf("DHCP: DISCOVER (try %u)\n", dhcp_retry);
		dhcp_send(1);
		dhcp_st = DHCP_SELECT;
	} else if (dhcp_st == DHCP_SELECT) {
		printf("DHCP: DISCOVER retry (try %u)\n", dhcp_retry);
		dhcp_send(1);
	} else {
		printf("DHCP: REQUEST (try %u)\n", dhcp_retry);
		dhcp_send(3);
	}
	dhcp_next = polls + DHCP_TICK_RETRY;
}

/* ---- chain-626: datapath liveness probe -------------------------------
 * A quiet link and a wedged rx path look identical on the host side: the
 * CE1 completion counter and the delivered-frames counter both freeze.
 * The only host-side way to tell them apart is to provoke traffic: send a
 * broadcast ARP request for the gateway and watch the unicast counter -
 * the reply is unicast, comes back through the whole rx path, and beacons
 * keep arriving over CE2/WMI meanwhile.  Three silent probes while the
 * link is up flag the datapath stuck; recover=1 acts on that. */
static uint32_t dhcp_probe_next;
static uint32_t dhcp_probe_uni_at;
static unsigned dhcp_probe_miss;
static int dhcp_datapath_stuck;

uint32_t wcn3990_hif_rx_unicast_count(void);

void
hakios_dhcp_datapath_probe(uint32_t polls, int link_up)
{
	uint8_t fr[42];
	uint32_t uni;
	unsigned i;

	if (!link_up || dhcp_yiaddr[0] == 0 || dhcp_router[0] == 0) return;
	uni = wcn3990_hif_rx_unicast_count();

	if (dhcp_probe_next == 0) {
		dhcp_probe_uni_at = uni;
		dhcp_probe_next = polls + 10000u;
		return;
	}
	if ((int32_t)(polls - dhcp_probe_next) < 0) {
		if (uni != dhcp_probe_uni_at) {
			dhcp_probe_miss = 0;
			dhcp_datapath_stuck = 0;
		}
		return;
	}
	dhcp_probe_next = polls + 10000u;
	if (uni != dhcp_probe_uni_at) {
		dhcp_probe_miss = 0;
		dhcp_datapath_stuck = 0;
		dhcp_probe_uni_at = uni;
		return;
	}
	dhcp_probe_miss++;
	printf("DHCP: probe %u: no unicast in ~43 s (miss %u%s)\n",
	    polls, dhcp_probe_miss,
	    dhcp_datapath_stuck ? ", datapath STUCK" : "");
	if (dhcp_probe_miss >= 3) dhcp_datapath_stuck = 1;
	/* Provoke rx: broadcast ARP "who has the gateway". */
	memset(fr, 0, sizeof(fr));
	for (i = 0; i < 6; i++) {
		fr[i] = 0xff;              /* eth dst: broadcast */
		fr[6 + i] = dhcp_mac[i];   /* eth src: us */
	}
	fr[12] = 0x08; fr[13] = 0x06;  /* ethertype ARP */
	fr[14] = 0x00; fr[15] = 0x01;  /* hrd ethernet */
	fr[16] = 0x08; fr[17] = 0x00;  /* pro IPv4 */
	fr[18] = 6; fr[19] = 4;        /* hln/pln */
	fr[20] = 0x00; fr[21] = 0x01;  /* op request */
	for (i = 0; i < 6; i++) fr[22 + i] = dhcp_mac[i];
	for (i = 0; i < 4; i++) fr[28 + i] = dhcp_yiaddr[i];
	/* fr[32..37] target hw stays zero; fr[38..41] target proto */
	for (i = 0; i < 4; i++) fr[38 + i] = dhcp_router[i];
	(void)picasso_dhcp_send(fr, sizeof(fr));
}

int
hakios_dhcp_datapath_stuck(void)
{
	return dhcp_datapath_stuck;
}
