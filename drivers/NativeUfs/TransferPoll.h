// SPDX-License-Identifier: BSD-2-Clause-Patent
// Single-slot completion policy. Port supplies ordered MMIO, invalidate-only
// descriptor reads, a monotonic deadline and bounded pacing. No resubmission.
enum class UfsTransferEnd { Completed, Fatal, Timeout };
struct UfsTransferPoll {
  uint32_t polls = 0, doorbellSeen = 0, interruptSeen = 0, ocs = 15;
};
static constexpr uint32_t kUfsFatalStatus = 0x71880U;

template <class Port>
static UfsTransferEnd pollUfsTransfer(Port &port, UfsTransferPoll &state)
{
  for (;;) {
    ++state.polls;
    port.beginPoll(state.polls);
    uint32_t doorbell = port.doorbell();
    state.doorbellSeen |= doorbell;
    state.interruptSeen |= port.interrupt();
    if (state.interruptSeen & kUfsFatalStatus) return UfsTransferEnd::Fatal;
    if (!(doorbell & 1)) {
      state.ocs = port.completedOcs();
      // A zero doorbell alone does not prove that this command was accepted.
      // Also allow a completion so fast that software never observes DB=1.
      if (state.ocs != 15) return UfsTransferEnd::Completed;
    }
    if (port.expired()) return UfsTransferEnd::Timeout;
    // PICASSO-DIAG: hard iteration cap. The deadline alone is the only exit today, so a broken
    // timebase (or a controller that never completes) wedges the platform-expert thread with no
    // output at all. Bail out by poll count as well so the caller can dump registers.
    //
    // 200000 was far too generous here: each iteration calls delay(), which is IODelay(2), and
    // IODelay on this board is orders of magnitude slower than its argument suggests. A stalled
    // NOP OUT therefore sat in this loop for minutes and looked exactly like a deadlock -- the
    // caller's failure path never ran, so neither the log line nor the panic ever appeared.
    if (state.polls >= 20000u) return UfsTransferEnd::Timeout;
    port.delay();
  }
}
