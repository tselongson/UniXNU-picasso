// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Picasso's rectangular status bar uses UIKit's native Pad_ForcedNoCell
// provider (selected in the Data seed), with no carrier and time at the left.
// Reserve another 14.5 mm at the right for the panel's camera area. Hook only
// this provider's metric; UIKit still owns item placement and animations.
#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>
#import <objc/runtime.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <dispatch/dispatch.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <pthread.h>
static void picasso_ls_dump(void);
static void picasso_cryptex_dump(void);
static void picasso_webkit_start(void);
static void picasso_path_dump(void);
static void picasso_ls_store_drop(void);
static void picasso_app_web_probe(const char *who);
static void picasso_metal_probe(void);
static void picasso_sb_launch_probe(void);
static void picasso_sb_view(void);
static void picasso_xpc_exec_probe(void);
#include <errno.h>
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
/* The in-kernel DHCP client writes /private/var/run/resolv.conf only after
 * the WCN3990 association completes (~t+168 s on this board), while configd
 * starts much earlier.  This detached pthread must bound-wait instead of
 * guessing the timing; it never blocks configd's main run loop. */
#define WIFIUI_DNS_WAIT_SECONDS  240
/* chain-607 c1 measured 6 configd restarts, each immediately after
 * "service begin" with EXC_CORPSE for configd's own pid.  v26 §4.8 already
 * concluded that creating an SCPreferences service from INSIDE configd is
 * structurally impossible (configd holds preferences.plist-lock on its own
 * main run loop).  Keep the reference for future work but never call it. */
#define WIFIUI_ALLOW_INTERNAL_SERVICE 0

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
    // SCPreferencesLock(prefs, FALSE) is the ONLY safe choice here.
    // Apple SCPLock.c:523 opens "<prefs>-lock" with O_EXLOCK|O_NONBLOCK when
    // wait==FALSE; configd's own main run loop holds that file lock, so a
    // non-blocking attempt returns EWOULDBLOCK / kSCStatusPrefsBusy.
    // pong uses FALSE from a SEPARATE process.  We are injected INTO configd;
    // chain-606-lite2 measured "service prefs=1 locked=0".  A wait==TRUE
    // attempt (5cb8977) was measured to DEADLOCK configd: 8 restarts in one
    // 400 s boot, EXC_CORPSE x50, never reaching the next checkpoint.  Do not
    // reintroduce it.  When the lock is unavailable we still publish DNS via
    // the dynamic store below, which needs no SCPreferences lock at all.
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

// ---------------------------------------------------------------------------
// Publish DNS through IPMonitor's *INPUT* keys, not its output key.
//
// Correction to c6725d1 (Apple OSS configd, verified locally and against the
// upstream repository): State:/Network/Global/DNS is WRITTEN BY IPMonitor
// (ip_plugin.c:6245/6262 update_dns_global_resolver -> keyChangeListSetValue),
// and ip_plugin_init() even removes it at startup (:9103).  IPMonitor's
// notification keys/patterns (ip_plugin_copy_keys/patterns, :8953-9004) cover
// Setup:/Network/Global/IPv4 and per-service State:/Setup:/Network/Service/
// <serviceID>/<entity> -- including kSCEntNetDNS (entityTypeNames[2], :572).
// A third-party write to the GLOBAL key therefore triggers NO recomputation:
// dns_configuration_copy() (dnsinfo_copy.c) is an XPC client of
// dnsinfo_server, fed only by dns_configuration_set() ->
// _dns_configuration_store() (dns-configuration.c:1758).
//
// The supported chain is:
//   State:/Network/Service/<id>/DNS change
//     -> parseNetworkServiceString() -> get_dns_changes()
//     -> S_service_state_dict -> update_dnsinfo() (:8673)
//     -> dns_configuration_set() -> _dns_configuration_store() -> resolver.
//
// The DNS dictionary MUST contain SupplementalMatchDomains: add_supplemental()
// (dns-configuration.c:270-276) discards any DNS entity without it; the empty
// string "" is Apple's "match all queries / no domain name" entry (:311-316).
// ServerAddresses alone is silently dropped, which is why writing only the
// Global/DNS key could never reach mDNSResponder.
//
// Addresses are NOT invented: the in-kernel DHCP client parses RFC 2132
// option 6 and writes them to /private/var/run/resolv.conf
// (hakios.dhcp.c dhcp_publish_dns()).  We bound-wait for that file.
// kSCPropNetDNSServerAddresses == "ServerAddresses"
// (SCSchemaDefinitions.c:126); kSCPropNetDNSSupplementalMatchDomains ==
// "SupplementalMatchDomains" (:130).
//
// This path needs NO SCPreferences lock.  On this board configd holds
// preferences.plist-lock on its own main run loop, so building a native
// service from inside the injected daemon cannot obtain it (chain-606-lite2:
// "service prefs=1 locked=0").  SCDynamicStoreSetValue is already proven
// on device by publish=1 and arrival_publish=1 above.
// ---------------------------------------------------------------------------
static void wifiui_publish_dns(const void *store, wifiui_store_set_fn store_set)
{
    static const char resolver_path[] = "/private/var/run/resolv.conf";
    char text[512];
    char line[320];
    CFMutableArrayRef servers;
    CFMutableDictionaryRef dns;
    int fd = -1, n, count = 0, i;
    unsigned waited = 0;
    char *p, *end;

    if (store == NULL || store_set == NULL) return;
    /* Bounded wait: configd starts long before the radio associates and the
     * kernel DHCP client writes resolv.conf (t+168 s).  This is the detached
     * pthread, so sleeping here never blocks configd's main run loop. */
    for (; waited < WIFIUI_DNS_WAIT_SECONDS && fd < 0; ++waited) {
        fd = open(resolver_path, O_RDONLY);
        if (fd < 0) sleep(1);
    }
    if (fd < 0) {
        (void)snprintf(line, sizeof(line),
            "PICASSO_WIFIUI dns resolver file absent after %us\n", waited);
        wifiui_log(line);
        return;
    }
    (void)snprintf(line, sizeof(line),
        "PICASSO_WIFIUI dns wait=%us\n", waited);
    wifiui_log(line);
    n = (int)read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0) {
        wifiui_log("PICASSO_WIFIUI dns resolver file empty\n");
        return;
    }
    text[n] = '\0';

    servers = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    if (servers == NULL) return;
    p = text;
    end = text + n;
    while (p < end && count < 4) {
        char *eol = strchr(p, '\n');
        char addr[64];
        size_t len;
        if (eol == NULL) eol = end;
        len = (size_t)(eol - p);
        if (len >= 11 && !strncmp(p, "nameserver ", 11)) {
            const char *value = p + 11;
            size_t vlen = len - 11;
            while (vlen > 0 && (value[vlen - 1] == ' ' || value[vlen - 1] == '\r' ||
                                value[vlen - 1] == '\t')) vlen--;
            if (vlen > 0 && vlen < sizeof(addr)) {
                CFStringRef s;
                memcpy(addr, value, vlen);
                addr[vlen] = '\0';
                s = CFStringCreateWithCString(NULL, addr, kCFStringEncodingUTF8);
                if (s != NULL) {
                    CFArrayAppendValue(servers, s);
                    CFRelease(s);
                    ++count;
                }
            }
        }
        p = (eol < end) ? eol + 1 : end;
    }
    (void)snprintf(line, sizeof(line),
        "PICASSO_WIFIUI dns servers=%u\n", count);
    wifiui_log(line);
    if (count == 0) {
        CFRelease(servers);
        return;
    }

    dns = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                    &kCFTypeDictionaryValueCallBacks);
    if (dns == NULL) {
        CFRelease(servers);
        return;
    }
    CFDictionarySetValue(dns, CFSTR("ServerAddresses"), servers);
    CFRelease(servers);
    /* add_supplemental() (dns-configuration.c:270-276) returns immediately
     * unless SupplementalMatchDomains is present.  The empty string is
     * Apple's "match all queries" entry (:311-316 removes DomainName), which
     * turns this into a real default resolver rather than a domain-scoped
     * one.  ServerAddresses alone was silently discarded. */
    {
        CFMutableArrayRef matches = CFArrayCreateMutable(NULL, 0,
                                                        &kCFTypeArrayCallBacks);
        if (matches != NULL) {
            CFArrayAppendValue(matches, CFSTR(""));
            CFDictionarySetValue(dns, CFSTR("SupplementalMatchDomains"),
                                 matches);
            CFRelease(matches);
        }
    }

    /* IPMonitor watches per-service Setup:/ and State:/ entity keys; write
     * both so get_dns_changes() sees a complete service, then the Global
     * state key as a status for other clients (it is IPMonitor's OUTPUT,
     * never its input). */
    static const char *const dns_keys[] = {
        "Setup:/Network/Service/picasso-wifi/DNS",
        "State:/Network/Service/picasso-wifi/DNS",
        "State:/Network/Global/DNS",
    };
    for (i = 0; i < 3; i++) {
        CFStringRef key = CFStringCreateWithCString(NULL, dns_keys[i],
                                                   kCFStringEncodingUTF8);
        int ok = 0;

        if (key != NULL) {
            ok = store_set(store, key, dns) ? 1 : 0;
            CFRelease(key);
        }
        (void)snprintf(line, sizeof(line),
            "PICASSO_WIFIUI dns set=%d key=%s\n", ok, dns_keys[i]);
        wifiui_log(line);
    }
    CFRelease(dns);
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
    // Keep the SCDynamicStore session open: DNS publication is deliberately
    // the LAST step (see below).  Its bounded wait must not delay the
    // *TIMEOUT* marker / service checkpoints already verified on device.

    (void)snprintf(line, sizeof(line),
        "PICASSO_WIFIUI service begin already=%u published=%d\n",
        already, published);
    wifiui_log(line);
    /* chain-607 c1: every "service begin" was followed by EXC_CORPSE for
     * configd's own pid and a launchd restart (6 times in 400 s).  Because
     * DNS publication used to sit AFTER this call, it never ran once
     * (dns wait=/dns servers=/dns set= all zero).  v26 §4.8 is the
     * structural reason: configd holds preferences.plist-lock itself.
     * Do not call it from inside configd; DNS now runs on its OWN detached
     * pthread (wifiui_dns_thread) so nothing here can prevent it. */
    if (WIFIUI_ALLOW_INTERNAL_SERVICE) {
        wifiui_configure_service(sc, iokit);
    } else {
        wifiui_log("PICASSO_WIFIUI service disabled (configd-internal prefs lock, chain-607)\n");
    }
    CFRelease(store);
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

