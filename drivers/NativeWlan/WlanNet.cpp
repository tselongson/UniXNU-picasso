// SPDX-License-Identifier: BSD-2-Clause-Patent
// The network side of the picasso WiFi module: the interface the stack routes through, and the
// Apple80211 requests the wireless framework issues against it.
//
// There is no IO80211 in this kernel -- that family is Apple's, closed, and not part of XNU -- so the
// interface is built on the BSD ifnet layer directly and the Apple80211 requests are answered as
// ioctls on it. That is the shape the sheng port uses, and the two details that make it work are
// worth stating because both fail silently otherwise:
//
//   IFNET_INIT_NX_NOAUTO  without it the stack puts a compatibility nexus in front of a Wi-Fi "en"
//                         interface and its input stops going through the interface's own input
//                         thread, so received frames are never delivered.
//   bpf_attach            a driver of its own interface attaches the packet filter itself; the DHCP
//                         client sends through it before the interface is otherwise usable.
//
// The interface is announced to the framework with an Apple vendor kernel event of class 6 (IEEE
// 802.11), subclass 1, whose payload starts with the interface name.
#include <IOKit/IOLib.h>
#include <sys/kernel_types.h>
#include <sys/kpi_mbuf.h>
#include <sys/kern_event.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <net/if.h>
#include <net/ethernet.h>
#include <net/bpf.h>
#include <net/dlil.h>
#include <string.h>

extern "C" {
#include "WlanBoard.h"
#include "WlanNet.h"
}

// The two Apple80211 ioctls. The encodings are _IOW('i', 200, ...) and _IOWR('i', 201, ...), spelled
// out because the headers that would define them are the closed family's.
#define SIOCSA80211 0x802869c8UL
#define SIOCGA80211 0xc02869c9UL

extern "C" int sheng_kev_post(struct kev_msg *) __asm__("_kev_post_msg");

namespace {

const char *const kInterfaceName = "en0";

struct apple80211req {
	char req_if_name[16];
	int32_t req_type;
	int32_t req_val;
	uint32_t req_len;
	user_addr_t req_data __attribute__((aligned(8)));
};

// The request types this driver answers. Anything else is refused rather than answered with zeroes,
// because a zeroed answer reads as a real value to the framework: a zero RSSI is a valid RSSI and a
// zero channel list is a valid empty scan.
enum {
	kSsid = 1, kAuthType = 2, kChannel = 4, kTxPower = 7, kBssid = 9,
	kCardCaps = 12, kState = 13, kPhyMode = 14, kOpMode = 15,
	kRssi = 16, kNoise = 17, kPower = 19, kAssociate = 20,
	kDisassociate = 22, kSupportedChannels = 27, kDriverVersion = 43,
	kHardwareVersion = 44, kCurrentNetwork = 103,
};

// Association results, as the framework numbers them.
enum {
	kAssocResultSuccess = 0,
	kAssocResultFailure = 1,
};

ifnet_t gIf;
uint8_t gMac[6];
lck_grp_t *gGrp;
lck_mtx_t *gLock;
bool gPowered;

// ---- events the framework listens for ----

void postEvent(uint32_t code, const void *payload, size_t length)
{
	uint8_t data[128];
	if (length > sizeof(data) - 16) {
		length = sizeof(data) - 16;
	}
	memset(data, 0, 16);
	snprintf(reinterpret_cast<char *>(data), 16, "%s", kInterfaceName);
	if (length) {
		memcpy(data + 16, payload, length);
	}
	struct kev_msg msg;
	memset(&msg, 0, sizeof(msg));
	msg.vendor_code = KEV_VENDOR_APPLE;
	msg.kev_class = 6;      // IEEE 802.11
	msg.kev_subclass = 1;
	msg.event_code = code;
	msg.dv[0].data_length = static_cast<uint32_t>(16 + length);
	msg.dv[0].data_ptr = data;
	sheng_kev_post(&msg);
}

void postLink(bool up)
{
	if (!gIf) {
		return;
	}
	ifnet_set_flags(gIf, up ? IFF_RUNNING : 0, IFF_RUNNING);

	struct {
		struct net_event_data data;
	} payload;
	memset(&payload, 0, sizeof(payload));
	payload.data.if_family = IFNET_FAMILY_ETHERNET;
	payload.data.if_unit = 0;
	snprintf(payload.data.if_name, sizeof(payload.data.if_name), "%s", kInterfaceName);

	struct kev_msg msg;
	memset(&msg, 0, sizeof(msg));
	msg.vendor_code = KEV_VENDOR_APPLE;
	msg.kev_class = KEV_NETWORK_CLASS;
	msg.kev_subclass = KEV_DL_SUBCLASS;
	msg.event_code = KEV_DL_LINK_ON;
	msg.dv[0].data_length = sizeof(payload);
	msg.dv[0].data_ptr = &payload;
	sheng_kev_post(&msg);
}

// ---- the two directions ----

errno_t ifOutput(ifnet_t, mbuf_t m)
{
	if (!gIf || !gPowered) {
		mbuf_freem(m);
		return 0;
	}
	const size_t length = mbuf_pkthdr_len(m);
	// The WCSS transmit path takes the frame from here. Until it exists the frame is dropped and the
	// counters stay honest rather than reporting a send that did not happen.
	wlanTxFrame(m);
	ifnet_stat_increment_out(gIf, 1, static_cast<uint32_t>(length), 0);
	return 0;
}

errno_t ifIoctl(ifnet_t, unsigned long command, void *data)
{
	if (!data) {
		return EINVAL;
	}
	if (command != SIOCSA80211 && command != SIOCGA80211) {
		return EOPNOTSUPP;
	}
	apple80211req *request = static_cast<apple80211req *>(data);
	return wlanApple80211(command == SIOCSA80211, request);
}

void ifEvent(ifnet_t, const struct kev_msg *) {}

} // namespace

