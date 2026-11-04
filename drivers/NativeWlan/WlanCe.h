#ifdef __cplusplus
extern "C" {
#endif
// SPDX-License-Identifier: BSD-2-Clause-Patent
// The WCN3990 copy engine register map, which is what this board's WCSS actually is.
//
// The board's WLAN part is the integrated WCSS of an SM7250, and that block is the WCN3990: the
// vendor module carries the hif_snoc_* interface and an athwlan.bin image, which is the SNOC host of
// ath10k rather than the PCIe one. So the parameters below are ath10k's wcn3990_regs and
// wcn3990_ce_regs, not ath11k's and not qca6174's -- the three differ in CE count and in descriptor
// metadata layout, and mixing them is the kind of mistake that produces a ring the target never
// reads.
//
// The CE windows are 4 KiB apart starting at 0x240000 inside the WCSS register block. On this board
// that block is qcom,icnss@18800000, so the addresses below are offsets from 0x18800000.
#pragma once

#include <stdint.h>

// qcom,icnss@18800000: membase 0x18800000, 8 MiB.
#define kWcssRegBase 0x18800000u

enum : uint32_t {
	kCeCount = 12,
	kCeWindowSize = 0x1000,
	kCe0Offset = 0x240000,      // CE0; CE n is this plus n * kCeWindowSize
	kCeWrapperOffset = 0x24c000,
};

// Registers inside one CE window, from ath10k's wcn3990_ce_regs. The two write indexes are the
// doorbells: the host advances the source ring write index to hand the target a descriptor, and the
// target advances the destination ring write index to hand the host one.
enum : uint32_t {
	kCeSrcRingBaseLo = 0x00,
	kCeSrcRingBaseHi = 0x04,
	kCeSrcRingSize = 0x08,
	kCeDstRingBaseLo = 0x0c,
	kCeDstRingBaseHi = 0x10,
	kCeDstRingSize = 0x14,
	kCeCtrl1 = 0x18,            // source/destination ring enables and the max burst
	kCeHostIntEnable = 0x2c,
	kCeMiscIntEnable = 0x34,
	kCeSrcWriteIndex = 0x3c,    // host -> target doorbell
	kCeDstWriteIndex = 0x40,    // target -> host doorbell
	kCeCurrentSrcIndex = 0x44,
	kCeCurrentDstIndex = 0x48,
};

// Descriptor metadata: the byte count sits in the high bits and the rest is flags. WCN3990 uses
// mask 0xfff0 with the count at bit 4, where qca6174 uses 0xfffc at bit 2.
enum : uint32_t {
	kCeDescMetaMask = 0xfff0,
	kCeDescMetaShift = 4,
};

// Ring index arithmetic. The rings are power-of-two sized, so the index wraps by mask rather than by
// a modulo the hardware would have to do.
static inline uint32_t ceRingNext(uint32_t index, uint32_t entries)
{
	return (index + 1) & (entries - 1);
}

// Where one copy engine's registers live. The caller supplies the WCSS base so the same code works
// whether the block is reached through the ICNSS window or through a mapped sub-window.
static inline uintptr_t ceBase(uintptr_t wcssBase, unsigned engine)
{
	return wcssBase + kCe0Offset + uintptr_t(engine) * kCeWindowSize;
}

// Sets up the twelve engines against the WCSS register window. Returns false if the base is zero.
bool wlanCeInit(uintptr_t wcssBase);

// Reads the chip identity through the copy engine block, which answers before any ring exists.
bool wlanCeProbeChipId(uintptr_t wcssBase, uint32_t *chipId);

// Points one engine's source and destination rings at memory the caller owns. Both descriptor arrays
// must be kCeRingEntries long and page aligned.
bool wlanCeRingSetup(unsigned engine, void *sourceDescriptors, uint64_t sourcePhysical,
                     void *destinationDescriptors, uint64_t destinationPhysical);

// Hands one descriptor to the target. Returns false when the ring is full or the length exceeds what
// a single descriptor can carry.
bool wlanCeSubmit(unsigned engine, uint64_t targetAddress, const void *data, uint32_t length,
                  uint32_t flags, void *sourceAddress);

// Descriptors the target has filled since the caller last advanced its read index.
uint32_t wlanCeCompleted(unsigned engine);

#ifdef __cplusplus
}
#endif