/* DNS publication gets its OWN detached pthread with its OWN dynamic-store
 * session.  chain-607 c1 proved that anything placed after
 * wifiui_configure_service() inside wifiui_bootstrap() can be skipped
 * entirely (6 configd restarts).  This thread starts at injection time,
 * independently waits for the kernel DHCP resolver file, and publishes to
 * IPMonitor's per-service input keys. */
static void *wifiui_dns_thread(void *unused)
{
    static const char sc_path[] =
        "/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration";
    wifiui_store_create_fn store_create = NULL;
    wifiui_store_set_fn store_set = NULL;
    const void *store;
    void *sc;

    (void)unused;
    sc = dlopen(sc_path, RTLD_LAZY | RTLD_LOCAL);
    if (sc == NULL) {
        wifiui_log("PICASSO_WIFIUI dns SC unavailable\n");
        return NULL;
    }
    WIFIUI_RESOLVE(store_create, sc, "SCDynamicStoreCreate");
    WIFIUI_RESOLVE(store_set, sc, "SCDynamicStoreSetValue");
    if (store_create == NULL || store_set == NULL) {
        wifiui_log("PICASSO_WIFIUI dns SC symbols unresolved\n");
        return NULL;
    }
    store = store_create(NULL, CFSTR("picasso DNS publisher"), NULL, NULL);
    if (store == NULL) {
        wifiui_log("PICASSO_WIFIUI dns store session unavailable\n");
        return NULL;
    }
    wifiui_publish_dns(store, store_set);
    CFRelease(store);
    return NULL;
}

#include "picasso-astc-guard.m"
#include "picasso-statusbar-metrics.m"
#include "picasso-keyboard-size.m"
#include "picasso-input-cache.m"

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


/* ---------------------------------------------------------------------------
 * TLS reachability probe (diagnostic, no hooks).
 *
 * Every https connection on this port stops right after the server's
 * certificate chain (74/66/1510/1510/1408 bytes in, then nothing), so the
 * failure lives in userland and never reaches the serial console.  This probe
 * makes the same request the browser would make and prints the exact error
 * domain/code, which separates "no trust anchors", "signature/decrypt error"
 * and "socket problem".
 *
 *   PICASSO_NETPROBE build=2 process=... clock=...
 *   PICASSO_NETPROBE ... https err=<code> domain=<domain> status=<http> bytes=<n>
 *   PICASSO_NETPROBE ... http  err=<code> domain=<domain> status=<http> bytes=<n>
 *
 * No interposing tuples and no Security.framework: the library stays
 * shared-cache eligible, so the build behaves exactly like the image that
 * already boots.
 *
 * IMPORTANT: printf() goes to the process stdout, which is NOT the serial
 * console for SpringBoard or apps; every line this project can see on COM5 is
 * written by wifiui_log() -> /dev/console.  The probe therefore has to use
 * picasso_probe_log(), otherwise the whole run is invisible on the console
 * (that is exactly what happened with build=3).
 * ------------------------------------------------------------------------- */
static void
picasso_probe_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    wifiui_log(line);
}

static void
picasso_net_probe_one(const char *who, const char *label, const char *url)
{
    NSURLSessionConfiguration *cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    NSURLSession *session;
    NSURLSessionDataTask *task;
    dispatch_semaphore_t done;
    __block long err_code = 0;
    __block long status = 0;
    __block unsigned long nbytes = 0;
    __block char *domain = NULL;
    __block char *desc = NULL;
    cfg.timeoutIntervalForRequest = 15;
    cfg.timeoutIntervalForResource = 25;
    cfg.waitsForConnectivity = NO;
    session = [NSURLSession sessionWithConfiguration:cfg];
    done = dispatch_semaphore_create(0);

    task = [session dataTaskWithURL:[NSURL URLWithString:[NSString stringWithUTF8String:url]]
                 completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
        if (err != nil) {
            const char *d = [[err domain] UTF8String];
            const char *m = [[err localizedDescription] UTF8String];
            err_code = (long)[err code];
            domain = strdup(d ? d : "?");
            desc = strdup(m ? m : "?");
        } else {
            status = (long)[(NSHTTPURLResponse *)resp statusCode];
            nbytes = (unsigned long)[data length];
        }
        dispatch_semaphore_signal(done);
    }];
    [task resume];
    dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 30ull * NSEC_PER_SEC));
    [session finishTasksAndInvalidate];

    picasso_probe_log("PICASSO_NETPROBE [%s] %s err=%ld domain=%s status=%ld bytes=%lu desc=%s\n",
           who, label, err_code, domain ? domain : "-", status, nbytes,
           desc ? desc : "-");
    if (domain != NULL) {
        free(domain);
    }
    if (desc != NULL) {
        free(desc);
    }
}

static void *
picasso_net_probe_thread(void *arg)
{
    char who[64];
    struct timeval tv;

    snprintf(who, sizeof(who), "%s", (const char *)arg);
    /* At boot the interface setup and DNS publication take ~130 s; an app that
     * starts later (Safari, opened after boot) already has them, so probe it
     * sooner instead of waiting pointlessly. */
    sleep(strcmp(who, "MobileSafari") == 0 ? 45 : 150);

    gettimeofday(&tv, NULL);

    @autoreleasepool {
        /* ---- 实测信任库，不靠推断 ---- */
        {
            static const char *paths[] = {
                "/private/var/protected/trustd",
                "/private/var/protected/trustd/TrustStore.sqlite3",
                "/System/Library/Security/Certificates.bundle",
                "/System/Library/Frameworks/Security.framework/Security",
                "/usr/lib/libcorecrypto.dylib",
                "/usr/lib/libnetwork.dylib",
                "/usr/lib/libpicasso-sb.dylib",
                "/System/Library/PrivateFrameworks/WebKit.framework/XPCServices",
                "/System/Library/PrivateFrameworks/WebKit.framework/XPCServices/com.apple.WebKit.WebContent.xpc",
                "/System/Library/PrivateFrameworks/WebKit.framework/XPCServices/com.apple.WebKit.Networking.xpc",
                "/System/Cryptexes",
                "/System/Cryptexes/OS",
                "/System/Cryptexes/App",
                "/System/Cryptexes/OS/System/Library/PrivateFrameworks/WebKit.framework/XPCServices",
                "/System/Cryptexes/App/System/Library/PrivateFrameworks/WebKit.framework/XPCServices",
                "/System/Library/ExtensionKit/Extensions",
                "/usr/libexec/runningboardd",
            };
            unsigned i;

            picasso_probe_log("PICASSO_NETPROBE build=13 process=%s clock=%ld\n", who, (long)tv.tv_sec);
            for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
                struct stat st;
                if (stat(paths[i], &st) == 0) {
                    picasso_probe_log("PICASSO_NETPROBE path=%s exists=1 size=%lld mode=%o\n",
                           paths[i], (long long)st.st_size, (unsigned)st.st_mode & 07777);
                } else {
                    picasso_probe_log("PICASSO_NETPROBE path=%s exists=0 errno=%d\n", paths[i], errno);
                }
            }

            /* What is actually under /System/Cryptexes?  The OS volume has no
             * WebKit XPCServices, so either the cryptex holds them or they are
             * absent from the image altogether. */
            {
                static const char *dirs[] = {
                    "/System/Cryptexes",
                    "/System/Cryptexes/App",
                    "/System/Library/PrivateFrameworks/WebKit.framework",
                    "/Applications",
                };
                unsigned d;

                for (d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
                    DIR *dp = opendir(dirs[d]);
                    char names[320];
                    size_t used = 0;
                    struct dirent *de;
                    int n = 0;

                    if (dp == NULL) {
                        picasso_probe_log("PICASSO_NETPROBE dir=%s open=0 errno=%d\n", dirs[d], errno);
                        continue;
                    }
                    names[0] = '\0';
                    while ((de = readdir(dp)) != NULL && n < 10) {
                        size_t len;

                        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
                            continue;
                        }
                        len = strlen(de->d_name);
                        if (used + len + 2 >= sizeof(names)) {
                            break;
                        }
                        memcpy(names + used, de->d_name, len);
                        used += len;
                        names[used++] = ',';
                        names[used] = '\0';
                        n++;
                    }
                    closedir(dp);
                    picasso_probe_log("PICASSO_NETPROBE dir=%s open=1 n=%d list=%s\n",
                                      dirs[d], n, names);
                }
            }

            /* /System/Cryptexes lists OS and App but stat() says ENOENT: they are
             * dangling links, i.e. the cryptexes are not mounted.  Read the link
             * targets and test whether anything there is writable, because that
             * decides where an App cryptex could be grafted in. */
            {
                static const char *links[] = {
                    "/System/Cryptexes/OS",
                    "/System/Cryptexes/App",
                    "/private/preboot/Cryptexes/App",
                };
                unsigned k;

                for (k = 0; k < sizeof(links) / sizeof(links[0]); k++) {
                    char target[256];
                    ssize_t n = readlink(links[k], target, sizeof(target) - 1);

                    if (n > 0) {
                        target[n] = '\0';
                        picasso_probe_log("PICASSO_NETPROBE link=%s -> %s\n", links[k], target);
                    } else {
                        struct stat st;

                        picasso_probe_log("PICASSO_NETPROBE link=%s n=%ld errno=%d stat=%d\n",
                                          links[k], (long)n, errno, stat(links[k], &st));
                    }
                }
                {
                    static const char *dirs_mk[] = {
                        "/System/Cryptexes/App",
                        "/private/preboot",
                    };
                    unsigned m;

                    for (m = 0; m < sizeof(dirs_mk) / sizeof(dirs_mk[0]); m++) {
                        int rc = mkdir(dirs_mk[m], 0755);

                        picasso_probe_log("PICASSO_NETPROBE mkdir=%s rc=%d errno=%d\n",
                                          dirs_mk[m], rc, errno);
                    }
                }
            }

            /* The real mount layout: stat() cannot tell a missing path from an
             * unmounted cryptex, getfsstat() can. */
            {
                struct statfs *mnt = NULL;
                int count = getfsstat(NULL, 0, MNT_NOWAIT);
                int i;

                picasso_probe_log("PICASSO_NETPROBE mounts=%d\n", count);
                if (count > 0) {
                    mnt = (struct statfs *)calloc((size_t)count, sizeof(*mnt));
                    if (mnt != NULL) {
                        count = getfsstat(mnt, count * (int)sizeof(*mnt), MNT_NOWAIT);
                        for (i = 0; i < count && i < 24; i++) {
                            picasso_probe_log("PICASSO_NETPROBE mount[%d] %s on %s\n",
                                              i, mnt[i].f_fstypename, mnt[i].f_mntonname);
                        }
                        free(mnt);
                    }
                }
            }

            /* 信任锚数量：0 = 系统没有任何根证书，https 必然失败 */
            {
                void *sec = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY);
                if (sec != NULL) {
                    CFArrayRef (*copy_anchors)(void) =
                        (CFArrayRef (*)(void))dlsym(sec, "SecTrustCopyAnchorCertificates");
                    if (copy_anchors != NULL) {
                        CFArrayRef anchors = copy_anchors();
                        if (anchors != NULL) {
                            picasso_probe_log("PICASSO_NETPROBE trust_anchors=%ld\n",
                                   (long)CFArrayGetCount(anchors));
                            CFRelease(anchors);
                        } else {
                            picasso_probe_log("PICASSO_NETPROBE trust_anchors=null\n");
                        }
                    } else {
                        picasso_probe_log("PICASSO_NETPROBE trust_anchors=symbol_missing\n");
                    }
                } else {
                    picasso_probe_log("PICASSO_NETPROBE trust_anchors=framework_missing\n");
                }
            }
        }

        picasso_net_probe_one(who, "https", "https://www.apple.com/library/test/success.html");
        picasso_net_probe_one(who, "https2", "https://captive.apple.com/hotspot-detect.html");
        picasso_net_probe_one(who, "http", "http://neverssl.com/");
    }
    return NULL;
}

