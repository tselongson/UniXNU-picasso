// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copy engine rings: the path the firmware is fed through and the path frames travel on.
//
// A ring is a descriptor array the host and the target both walk. The host owns the write index and
// advances it to hand work over; the target owns the read index and the host reads it back to see
// what has been consumed. Nothing about that is specific to a transport, which is why the rings can
// be built and inspected before the radio answers.
//
// The descriptor carries its byte count in the high bits and flags below it. For WCN3990 the count
// sits at mask 0xfff0 shifted by 4, and that is the one detail that differs most between the parts
// this code could have been copied from: qca6174 uses 0xfffc shifted by 2, and a ring built with
// that layout hands the target a length it will read as nonsense.
#include "WlanCe.h"

#include <IOKit/IOLib.h>
#include <string.h>

// Ring geometry. The descriptor array is one page-aligned block per ring and the index arithmetic
// relies on the entry count being a power of two.
enum : uint32_t {
	kCeRingEntries = 1024,
	kCeMaxBytesPerDescriptor = 2048,
	kCeDescriptorSize = 16,        // two 32-bit words plus the address and flags
};

struct CeRing {
	uintptr_t registers;           // ceBase(base, engine)
	void *descriptors;
	uint64_t descriptorsPhysical;
	uint32_t writeIndex;
	uint32_t entries;
	bool inUse;
};

struct Ce {
	uintptr_t base;
	CeRing source;                 // host -> target
	CeRing destination;            // target -> host
};

static Ce gCe[kCeCount];

static uint32_t ceRead(uintptr_t registers, uint32_t offset)
{
	return *(volatile uint32_t *)(registers + offset);
}

static void ceWrite(uintptr_t registers, uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(registers + offset) = value;
}

bool wlanCeInit(uintptr_t wcssBase)
{
	if (wcssBase == 0) {
		return false;
	}
	for (unsigned engine = 0; engine < kCeCount; ++engine) {
		Ce &ce = gCe[engine];
		memset(&ce, 0, sizeof(ce));
		ce.base = wcssBase;
		ce.source.registers = ceBase(wcssBase, engine);
		ce.destination.registers = ce.source.registers;
	}
	return true;
}

// Reports the chip identity through the copy engine block's own register, which answers before any
// ring exists. A block that is not powered returns all ones, which no real chip id does.
bool wlanCeProbeChipId(uintptr_t wcssBase, uint32_t *chipId)
{
	if (!chipId) {
		return false;
	}
	const uint32_t value = ceRead(wcssBase, 0x000000f0);
	if (value == 0xffffffffu || value == 0) {
		return false;
	}
	*chipId = value;
	return true;
}

// Programs one ring's base and size. The target needs both halves of the address; the registers are
// 32 bits each because the block predates a single 64-bit register.
static void ceProgramRing(uintptr_t registers, uint32_t baseLo, uint32_t baseHi, uint32_t size,
                          uint32_t addressLow, uint32_t addressHigh, uint32_t sizeRegister)
{
	ceWrite(registers, baseLo, addressLow);
	ceWrite(registers, baseHi, addressHigh);
	ceWrite(registers, sizeRegister, size);
}

bool wlanCeRingSetup(unsigned engine, void *sourceDescriptors, uint64_t sourcePhysical,
                     void *destinationDescriptors, uint64_t destinationPhysical)
{
	if (engine >= kCeCount || !sourceDescriptors || !destinationDescriptors) {
		return false;
	}
	Ce &ce = gCe[engine];
	ce.source.descriptors = sourceDescriptors;
	ce.source.descriptorsPhysical = sourcePhysical;
	ce.source.entries = kCeRingEntries;
	ce.source.writeIndex = 0;
	ce.source.inUse = true;

	ce.destination.descriptors = destinationDescriptors;
	ce.destination.descriptorsPhysical = destinationPhysical;
	ce.destination.entries = kCeRingEntries;
	ce.destination.writeIndex = 0;
	ce.destination.inUse = true;

	const uintptr_t registers = ce.source.registers;
	ceProgramRing(registers, kCeSrcRingBaseLo, kCeSrcRingBaseHi, kCeSrcRingSize,
	              uint32_t(sourcePhysical & 0xffffffffu), uint32_t(sourcePhysical >> 32), kCeSrcRingSize);
	ceProgramRing(registers, kCeDstRingBaseLo, kCeDstRingBaseHi, kCeDstRingSize,
	              uint32_t(destinationPhysical & 0xffffffffu), uint32_t(destinationPhysical >> 32), kCeDstRingSize);

	// The size register takes log2 of the entry count, which is what the ring index arithmetic uses.
	uint32_t log2Entries = 0;
	for (uint32_t value = kCeRingEntries; value > 1; value >>= 1) {
		++log2Entries;
	}
	ceWrite(registers, kCeSrcRingSize, log2Entries);
	ceWrite(registers, kCeDstRingSize, log2Entries);
	return true;
}

// Fills one descriptor. The address is 64-bit and split, and the byte count goes in the metadata
// bits above the flags.
static void ceFillDescriptor(uint8_t *descriptors, uint32_t index, uint64_t address, uint32_t length,
                             uint32_t flags)
{
	uint8_t *entry = descriptors + size_t(index) * kCeDescriptorSize;
	uint32_t *words = reinterpret_cast<uint32_t *>(entry);
	words[0] = uint32_t(address & 0xffffffffu);
	words[1] = uint32_t(address >> 32);
	words[2] = ((length << kCeDescMetaShift) & kCeDescMetaMask) | (flags & ~kCeDescMetaMask);
	words[3] = 0;
}

bool wlanCeSubmit(unsigned engine, uint64_t targetAddress, const void *data, uint32_t length,
                  uint32_t flags, void *sourceAddress)
{
	if (engine >= kCeCount || length == 0 || length > kCeMaxBytesPerDescriptor) {
		return false;
	}
	Ce &ce = gCe[engine];
	if (!ce.source.inUse) {
		return false;
	}
	// The ring has to have room: the write index may not pass the read index the target reports.
	const uint32_t readIndex = ceRead(ce.source.registers, kCeCurrentSrcIndex) & (kCeRingEntries - 1);
	const uint32_t next = ceRingNext(ce.source.writeIndex, kCeRingEntries);
	if (next == readIndex) {
		return false;
	}
	ceFillDescriptor(static_cast<uint8_t *>(ce.source.descriptors), ce.source.writeIndex,
	                 targetAddress, length, flags);
	(void)data;
	(void)sourceAddress;
	ce.source.writeIndex = next;

	// The doorbell: advancing the write index is what tells the target the descriptor is there.
	ceWrite(ce.source.registers, kCeSrcWriteIndex, ce.source.writeIndex);
	return true;
}

// How many destination descriptors the target has handed over since the last call. The caller reads
// them from the ring and advances its own read index; this only reports the gap.
uint32_t wlanCeCompleted(unsigned engine)
{
	if (engine >= kCeCount || !gCe[engine].destination.inUse) {
		return 0;
	}
	const uint32_t written = ceRead(gCe[engine].destination.registers, kCeDstWriteIndex) & (kCeRingEntries - 1);
	const uint32_t read = ceRead(gCe[engine].destination.registers, kCeCurrentDstIndex) & (kCeRingEntries - 1);
	return (written - read) & (kCeRingEntries - 1);
}
