// Link-only stand-in for the owned Legacy display subclass.
//
// build-native-legacy-display.py generates its archive from
// build/native-prelink/native-owner-mobile-graphics-candidate.macho, and nothing produces that file:
// prelink emits out/<device>/kernel/kernel.macho, not a per-record candidate. So on a clean tree the
// real subclass cannot be generated at all.
//
// The platform-power shim binds to more of it than two entry points: the class metaclass, the
// translation unit's static initializer (__GLOBAL__sub_I_NativeLegacyDisplay.cpp, which is why this
// file carries the name it does), the allocator, the readiness probes, and the keystore hooks it
// tests before calling. All of them are here so the shim keeps its registration path; none of them
// drive the panel. The screen keeps whatever the firmware left on it.
#include <IOKit/IOService.h>
#include <mach/kern_return.h>

class NativeLegacyDisplay : public IOService
{
  OSDeclareDefaultStructors(NativeLegacyDisplay)
};

OSDefineMetaClassAndStructors(NativeLegacyDisplay, IOService)

// The shim calls this unit's static initializer by name. The metaclass macros already give the unit a
// global to initialize, so clang emits __GLOBAL__sub_I_NativeLegacyDisplay.cpp itself -- defining it
// here as well is a duplicate symbol.

extern "C" IOService *native_legacy_display_allocate(void)
{
  return nullptr;
}

extern "C" bool native_legacy_display_probe_ready(void)
{
  return false;
}

extern "C" bool native_legacy_surface_ready(void)
{
  return false;
}

extern "C" void native_legacy_display_late_probe(void)
{
}

extern "C" void native_legacy_display_probe(void)
{
}

extern "C" kern_return_t native_publish_display_and_report(void)
{
  return 0;
}

// The keystore hooks are declared weak by the shim and tested for null before use, so an empty
// implementation is the same as the feature being absent.
extern "C" void picasso_keystore_probe(void)
{
}

extern "C" bool picasso_aks_ready(void)
{
  return false;
}