static void
picasso_net_probe_start(const char *who)
{
    pthread_t t;
    char *copy = strdup(who);

    if (copy == NULL) {
        return;
    }
    if (pthread_create(&t, NULL, picasso_net_probe_thread, copy) == 0) {
        pthread_detach(t);
    } else {
        free(copy);
    }
}

/* ---------------------------------------------------------------------------
 * WebKit end-to-end probe.
 *
 * The system stack is already proven: NSURLSession inside SpringBoard gets
 * HTTP 200 from https://www.apple.com.  What is still unproven is the WebKit
 * path (WKWebView -> WebContent/Networking XPC) that Safari itself uses, so a
 * failing Safari cannot yet be told apart from a failing browser engine.  This
 * probe loads the same URL through WebKit and prints the navigation result.
 * ------------------------------------------------------------------------- */
@interface PicassoWebProbe : NSObject <WKNavigationDelegate>
@end

static PicassoWebProbe *g_web_probe;
static WKWebView *g_web_view;

@implementation PicassoWebProbe

- (void)webView:(WKWebView *)view didFinishNavigation:(WKNavigation *)navigation
{
    NSString *url = [[view URL] absoluteString];
    NSString *title = [view title];

    picasso_probe_log("PICASSO_WEBPROBE didFinish url=%s title=%s\n",
                      url != nil ? [url UTF8String] : "-",
                      title != nil ? [title UTF8String] : "-");
}

- (void)webView:(WKWebView *)view didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error
{
    const char *desc = [[error localizedDescription] UTF8String];

    (void)view;
    (void)navigation;
    picasso_probe_log("PICASSO_WEBPROBE didFailProvisional err=%ld domain=%s desc=%s\n",
                      (long)[error code], [[error domain] UTF8String], desc != NULL ? desc : "-");
}

- (void)webView:(WKWebView *)view didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error
{
    const char *desc = [[error localizedDescription] UTF8String];

    (void)view;
    (void)navigation;
    picasso_probe_log("PICASSO_WEBPROBE didFail err=%ld domain=%s desc=%s\n",
                      (long)[error code], [[error domain] UTF8String], desc != NULL ? desc : "-");
}

@end

static void
picasso_web_probe_start(void)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        Class wk = NSClassFromString(@"WKWebView");
        void *image = dlopen("/System/Library/Frameworks/WebKit.framework/WebKit", RTLD_LAZY);
        const char *err = dlerror();

        picasso_probe_log("PICASSO_WEBPROBE start class=%s dlopen=%s err=%s\n",
                          wk != nil ? "yes" : "no",
                          image != NULL ? "ok" : "fail",
                          err != NULL ? err : "-");
        g_web_probe = [[PicassoWebProbe alloc] init];
        if (wk != nil) {
            g_web_view = [[WKWebView alloc] initWithFrame:CGRectMake(0, 0, 320, 480)];
        }
        picasso_probe_log("PICASSO_WEBPROBE view=%s\n", g_web_view != nil ? "ok" : "nil");
        if (g_web_view != nil) {
            g_web_view.navigationDelegate = g_web_probe;
            [g_web_view loadRequest:[NSURLRequest requestWithURL:
                [NSURL URLWithString:@"https://www.apple.com/library/test/success.html"]]];
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 60ull * NSEC_PER_SEC),
                           dispatch_get_main_queue(), ^{
                NSString *url = [[g_web_view URL] absoluteString];

                picasso_probe_log("PICASSO_WEBPROBE timeout60 url=%s\n",
                                  url != nil ? [url UTF8String] : "-");
                if (url == nil && g_web_view != nil) {
                    picasso_probe_log("PICASSO_WEBPROBE html-load begin\n");
                    [g_web_view loadHTMLString:@"<html><body><h1>picasso</h1></body></html>"
                                       baseURL:nil];
                    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30ull * NSEC_PER_SEC),
                                   dispatch_get_main_queue(), ^{
                        NSString *after = [[g_web_view URL] absoluteString];

                        picasso_probe_log("PICASSO_WEBPROBE html-load url=%s\n",
                                          after != nil ? [after UTF8String] : "-");
                    });
                }
            });
        }

        /* The WKWebView above may be refused in SpringBoard; the real question is
         * whether Safari itself can show the page.  Try the same path the Home
         * screen icon uses: LaunchServices first, FrontBoard as the fallback.
         * openURL was refused (ok=0), so UIApplication is not the way in here. */
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20ull * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{
            Class ls = NSClassFromString(@"LSApplicationWorkspace");
            id workspace = ls != nil ? [ls performSelector:@selector(defaultWorkspace)] : nil;

            picasso_probe_log("PICASSO_WEBPROBE ls=%s ws=%s\n",
                              ls != nil ? "yes" : "no", workspace != nil ? "ok" : "nil");
            if (workspace != nil) {
                BOOL ok = NO;
                @try {
                    ok = (BOOL)[workspace performSelector:@selector(openApplicationWithBundleID:)
                                              withObject:@"com.apple.mobilesafari"];
                } @catch (NSException *e) {
                    picasso_probe_log("PICASSO_WEBPROBE ls-exception %s\n",
                                      [[e reason] UTF8String]);
                }
                picasso_probe_log("PICASSO_WEBPROBE launch-safari ls=%d\n", ok ? 1 : 0);
            }

            Class fb = NSClassFromString(@"FBSystemService");
            picasso_probe_log("PICASSO_WEBPROBE fb=%s (class only, signature not safe)\n",
                              fb != nil ? "yes" : "no");

            /* SpringBoard's own launch path: SpringBoardServices/SBSLaunchApplicationWithIdentifier.
             * LSApplicationWorkspace and UIApplication openURL both refused (ls=0 / ok=0),
             * but this is the call the Home screen icon ends up in. */
            {
                void *sbs = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", RTLD_LAZY);
                int (*sbs_launch)(CFStringRef, unsigned char) =
                    sbs != NULL ? (int (*)(CFStringRef, unsigned char))dlsym(sbs, "SBSLaunchApplicationWithIdentifier") : NULL;

                picasso_probe_log("PICASSO_WEBPROBE sbs=%s sym=%s\n",
                                  sbs != NULL ? "ok" : "fail", sbs_launch != NULL ? "ok" : "no");
                if (sbs_launch != NULL) {
                    int rc = sbs_launch(CFSTR("com.apple.mobilesafari"), 0);

                    picasso_probe_log("PICASSO_WEBPROBE sbs-launch rc=%d\n", rc);
                    picasso_ls_dump();
                    picasso_path_dump();
                    picasso_webkit_start();
                }
                picasso_metal_probe();
                picasso_sb_view();
                picasso_sb_launch_probe();
            }
        });
    });
}

/* ---------------------------------------------------------------------------
 * App cryptex bridge.
 *
 * Measured on device: /System/Cryptexes/App is a symlink to
 * ../../private/preboot/Cryptexes/App, and that target does not exist, so the
 * App cryptex -- where Safari's WebKit services (WebContent/Networking/GPU)
 * live -- is unreachable and a page can never render.  The seed can carry the
 * cryptex tree under /private/var/appcryptex; this makes the stock symlink
 * resolve to it.  Runs in configd (root, early), before any app is launched.
 * ------------------------------------------------------------------------- */
