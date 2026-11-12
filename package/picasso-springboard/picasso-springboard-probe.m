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

    // A real bounded wait.  kIOMainPortDefault is MACH_PORT_NULL (0).
    int quiet_result = -1;
    const char *marker = "*TIMEOUT*";
    if (wait_quiet != NULL) {
        mach_timespec_t deadline = { 5, 0 };
        quiet_result = wait_quiet(0, &deadline);
        if (quiet_result == 0) marker = "*QUIET*";
    } else {
        // 20H392 may not export IOKitWaitQuiet (Apple documents it for
        // iOS 18.4+).  Still perform a real bounded wait rather than
        // publishing a terminal marker without waiting.
        sleep(5);
    }
    (void)snprintf(line, sizeof(line),
        "PICASSO_WIFIUI pid=%d euid=%u quiet_api=%s quiet_result=%d marker=%s\n",
        (int)getpid(), (unsigned)geteuid(), wait_quiet != NULL ? "yes" : "no",
        quiet_result, marker);
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
    if (!already) {
        CFStringRef chosen = quiet_result == 0 ? CFSTR("*QUIET*") : CFSTR("*TIMEOUT*");
        CFDictionarySetValue(record, chosen, kCFBooleanTrue);
        published = store_set(store, key, record) ? 1 : 0;
    }
    (void)snprintf(line, sizeof(line),
        "PICASSO_WIFIUI key=Plugin:InterfaceNamer already=%u publish=%d\n",
        already, published);
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
    @autoreleasepool { wifiui_bootstrap(); }
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
