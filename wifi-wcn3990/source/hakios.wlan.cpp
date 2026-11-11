/*
 * HakiosWLAN: the registry entries by which Darwin finds the Wi-Fi interface.
 *
 * en0 is made with the BSD interface KPI (drivers/net/hakios.net.c) and has no IOKit object behind
 * it of its own; Darwin looks for two, as IO80211Family would have made them:
 *
 *   - wifid's device is whatever service has the property IO80211InterfaceRole =
 *     "Infrastructure", and IO80211.framework binds to an interface through a service whose
 *     IOInterfaceName is the interface's name. The service has no user client, on purpose:
 *     the framework's attempt to open one fails, it says "Falling back to IOCTL path", and
 *     from then on every request is an ioctl on a socket, which the BSD en0 answers.
 *   - SystemConfiguration (SCNetworkInterfaceCopyAll) lists the network interfaces it finds as
 *     objects of the class IONetworkInterface, and calls one a Wi-Fi interface when its parent
 *     is of the class IO80211Controller; type, unit, name and built-in-ness are properties of
 *     the interface, the hardware address one of the controller. configd makes a network
 *     service (DHCP and the rest) for an interface it has listed, and only an interface with a
 *     service can be the one the system routes through -- which wifid waits for after a join
 *     ("failed to become routable interface") before Settings ticks the network.
 *
 *   - MobileGestalt, which lsd asks for the Wi-Fi address of every bundle it registers, looks the
 *     address up as the property local-mac-address of the device tree's "wlan" node, as a phone
 *     has it; without that it waits, up to 5 s every time, for a service of the class
 *     IONetworkController under that node. With neither, each such question cost 5 s, and a
 *     LaunchServices database rebuilt in full (a first boot on an empty data volume) takes hours,
 *     SpringBoard waiting. (Found by the ios-sapphire port, patches 0157 and 0158, which carries
 *     this file on: github.com/Angelpro09xd/ios-sapphire, APSL 2.0.)
 *
 * iOS 16 note (2026-11-06): the board now prelinks the real
 * com.apple.iokit.IONetworkingFamily (board.toml roots: "InterfaceNamer needs the original
 * IOBSD-matched stack"), so this file must not define the family's class names any more -- the
 * first ios16-wifi boot panicked at OSMetaClass.cpp:896 with "Kext
 * com.apple.iokit.IONetworkingFamily class IOEthernetController is a duplicate; kext __kernel__
 * already has a class by that name" while the family's own registration ran.  The interface
 * service is therefore an instance of the family's real IONetworkInterface, allocated by name
 * through OSMetaClass::allocClassWithName(), and only IOService's own state is initialised:
 * these objects are property carriers, not driven hardware.  A bare real IONetworkController is
 * deliberately not published: it is the family's abstract driver base, and InterfaceNamer
 * attaches to every IONetworkController that registers.  MobileGestalt keeps its first lookup
 * (the wlan node's local-mac-address, which is set here); the IONetworkController-class
 * fallback is left for a later round, if the 5 s waits ever show up.  IO80211Controller is
 * still defined here because IO80211Family is not in the board's prelink roots, so no loaded
 * kext owns that name, and configd needs it as the interface's parent class to call the
 * service Wi-Fi.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>

extern "C" void HakiosWLANStart(const char * interface, const uint8_t * address);

class IO80211Controller : public IOService
{
	OSDeclareDefaultStructors(IO80211Controller);
};

OSDefineMetaClassAndStructors(IO80211Controller, IOService);

/* Which of the names the userspace lookups need are actually registered: the
 * family is prelinked (board.toml roots) yet the first allocClassWithName
 * returned nothing, and "kext not started" vs "wrong name" is otherwise
 * indistinguishable.  Read-only, once per radio. */
static void
say_class_presence(void)
{
	static const char * const names[] = {
		"IOService", "IONetworkController", "IOEthernetController",
		"IONetworkInterface", "IO80211Interface", "IOEthernetInterface",
		"IO80211Controller", "IONetworkStack", "IONetworkStackUserClient",
	};
	unsigned i;

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		const OSSymbol * symbol = OSSymbol::withCString(names[i]);
		const OSMetaClass * meta = symbol != NULL ?
		    OSMetaClass::getMetaClassWithName(symbol) : NULL;
		OSObject * instance = meta != NULL ?
		    OSMetaClass::allocClassWithName(names[i]) : NULL;
		IOService * service = OSDynamicCast(IOService, instance);

		if (instance != NULL && service == NULL) {
			instance->release();
		} else if (service != NULL) {
			service->release();
		}
		if (symbol != NULL) {
			symbol->release();
		}
		IOLog("HakiosWLAN: class %s is %s alloc %s\n", names[i],
		    meta != NULL ? "loaded" : "absent",
		    service != NULL ? "ok" : "NULL");
	}
}

