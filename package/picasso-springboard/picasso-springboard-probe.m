// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Picasso's rectangular status bar uses UIKit's native Pad_ForcedNoCell
// provider (selected in the Data seed), with no carrier and time at the left.
// Reserve another 14.5 mm at the right for the panel's camera area. Hook only
// this provider's metric; UIKit still owns item placement and animations.
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <dispatch/dispatch.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <net/if.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/clock_types.h>

// Read-only diagnostics also use a nonexistent marker path. The bounded namei
// probe records the attempt before lookup, so no file needs to be created and
// no sandbox/AMFI policy is relaxed when a sysctl or console open is denied.
static void picasso_ack(const char *name)
{
    int value = 0;
    size_t size = sizeof(value);
    // Height and baseline have marker-only acknowledgements; the original
    // kernel ABI has one generic metrics-used sysctl for edgeInsets.
    if (strcmp(name, "kern.picasso_metrics_height_used") &&
        strcmp(name, "kern.picasso_metrics_baseline_used"))
        (void)sysctlbyname(name, &value, &size, NULL, 0);
    static const char prefix[] = "kern.picasso_";
    if (strncmp(name, prefix, sizeof(prefix) - 1)) return;
    char marker[160];
    int length = snprintf(marker, sizeof(marker), "/private/var/picasso/picasso-marker-%s",
                          name + sizeof(prefix) - 1);
    if (length <= 0 || (size_t)length >= sizeof(marker)) return;
    int fd = open(marker, O_RDONLY);
    if (fd >= 0) close(fd);
}

// ---------------------------------------------------------------------------
// InterfaceNamer terminal marker (wifiui=1).
//
// Settings > Wi-Fi, wifid and SpringBoard all block inside
// SCNetworkInterfaceCopyAll() -> __wait_for_IOKit_to_quiesce(), which waits for
// Plugin:InterfaceNamer to carry *QUIET*, *COMPLETE* or *TIMEOUT*.  Apple
// configd's InterfaceNamer plugin never reaches that point on this board:
// IOResources stays busy (PICASSO_NETWORK ... resource_busy=1) and the plugin's
// bundle is a read-only union mount that CFBundleCreateBundlesFromDirectory
// does not enumerate.  UniXNU-pong services107/108 solved the identical wait
// on 22H374 by publishing a real TIMEOUT from an independent SC client; this
// is that logic, running in configd (root, euid 0), which checkWriteAccess()
// in configd.tproj/session.c:1039 explicitly allows.
//
// Truthfulness: *QUIET* is published only when IOKitWaitQuiet reports success.
// A timeout or a missing IOKitWaitQuiet symbol publishes *TIMEOUT*, never a
// fabricated quiet.  Existing native terminal markers are preserved.
// All SystemConfiguration / IOKit symbols are resolved at runtime so this
// library keeps its existing link set and can still be injected into
// SpringBoard without pulling a UI framework into configd.
// ---------------------------------------------------------------------------
#include <stdint.h>

typedef const void *(*wifiui_store_create_fn)(const void *, const void *, void *, void *);
typedef const void *(*wifiui_store_copy_fn)(const void *, const void *);
typedef unsigned char (*wifiui_store_set_fn)(const void *, const void *, const void *);
typedef int (*wifiui_wait_quiet_fn)(unsigned int, mach_timespec_t *);

/* Mach / IOKit terminal outcomes (osfmk/mach/kern_return.h,
 * iokit/IOKit/IOReturn.h).  kIOReturnTimeout == iokit_common_err(0x2d6);
 * the constant used before this fix was off by 0x10 and is removed. */
#define WIFIUI_KERN_SUCCESS      0
#define WIFIUI_KERN_DENIED       53
#define WIFIUI_IORETURN_TIMEOUT  0xe00002d6u
#define WIFIUI_WAIT_SECONDS      5

static void wifiui_log(const char *text)
{
    int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) {
        (void)write(fd, text, strlen(text));
        close(fd);
    }
}

#define WIFIUI_RESOLVE(slot, image, symbol) do {                        \
        void *address_ = dlsym((image), (symbol));                      \
        __builtin_memcpy(&(slot), &address_, sizeof(address_));         \
    } while (0)

