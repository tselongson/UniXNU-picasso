// SPDX-License-Identifier: BSD-2-Clause
//
// Modem (MPSS) firmware access for picasso.
//
// The WLAN firmware on this platform is not a file the AP loads: the modem's own
// WLAN processor image is served to MPSS, and the WLFW service that ath10k-style
// hosts talk to only exists once MPSS is running. Everything here therefore works
// towards one goal - making the modem firmware on the device's modem partition
// reachable from the kernel, then handing it to the secure monitor's PIL service.
//
// Stage 1 (this file, safe, read-only): read the modem partition through the
// read-only hook NativeUfs exports, parse its FAT16 directory, locate
// modem.mdt + the segment files it names, and report the ELF program headers.
// Nothing is written anywhere and no hardware block is touched, so it cannot
// hang a bus the way a premature WCSS MMIO read does.

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <pexpert/pexpert.h>
#include <string.h>

extern "C" {
uint32_t native_ufs_raw_max_blocks(void);
int native_ufs_raw_read(uint32_t lun, uint64_t lba, uint32_t blocks, void *dst);
void native_ufs_raw_diag(uint32_t *out);
}

namespace {

// picasso modem partition: LUN 4, start_sector 61606 in 4096-byte units
// (rawprogram4.xml, SECTOR_SIZE_IN_BYTES=4096, 81920 sectors).
constexpr uint32_t kModemLun = 4;
constexpr uint64_t kModemLba = 61606;
constexpr uint32_t kBlockBytes = 4096;

struct FatVolume {
    uint32_t lun;
    uint64_t partLba;
    uint32_t bytesPerSector;
    uint32_t sectorsPerCluster;
    uint32_t reservedSectors;
    uint32_t fatCount;
    uint32_t rootEntries;
    uint32_t sectorsPerFat;
    uint64_t totalSectors;
    uint64_t fatByteOffset;     // relative to partition start
    uint64_t rootByteOffset;    // relative to partition start
    uint64_t dataByteOffset;    // relative to partition start
};

struct FatEntry {
    char name[64];
    uint32_t size;
    uint32_t firstCluster;
    bool directory;
    bool valid;
};


// The UFS host only becomes readable once the root publisher thread has finished its bring-up
// (native-ufs-start-delay plus the LUN-ready retries). Every entry point in this file waits for
// that flag instead of assuming a fixed delay: the boot page runs long before root is mounted.
bool waitForUfsSeconds(unsigned seconds, const char *who)
{
    for (unsigned step = 0; step < seconds; ++step) {
        uint32_t diag[5] = {0, 0, 0, 0, 0};
        native_ufs_raw_diag(diag);
        if (diag[0]) {
            if (step) printf("PICASSO_WLAN ufs: ready after %us (%s)\n", step, who);
            return true;
        }
        IOSleep(1000);
    }
    printf("PICASSO_WLAN ufs: not ready after %us (%s)\n", seconds, who);
    return false;
}

uint8_t gBlock[kBlockBytes] __attribute__((aligned(4096)));

bool readPartitionBytes(const FatVolume &volume, uint64_t offset, uint32_t bytes, void *dst)
{
    if (!bytes || bytes > kBlockBytes) return false;
    const uint64_t absolute = volume.partLba * kBlockBytes + offset;
    const uint64_t lba = absolute / kBlockBytes;
    const uint32_t skip = static_cast<uint32_t>(absolute % kBlockBytes);
    if (skip + bytes <= kBlockBytes) {
        if (native_ufs_raw_read(volume.lun, lba, 1, gBlock) != 0) return false;
        memcpy(dst, gBlock + skip, bytes);
        return true;
    }
    // Rare in this layout (only when a caller crosses a block boundary).
    uint32_t done = 0;
    while (done < bytes) {
        const uint64_t at = offset + done;
        const uint64_t curLba = (volume.partLba * kBlockBytes + at) / kBlockBytes;
        const uint32_t curSkip = static_cast<uint32_t>((volume.partLba * kBlockBytes + at) % kBlockBytes);
        if (native_ufs_raw_read(volume.lun, curLba, 1, gBlock) != 0) return false;
        uint32_t take = kBlockBytes - curSkip;
        if (take > bytes - done) take = bytes - done;
        memcpy(static_cast<uint8_t *>(dst) + done, gBlock + curSkip, take);
        done += take;
    }
    return true;
}

uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool fatOpen(FatVolume &volume, uint32_t lun, uint64_t partLba)
{
    uint8_t boot[kBlockBytes];
    memset(boot, 0, sizeof boot);
    volume.lun = lun;
    volume.partLba = partLba;
    if (!readPartitionBytes(volume, 0, 512, boot)) return false;
    if (boot[510] != 0x55 || boot[511] != 0xAA) return false;
    volume.bytesPerSector = le16(boot + 11);
    volume.sectorsPerCluster = boot[13];
    volume.reservedSectors = le16(boot + 14);
    volume.fatCount = boot[16];
    volume.rootEntries = le16(boot + 17);
    uint32_t total16 = le16(boot + 19);
    volume.totalSectors = total16 ? total16 : le32(boot + 32);
    volume.sectorsPerFat = le16(boot + 22);
    if (!volume.sectorsPerFat) volume.sectorsPerFat = le32(boot + 36);
    if (!volume.bytesPerSector || !volume.sectorsPerCluster || !volume.fatCount ||
        !volume.sectorsPerFat || volume.bytesPerSector > kBlockBytes)
        return false;
    volume.fatByteOffset = static_cast<uint64_t>(volume.reservedSectors) * volume.bytesPerSector;
    volume.rootByteOffset = volume.fatByteOffset + static_cast<uint64_t>(volume.fatCount) * volume.sectorsPerFat * volume.bytesPerSector;
    volume.dataByteOffset = volume.rootByteOffset + static_cast<uint64_t>(volume.rootEntries) * 32;
    return true;
}

bool fatNextCluster(const FatVolume &volume, uint32_t cluster, uint32_t &next)
{
    uint8_t entry[2];
    if (!readPartitionBytes(volume, volume.fatByteOffset + cluster * 2, 2, entry)) return false;
    next = le16(entry);
    return true;
}

// Reads `bytes` from a cluster chain into dst. Bounded by `limit` so a corrupt
// chain cannot run away.
bool fatReadChain(const FatVolume &volume, uint32_t firstCluster, uint32_t bytes, uint8_t *dst, uint32_t limit)
{
    const uint32_t clusterBytes = volume.sectorsPerCluster * volume.bytesPerSector;
    uint32_t done = 0;
    uint32_t cluster = firstCluster;
    while (done < bytes && cluster >= 2 && cluster < 0xFFF8U) {
        const uint64_t offset = volume.dataByteOffset + static_cast<uint64_t>(cluster - 2) * clusterBytes;
        uint32_t chunk = clusterBytes;
        if (chunk > bytes - done) chunk = bytes - done;
        if (chunk > limit - done) chunk = limit - done;
        if (!chunk) break;
        if (!readPartitionBytes(volume, offset, chunk, dst + done)) return false;
        done += chunk;
        if (done >= bytes) break;
        uint32_t next = 0;
        if (!fatNextCluster(volume, cluster, next)) return false;
        cluster = next;
    }
    return done == bytes;
}

// Walks the root directory and fills `out` for the requested short name
// ("MODEM   MDT" style: 8.3, space padded, upper case). Long-name entries are
// skipped because the firmware files use plain 8.3 names.
bool fatFindFile(const FatVolume &volume, const char *shortName, FatEntry &out)
{
    out.valid = false;
    const uint32_t entryBytes = volume.rootEntries * 32;
    const uint32_t maxChunk = 2048;
    uint8_t buffer[maxChunk];
    for (uint32_t offset = 0; offset < entryBytes; offset += maxChunk) {
        uint32_t chunk = entryBytes - offset;
        if (chunk > maxChunk) chunk = maxChunk;
        if (!readPartitionBytes(volume, volume.rootByteOffset + offset, chunk, buffer)) return false;
        for (uint32_t index = 0; index + 32 <= chunk; index += 32) {
            const uint8_t *entry = buffer + index;
            if (entry[0] == 0) return true;
            if (entry[0] == 0xE5) continue;
            if (entry[11] == 0x0F) continue;
            if (entry[11] & 0x08) continue;
            if (memcmp(entry, shortName, 11) != 0) continue;
            uint32_t nameLength = 0;
            for (unsigned c = 0; c < 8 && entry[c] != ' '; ++c) out.name[nameLength++] = static_cast<char>(entry[c]);
            if (entry[8] != ' ') {
                out.name[nameLength++] = '.';
                for (unsigned c = 8; c < 11 && entry[c] != ' '; ++c) out.name[nameLength++] = static_cast<char>(entry[c]);
            }
            out.name[nameLength] = 0;
            out.size = le32(entry + 28);
            out.firstCluster = le16(entry + 26);
            out.directory = (entry[11] & 0x10) != 0;
            out.valid = true;
            return true;
        }
    }
    return true;
}

struct ElfSummary {
    uint32_t programHeaders;
    uint64_t loadableBytes;
    uint64_t minAddress;
    uint64_t maxAddress;
    uint32_t entrypoint;
};

bool summariseElf(const uint8_t *image, uint32_t bytes, ElfSummary &summary)
{
    memset(&summary, 0, sizeof summary);
    if (bytes < 52 || memcmp(image, "\177ELF", 4) != 0) return false;
    if (image[4] != 1) return false;                       // ELF32
    const uint32_t phoff = le32(image + 28);
    const uint16_t phentsize = le16(image + 42);
    const uint16_t phnum = le16(image + 44);
    summary.entrypoint = le32(image + 24);
    summary.programHeaders = phnum;
    summary.minAddress = ~0ULL;
    for (unsigned index = 0; index < phnum; ++index) {
        const uint64_t at = static_cast<uint64_t>(phoff) + static_cast<uint64_t>(index) * phentsize;
        if (at + 32 > bytes) return false;
        const uint8_t *ph = image + at;
        const uint32_t type = le32(ph + 0);
        const uint32_t offset = le32(ph + 4);
        const uint32_t vaddr = le32(ph + 8);
        const uint32_t paddr = le32(ph + 12);
        const uint32_t filesz = le32(ph + 16);
        const uint32_t memsz = le32(ph + 20);
        if (type != 1) continue;                            // PT_LOAD
        if (offset + filesz > bytes) return false;
        summary.loadableBytes += memsz;
        if (paddr < summary.minAddress) summary.minAddress = paddr;
        if (paddr + memsz > summary.maxAddress) summary.maxAddress = paddr + memsz;
    }
    return summary.programHeaders != 0;
}

}  // namespace