/* Allocate one instance of a class owned by a loaded kext (+1 retained), or NULL when the
 * family that owns the name is not prelinked. */
static IOService *
alloc_service(const char * name)
{
	OSObject * object = OSMetaClass::allocClassWithName(name);
	IOService * service = OSDynamicCast(IOService, object);

	if (object != NULL && service == NULL) {
		object->release();
	}
	return service;
}

/* IONetworkInterface is an abstract class: allocClassWithName() on the name
 * itself returns NULL by design, which is why the first iOS 16 port logged
 * "no IONetworkInterface class is loaded" even though the family is prelinked.
 * The family ships concrete subclasses; try those, keeping the abstract name
 * last as a fallback so this can never be worse than the old behaviour.  The
 * class actually allocated is logged, so a device log is the evidence. */
static IOService *
alloc_interface(void)
{
	static const char * const candidates[] = {
		"IO80211Interface", "IOEthernetInterface", "IONetworkInterface",
	};
	unsigned i;

	for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		IOService * service = alloc_service(candidates[i]);
		if (service != NULL) {
			IOLog("HakiosWLAN: interface allocated as %s\n", candidates[i]);
			return service;
		}
	}
	return NULL;
}

/* attach, start, register: the service, or NULL */
static IOService *
publish(IOService * service, IOService * provider, const char * name)
{
	if (service == NULL || !service->attach(provider)) {
		OSSafeReleaseNULL(service);
		return NULL;
	}
	service->setName(name);
	if (!service->start(provider)) {
		service->detach(provider);
		service->release();
		return NULL;
	}
	service->registerService();
	return service;
}

/* Once, when the interface of a radio exists. */
void
HakiosWLANStart(const char * interface, const uint8_t * address)
{
	IOService * platform = IOService::getPlatform();
	OSDictionary * match = IOService::nameMatching("wlan");
	IOService * node = match != NULL ? IOService::copyMatchingService(match) : NULL;   /* the device tree's */
	IO80211Controller * controller = OSTypeAlloc(IO80211Controller);
	IOService * wlan;
	OSData * mac;
	OSNumber * number;

	say_class_presence();
	if (platform == NULL || controller == NULL || !controller->init()) {
		OSSafeReleaseNULL(controller);
		OSSafeReleaseNULL(node);
		OSSafeReleaseNULL(match);
		IOLog("HakiosWLAN: cannot create the controller\n");
		return;
	}
	if ((mac = OSData::withBytes(address, 6)) != NULL) {
		controller->setProperty("IOMACAddress", mac);
		if (node != NULL) {
			node->setProperty("local-mac-address", mac);     /* where MobileGestalt reads it first */
		}
		mac->release();
	}
	controller->setProperty("IOBuiltin", true);
	OSSafeReleaseNULL(match);
	controller = (IO80211Controller *) publish(controller, node != NULL ? node : platform, "HakiosWLANController");
	OSSafeReleaseNULL(node);
	if (controller == NULL) {
		IOLog("HakiosWLAN: the controller did not start\n");
		return;
	}

	wlan = alloc_interface();
	if (wlan == NULL) {
		controller->release();
		IOLog("HakiosWLAN: no concrete IONetworkInterface subclass could be allocated\n");
		return;
	}
	/* init() is called on IOService only: the family's own init() overrides expect their
	 * driver context, which a property carrier does not have. */
	if (!wlan->IOService::init()) {
		wlan->release();
		controller->release();
		IOLog("HakiosWLAN: the interface cannot be initialised\n");
		return;
	}
	wlan->setProperty("IOInterfaceName", interface);
	wlan->setProperty("BSD Name", interface);
	wlan->setProperty("IOInterfaceNamePrefix", "en");
	if ((number = OSNumber::withNumber(6ULL, 32)) != NULL) {        /* IFT_ETHER */
		wlan->setProperty("IOInterfaceType", number);
		number->release();
	}
	if ((number = OSNumber::withNumber((unsigned long long) (interface[2] - '0'), 32)) != NULL) {
		wlan->setProperty("IOInterfaceUnit", number);
		number->release();
	}
	wlan->setProperty("IOBuiltin", true);
	wlan->setProperty("IOPrimaryInterface", true);
	wlan->setProperty("IO80211InterfaceRole", "Infrastructure");
	if (publish(wlan, controller, "HakiosWLAN") == NULL) {
		IOLog("HakiosWLAN: the service did not start\n");
	} else {
		wlan->release();
	}
	controller->release();
	IOLog("HakiosWLAN: published HakiosWLANController + HakiosWLAN(%s) on the prelinked family's classes\n",
	    interface);
}