// ---------------------------------------------------------------------------
// Native SC network service (UniXNU-pong/userland/wifi-bootstrap.c:27-97).
//
// Publishing *TIMEOUT* unblocks SCNetworkInterfaceCopyAll(), but configd still
// has no network service for en0, so IPMonitor never publishes
// State:/Network/Global/DNS -- that is the remaining DNS gap (handoff v23 §5).
// Pong's services108 records the user confirming system Wi-Fi works after
// configure_interface() creates the service from the *real* IOKit interface
// object with SCPreferencesCommitChanges/ApplyChanges.  Order matters: pong
// calls it AFTER publish_namer_result(), because SCPreferencesApplyChanges
// itself waits for InterfaceNamer.
//
// Board difference (not a guess): this kernel prelinks no IO80211Interface
// (hakios.wlan.cpp:100), our interface service is IOEthernetInterface.  So the
// pong class check is replaced by the two properties SystemConfiguration
// itself uses: BSD name == en0 and SCNetworkInterfaceGetInterfaceType(), which
// SCNetworkInterface.c:1790 derives from the IO80211Controller parent (our
// controller stub is published at hakios.wlan.cpp:304).  No device dictionary
// or enabled Wi-Fi UI state is manufactured.
// ---------------------------------------------------------------------------
static void wifiui_configure_service(void *sc, void *iokit)
{
    typedef const void *(*match_bsd_fn)(unsigned int, unsigned int, const char *);
    typedef const void *(*match_class_fn)(const char *);
    typedef unsigned int (*get_service_fn)(unsigned int, const void *);
    typedef const void *(*registry_prop_fn)(unsigned int, const void *, const void *, unsigned int);
    typedef int (*release_fn)(unsigned int);
    typedef unsigned char (*conforms_fn)(unsigned int, const char *);
    typedef const void *(*iface_create_fn)(unsigned int);
    typedef const void *(*ptr1_fn)(const void *);
    typedef const void *(*ptr2_fn)(const void *, const void *);
    typedef const void *(*ptr3_fn)(const void *, const void *, const void *);
    typedef unsigned char (*bool1_fn)(const void *);
    typedef unsigned char (*bool2_fn)(const void *, unsigned char);
    typedef unsigned char (*boolptr2_fn)(const void *, const void *);

    char line[320];
    match_bsd_fn match_bsd = NULL;
    match_class_fn match_class = NULL;
    get_service_fn get_service = NULL;
    registry_prop_fn registry_prop = NULL;
    release_fn release_service = NULL;
    conforms_fn conforms = NULL;
    iface_create_fn iface_create = NULL;
    ptr1_fn iface_bsd = NULL, iface_type = NULL, services_copy = NULL,
            service_iface = NULL, set_copy = NULL, set_create = NULL,
            defaults = NULL, commit = NULL, apply = NULL;
    ptr2_fn service_create = NULL;
    ptr3_fn prefs_create = NULL;
    bool1_fn unlock = NULL, set_current = NULL;
    bool2_fn lock = NULL, enable = NULL;
    boolptr2_fn set_add = NULL;

    if (iokit == NULL) {
        wifiui_log("PICASSO_WIFIUI service IOKit unavailable\n");
        return;
    }
    WIFIUI_RESOLVE(match_bsd, iokit, "IOBSDNameMatching");
    WIFIUI_RESOLVE(match_class, iokit, "IOServiceMatching");
    WIFIUI_RESOLVE(get_service, iokit, "IOServiceGetMatchingService");
    WIFIUI_RESOLVE(registry_prop, iokit, "IORegistryEntryCreateCFProperty");
    WIFIUI_RESOLVE(release_service, iokit, "IOObjectRelease");
    WIFIUI_RESOLVE(conforms, iokit, "IOObjectConformsTo");
    WIFIUI_RESOLVE(iface_create, sc, "_SCNetworkInterfaceCreateWithIONetworkInterfaceObject");
    WIFIUI_RESOLVE(iface_bsd, sc, "SCNetworkInterfaceGetBSDName");
    WIFIUI_RESOLVE(iface_type, sc, "SCNetworkInterfaceGetInterfaceType");
    WIFIUI_RESOLVE(prefs_create, sc, "SCPreferencesCreate");
    WIFIUI_RESOLVE(lock, sc, "SCPreferencesLock");
    WIFIUI_RESOLVE(unlock, sc, "SCPreferencesUnlock");
    WIFIUI_RESOLVE(commit, sc, "SCPreferencesCommitChanges");
    WIFIUI_RESOLVE(apply, sc, "SCPreferencesApplyChanges");
    WIFIUI_RESOLVE(services_copy, sc, "SCNetworkServiceCopyAll");
    WIFIUI_RESOLVE(service_iface, sc, "SCNetworkServiceGetInterface");
    WIFIUI_RESOLVE(service_create, sc, "SCNetworkServiceCreate");
    WIFIUI_RESOLVE(defaults, sc, "SCNetworkServiceEstablishDefaultConfiguration");
    WIFIUI_RESOLVE(enable, sc, "SCNetworkServiceSetEnabled");
    WIFIUI_RESOLVE(set_copy, sc, "SCNetworkSetCopyCurrent");
    WIFIUI_RESOLVE(set_create, sc, "SCNetworkSetCreate");
    WIFIUI_RESOLVE(set_current, sc, "SCNetworkSetSetCurrent");
    WIFIUI_RESOLVE(set_add, sc, "SCNetworkSetAddService");
    if (match_bsd == NULL || match_class == NULL || get_service == NULL ||
        registry_prop == NULL || release_service == NULL || conforms == NULL ||
        iface_create == NULL || iface_bsd == NULL || iface_type == NULL ||
        prefs_create == NULL || lock == NULL || unlock == NULL ||
        commit == NULL || apply == NULL || services_copy == NULL ||
        service_iface == NULL || service_create == NULL || defaults == NULL ||
        enable == NULL || set_copy == NULL || set_create == NULL ||
        set_current == NULL || set_add == NULL) {
        wifiui_log("PICASSO_WIFIUI service SC/IOKit symbols unresolved\n");
        return;
    }

    // Locate the real IOKit object that carries BSD Name = en0.  Pong's
    // IOBSDNameMatching first, then concrete interface classes as fallback.
    unsigned int entry = 0;
    const void *matching = match_bsd(0, 0, "en0");
    if (matching != NULL) {
        entry = get_service(0, matching);
        CFRelease(matching);
    }
    if (entry == 0) {
        static const char *const classes[] = {
            "IO80211Interface", "IOEthernetInterface", "IONetworkInterface"
        };
        for (unsigned i = 0; i < sizeof(classes) / sizeof(classes[0]); ++i) {
            const void *dict = match_class(classes[i]);
            if (dict == NULL) continue;
            unsigned int candidate = get_service(0, dict);
            CFRelease(dict);
            if (candidate == 0) continue;
            const void *name = registry_prop(candidate, CFSTR("BSD Name"), NULL, 0);
            unsigned valid = (name != NULL &&
                              CFGetTypeID(name) == CFStringGetTypeID() &&
                              CFEqual(name, CFSTR("en0"))) ? 1u : 0u;
            if (name != NULL) CFRelease(name);
            if (valid) { entry = candidate; break; }
            (void)release_service(candidate);
        }
    }
    if (entry == 0) {
        wifiui_log("PICASSO_WIFIUI service registry en0 absent\n");
        return;
    }
    (void)snprintf(line, sizeof(line),
        "PICASSO_WIFIUI service entry=%u wifi_class=%u\n",
        entry, (unsigned)conforms(entry, "IO80211Interface"));
    wifiui_log(line);

    const void *interface = iface_create(entry);
    (void)release_service(entry);
    if (interface == NULL) {
        wifiui_log("PICASSO_WIFIUI service interface create failed\n");
        return;
    }
    const void *bsd = iface_bsd(interface);
    const void *type = iface_type(interface);
    char typebuf[64] = "?";
    if (type != NULL && CFGetTypeID(type) == CFStringGetTypeID())
        (void)CFStringGetCString((CFStringRef)type, typebuf, sizeof(typebuf),
                                 kCFStringEncodingUTF8);
    unsigned named = (bsd != NULL && CFGetTypeID(bsd) == CFStringGetTypeID() &&
                      CFEqual(bsd, CFSTR("en0"))) ? 1u : 0u;
    (void)snprintf(line, sizeof(line),
        "PICASSO_WIFIUI service en0_named=%u type=%s\n", named, typebuf);
    wifiui_log(line);
    if (!named) {
        CFRelease(interface);
        return;
    }

    const void *prefs = prefs_create(NULL, CFSTR("picasso native WLAN service"), NULL);
    int result = 1;
    unsigned locked = (prefs != NULL) ? (lock(prefs, 0) ? 1u : 0u) : 0u;
    (void)snprintf(line, sizeof(line),
        "PICASSO_WIFIUI service prefs=%d locked=%u\n", prefs != NULL, locked);
    wifiui_log(line);
    if (prefs != NULL && locked) {
        const void *services = services_copy(prefs);
        unsigned found = 0;
        if (services != NULL && CFGetTypeID(services) == CFArrayGetTypeID()) {
            CFArrayRef list = (CFArrayRef)services;
            for (CFIndex i = 0; i < CFArrayGetCount(list); ++i) {
                const void *candidate = service_iface(CFArrayGetValueAtIndex(list, i));
                const void *name = candidate != NULL ? iface_bsd(candidate) : NULL;
                if (name != NULL && CFGetTypeID(name) == CFStringGetTypeID() &&
                    CFEqual(name, CFSTR("en0"))) { found = 1; break; }
            }
        }
        if (services != NULL) CFRelease(services);
        if (found) {
            result = 0;   // idempotent: a native en0 service already exists
        } else {
            const void *set = set_copy(prefs);
            unsigned current = (set != NULL) ? 1u : 0u;
            if (set == NULL) {
                set = set_create(prefs);
                current = (set != NULL && set_current(set)) ? 1u : 0u;
            }
            const void *service = current ? service_create(prefs, interface) : NULL;
            (void)snprintf(line, sizeof(line),
                "PICASSO_WIFIUI service found=%u set=%d current=%u service=%d\n",
                found, set != NULL, current, service != NULL);
            wifiui_log(line);
            if (service != NULL) {
                unsigned d = defaults(service) ? 1u : 0u;
                unsigned e = enable(service, 1) ? 1u : 0u;
                unsigned a = set_add(set, service) ? 1u : 0u;
                wifiui_log("PICASSO_WIFIUI service applying SCPreferences changes\n");
                unsigned c = commit(prefs) ? 1u : 0u;
                unsigned p = apply(prefs) ? 1u : 0u;
                (void)snprintf(line, sizeof(line),
                    "PICASSO_WIFIUI service defaults=%u enable=%u add=%u commit=%u apply=%u\n",
                    d, e, a, c, p);
                wifiui_log(line);
                result = (d && e && a && c && p) ? 0 : 1;
                CFRelease(service);
            }
            if (set != NULL) CFRelease(set);
        }
        (void)unlock(prefs);
    }
    if (prefs != NULL) CFRelease(prefs);
    CFRelease(interface);
    (void)snprintf(line, sizeof(line),
        "PICASSO_WIFIUI service result=%d type=%s\n", result, typebuf);
    wifiui_log(line);
}