static void
picasso_cryptex_bridge(void)
{
    static const char target[] = "/private/preboot/Cryptexes/App";
    struct stat st;

    if (stat("/private/var/appcryptex", &st) != 0) {
        return;
    }
    (void)mkdir("/private/preboot", 0755);
    (void)mkdir("/private/preboot/Cryptexes", 0755);
    if (lstat(target, &st) == 0) {
        picasso_probe_log("PICASSO_CRYPTEX target exists mode=%o\n", (unsigned)st.st_mode);
        return;
    }
    if (symlink("/private/var/appcryptex", target) == 0) {
        picasso_probe_log("PICASSO_CRYPTEX linked /private/preboot/Cryptexes/App -> /private/var/appcryptex\n");
    } else {
        int rc = errno;

        picasso_probe_log("PICASSO_CRYPTEX link failed errno=%d\n", rc);
    }
}


/* ---------------------------------------------------------------------------
 * Cryptex reality check, from configd (root) instead of SpringBoard.
 *
 * Every earlier "exists=0" came from a sandboxed SpringBoard lookup, and the
 * sandbox answers ENOENT for paths it does not own -- /private/preboot is one
 * of them.  So those results never proved the cryptex was absent.  This dump
 * is the durable record: the real mount table plus stat()/readdir() of the
 * cryptex trees, taken by a root daemon.
 * ------------------------------------------------------------------------- */

static void
picasso_cryptex_dump(void)
{
    static const char *paths[] = {
        "/private/preboot",
        "/private/preboot/Cryptexes",
        "/private/preboot/Cryptexes/OS",
        "/private/preboot/Cryptexes/App",
        "/System/Cryptexes",
        "/System/Cryptexes/OS",
        "/System/Cryptexes/App",
        "/private/preboot/Cryptexes/OS/System/Library/PrivateFrameworks/WebKit.framework",
        "/private/preboot/Cryptexes/OS/System/Library/PrivateFrameworks/WebKit.framework/XPCServices",
        "/System/Library/PrivateFrameworks/WebKit.framework",
        "/System/Library/PrivateFrameworks",
        "/usr/libexec/webinspectord",
    };
    struct statfs *mounts = NULL;
    int count = getfsstat(NULL, 0, MNT_NOWAIT);
    int i;
    unsigned k;

    picasso_probe_log("PICASSO_CRYPTEX mounts=%d\n", count);
    if (count > 0) {
        mounts = (struct statfs *)calloc((size_t)count, sizeof(*mounts));
        if (mounts != NULL) {
            count = getfsstat(mounts, count * (int)sizeof(*mounts), MNT_NOWAIT);
            for (i = 0; i < count && i < 32; i++) {
                picasso_probe_log("PICASSO_CRYPTEX mount[%d] %s on %s\n",
                                  i, mounts[i].f_fstypename, mounts[i].f_mntonname);
            }
            free(mounts);
        }
    }
    for (k = 0; k < sizeof(paths) / sizeof(paths[0]); k++) {
        struct stat st;

        if (stat(paths[k], &st) == 0) {
            picasso_probe_log("PICASSO_CRYPTEX stat=%s ok mode=%o type=%s\n",
                              paths[k], (unsigned)st.st_mode,
                              S_ISDIR(st.st_mode) ? "dir" : (S_ISREG(st.st_mode) ? "file" : "other"));
        } else {
            picasso_probe_log("PICASSO_CRYPTEX stat=%s errno=%d\n", paths[k], errno);
        }
    }
    {
        static const char *dirs[] = {
            "/private/preboot",
            "/private/preboot/Cryptexes",
            "/System/Cryptexes",
            "/System/Library/PrivateFrameworks",
        };

        for (k = 0; k < sizeof(dirs) / sizeof(dirs[0]); k++) {
            DIR *dp = opendir(dirs[k]);
            char names[400];
            size_t used = 0;
            struct dirent *entry;
            unsigned n = 0;

            if (dp == NULL) {
                picasso_probe_log("PICASSO_CRYPTEX dir=%s open=0 errno=%d\n", dirs[k], errno);
                continue;
            }
            names[0] = '\0';
            while ((entry = readdir(dp)) != NULL && n < 12) {
                size_t len;

                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                len = strlen(entry->d_name);
                if (used + len + 2 >= sizeof(names)) {
                    break;
                }
                memcpy(names + used, entry->d_name, len);
                used += len;
                names[used++] = ',';
                names[used] = '\0';
                n++;
            }
            closedir(dp);
            picasso_probe_log("PICASSO_CRYPTEX dir=%s open=1 n=%u list=%s\n", dirs[k], n, names);
        }
    }
}


/* ---------------------------------------------------------------------------
 * Is MobileSafari actually registered with LaunchServices?
 *
 * The launch attempts failed three different ways (openApplicationWithBundleID
 * -> NO, openURL -> NO, SBSLaunchApplicationWithIdentifier -> 11).  If the app
 * is missing from lsd's database, every one of those fails no matter what
 * WebKit does, and that would be a second, independent blocker.  Enumerate the
 * database and say plainly whether the bundle is there.
 * ------------------------------------------------------------------------- */

static void
picasso_ls_dump(void)
{
    Class workspace_class = NSClassFromString(@"LSApplicationWorkspace");
    id workspace;
    NSArray *apps;
    unsigned listed = 0;
    int found = 0;

    if (workspace_class == Nil) {
        picasso_probe_log("PICASSO_LS no LSApplicationWorkspace\n");
        return;
    }
    workspace = [workspace_class performSelector:@selector(defaultWorkspace)];
    if (workspace == nil) {
        picasso_probe_log("PICASSO_LS no defaultWorkspace\n");
        return;
    }
    apps = [workspace performSelector:@selector(allApplications)];
    picasso_probe_log("PICASSO_LS apps=%lu\n", (unsigned long)apps.count);
    for (id app in apps) {
        NSString *bundle = nil;

        if ([app respondsToSelector:@selector(bundleIdentifier)]) {
            bundle = [app performSelector:@selector(bundleIdentifier)];
        }
        if (bundle == nil) {
            continue;
        }
        if ([bundle isEqualToString:@"com.apple.mobilesafari"]) {
            found = 1;
            picasso_probe_log("PICASSO_LS found=com.apple.mobilesafari\n");
        }
        if (listed < 8) {
            picasso_probe_log("PICASSO_LS app[%u]=%s\n", listed,
                              [bundle UTF8String]);
            listed++;
        }
    }
    picasso_probe_log("PICASSO_LS safari=%d listed=%u\n", found, listed);
    if (!found) {
        /* The staged mount may postdate lsd's scan; ask it to look again. */
        SEL refresh = @selector(enumerateBundlesOfType:block:);
        if ([workspace respondsToSelector:refresh]) {
            picasso_probe_log("PICASSO_LS refresh=available\n");
        }
    }
}


/* ---------------------------------------------------------------------------
 * What does the system think the network path is?
 *
 * Time sync works (a daemon opening a raw socket), yet apps behave as if there
 * is no network.  Those are two different questions in iOS: a daemon can put
 * bytes on the wire while Network.framework reports the path as unsatisfied,
 * because path evaluation comes from NECP policy, not from "can I connect".
 * Safari consults exactly this before it even starts a load.  Log it.
 *
 * Network.framework lives only in the dyld shared cache on this image, so the
 * symbols are resolved with dlsym rather than linked.
 * ------------------------------------------------------------------------- */

typedef void *picasso_nw_path_monitor;
typedef void *picasso_nw_path;
typedef void (^picasso_path_handler)(picasso_nw_path);

static void
picasso_path_dump(void)
{
    void *framework = dlopen("/System/Library/Frameworks/Network.framework/Network", RTLD_LAZY);
    picasso_nw_path_monitor (*monitor_create)(void);
    void (*set_handler)(picasso_nw_path_monitor, picasso_path_handler);
    void (*set_queue)(picasso_nw_path_monitor, dispatch_queue_t);
    void (*monitor_start)(picasso_nw_path_monitor);
    int (*get_status)(picasso_nw_path);
    char description[256];
    picasso_nw_path_monitor monitor;

    picasso_probe_log("PICASSO_PATH dlopen=%s\n", framework != NULL ? "ok" : "fail");
    if (framework == NULL) {
        return;
    }
    monitor_create = (picasso_nw_path_monitor (*)(void))dlsym(framework, "nw_path_monitor_create");
    set_handler = (void (*)(picasso_nw_path_monitor, picasso_path_handler))dlsym(framework, "nw_path_monitor_set_update_handler");
    set_queue = (void (*)(picasso_nw_path_monitor, dispatch_queue_t))dlsym(framework, "nw_path_monitor_set_queue");
    monitor_start = (void (*)(picasso_nw_path_monitor))dlsym(framework, "nw_path_monitor_start");
    get_status = (int (*)(picasso_nw_path))dlsym(framework, "nw_path_get_status");
    if (monitor_create == NULL || set_handler == NULL || monitor_start == NULL || get_status == NULL) {
        picasso_probe_log("PICASSO_PATH symbols missing create=%d handler=%d start=%d status=%d\n",
                          monitor_create != NULL, set_handler != NULL,
                          monitor_start != NULL, get_status != NULL);
        return;
    }
    snprintf(description, sizeof(description), "PICASSO_PATH");
    monitor = monitor_create();
    set_handler(monitor, ^(picasso_nw_path path) {
        /* 1 = satisfied, 2 = unsatisfied, 3 = satisfiable */
        picasso_probe_log("PICASSO_PATH status=%d\n", get_status(path));
    });
    if (set_queue != NULL) {
        set_queue(monitor, dispatch_get_global_queue(0, 0));
    }
    monitor_start(monitor);
    sleep(6);
    picasso_probe_log("PICASSO_PATH done\n");
}


