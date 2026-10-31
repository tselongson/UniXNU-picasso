// Link-only stand-in for the owned Legacy display subclass.
//
// build-native-legacy-display.py generates its archive from
// build/native-prelink/native-owner-mobile-graphics-candidate.macho, and nothing produces that file:
// prelink emits out/<device>/kernel/kernel.macho, not a per-record candidate. So on a clean tree the
// real subclass cannot be generated at all.
//
// The platform-power shim still needs three things from it: NativeLegacyDisplay::gMetaClass, the
// translation unit's static initializer (__GLOBAL__sub_I_NativeLegacyDisplay.cpp, which is why this
// file carries the name it does), and the two entry points the kernel references. A class with the
// usual IOKit metaclass macros gives the first two, and the functions below give the rest.
//
// Nothing here drives the panel: the screen keeps whatever the firmware left on it. This exists so
// the system can boot; the real subclass goes back in once its candidate has a producer.
#include <IOKit/IOService.h>
#include <mach/kern_return.h>

class NativeLegacyDisplay : public IOService
{
  OSDeclareDefaultStructors(NativeLegacyDisplay)
};

OSDefineMetaClassAndStructors(NativeLegacyDisplay, IOService)

extern "C" void native_legacy_display_probe(void)
{
}

extern "C" kern_return_t native_publish_display_and_report(void)
{
  return 0;
}
