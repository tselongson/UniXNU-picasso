// Link-only stand-ins for the Picasso platform-power shim.
//
// The real archive derives its slot addresses from the linked kernel
// (out/picasso/kernel/kernel.macho), and the kernel cannot link until that archive exists -- so a
// clean tree can build neither, and the prelink candidates both archives audit against only appear
// after a kernel has linked.
//
// These no-op definitions break the loop for one pass: the kernel links, the prelink step runs, and
// the candidates exist. That kernel is never booted. The second pass swaps this archive for the real
// one, which is what the image ships.
#include <mach/kern_return.h>

extern "C" kern_return_t native_register_display_and_report(void)
{
  return 0;
}

extern "C" kern_return_t native_start_kext_and_report(const char *, const char *)
{
  return 0;
}

extern "C" void sheng_pmgr_run(void)
{
}

// The first pass also drops the display archive, and the kernel references these two from bsd_init
// and from the USB console patch, so the stand-ins live here for that pass.
extern "C" void native_legacy_display_probe(void)
{
}

extern "C" kern_return_t native_publish_display_and_report(void)
{
  return 0;
}