// Reports whether the modem partition is reachable and what its firmware
// directory looks like. Gated by sheng-wlan-mpss=1 at the call site.
extern "C" void native_wlan_modem_inventory(void)
{
    FatVolume volume;
    if (!fatOpen(volume, kModemLun, kModemLba)) {
        printf("PICASSO_WLAN mpss: modem FAT unreadable lun=%u lba=%llu\n", kModemLun,
               static_cast<unsigned long long>(kModemLba));
        return;
    }
    printf("PICASSO_WLAN mpss: fat bps=%u spc=%u rsv=%u fats=%u root=%u spf=%u total=%llu dataOff=%llu\n",
           volume.bytesPerSector, volume.sectorsPerCluster, volume.reservedSectors, volume.fatCount,
           volume.rootEntries, volume.sectorsPerFat, static_cast<unsigned long long>(volume.totalSectors),
           static_cast<unsigned long long>(volume.dataByteOffset));

    FatEntry mdt;
    if (!fatFindFile(volume, "MODEM   MDT", mdt) || !mdt.valid) {
        printf("PICASSO_WLAN mpss: modem.mdt not found\n");
        return;
    }
    printf("PICASSO_WLAN mpss: modem.mdt cluster=%u size=%u\n", mdt.firstCluster, mdt.size);

    uint8_t *image = static_cast<uint8_t *>(IOMalloc(mdt.size ? mdt.size : 1));
    if (!image) return;
    if (!fatReadChain(volume, mdt.firstCluster, mdt.size, image, mdt.size)) {
        printf("PICASSO_WLAN mpss: modem.mdt read failed\n");
        IOFree(image, mdt.size);
        return;
    }
    ElfSummary summary;
    const bool elf = summariseElf(image, mdt.size, summary);
    printf("PICASSO_WLAN mpss: mdt elf=%u phdrs=%u load=%llu range=%llx-%llx entry=%x magic=%02x%02x%02x%02x\n",
           elf ? 1U : 0U, summary.programHeaders, static_cast<unsigned long long>(summary.loadableBytes),
           static_cast<unsigned long long>(summary.minAddress),
           static_cast<unsigned long long>(summary.maxAddress), summary.entrypoint,
           image[0], image[1], image[2], image[3]);
    IOFree(image, mdt.size);

    // Count the segment files the directory carries so a later loader can plan
    // its buffer sizes without another pass.
    unsigned segments = 0;
    uint64_t segmentBytes = 0;
    for (unsigned index = 0; index < 64; ++index) {
        char shortName[12];
        static const char hex[] = "0123456789ABCDEF";
        shortName[0] = 'M';
        shortName[1] = 'O';
        shortName[2] = 'D';
        shortName[3] = 'E';
        shortName[4] = 'M';
        shortName[5] = ' ';
        shortName[6] = ' ';
        shortName[7] = ' ';
        shortName[8] = 'B';
        shortName[9] = hex[(index >> 4) & 0xF];
        shortName[10] = hex[index & 0xF];
        FatEntry entry;
        if (!fatFindFile(volume, shortName, entry) || !entry.valid) {
            if (index == 0) continue;
            break;
        }
        segments++;
        segmentBytes += entry.size;
    }
    printf("PICASSO_WLAN mpss: segments=%u bytes=%llu\n", segments,
           static_cast<unsigned long long>(segmentBytes));
}