/* ---------------------------------------------------------------------------
 * Can anything be created on the system volume?
 *
 * The port already bind-mounts onto /System/Library/SystemConfiguration/
 * InterfaceNamer.bundle, so mounts into the system tree are possible -- but
 * only onto paths that exist.  WebKit.framework does not exist, so the mount
 * needs the directory created first, and the root volume is APFS read-only.
 * Find out which side of that line this image is on before building on it.
 * ------------------------------------------------------------------------- */

static void
picasso_fs_write_probe(void)
{
    static const char *dirs[] = {
        "/System/Library/PrivateFrameworks/WebKit.framework",
        "/System/Library/PrivateFrameworks/.picasso-probe",
    };
    unsigned i;

    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        int rc = mkdir(dirs[i], 0755);
        int saved = errno;

        picasso_probe_log("PICASSO_FS mkdir=%s rc=%d errno=%d\n", dirs[i], rc, saved);
        if (rc == 0) {
            (void)rmdir(dirs[i]);
        }
    }
    {
        const char *file = "/System/Library/PrivateFrameworks/.picasso-write-test";
        int fd = open(file, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        int saved = errno;

        picasso_probe_log("PICASSO_FS creat=%s fd=%d errno=%d\n", file, fd, saved);
        if (fd >= 0) {
            close(fd);
            (void)unlink(file);
        }
    }
}


/* ---------------------------------------------------------------------------
 * App cryptex pull (device side).
 *
 * Why this exists: the WebKit services Safari needs (WebContent/Networking/GPU)
 * are not in the OS volume at all -- measured, XPCServices exists=0 -- they come
 * from the App cryptex, which this image does not carry.  The Data seed cannot
 * carry it either: the packed seed format is capped at 4 MiB and the cryptex is
 * 10 MiB.  The device's own WiFi works (proven with a 3961-byte page), so the
 * device fetches the cryptex over the LAN after boot and unpacks it to exactly
 * the path the stock /System/Cryptexes/App symlink points at.
 *
 * Runs in configd (root, early): a detached thread retries until the radio is
 * up, downloads once, unpacks, then verifies the completeness marker.
 * ------------------------------------------------------------------------- */

static const char kWebkitURL[] = "http://192.168.101.4:8099/webkit.tar";
static const char kWebkitRoot[] = "/private/var/picasso-webkit";
static const char kWebkitMark[] =
    "/private/var/picasso-webkit/WebKit.framework/Info.plist";

static unsigned long long
picasso_tar_octal(const char *text, size_t width)
{
    unsigned long long value = 0;
    size_t i;

    for (i = 0; i < width && text[i] != '\0'; i++) {
        if (text[i] >= '0' && text[i] <= '7') {
            value = value * 8 + (unsigned long long)(text[i] - '0');
        } else if (text[i] == ' ' || text[i] == '\0') {
            break;
        }
    }
    return value;
}

static void
picasso_mkdirs(const char *path)
{
    char buffer[2048];
    size_t i, length = strlen(path);

    if (length == 0 || length >= sizeof(buffer)) {
        return;
    }
    memcpy(buffer, path, length + 1);
    for (i = 1; i < length; i++) {
        if (buffer[i] == '/') {
            buffer[i] = '\0';
            (void)mkdir(buffer, 0755);
            buffer[i] = '/';
        }
    }
    (void)mkdir(buffer, 0755);
}

static int
picasso_tar_unsafe(const char *name)
{
    const char *p = name;

    if (name[0] == '/') {
        return 1;
    }
    while (*p != '\0') {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
            return 1;
        }
        while (*p != '\0' && *p != '/') {
            p++;
        }
        while (*p == '/') {
            p++;
        }
    }
    return 0;
}

static unsigned
picasso_untar(const unsigned char *base, size_t length, const char *root)
{
    const unsigned char *cursor = base;
    const unsigned char *end = base + length;
    char longname[1600];
    char longlink[1600];
    unsigned written = 0;

    longname[0] = '\0';
    longlink[0] = '\0';

    while ((size_t)(end - cursor) >= 512) {
        const char *header = (const char *)cursor;
        char name[1600];
        char link[1600];
        unsigned long long size;
        unsigned mode;
        char type;

        if (header[0] == '\0') {
            break;
        }
        size = picasso_tar_octal(header + 124, 12);
        mode = (unsigned)picasso_tar_octal(header + 100, 8) & 07777u;
        type = header[156];

        if (type == 'L' || type == 'K') {
            char *slot = (type == 'L') ? longname : longlink;
            size_t room = (type == 'L') ? sizeof(longname) : sizeof(longlink);
            size_t take = (size_t)size < room - 1 ? (size_t)size : room - 1;

            memcpy(slot, cursor + 512, take);
            slot[take] = '\0';
            cursor += 512 + ((size + 511ull) & ~511ull);
            continue;
        }
        if (type == 'x' || type == 'g') {                 /* pax headers: skip */
            cursor += 512 + ((size + 511ull) & ~511ull);
            continue;
        }
        if (longname[0] != '\0') {
            strlcpy(name, longname, sizeof(name));
            longname[0] = '\0';
        } else {
            const char *prefix;

            memcpy(name, header, 100);
            name[100] = '\0';
            prefix = header + 345;
            if (prefix[0] != '\0' && memcmp(header + 257, "ustar", 5) == 0) {
                char joined[1600];

                snprintf(joined, sizeof(joined), "%s/%s", prefix, name);
                strlcpy(name, joined, sizeof(name));
            }
        }
        if (longlink[0] != '\0') {
            strlcpy(link, longlink, sizeof(link));
            longlink[0] = '\0';
        } else {
            memcpy(link, header + 157, 100);
            link[100] = '\0';
        }
        while (name[0] == '.' && name[1] == '/') {
            memmove(name, name + 2, strlen(name + 2) + 1);
        }
        if (name[0] == '\0' || picasso_tar_unsafe(name)) {
            cursor += 512 + ((size + 511ull) & ~511ull);
            continue;
        }
        {
            char destination[2048];
            char parent[2048];
            char *slash;

            snprintf(destination, sizeof(destination), "%s/%s", root, name);
            strlcpy(parent, destination, sizeof(parent));
            slash = strrchr(parent, '/');
            if (slash != NULL) {
                *slash = '\0';
                picasso_mkdirs(parent);
            }
            if (type == '5') {
                picasso_mkdirs(destination);
            } else if (type == '2') {
                (void)unlink(destination);
                if (symlink(link, destination) == 0) {
                    written++;
                }
            } else if (type == '0' || type == '\0' || type == '7') {
                int fd = open(destination, O_CREAT | O_WRONLY | O_TRUNC,
                              (mode & 0777u) != 0 ? (mode & 0777u) : 0644);

                if (fd >= 0) {
                    size_t left = (size_t)size;
                    const unsigned char *chunk = cursor + 512;

                    while (left > 0) {
                        ssize_t rc = write(fd, chunk, left);

                        if (rc <= 0) {
                            break;
                        }
                        chunk += rc;
                        left -= (size_t)rc;
                    }
                    close(fd);
                    if (mode & 0111u) {
                        (void)chmod(destination, (mode & 0777u) | 0700u);
                    }
                    written++;
                }
            }
        }
        cursor += 512 + ((size + 511ull) & ~511ull);
    }
    return written;
}

static void *
picasso_webkit_thread(void *unused)
{
    unsigned attempt;

    (void)unused;
    for (attempt = 0; attempt < 45; attempt++) {
        @autoreleasepool {
            __block NSData *payload = nil;
            dispatch_semaphore_t done;
            NSURLSession *session;
            NSURL *url;

            if (access(kWebkitMark, F_OK) == 0) {
                picasso_probe_log("PICASSO_WEBKIT_PULL already present attempt=%u\n", attempt);
                return NULL;
            }
            url = [NSURL URLWithString:[NSString stringWithUTF8String:kWebkitURL]];
            if (url == nil) {
                return NULL;
            }
            done = dispatch_semaphore_create(0);
            session = [NSURLSession sessionWithConfiguration:
                       [NSURLSessionConfiguration ephemeralSessionConfiguration]];
            [[session dataTaskWithURL:url completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
                NSInteger status = 0;

                if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
                    status = [(NSHTTPURLResponse *)response statusCode];
                }
                if (data.length > 0 && status == 200) {
                    payload = data;
                }
                picasso_probe_log("PICASSO_WEBKIT_PULL get attempt=%u status=%ld bytes=%lu err=%ld\n",
                                  attempt, (long)status, (unsigned long)data.length,
                                  (long)(error != nil ? [error code] : 0));
                dispatch_semaphore_signal(done);
            }] resume];
            dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 90ull * NSEC_PER_SEC));
            [session invalidateAndCancel];

            if (payload.length > 1024) {
                unsigned count = picasso_untar((const unsigned char *)[payload bytes],
                                               (size_t)[payload length], kWebkitRoot);

                picasso_probe_log("PICASSO_WEBKIT_PULL unpack files=%u marker=%d\n",
                                  count, access(kWebkitMark, F_OK) == 0);
                return NULL;
            }
            sleep(8);
        }
    }
    picasso_probe_log("PICASSO_WEBKIT_PULL gave up\n");
    return NULL;
}

static void
picasso_webkit_start(void)
{
    pthread_t thread;

    if (pthread_create(&thread, NULL, picasso_webkit_thread, NULL) == 0) {
        (void)pthread_detach(thread);
    }
}