static void wifiui_bootstrap(void)
{
    static const char sc_path[] =
        "/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration";
    static const char iokit_path[] =
        "/System/Library/Frameworks/IOKit.framework/IOKit";
    char line[320];
    void *sc = dlopen(sc_path, RTLD_LAZY | RTLD_LOCAL);
    void *iokit = dlopen(iokit_path, RTLD_LAZY | RTLD_LOCAL);
    wifiui_store_create_fn store_create = NULL;
    wifiui_store_copy_fn store_copy = NULL;
    wifiui_store_set_fn store_set = NULL;
    wifiui_wait_quiet_fn wait_quiet = NULL;

    if (sc == NULL) {
        wifiui_log("PICASSO_WIFIUI SystemConfiguration unavailable\n");
        return;
    }
    WIFIUI_RESOLVE(store_create, sc, "SCDynamicStoreCreate");
    WIFIUI_RESOLVE(store_copy, sc, "SCDynamicStoreCopyValue");
    WIFIUI_RESOLVE(store_set, sc, "SCDynamicStoreSetValue");
    if (iokit != NULL)
        WIFIUI_RESOLVE(wait_quiet, iokit, "IOKitWaitQuiet");
    if (store_create == NULL || store_copy == NULL || store_set == NULL) {
        wifiui_log("PICASSO_WIFIUI SCDynamicStore symbols unresolved\n");
        return;
    }

    // A real bounded wait before publishing any terminal marker.
    // kIOMainPortDefault is MACH_PORT_NULL (0), which is what Apple's
    // IOKitWaitQuiet() expects (it resolves the default main port itself).
    //
    // Honesty rule (UniXNU-pong services107): claim *QUIET* only when IOKit
    // actually reported success.  Every other outcome publishes *TIMEOUT*,
    // which means "we stopped waiting" -- it does NOT assert that IOKit
    // quiesced.  SCNetworkInterface.c:4946-4950 accepts *TIMEOUT* on iOS,
    // and ifnamer.c:4312 timerCallback() publishes the same marker natively.
    //
    // On iOS 16 (20H392) IOKitWaitQuiet exists but is gated and returns
    // KERN_DENIED (53) from io_registry_entry_from_path(); Apple only
    // documents the API as public from iOS 18.4.  That is an "API
    // unavailable" outcome, not a busy IOKit, and is handled exactly like
    // the missing-symbol case: a real bounded sleep, then *TIMEOUT*.
    // No device-specific error value is special-cased in this control flow.
    int quiet_result = -1;
    unsigned wait_truthful = 1;
    const char *marker = "*TIMEOUT*";
    unsigned waited = 0;
    if (wait_quiet != NULL) {
        mach_timespec_t deadline = { WIFIUI_WAIT_SECONDS, 0 };
        quiet_result = wait_quiet(0, &deadline);
        if (quiet_result == WIFIUI_KERN_SUCCESS) {
            marker = "*QUIET*";
        } else if ((unsigned)quiet_result == WIFIUI_IORETURN_TIMEOUT) {
            waited = 1;          // the native call really waited 5 s
        }
        // KERN_DENIED / KERN_NOT_SUPPORTED / MIG or entitlement failures all
        // mean "this API cannot tell us whether IOKit is quiet".  Do the
        // bounded wait ourselves rather than treating it as a fatal error.
    }
    if (!waited && quiet_result != WIFIUI_KERN_SUCCESS)
        sleep(WIFIUI_WAIT_SECONDS);
    if (quiet_result == WIFIUI_KERN_DENIED)
        wifiui_log("PICASSO_WIFIUI quiet KERN_DENIED: API gated on this OS, bounded wait elapsed\n");
    (void)snprintf(line, sizeof(line),
        "PICASSO_WIFIUI pid=%d euid=%u quiet_api=%s quiet_result=%d truthful=%u marker=%s\n",
        (int)getpid(), (unsigned)geteuid(), wait_quiet != NULL ? "yes" : "no",
        quiet_result, wait_truthful, marker);
    wifiui_log(line);

    const void *store = store_create(NULL, CFSTR("picasso InterfaceNamer bootstrap"), NULL, NULL);
    if (store == NULL) {
        wifiui_log("PICASSO_WIFIUI store_create=NULL\n");
        return;
    }
    const void *key = CFSTR("Plugin:InterfaceNamer");
    const void *previous = store_copy(store, key);
    CFMutableDictionaryRef record;
    if (previous != NULL && CFGetTypeID(previous) == CFDictionaryGetTypeID())
        record = CFDictionaryCreateMutableCopy(NULL, 0, (CFDictionaryRef)previous);
    else
        record = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                           &kCFTypeDictionaryValueCallBacks);
    if (record == NULL) {
        wifiui_log("PICASSO_WIFIUI record=NULL\n");
        if (previous != NULL) CFRelease(previous);
        CFRelease(store);
        return;
    }

    const CFStringRef terminal[3] = {
        CFSTR("*QUIET*"), CFSTR("*COMPLETE*"), CFSTR("*TIMEOUT*")
    };
    unsigned already = 0;
    for (unsigned i = 0; i < 3; ++i)
        if (CFDictionaryContainsKey(record, terminal[i])) already = 1;

    int published = -1;
    if (!already && wait_truthful) {
        CFStringRef chosen = quiet_result == 0 ? CFSTR("*QUIET*") : CFSTR("*TIMEOUT*");
        CFDictionarySetValue(record, chosen, kCFBooleanTrue);
        published = store_set(store, key, record) ? 1 : 0;
    }
    (void)snprintf(line, sizeof(line),
        "PICASSO_WIFIUI key=Plugin:InterfaceNamer already=%u truthful=%u publish=%d\n",
        already, wait_truthful, published);
    wifiui_log(line);

    // The interface arrival notification is what wifid consumes before it
    // enumerates devices.  State: is not write-protected for a root client.
    const void *arrival_key = CFSTR("State:/Network/Interface");
    const void *arrival_old = store_copy(store, arrival_key);
    CFMutableDictionaryRef arrival;
    if (arrival_old != NULL && CFGetTypeID(arrival_old) == CFDictionaryGetTypeID())
        arrival = CFDictionaryCreateMutableCopy(NULL, 0, (CFDictionaryRef)arrival_old);
    else
        arrival = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                            &kCFTypeDictionaryValueCallBacks);
    if (arrival != NULL) {
        CFStringRef en0 = CFSTR("en0");
        CFTypeRef interfaces = CFDictionaryGetValue(arrival, CFSTR("Interfaces"));
        CFMutableArrayRef names;
        if (interfaces != NULL && CFGetTypeID(interfaces) == CFArrayGetTypeID())
            names = CFArrayCreateMutableCopy(NULL, 0, (CFArrayRef)interfaces);
        else
            names = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
        if (names != NULL) {
            if (!CFArrayContainsValue(names, CFRangeMake(0, CFArrayGetCount(names)), en0))
                CFArrayAppendValue(names, en0);
            CFDictionarySetValue(arrival, CFSTR("Interfaces"), names);
            int arrival_ok = store_set(store, arrival_key, arrival) ? 1 : 0;
            (void)snprintf(line, sizeof(line),
                "PICASSO_WIFIUI en0_known=%u arrival_publish=%d\n",
                if_nametoindex("en0") != 0 ? 1u : 0u, arrival_ok);
            wifiui_log(line);
            CFRelease(names);
        }
        CFRelease(arrival);
    }
    if (arrival_old != NULL) CFRelease(arrival_old);
    CFRelease(record);
    if (previous != NULL) CFRelease(previous);
    CFRelease(store);

    // UniXNU-pong/userland/wifi-bootstrap.c --configure-only runs
    // configure_interface() unconditionally, even when publish_namer_result()
    // failed.  Truthfulness constrains only whether *QUIET* may be claimed;
    // it must not prevent the native en0 service from being created.  The
    // service is built from the REAL IORegistry en0 object (no fabricated
    // device dictionary), and wifiui_configure_service() is idempotent.
    // This runs on our detached pthread, never configd's main run loop, so a
    // stalled SCPreferencesApplyChanges cannot stall configd itself.
    (void)snprintf(line, sizeof(line),
        "PICASSO_WIFIUI service begin already=%u published=%d\n",
        already, published);
    wifiui_log(line);
    wifiui_configure_service(sc, iokit);
}