// ---------------------------------------------------------------------------------------------
// Stage 2: hand the modem image to the secure monitor's PIL service.
//
// The recipe is the one this tree already proved on the GPU zap (wf120/wf121/wf126 in
// docs/handoff-20261102-gpu-first-submit-stall.md): every PAS call runs as a synchronous SMC with
// Non-secure interrupt delivery masked (ICC_PMR_EL1=0, DAIF.I=1) and unmasked for the retry. The
// metadata buffer must stay resident afterwards - the vendor never frees its PIL region either.
//
// The modem image is fixed-address (p_paddr 0x8c000000..0x9b800000), so unlike the relocatable zap
// there is no allocation: the segments are staged into the carveout the image itself names.
// ---------------------------------------------------------------------------------------------

namespace {

struct ScmReply { uint32_t a0, a1, a2, a3; uint64_t a6; };

ScmReply scmSipCall(uint32_t function, uint32_t arginfo, uint64_t arg0,
                    uint64_t arg1 = 0, uint64_t arg2 = 0, uint64_t arg3 = 0,
                    uint64_t arg6 = 0)
{
    register uint64_t x0 __asm__("x0") = function;
    register uint64_t x1 __asm__("x1") = arginfo;
    register uint64_t x2 __asm__("x2") = arg0;
    register uint64_t x3 __asm__("x3") = arg1;
    register uint64_t x4 __asm__("x4") = arg2;
    register uint64_t x5 __asm__("x5") = arg3;
    register uint64_t x6 __asm__("x6") = arg6;
    register uint64_t x7 __asm__("x7") = 0;
    __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x6), "+r"(x7)
                     : "r"(x4), "r"(x5)
                     : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    return ScmReply{uint32_t(x0), uint32_t(x1), uint32_t(x2), uint32_t(x3), x6};
}

