// Link-only stand-ins for the Legacy display subclass.
//
// build-native-legacy-display.py generates its archive from build/native-prelink/
// native-owner-mobile-graphics-candidate.macho, and nothing in this repository or the pinned tree
// produces that candidate: prelink emits out/<device>/kernel/kernel.macho, not a per-record file. So
// on a clean tree the display archive cannot be generated at all.
//
// This archive takes its place: the configuration still links build/native-legacy-display/
// libNativeLegacyDisplay.a, and these definitions satisfy the two symbols the kernel references.
// The screen therefore keeps whatever the firmware left on it. Booting the system comes first; the
// real subclass goes back in once its candidate has a producer.
#include <mach/kern_return.h>

extern "C" void native_legacy_display_probe(void)
{
}

extern "C" kern_return_t native_publish_display_and_report(void)
{
  return 0;
}