/* ---------------------------------------------------------------------------
 * Is /private/preboot writable?
 *
 * The rootfs keeps WebKit at a symlink chain:
 *   /System/Library/Frameworks/WebKit.framework
 *     -> /System/Cryptexes/OS/System/Library/Frameworks/WebKit.framework
 *     -> /private/preboot/Cryptexes/OS/System/Library/Frameworks/WebKit.framework
 * and the SystemOS cryptex holds the real directory.  Everything after that
 * path is missing on this image, so if the data volume is writable there we can
 * simply put the framework there and the chain resolves -- no kernel change.
 * ------------------------------------------------------------------------- */

static void
picasso_preboot_probe(void)
{
    static const char *dirs[] = {
        "/private/preboot",
        "/private/preboot/Cryptexes",
        "/private/preboot/Cryptexes/OS",
    };
    unsigned i;

    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        struct stat st;

        if (stat(dirs[i], &st) == 0) {
            picasso_probe_log("PICASSO_PREBOOT stat=%s ok mode=%o\n", dirs[i], (unsigned)st.st_mode);
        } else {
            picasso_probe_log("PICASSO_PREBOOT stat=%s errno=%d\n", dirs[i], errno);
        }
    }
    {
        const char *path = "/private/preboot/Cryptexes";
        int rc = mkdir(path, 0755);

        picasso_probe_log("PICASSO_PREBOOT mkdir=%s rc=%d errno=%d\n", path, rc, errno);
    }
    {
        const char *file = "/private/preboot/.picasso-write-test";
        int fd = open(file, O_CREAT | O_WRONLY | O_TRUNC, 0644);

        picasso_probe_log("PICASSO_PREBOOT creat=%s fd=%d errno=%d\n", file, fd, errno);
        if (fd >= 0) {
            close(fd);
            (void)unlink(file);
        }
    }
}


/* ---------------------------------------------------------------------------
 * Can configd (root) mount by itself?
 *
 * /private/preboot is on the read-only root, and the rootfs keeps WebKit behind
 * a symlink chain that ends there:
 *   /System/Library/Frameworks/WebKit.framework
 *     -> /System/Cryptexes/OS/... -> /private/preboot/Cryptexes/OS/...
 * So the directory has to be grafted in.  If mount(2) is permitted here the
 * whole fix is userland; if it returns EPERM it needs a kernel hook.
 * The bindfs type is the one this port already uses for /Applications.
 * ------------------------------------------------------------------------- */

static void
picasso_mount_probe(void)
{
    static const char *source = "/private/var/ngpu/preboot";
    int rc;

    (void)mkdir("/private/var/ngpu", 0755);
    (void)mkdir("/private/var/ngpu/preboot", 0755);
    (void)mkdir("/private/var/ngpu/preboot/Cryptexes", 0755);
    picasso_probe_log("PICASSO_MOUNT source_ok=%d\n", access(source, F_OK) == 0);

    errno = 0;
    rc = mount("bindfs", "/private/preboot", MNT_RDONLY | MNT_UNION, (void *)source);
    picasso_probe_log("PICASSO_MOUNT union rc=%d errno=%d\n", rc, errno);

    if (rc != 0) {
        errno = 0;
        rc = mount("bindfs", "/private/preboot", MNT_RDONLY, (void *)source);
        picasso_probe_log("PICASSO_MOUNT plain rc=%d errno=%d\n", rc, errno);
    }
    {
        struct stat st;

        picasso_probe_log("PICASSO_MOUNT after cryptexes=%d errno=%d\n",
                          stat("/private/preboot/Cryptexes", &st), errno);
    }
}


/* ---------------------------------------------------------------------------
 * Does the WebKit chain resolve end to end?
 *
 * Four links, in order: the public framework path, the cryptex view, the
 * preboot union, and the seed copy.  Printing all four says exactly which link
 * is still broken instead of only that "WebKit is missing".
 * ------------------------------------------------------------------------- */

static void
picasso_webkit_chain(void)
{
    static const char *paths[] = {
        "/System/Library/Frameworks/WebKit.framework/Info.plist",
        "/System/Cryptexes/OS/System/Library/Frameworks/WebKit.framework/Info.plist",
        "/private/preboot/Cryptexes/OS/System/Library/Frameworks/WebKit.framework/Info.plist",
        "/private/var/ngpu/preboot/Cryptexes/OS/System/Library/Frameworks/WebKit.framework/Info.plist",
    };
    unsigned i;

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        struct stat st;

        if (stat(paths[i], &st) == 0) {
            picasso_probe_log("PICASSO_WEBKIT_CHAIN ok size=%lld %s\n",
                              (long long)st.st_size, paths[i]);
        } else {
            picasso_probe_log("PICASSO_WEBKIT_CHAIN errno=%d %s\n", errno, paths[i]);
        }
    }
    {
        DIR *dp = opendir("/System/Library/Frameworks/WebKit.framework/XPCServices");
        char names[300];
        size_t used = 0;
        struct dirent *entry;
        unsigned n = 0;

        if (dp == NULL) {
            picasso_probe_log("PICASSO_WEBKIT_CHAIN xpc_open=0 errno=%d\n", errno);
            return;
        }
        names[0] = '\0';
        while ((entry = readdir(dp)) != NULL && n < 8) {
            size_t len;

            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            len = strlen(entry->d_name);
            if (used + len + 2 >= sizeof(names)) {
                break;
            }
            memcpy(names + used, entry->d_name, len);
            used += len;
            names[used++] = ',';
            names[used] = '\0';
            n++;
        }
        closedir(dp);
        picasso_probe_log("PICASSO_WEBKIT_CHAIN xpc n=%u list=%s\n", n, names);
    }
}


/* ---------------------------------------------------------------------------
 * Rebuild the LaunchServices database once, now that WebKit is reachable.
 *
 * lsd built its store while /System/Cryptexes/OS led nowhere, so it never
 * registered what sits behind it -- WebKit's content processes among them --
 * and it does not re-register system content on a later boot of the same OS
 * build.  Deleting the store once makes lsd rebuild with the path in place; a
 * marker keeps later boots from repeating it.
 *
 * The store lives in a daemon container whose UUID differs per install, so
 * every candidate directory is scanned instead of hard-coding one.
 * ------------------------------------------------------------------------- */