constexpr uint32_t kPasSvc = 0x02;
constexpr uint32_t kPasInitImage = 0x01;
constexpr uint32_t kPasMemSetup = 0x02;
constexpr uint32_t kPasAuthAndReset = 0x05;
constexpr uint32_t kArgInfoInitImage = 0x82;
constexpr uint32_t kArgInfoMemSetup = 0x03;
constexpr uint32_t kArgInfoAuth = 0x01;
constexpr uint32_t kScmSipStd = 0x42000000u;
constexpr uint32_t kModemPasId = 1;

struct PasResult { ScmReply reply; uint64_t ticks; };

PasResult pasCall(uint32_t command, uint32_t arginfo, uint64_t arg0, uint64_t arg1 = 0)
{
    uint64_t pmr = 0, daif = 0, begin = 0, end = 0;
    __asm__ volatile("mrs %0, icc_pmr_el1" : "=r"(pmr));
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #2" ::: "memory");
    __asm__ volatile("msr icc_pmr_el1, %0" :: "r"(UINT64_C(0)) : "memory");
    __asm__ volatile("isb" ::: "memory");
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(begin));
    const ScmReply reply = scmSipCall(kScmSipStd | (kPasSvc << 8) | command, arginfo, kModemPasId, arg0, arg1);
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(end));
    __asm__ volatile("msr icc_pmr_el1, %0" :: "r"(pmr) : "memory");
    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
    __asm__ volatile("isb" ::: "memory");
    return PasResult{reply, end - begin};
}