static void *wifiui_thread(void *unused)
{
    (void)unused;
    // SpringBoard's status bar work is untouched; this thread only runs in
    // configd.  Wait for en0 so the marker is not published before the
    // interface exists (HIF starts around t+168 s on this board).
    for (unsigned i = 0; i < 200 && if_nametoindex("en0") == 0; ++i)
        sleep(1);
    if (if_nametoindex("en0") == 0) {
        wifiui_log("PICASSO_WIFIUI en0 absent after 200 s, no marker\n");
        return NULL;
    }
    // Pure C on purpose: configd is a non-UI root daemon and must not touch
    // the Objective-C/UIKit runtime on this path.
    wifiui_bootstrap();
    return NULL;
}

#include "picasso-astc-guard.m"
#include "picasso-statusbar-metrics.m"

static void picasso_statusbar_start(void)
{
    BOOL installed = picasso_install_statusbar();
    picasso_ack(installed ? "kern.picasso_metrics_installed" : "kern.picasso_metrics_unavailable");
    const char *line = installed ? "PICASSO_SBINJECT statusbar metrics installed (left 8.91 pt, down 11.42 pt, battery shift 14.5 mm)\n"
                                 : "PICASSO_SBINJECT statusbar metric unavailable\n";
    int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) {
        (void)write(fd, line, strlen(line));
        close(fd);
    }
    if (!installed) {
        // UIKit is a linked dependency, but a missing runtime class must not
        // turn the sole constructor attempt into a permanent silent failure.
        // One main-queue retry, no polling and no metric calls faking "used".
        dispatch_async(dispatch_get_main_queue(), ^{
            BOOL ready = picasso_install_statusbar();
            picasso_ack(ready ? "kern.picasso_metrics_installed" : "kern.picasso_metrics_unavailable");
        });
    }
}

__attribute__((constructor))
static void picasso_springboard_probe(void)
{
    // The same library is injected into configd by wifiui=1.  configd is a
    // non-UI root daemon: run only the pure-C InterfaceNamer bootstrap there,
    // on a detached thread, and never touch UIKit/objc in that process.
    const char *program = getprogname();
    if (program != NULL && !strcmp(program, "configd")) {
        wifiui_log("PICASSO_WIFIUI loaded in configd\n");
        pthread_t worker;
        if (pthread_create(&worker, NULL, wifiui_thread, NULL) == 0)
            pthread_detach(worker);
        return;
    }
    // Record entry before either installation path can fail.
    picasso_ack("kern.picasso_inject_ack");
    @autoreleasepool {
        picasso_statusbar_start();
        picasso_install_astc_guard();
    }
}