static unsigned
picasso_ls_drop_dir(const char *dir)
{
    DIR *dp = opendir(dir);
    struct dirent *entry;
    unsigned removed = 0;

    if (dp == NULL) {
        return 0;
    }
    while ((entry = readdir(dp)) != NULL) {
        char path[1024];

        if (strncmp(entry->d_name, "com.apple.LaunchServices-", 25) != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        if (unlink(path) == 0) {
            removed++;
            picasso_probe_log("PICASSO_LS_DROP unlink %s\n", path);
        }
    }
    closedir(dp);
    return removed;
}

static void
picasso_ls_store_drop(void)
{
    static const char marker[] = "/private/var/ngpu/ls-store-rebuilt-2";
    static const char *const roots[] = {
        "/private/var/mobile/Library/Caches",
        "/private/var/root/Library/Caches",
        "/private/var/installd/Library/Caches",
    };
    static const char daemons[] = "/private/var/mobile/Containers/Data/InternalDaemon";
    unsigned removed = 0, dirs = 0, i;
    int fd;

    if (access(marker, F_OK) == 0) {
        picasso_probe_log("PICASSO_LS_DROP already done\n");
        return;
    }
    /* Only act once the cryptex view is actually in place. */
    if (access("/System/Cryptexes/OS/System/Library/Frameworks/WebKit.framework/Info.plist",
               F_OK) != 0) {
        picasso_probe_log("PICASSO_LS_DROP skipped webkit=absent\n");
        return;
    }
    for (i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        unsigned n = picasso_ls_drop_dir(roots[i]);

        if (n > 0) {
            dirs++;
            removed += n;
        }
    }
    /* InternalDaemon holds one container per daemon; scan them all. */
    {
        DIR *dp = opendir(daemons);
        struct dirent *entry;

        if (dp != NULL) {
            while ((entry = readdir(dp)) != NULL) {
                char caches[1024];
                unsigned n;

                if (entry->d_name[0] == '.') {
                    continue;
                }
                snprintf(caches, sizeof(caches), "%s/%s/Library/Caches", daemons,
                         entry->d_name);
                n = picasso_ls_drop_dir(caches);
                if (n > 0) {
                    dirs++;
                    removed += n;
                }
            }
            closedir(dp);
        }
    }
    fd = open(marker, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) {
        close(fd);
    }
    picasso_probe_log("PICASSO_LS_DROP dirs=%u removed=%u\n", dirs, removed);
}
/* ---------------------------------------------------------------------------
 * Launch Safari the way the Home screen does.
 *
 * SBSLaunchApplicationWithIdentifier is documented to work only for callers
 * that hold com.apple.springboard.launchapplications, and our injected library
 * does not; LSApplicationWorkspace::openApplicationWithBundleID refused too
 * (ls=0).  Inside SpringBoard the icon tap ends in SpringBoard's own
 * controllers, so drive those and report exactly which step the runtime
 * refuses.  Private selectors are declared on NSObject so the compiler emits
 * ordinary sends; everything is guarded by respondsToSelector:.
 * ------------------------------------------------------------------------- */

@interface UIApplication (PicassoPrivateLaunch)
- (BOOL)launchApplicationWithIdentifier:(NSString *)identifier suspended:(BOOL)suspended;
@end

@interface NSObject (PicassoFrontBoardOpen)
+ (id)sharedService;
- (void)openApplication:(NSString *)identifier options:(id)options
             withResult:(void (^)(BOOL ok, NSError *error))result;
@end

@interface NSObject (PicassoSpringBoardLaunch)
+ (id)sharedInstance;
- (id)applicationWithBundleIdentifier:(NSString *)identifier;
- (id)applicationWithDisplayIdentifier:(NSString *)identifier;
- (void)activateApplicationFromSwitcher:(id)application;
- (void)activateApplicationAnimated:(id)application;
- (void)launch;
- (void)_activateApplicationFromAccessibility:(id)application;
- (void)activateApplication:(id)application fromIcon:(id)icon location:(id)location
    activationSettings:(id)settings actions:(id)actions;
@end


static void
picasso_sb_list_selectors(Class cls, const char *label, const char *const *needles, unsigned needles_n)
{
    Method *methods;
    unsigned count = 0, i, n, listed = 0;
    char names[700];
    size_t used = 0;

    if (cls == nil) {
        picasso_probe_log("PICASSO_SBLAUNCH %s=no\n", label);
        return;
    }
    methods = class_copyMethodList(cls, &count);
    if (methods == NULL) {
        picasso_probe_log("PICASSO_SBLAUNCH %s methods=0\n", label);
        return;
    }
    names[0] = '\0';
    for (i = 0; i < count && listed < 10; i++) {
        const char *name = sel_getName(method_getName(methods[i]));

        for (n = 0; n < needles_n; n++) {
            if (strstr(name, needles[n]) == NULL) {
                continue;
            }
            if (used + strlen(name) + 2 < sizeof(names)) {
                int w = snprintf(names + used, sizeof(names) - used, "%s,", name);

                if (w > 0) {
                    used += (size_t)w;
                    listed++;
                }
            }
            break;
        }
    }
    free(methods);
    picasso_probe_log("PICASSO_SBLAUNCH %s total=%u sel=%s\n", label, count,
                      names[0] != '\0' ? names : "-");
}

static void
picasso_sb_launch_probe(void)
{
    Class appc = NSClassFromString(@"SBApplicationController");
    Class uic = NSClassFromString(@"SBUIController");
    id app = nil;
    unsigned total, i, matched = 0, listed = 0;
    char names[640];
    size_t used = 0;

    names[0] = '\0';
    total = objc_getClassList(NULL, 0);
    if (total > 0 && total < 40000) {
        Class *all = (Class *)calloc(total, sizeof(Class));

        if (all != NULL) {
            total = objc_getClassList(all, (int)total);
            for (i = 0; i < total && listed < 10; i++) {
                const char *name = class_getName(all[i]);

                if (name == NULL ||
                    (strstr(name, "ApplicationController") == NULL &&
                     strstr(name, "UIController") == NULL &&
                     strstr(name, "IconController") == NULL)) {
                    continue;
                }
                matched++;
                if (used + strlen(name) + 2 < sizeof(names)) {
                    int n = snprintf(names + used, sizeof(names) - used, "%s,", name);

                    if (n > 0) {
                        used += (size_t)n;
                        listed++;
                    }
                }
            }
            free(all);
        }
    }
    picasso_probe_log("PICASSO_SBLAUNCH classes=%u names=%s\n", matched,
                      names[0] != '\0' ? names : "-");

    if (appc == nil) {
        picasso_probe_log("PICASSO_SBLAUNCH appc=no\n");
    } else {
        id controller = nil;

        @try {
            controller = [appc sharedInstance];
        } @catch (NSException *e) {
            picasso_probe_log("PICASSO_SBLAUNCH appc-exception %s\n", [[e reason] UTF8String]);
        }
        picasso_probe_log("PICASSO_SBLAUNCH appc=yes svc=%s\n",
                          controller != nil ? "ok" : "nil");
        if (controller != nil) {
            @try {
                if ([controller respondsToSelector:@selector(applicationWithBundleIdentifier:)]) {
                    app = [controller applicationWithBundleIdentifier:@"com.apple.mobilesafari"];
                }
                if (app == nil &&
                    [controller respondsToSelector:@selector(applicationWithDisplayIdentifier:)]) {
                    app = [controller applicationWithDisplayIdentifier:@"com.apple.mobilesafari"];
                }
                picasso_probe_log("PICASSO_SBLAUNCH app=%s class=%s\n",
                                  app != nil ? "ok" : "nil",
                                  app != nil ? class_getName([app class]) : "-");
            } @catch (NSException *e) {
                picasso_probe_log("PICASSO_SBLAUNCH app-exception %s\n",
                                  [[e reason] UTF8String]);
            }
        }
    }

    {
        static const char *const ui_needles[] = {"activate", "launch", "open", "present"};
        static const char *const app_needles[] = {"launch", "activate", "open"};

        picasso_sb_list_selectors(uic, "uicm", ui_needles,
                                  sizeof(ui_needles) / sizeof(ui_needles[0]));
        if (app != nil) {
            picasso_sb_list_selectors([app class], "appm", app_needles,
                                      sizeof(app_needles) / sizeof(app_needles[0]));
        }
    }

    if (uic == nil) {
        picasso_probe_log("PICASSO_SBLAUNCH uic=no\n");
    } else {
        id controller = nil;

        @try {
            controller = [uic sharedInstance];
        } @catch (NSException *e) {
            picasso_probe_log("PICASSO_SBLAUNCH uic-exception %s\n", [[e reason] UTF8String]);
        }
        picasso_probe_log("PICASSO_SBLAUNCH uic=yes svc=%s\n",
                          controller != nil ? "ok" : "nil");
        if (controller != nil && app != nil) {
            @try {
                if ([controller respondsToSelector:@selector(activateApplicationFromSwitcher:)]) {
                    [controller activateApplicationFromSwitcher:app];
                    picasso_probe_log("PICASSO_SBLAUNCH activate=switcher\n");
                } else if ([controller respondsToSelector:@selector(activateApplicationAnimated:)]) {
                    [controller activateApplicationAnimated:app];
                    picasso_probe_log("PICASSO_SBLAUNCH activate=animated\n");
                } else {
                    picasso_probe_log("PICASSO_SBLAUNCH activate=nosel\n");
                }
            } @catch (NSException *e) {
                picasso_probe_log("PICASSO_SBLAUNCH activate-exception %s\n",
                                  [[e reason] UTF8String]);
            }
        }
        /* iOS 16 renamed the activation entry points; try the ones the runtime
         * actually reports before giving up. */
        if (controller != nil && app != nil) {
            @try {
                if ([controller respondsToSelector:@selector(_activateApplicationFromAccessibility:)]) {
                    [controller _activateApplicationFromAccessibility:app];
                    picasso_probe_log("PICASSO_SBLAUNCH activate=accessibility\n");
                }
                if ([controller respondsToSelector:
                        @selector(activateApplication:fromIcon:location:activationSettings:actions:)]) {
                    [controller activateApplication:app fromIcon:nil location:@"SBIconLocationDock"
                                 activationSettings:nil actions:nil];
                    picasso_probe_log("PICASSO_SBLAUNCH activate=workspace\n");
                }
            } @catch (NSException *e) {
                picasso_probe_log("PICASSO_SBLAUNCH activate2-exception %s\n",
                                  [[e reason] UTF8String]);
            }
        }
        /* The old SpringBoard-only private API; harmless when absent. */
        {
            UIApplication *app_shared = [UIApplication sharedApplication];

            if ([app_shared respondsToSelector:
                    @selector(launchApplicationWithIdentifier:suspended:)]) {
                @try {
                    BOOL ok = [app_shared launchApplicationWithIdentifier:@"com.apple.mobilesafari"
                                                                suspended:NO];
                    picasso_probe_log("PICASSO_SBLAUNCH uiapp-launch ok=%d\n", ok ? 1 : 0);
                } @catch (NSException *e) {
                    picasso_probe_log("PICASSO_SBLAUNCH uiapp-exception %s\n",
                                      [[e reason] UTF8String]);
                }
            } else {
                picasso_probe_log("PICASSO_SBLAUNCH uiapp-launch=nosel\n");
            }
        }

        /* FrontBoard's own entry point: what a tap ends up calling in iOS 16. */
        {
            Class fbs = NSClassFromString(@"FBSSystemService");

            if (fbs != nil) {
                id service = nil;

                @try {
                    service = [fbs sharedService];
                } @catch (NSException *e) {
                    picasso_probe_log("PICASSO_SBLAUNCH fbs-exception %s\n",
                                      [[e reason] UTF8String]);
                }
                picasso_probe_log("PICASSO_SBLAUNCH fbs=%s svc=%s\n",
                                  "yes", service != nil ? "ok" : "nil");
                if (service != nil &&
                    [service respondsToSelector:@selector(openApplication:options:withResult:)]) {
                    @try {
                        [service openApplication:@"com.apple.mobilesafari" options:@{}
                                      withResult:^(BOOL ok, NSError *error) {
                            picasso_probe_log("PICASSO_SBLAUNCH fbs-open ok=%d err=%s\n",
                                              ok ? 1 : 0,
                                              error != nil ? [[error description] UTF8String] : "-");
                        }];
                        picasso_probe_log("PICASSO_SBLAUNCH fbs-open sent\n");
                    } @catch (NSException *e) {
                        picasso_probe_log("PICASSO_SBLAUNCH fbs-open-exception %s\n",
                                          [[e reason] UTF8String]);
                    }
                }
            } else {
                picasso_probe_log("PICASSO_SBLAUNCH fbs=no\n");
            }
        }

        if (app != nil) {
            @try {
                if ([app respondsToSelector:@selector(launch)]) {
                    [app launch];
                    picasso_probe_log("PICASSO_SBLAUNCH app-launch=ok\n");
                }
            } @catch (NSException *e) {
                picasso_probe_log("PICASSO_SBLAUNCH activate-exception %s\n",
                                  [[e reason] UTF8String]);
            }
        }
    }

    /* Is the bundle's executable actually on disk and readable? */
    {
        struct stat st;
        int rc = stat("/Applications/MobileSafari.app/MobileSafari", &st);

        picasso_probe_log("PICASSO_SBLAUNCH exe rc=%d errno=%d size=%lld\n", rc,
                          rc == 0 ? 0 : errno, rc == 0 ? (long long)st.st_size : 0LL);
    }
}

/* ---------------------------------------------------------------------------
 * What does a sandboxed app actually see?
 *
 * Root (configd) resolved every link of the WebKit chain, but apps do not run
 * with that view: the chain ends inside /private/preboot, which application
 * sandboxes do not reach.  Print the same probes from SpringBoard so a denial
 * shows up as EPERM instead of silently making WebKit look absent.
 * ------------------------------------------------------------------------- */

static void
picasso_sb_view(void)
{
    static const char *const paths[] = {
        "/System/Library/Frameworks/WebKit.framework",
        "/System/Library/Frameworks/WebKit.framework/Info.plist",
        "/System/Library/Frameworks/WebKit.framework/XPCServices",
        "/System/Cryptexes/OS/System/Library/Frameworks/WebKit.framework",
        "/private/preboot/Cryptexes/OS/System/Library/Frameworks/WebKit.framework",
    };
    unsigned i;

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        struct stat st;

        if (stat(paths[i], &st) == 0) {
            picasso_probe_log("PICASSO_SBVIEW ok type=%s size=%lld %s\n",
                              S_ISDIR(st.st_mode) ? "dir" : "file",
                              (long long)st.st_size, paths[i]);
        } else {
            picasso_probe_log("PICASSO_SBVIEW errno=%d %s\n", errno, paths[i]);
        }
    }
    {
        DIR *dp = opendir("/System/Library/Frameworks/WebKit.framework/XPCServices");
        struct dirent *entry;
        unsigned n = 0;

        if (dp == NULL) {
            picasso_probe_log("PICASSO_SBVIEW xpc opendir=no errno=%d\n", errno);
            return;
        }
        while ((entry = readdir(dp)) != NULL) {
            if (entry->d_name[0] == '.') {
                continue;
            }
            if (n < 6) {
                picasso_probe_log("PICASSO_SBVIEW xpc entry=%s\n", entry->d_name);
            }
            n++;
        }
        closedir(dp);
        picasso_probe_log("PICASSO_SBVIEW xpc entries=%u\n", n);
    }

    /* Where does lsd keep its store on this install?  configd cannot read the
     * mobile containers, so ask from SpringBoard, which runs as mobile. */
    {
        static const char *const stores[] = {
            "/private/var/mobile/Library/Caches",
            "/private/var/mobile/Containers/Data/InternalDaemon",
            "/private/var/mobile/Containers/Data/InternalDaemon/5C583048-0C53-456F-B110-BB12068B787F/Library/Caches",
            "/private/var/root/Library/Caches",
        };
        unsigned k;

        for (k = 0; k < sizeof(stores) / sizeof(stores[0]); k++) {
            DIR *dp = opendir(stores[k]);

            if (dp == NULL) {
                picasso_probe_log("PICASSO_SBVIEW store errno=%d %s\n", errno, stores[k]);
                continue;
            }
            {
                struct dirent *entry;
                unsigned ls = 0, total = 0, shown = 0;

                while ((entry = readdir(dp)) != NULL) {
                    if (entry->d_name[0] == '.') {
                        continue;
                    }
                    total++;
                    if (strncmp(entry->d_name, "com.apple.LaunchServices", 24) == 0) {
                        ls++;
                    }
                    if (shown < 3) {
                        picasso_probe_log("PICASSO_SBVIEW store entry=%s %s\n",
                                          entry->d_name, stores[k]);
                        shown++;
                    }
                }
                picasso_probe_log("PICASSO_SBVIEW store total=%u ls=%u %s\n", total, ls,
                                  stores[k]);
            }
            closedir(dp);
        }
    }
}
#include <spawn.h>
#include <sys/wait.h>