// Device-memory stores: byte stores fault nowhere but are slow, so the bulk runs as aligned
// 8-byte stores with byte heads and tails. No bzero/memcpy here - both end in DC ZVA or unaligned
// 16-byte stores, which fault on a kIOMapInhibitCache mapping (wf104/wf105).
void copyToDevice(volatile uint8_t *dst, const uint8_t *src, uint64_t bytes)
{
    uint64_t index = 0;
    while (index < bytes && (reinterpret_cast<uintptr_t>(dst + index) & 7ULL)) {
        dst[index] = src[index];
        ++index;
    }
    volatile uint64_t *wide = reinterpret_cast<volatile uint64_t *>(const_cast<uint8_t *>(dst + index));
    const uint64_t *source = reinterpret_cast<const uint64_t *>(src + index);
    while (index + 8 <= bytes) {
        *wide++ = *source++;
        index += 8;
    }
    while (index < bytes) {
        dst[index] = src[index];
        ++index;
    }
}

void zeroDevice(volatile uint8_t *dst, uint64_t bytes)
{
    uint64_t index = 0;
    while (index < bytes && (reinterpret_cast<uintptr_t>(dst + index) & 7ULL)) dst[index++] = 0;
    volatile uint64_t *wide = reinterpret_cast<volatile uint64_t *>(const_cast<uint8_t *>(dst + index));
    while (index + 8 <= bytes) {
        *wide++ = 0;
        index += 8;
    }
    while (index < bytes) dst[index++] = 0;
}

}  // namespace

