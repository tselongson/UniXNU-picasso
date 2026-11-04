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
#include <pexpert/pexpert.h>
#include <string.h>

extern "C" {
uint32_t native_ufs_raw_max_blocks(void);
int native_ufs_raw_read(uint32_t lun, uint64_t lba, uint32_t blocks, void *dst);
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