/* ---------------------------------------------------------------------------
 * Can the WebContent helper even be executed on this port?
 *
 * The framework's code lives in the dyld shared cache, but the helper is a real
 * Mach-O on disk that launchd/xpcproxy has to exec.  Spawning it here separates
 * "WebKit never asked for it" from "the port refuses to run it" -- the helper
 * exits at once without its XPC bootstrap, so this only reports the exec result.
 * ------------------------------------------------------------------------- */

static void
picasso_xpc_exec_probe(void)
{
    static const char helper[] =
        "/System/Library/Frameworks/WebKit.framework/XPCServices/"
        "com.apple.WebKit.WebContent.xpc/com.apple.WebKit.WebContent";
    struct stat st;
    pid_t pid = 0;
    int rc;
    const char *argv[2];

    if (stat(helper, &st) != 0) {
        picasso_probe_log("PICASSO_XPCEXEC stat errno=%d\n", errno);
        return;
    }
    picasso_probe_log("PICASSO_XPCEXEC stat ok size=%lld mode=%o\n",
                      (long long)st.st_size, st.st_mode & 07777);
    argv[0] = helper;
    argv[1] = NULL;
    rc = posix_spawn(&pid, helper, NULL, NULL, (char *const *)argv, NULL);
    picasso_probe_log("PICASSO_XPCEXEC spawn rc=%d errno=%d pid=%d\n", rc,
                      rc == 0 ? 0 : errno, (int)pid);
    if (rc == 0 && pid > 0) {
        /* It has no XPC bootstrap and will exit on its own; reap it. */
        int status = 0;

        waitpid(pid, &status, 0);
        picasso_probe_log("PICASSO_XPCEXEC exited status=%d\n", status);
    }
}

/* ---------------------------------------------------------------------------
 * Which host can actually start WebKit?
 *
 * Every app's sandbox allows the WebKit XPC service names; SpringBoard's may
 * not, which would explain a WKWebView that is created but never spawns a
 * content process.  Two independent readings: ask launchd for the service and
 * report the XPC error, and (for real apps) load a page from a host whose
 * profile is meant for it.
 * ------------------------------------------------------------------------- */

#include <mach-o/dyld.h>

static int
picasso_app_executable(char *buffer, size_t size)
{
    uint32_t needed = (uint32_t)size;

    if (_NSGetExecutablePath(buffer, &needed) != 0) {
        buffer[0] = '\0';
        return 0;
    }
    return 1;
}

/* Only a process with an app bundle gets here; it is the host WebKit expects. */
static void
picasso_app_web_probe(const char *who)
{
    char executable[1024];

    if (!picasso_app_executable(executable, sizeof(executable))) {
        return;
    }
    if (strncmp(executable, "/Applications/", 14) != 0) {
        return;
    }
    picasso_probe_log("PICASSO_APPWEB host=%s name=%s\n", executable, who);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 15ull * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
        picasso_probe_log("PICASSO_APPWEB webview begin\n");
        picasso_web_probe_start();
    });

    /* The supported way for an app to hand a URL to the browser.  From inside a
     * real app this needs no entitlement -- it is what any app does when it
     * opens a link. */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 25ull * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
        NSURL *url = [NSURL URLWithString:@"https://www.apple.com/"];

        picasso_probe_log("PICASSO_APPWEB openurl begin\n");
        [[UIApplication sharedApplication] openURL:url options:@{}
            completionHandler:^(BOOL ok) {
            picasso_probe_log("PICASSO_APPWEB openurl ok=%d\n", ok ? 1 : 0);
        }];
    });
}

/* ---------------------------------------------------------------------------
 * Can this port composite at all?
 *
 * WebKit renders through its GPU process, which needs a Metal device.  The port
 * is known to run the display from a software framebuffer and its GPU fence
 * never advances, so ask Metal directly: a nil device here means a page could
 * never be drawn even with every file and service in place.
 * ------------------------------------------------------------------------- */

static void
picasso_metal_probe(void)
{
    void *metal = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_LAZY);
    id (*create_device)(void);
    id device = nil;

    if (metal == NULL) {
        picasso_probe_log("PICASSO_METAL dlopen=fail\n");
        return;
    }
    create_device = (id (*)(void))dlsym(metal, "MTLCreateSystemDefaultDevice");
    if (create_device == NULL) {
        picasso_probe_log("PICASSO_METAL sym=no\n");
        return;
    }
    @try {
        device = create_device();
    } @catch (NSException *e) {
        picasso_probe_log("PICASSO_METAL exception %s\n", [[e reason] UTF8String]);
        return;
    }
    if (device == nil) {
        picasso_probe_log("PICASSO_METAL device=nil\n");
        return;
    }
    picasso_probe_log("PICASSO_METAL device=ok name=%s\n", [[device name] UTF8String]);
}

__attribute__((constructor))
static void picasso_springboard_probe(void)
{
    // The same library is injected into configd by wifiui=1.  configd is a
    // non-UI root daemon: run only the pure-C InterfaceNamer bootstrap there,
    // on a detached thread, and never touch UIKit/objc in that process.
    const char *program = getprogname();
    if (program != NULL && !strcmp(program, "configd")) {
        pthread_t worker, dns_worker;

        wifiui_log("PICASSO_WIFIUI loaded in configd\n");
        picasso_cryptex_bridge();
        picasso_cryptex_dump();
        picasso_fs_write_probe();
        picasso_preboot_probe();
        picasso_mount_probe();
        picasso_webkit_chain();
        picasso_ls_store_drop();
        picasso_xpc_exec_probe();
        if (pthread_create(&worker, NULL, wifiui_thread, NULL) == 0)
            pthread_detach(worker);
        /* Independent of the bootstrap path on purpose (chain-607). */
        if (pthread_create(&dns_worker, NULL, wifiui_dns_thread, NULL) == 0)
            pthread_detach(dns_worker);
        return;
    }
    // Record entry before either installation path can fail.
    picasso_ack("kern.picasso_inject_ack");
    /* Immediate proof that THIS build of the library is the one loaded. */
    picasso_probe_log("PICASSO_NETPROBE build=13 armed in %s\n", program ? program : "app");
    picasso_net_probe_start(program ? program : "app");
    picasso_app_web_probe(program ? program : "app");
    if (program != NULL && (!strcmp(program, "SpringBoard") || !strcmp(program, "MobileSafari"))) {
        picasso_web_probe_start();
    }
    @autoreleasepool {
        picasso_statusbar_start();
        picasso_install_astc_guard();
    }
}