// Stages the modem image into its fixed carveout and runs the PAS session.
// Gated by sheng-wlan-ufs=2 at the call site; returns true when AUTH_AND_RESET accepted it.
extern "C" bool native_wlan_mpss_start(void)
{
    if (!waitForUfsSeconds(150, "mpss")) return false;
    FatVolume volume;
    if (!fatOpen(volume, kModemLun, kModemLba)) {
        printf("PICASSO_WLAN mpss: start skipped, modem FAT unreadable\n");
        return false;
    }
    FatEntry mdt;
    if (!fatFindFile(volume, "MODEM   MDT", mdt) || !mdt.valid) {
        printf("PICASSO_WLAN mpss: modem.mdt missing\n");
        return false;
    }
    uint8_t *meta = static_cast<uint8_t *>(IOMalloc(mdt.size ? mdt.size : 1));
    if (!meta) return false;
    if (!fatReadChain(volume, mdt.firstCluster, mdt.size, meta, mdt.size)) {
        printf("PICASSO_WLAN mpss: modem.mdt read failed\n");
        IOFree(meta, mdt.size);
        return false;
    }
    if (memcmp(meta, "\177ELF", 4) != 0 || meta[4] != 1) {
        printf("PICASSO_WLAN mpss: modem.mdt is not ELF32\n");
        IOFree(meta, mdt.size);
        return false;
    }
    const uint32_t phoff = le32(meta + 28);
    const uint16_t phentsize = le16(meta + 42);
    const uint16_t phnum = le16(meta + 44);

    // First pass: physical span of the image.
    uint64_t minAddress = ~0ULL, maxAddress = 0, imageBytes = 0;
    for (unsigned index = 0; index < phnum; ++index) {
        const uint64_t at = static_cast<uint64_t>(phoff) + static_cast<uint64_t>(index) * phentsize;
        if (at + 32 > mdt.size) { IOFree(meta, mdt.size); return false; }
        const uint8_t *ph = meta + at;
        if (le32(ph + 0) != 1) continue;
        const uint32_t paddr = le32(ph + 12);
        const uint32_t memsz = le32(ph + 20);
        if (paddr < minAddress) minAddress = paddr;
        if (static_cast<uint64_t>(paddr) + memsz > maxAddress) maxAddress = static_cast<uint64_t>(paddr) + memsz;
        imageBytes += memsz;
    }
    const uint64_t span = (maxAddress - minAddress + 0xfffffULL) & ~0xfffffULL;
    printf("PICASSO_WLAN mpss: image base=%llx span=%llx memsz=%llu phdrs=%u\n",
           static_cast<unsigned long long>(minAddress), static_cast<unsigned long long>(span),
           static_cast<unsigned long long>(imageBytes), phnum);

    // The carveout is fixed by the image; map it device-uncached and stage every segment.
    IODeviceMemory *carveout = IODeviceMemory::withRange(minAddress, span);
    if (!carveout) {
        printf("PICASSO_WLAN mpss: carveout descriptor failed\n");
        IOFree(meta, mdt.size);
        return false;
    }
    IOMemoryMap *carveoutMap = carveout->map(kIOMapAnywhere | kIOMapInhibitCache);
    carveout->release();
    if (!carveoutMap) {
        printf("PICASSO_WLAN mpss: carveout map failed\n");
        IOFree(meta, mdt.size);
        return false;
    }
    volatile uint8_t *region = reinterpret_cast<volatile uint8_t *>(
        static_cast<uintptr_t>(carveoutMap->getAddress()));
    printf("PICASSO_WLAN mpss: carveout va=%llx\n",
           static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(region)));

    unsigned staged = 0;
    uint64_t stagedBytes = 0;
    bool ok = true;
    for (unsigned index = 0; index < phnum && ok; ++index) {
        const uint64_t at = static_cast<uint64_t>(phoff) + static_cast<uint64_t>(index) * phentsize;
        const uint8_t *ph = meta + at;
        if (le32(ph + 0) != 1) continue;
        const uint32_t paddr = le32(ph + 12);
        const uint32_t filesz = le32(ph + 16);
        const uint32_t memsz = le32(ph + 20);
        const uint64_t offset = paddr - minAddress;
        if (offset + memsz > span) { ok = false; break; }
        if (!filesz) {
            zeroDevice(region + offset, memsz);
            continue;
        }
        char shortName[12];
        static const char hex[] = "0123456789ABCDEF";
        memcpy(shortName, "MODEM   B", 9);
        shortName[9] = hex[(index >> 4) & 0xF];
        shortName[10] = hex[index & 0xF];
        shortName[11] = 0;
        FatEntry segment;
        if (!fatFindFile(volume, shortName, segment) || !segment.valid) {
            printf("PICASSO_WLAN mpss: segment %u file %s missing\n", index, shortName);
            ok = false;
            break;
        }
        uint8_t *data = static_cast<uint8_t *>(IOMalloc(segment.size ? segment.size : 1));
        if (!data) { ok = false; break; }
        if (!fatReadChain(volume, segment.firstCluster, segment.size, data, segment.size)) {
            printf("PICASSO_WLAN mpss: segment %u read failed\n", index);
            IOFree(data, segment.size);
            ok = false;
            break;
        }
        uint32_t copyBytes = segment.size;
        if (copyBytes > filesz) copyBytes = filesz;
        copyToDevice(region + offset, data, copyBytes);
        if (memsz > copyBytes) zeroDevice(region + offset + copyBytes, memsz - copyBytes);
        IOFree(data, segment.size);
        staged++;
        stagedBytes += copyBytes;
        if ((index & 7) == 0 || index + 1 == phnum) {
            printf("PICASSO_WLAN mpss: seg %u paddr=%x filesz=%x memsz=%x\n", index, paddr, filesz, memsz);
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    printf("PICASSO_WLAN mpss: staged=%u bytes=%llu ok=%u\n", staged,
           static_cast<unsigned long long>(stagedBytes), ok ? 1U : 0U);
    if (!ok) {
        carveoutMap->release();
        IOFree(meta, mdt.size);
        return false;
    }

    // Metadata must be physically contiguous and stay resident (wf126).
    const IOOptionBits metaOptions = static_cast<IOOptionBits>(kIODirectionOut) |
                                     static_cast<IOOptionBits>(kIOMemoryMapperNone) |
                                     static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous);
    IOBufferMemoryDescriptor *metaBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, metaOptions, (mdt.size + 0xfffU) & ~0xfffU, UINT64_C(0xfffffffffff00000));
    bool started = false;
    if (!metaBuffer || metaBuffer->prepare() != kIOReturnSuccess) {
        printf("PICASSO_WLAN mpss: metadata allocation failed\n");
        if (metaBuffer) metaBuffer->release();
    } else {
        IOByteCount metaLength = 0;
        const uint64_t metaPhys = metaBuffer->getPhysicalSegment(0, &metaLength, kIOMemoryMapperNone);
        IOMemoryMap *metaMap = metaBuffer->map(kIOMapAnywhere | kIOMapInhibitCache);
        if (!metaMap || !metaPhys) {
            printf("PICASSO_WLAN mpss: metadata mapping failed\n");
            if (metaMap) metaMap->release();
        } else {
            copyToDevice(reinterpret_cast<volatile uint8_t *>(static_cast<uintptr_t>(metaMap->getAddress())),
                         meta, mdt.size);
            __asm__ volatile("dsb sy" ::: "memory");
            printf("PICASSO_WLAN mpss: metadata phys=%llx bytes=%u\n",
                   static_cast<unsigned long long>(metaPhys), mdt.size);

            const PasResult init = pasCall(kPasInitImage, kArgInfoInitImage, metaPhys);
            printf("PICASSO_WLAN mpss: init a0=%08x a1=%08x ticks=%llu\n", init.reply.a0, init.reply.a1,
                   static_cast<unsigned long long>(init.ticks));
            if (init.reply.a0 == 0) {
                const PasResult setup = pasCall(kPasMemSetup, kArgInfoMemSetup, minAddress, span);
                printf("PICASSO_WLAN mpss: memsetup a0=%08x a1=%08x ticks=%llu\n", setup.reply.a0, setup.reply.a1,
                       static_cast<unsigned long long>(setup.ticks));
                if (setup.reply.a0 == 0) {
                    const PasResult auth = pasCall(kPasAuthAndReset, kArgInfoAuth, 0);
                    printf("PICASSO_WLAN mpss: auth a0=%08x a1=%08x ticks=%llu\n", auth.reply.a0, auth.reply.a1,
                           static_cast<unsigned long long>(auth.ticks));
                    started = auth.reply.a0 == 0;
                }
            }
            // Retain both mappings and buffers: TZ keeps using the region after AUTH (wf126).
            printf("PICASSO_WLAN mpss: started=%u\n", started ? 1U : 0U);
        }
    }
    return started;
}