// The WCSS driver's transmit entry point. Until that driver exists the frame is released here: a
// transmit that reports success and then drops the frame is worse than one that drops it where the
// drop is visible, because the stack would keep a retransmit timer running against a link that never
// carried anything.
void wlanTxFrame(mbuf_t frame)
{
	mbuf_freem(frame);
}

bool wlanNetAttach(const uint8_t mac[6])
{
	if (gIf) {
		return true;
	}
	memcpy(gMac, mac, 6);
	gGrp = lck_grp_alloc_init("picasso-wifi-net", LCK_GRP_ATTR_NULL);
	gLock = lck_mtx_alloc_init(gGrp, LCK_ATTR_NULL);

	struct ifnet_init_eparams p;
	memset(&p, 0, sizeof(p));
	p.ver = IFNET_INIT_CURRENT_VERSION;
	p.len = sizeof(p);
	// NX_NOAUTO is what keeps the stack from inserting a compatibility nexus in front of the Wi-Fi
	// interface; with that nexus the input path stops using this interface's own thread.
	p.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO;
	p.name = kInterfaceName;
	p.output = ifOutput;
	p.demux = ether_demux;
	p.add_proto = ether_add_proto;
	p.del_proto = ether_del_proto;
	p.ioctl = ifIoctl;
	p.event = ifEvent;
	static const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	p.broadcast_addr = broadcast;

	ifnet_t ifp = nullptr;
	errno_t error = ifnet_allocate_extended(&p, &ifp);
	if (error || !ifp) {
		IOLog("PICASSO_WLAN net: ifnet allocation failed: %d\n", error);
		return false;
	}
	ifnet_set_mtu(ifp, 1500);
	ifnet_set_flags(ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST, 0xffff);
	ifnet_set_hdrlen(ifp, 14);
	ifnet_set_addrlen(ifp, 6);

	error = ifnet_attach(ifp, nullptr);
	if (error) {
		IOLog("PICASSO_WLAN net: ifnet attach failed: %d\n", error);
		ifnet_release(ifp);
		return false;
	}
	ifnet_set_lladdr(ifp, mac, 6);

	// The driver attaches the packet filter itself: the DHCP client sends through it before the
	// interface is otherwise usable.
	error = bpf_attach(ifp, DLT_EN10MB, 14, nullptr, nullptr);
	if (error) {
		IOLog("PICASSO_WLAN net: packet filter attach failed: %d\n", error);
	}

	gIf = ifp;
	ifnet_set_flags(ifp, IFF_UP, IFF_UP);
	IOLog("PICASSO_WLAN net: %s up, hardware address %02x:%02x:%02x:%02x:%02x:%02x\n",
	      kInterfaceName, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return true;
}

void wlanNetSetPowered(bool powered)
{
	gPowered = powered;
	postEvent(1 /* kEvPowerChanged */, nullptr, 0);
}

void wlanNetInput(mbuf_t frame)
{
	if (!gIf || !frame) {
		if (frame) {
			mbuf_freem(frame);
		}
		return;
	}
	struct ifnet_stat_increment_param increment;
	memset(&increment, 0, sizeof(increment));
	increment.packets_in = 1;
	increment.bytes_in = static_cast<uint32_t>(mbuf_pkthdr_len(frame));
	const errno_t error = ifnet_input(gIf, frame, &increment);
	if (error) {
		IOLog("PICASSO_WLAN net: input refused: %d\n", error);
	}
}

void wlanNetLink(bool up)
{
	postLink(up);
}