// ---------------------------------------------------------------------------------------------
// Stage 0: where is the WLAN firmware?
//
// The ROM's NON-HLOS.bin holds the modem FAT but no WLAN image, and neither vendor.img nor
// system.img carry one. The device's modem partition is larger than the image the ROM writes, so
// anything the factory left past that image is still on the medium. This scan walks the whole
// partition in 64 KiB reads and reports any offset that spells one of the WLAN image names, plus
// the FAT geometry of every plausible boot sector it passes. Read-only, no writes anywhere.
// ---------------------------------------------------------------------------------------------

namespace {

struct Needle {
    const char *text;
    uint32_t length;
};

constexpr uint32_t kScanChunkBlocks = 16;      // 64 KiB per UFS command
constexpr uint64_t kScanLimitBytes = 0x14000000ULL;  // 320 MiB, the modem partition's size

void scanForNeedles(const uint8_t *buffer, uint32_t bytes, uint64_t base, unsigned &hits)
{
    static const char *const needles[] = {"athwlan", "wlanmdsp", "bdwlan", "WCNSS", "regdb"};
    for (unsigned n = 0; n < sizeof(needles) / sizeof(needles[0]); ++n) {
        const char *needle = needles[n];
        const uint32_t length = static_cast<uint32_t>(strlen(needle));
        if (bytes < length) continue;
        for (uint32_t index = 0; index + length <= bytes; ++index) {
            bool match = true;
            for (uint32_t c = 0; c < length; ++c) {
                if (buffer[index + c] != static_cast<uint8_t>(needle[c])) { match = false; break; }
            }
            if (match) {
                printf("PICASSO_WLAN scan: %s at modem+%llx\n", needle,
                       static_cast<unsigned long long>(base + index));
                if (++hits >= 24) return;
            }
        }
    }
}

}  // namespace

extern "C" void native_wlan_modem_scan(void)
{
    if (!waitForUfsSeconds(150, "scan")) return;
    uint8_t *buffer = static_cast<uint8_t *>(IOMalloc(kScanChunkBlocks * 4096));
    if (!buffer) return;
    unsigned hits = 0;
    unsigned fatSeen = 0;
    const uint64_t chunkBytes = static_cast<uint64_t>(kScanChunkBlocks) * 4096;
    uint64_t lba = kModemLba;
    const uint64_t firstLba = kModemLba;
    const uint64_t limitLba = kModemLba + kScanLimitBytes / 4096;
    printf("PICASSO_WLAN scan: modem lun=%u lba=%llu..%llu\n", kModemLun,
           static_cast<unsigned long long>(firstLba), static_cast<unsigned long long>(limitLba));
    while (lba < limitLba && hits < 24) {
        if (native_ufs_raw_read(kModemLun, lba, kScanChunkBlocks, buffer) != 0) {
            printf("PICASSO_WLAN scan: read failed at lba=%llu\n", static_cast<unsigned long long>(lba));
            break;
        }
        const uint64_t base = (lba - firstLba) * 4096;
        // A FAT boot sector announces itself with 0x55AA at 510 and a sane bytes-per-sector at 11.
        if (buffer[510] == 0x55 && buffer[511] == 0xAA) {
            const unsigned bps = buffer[11] | (buffer[12] << 8);
            if (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096) {
                printf("PICASSO_WLAN scan: FAT candidate at modem+%llx bps=%u spc=%u\n",
                       static_cast<unsigned long long>(base), bps, buffer[13]);
                if (++fatSeen >= 6) fatSeen = 6;
            }
        }
        scanForNeedles(buffer, static_cast<uint32_t>(chunkBytes), base, hits);
        lba += kScanChunkBlocks;
    }
    printf("PICASSO_WLAN scan: done hits=%u fat=%u\n", hits, fatSeen);
    IOFree(buffer, kScanChunkBlocks * 4096);
}
