#include "core/fat.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace carver {

namespace {

constexpr uint64_t MAX_TABLE_BYTES = 512ull * 1024ull * 1024ull;
constexpr uint64_t ROOT_DIRECTORY_PROBE = 256ull * 1024ull;

uint16_t readU16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(data[1] << 8);
}

uint32_t readU32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t readU64(const uint8_t* data) {
    uint64_t value = 0;
    for (int index = 7; index >= 0; --index) {
        value = (value << 8) | data[index];
    }
    return value;
}

bool readAt(RawDevice& device, uint64_t offset, uint8_t* buffer, uint32_t length) {
    uint32_t got = 0;
    std::string ignored;
    return device.readAt(offset, buffer, length, got, ignored) && got == length;
}

bool readRange(RawDevice& device, uint64_t offset, uint64_t length, std::vector<uint8_t>& out,
               std::string& error) {
    out.clear();
    if (length == 0) {
        return true;
    }
    if (length > MAX_TABLE_BYTES) {
        error = "allocation table is implausibly large (" + std::to_string(length) + " bytes)";
        return false;
    }

    out.resize(static_cast<size_t>(length));
    size_t written = 0;
    uint64_t position = offset;

    while (written < out.size()) {
        const uint32_t want = static_cast<uint32_t>(
            std::min<uint64_t>(out.size() - written, 1u << 20));
        if (!readAt(device, position, out.data() + written, want)) {
            error = "cannot read the allocation table at offset " + std::to_string(position);
            return false;
        }
        written += want;
        position += want;
    }

    return true;
}

std::vector<ByteRange> rangesFromAllocated(const std::vector<bool>& allocated, uint64_t dataStart,
                                          uint64_t bytesPerCluster) {
    std::vector<ByteRange> ranges;
    const uint64_t count = allocated.size();

    bool inFree = false;
    uint64_t start = 0;

    for (uint64_t index = 0; index < count; ++index) {
        if (!allocated[index]) {
            if (!inFree) {
                start = index;
                inFree = true;
            }
        } else if (inFree) {
            ranges.push_back(ByteRange{dataStart + start * bytesPerCluster,
                                       dataStart + index * bytesPerCluster});
            inFree = false;
        }
    }

    if (inFree) {
        ranges.push_back(ByteRange{dataStart + start * bytesPerCluster,
                                   dataStart + count * bytesPerCluster});
    }

    return ranges;
}

uint32_t readFat12Entry(const std::vector<uint8_t>& table, uint64_t cluster) {
    const uint64_t index = cluster + (cluster / 2);
    if (index + 1 >= table.size()) {
        return 0x0FFFFFFFu;
    }
    const uint16_t pair = static_cast<uint16_t>(table[index]) |
                          static_cast<uint16_t>(table[index + 1] << 8);
    return (cluster % 2 == 0) ? (pair & 0x0FFFu) : (pair >> 4);
}

std::vector<ByteRange> fatTableFreeRanges(RawDevice& device, const FatVolumeInfo& info,
                                          uint32_t fatOffsetSectors, uint32_t fatSizeSectors,
                                          std::string& error) {
    const uint64_t tableOffset = info.baseOffset +
                                 static_cast<uint64_t>(fatOffsetSectors) * info.bytesPerSector;
    const uint64_t tableLength = static_cast<uint64_t>(fatSizeSectors) * info.bytesPerSector;

    std::vector<uint8_t> table;
    if (!readRange(device, tableOffset, tableLength, table, error)) {
        return {};
    }

    std::vector<bool> allocated(static_cast<size_t>(info.clusterCount), false);

    for (uint64_t index = 0; index < info.clusterCount; ++index) {
        const uint64_t cluster = index + 2;
        uint32_t entry = 0;
        if (info.kind == FatKind::Fat32) {
            const uint64_t at = cluster * 4;
            entry = at + 4 <= table.size() ? (readU32(table.data() + at) & 0x0FFFFFFFu)
                                           : 0x0FFFFFFFu;
        } else if (info.kind == FatKind::Fat16) {
            const uint64_t at = cluster * 2;
            entry = at + 2 <= table.size() ? readU16(table.data() + at) : 0xFFFFu;
        } else {
            entry = readFat12Entry(table, cluster);
        }

        allocated[static_cast<size_t>(index)] = entry != 0;
    }

    return rangesFromAllocated(allocated, info.dataStart, info.bytesPerCluster);
}

std::vector<ByteRange> exFatBitmapFreeRanges(RawDevice& device, const FatVolumeInfo& info,
                                             uint32_t rootCluster, std::string& error) {
    if (rootCluster < 2) {
        error = "exFAT root directory cluster is invalid";
        return {};
    }

    const uint64_t rootOffset = info.dataStart +
                               static_cast<uint64_t>(rootCluster - 2) * info.bytesPerCluster;
    const uint64_t probe = std::min<uint64_t>(ROOT_DIRECTORY_PROBE,
                                             static_cast<uint64_t>(info.bytesPerCluster) * 64);

    std::vector<uint8_t> directory;
    if (!readRange(device, rootOffset, probe, directory, error)) {
        return {};
    }

    uint32_t bitmapCluster = 0;
    uint64_t bitmapLength = 0;

    for (size_t offset = 0; offset + 32 <= directory.size(); offset += 32) {
        const uint8_t entryType = directory[offset];
        if (entryType == 0x00) {
            break;
        }
        if (entryType != 0x81) {
            continue;
        }

        bitmapCluster = readU32(directory.data() + offset + 0x04);
        bitmapLength = readU64(directory.data() + offset + 0x08);
        break;
    }

    if (bitmapCluster < 2 || bitmapLength == 0) {
        error = "the exFAT allocation bitmap entry was not found in the root directory";
        return {};
    }

    const uint64_t expected = (info.clusterCount + 7) / 8;
    if (bitmapLength > expected) {
        bitmapLength = expected;
    }

    const uint64_t bitmapOffset = info.dataStart +
                                 static_cast<uint64_t>(bitmapCluster - 2) * info.bytesPerCluster;

    std::vector<uint8_t> bitmap;
    if (!readRange(device, bitmapOffset, bitmapLength, bitmap, error)) {
        return {};
    }

    std::vector<bool> allocated(static_cast<size_t>(info.clusterCount), true);

    for (uint64_t cluster = 2; cluster < info.clusterCount + 2; ++cluster) {
        const uint64_t bit = cluster - 2;
        const size_t byteIndex = static_cast<size_t>(bit / 8);
        if (byteIndex >= bitmap.size()) {
            break;
        }
        allocated[static_cast<size_t>(bit)] = ((bitmap[byteIndex] >> (bit % 8)) & 1u) != 0;
    }

    return rangesFromAllocated(allocated, info.dataStart, info.bytesPerCluster);
}

}

std::string describeFatKind(FatKind kind) {
    switch (kind) {
    case FatKind::Fat12: return "FAT12";
    case FatKind::Fat16: return "FAT16";
    case FatKind::Fat32: return "FAT32";
    case FatKind::ExFat: return "exFAT";
    default: return "not FAT";
    }
}

bool parseFatBootSector(const uint8_t* sector, size_t length, FatVolumeInfo& info, std::string& error) {
    if (length < 512) {
        error = "boot sector is shorter than 512 bytes";
        return false;
    }
    if (sector[0x1FE] != 0x55 || sector[0x1FF] != 0xAA) {
        error = "boot sector signature 0x55AA is missing";
        return false;
    }

    if (std::memcmp(sector + 0x03, "EXFAT   ", 8) == 0) {
        const uint8_t sectorShift = sector[0x6C];
        const uint8_t clusterShift = sector[0x6D];

        if (sectorShift < 9 || sectorShift > 12) {
            error = "implausible exFAT sector size shift";
            return false;
        }
        if (clusterShift > 25) {
            error = "implausible exFAT cluster size shift";
            return false;
        }

        info.kind = FatKind::ExFat;
        info.bytesPerSector = 1u << sectorShift;
        info.bytesPerCluster = (1u << sectorShift) << clusterShift;

        const uint32_t clusterHeapOffset = readU32(sector + 0x58);
        info.clusterCount = readU32(sector + 0x5C);
        const uint32_t rootCluster = readU32(sector + 0x60);

        if (clusterHeapOffset == 0 || info.clusterCount == 0 || rootCluster < 2) {
            error = "exFAT volume geometry is invalid";
            return false;
        }

        info.dataStart = info.baseOffset +
                         static_cast<uint64_t>(clusterHeapOffset) * info.bytesPerSector;

        return true;
    }

    const uint16_t bytesPerSector = readU16(sector + 0x0B);
    const uint8_t sectorsPerCluster = sector[0x0D];
    const uint16_t reservedSectors = readU16(sector + 0x0E);
    const uint8_t fatCount = sector[0x10];
    const uint16_t rootEntries = readU16(sector + 0x11);
    const uint16_t fatSize16 = readU16(sector + 0x16);
    uint32_t totalSectors = readU16(sector + 0x13);
    if (totalSectors == 0) {
        totalSectors = readU32(sector + 0x20);
    }
    const uint32_t fatSize32 = readU32(sector + 0x24);

    if (bytesPerSector < 512 || bytesPerSector > 4096 || (bytesPerSector % 512) != 0) {
        error = "implausible FAT bytes-per-sector";
        return false;
    }
    if (sectorsPerCluster == 0 || (sectorsPerCluster & (sectorsPerCluster - 1)) != 0) {
        error = "implausible FAT sectors-per-cluster";
        return false;
    }
    if (reservedSectors == 0 || fatCount == 0 || totalSectors == 0) {
        error = "FAT volume geometry is invalid";
        return false;
    }

    const uint32_t fatSize = fatSize16 != 0 ? fatSize16 : fatSize32;
    if (fatSize == 0) {
        error = "FAT size is zero";
        return false;
    }

    const uint64_t rootDirSectors =
        (static_cast<uint64_t>(rootEntries) * 32 + bytesPerSector - 1) / bytesPerSector;
    const uint64_t overhead = static_cast<uint64_t>(reservedSectors) +
                              static_cast<uint64_t>(fatCount) * fatSize + rootDirSectors;
    if (overhead >= totalSectors) {
        error = "FAT volume overhead exceeds its size";
        return false;
    }

    const uint64_t clusters = (totalSectors - overhead) / sectorsPerCluster;
    if (clusters == 0) {
        error = "FAT volume reports no data clusters";
        return false;
    }

    if (clusters < 4085) {
        info.kind = FatKind::Fat12;
    } else if (clusters < 65525) {
        info.kind = FatKind::Fat16;
    } else {
        info.kind = FatKind::Fat32;
    }

    info.bytesPerSector = bytesPerSector;
    info.bytesPerCluster = static_cast<uint32_t>(bytesPerSector) * sectorsPerCluster;
    info.clusterCount = clusters;
    info.dataStart = info.baseOffset +
                     (static_cast<uint64_t>(reservedSectors) +
                      static_cast<uint64_t>(fatCount) * fatSize + rootDirSectors) * bytesPerSector;

    return true;
}

std::vector<ByteRange> fatFreeRanges(RawDevice& device, const FatVolumeInfo& info, std::string& error) {
    std::vector<ByteRange> ranges;

    if (info.kind == FatKind::None) {
        error = "not a FAT volume";
        return ranges;
    }

    if (info.kind == FatKind::ExFat) {
        std::vector<uint8_t> sector(info.bytesPerSector, 0);
        const uint64_t vbrOffset = info.baseOffset;

        if (!readAt(device, vbrOffset, sector.data(), info.bytesPerSector)) {
            error = "cannot re-read the exFAT volume boot record";
            return ranges;
        }

        const uint32_t rootCluster = readU32(sector.data() + 0x60);
        return exFatBitmapFreeRanges(device, info, rootCluster, error);
    }

    std::vector<uint8_t> sector(info.bytesPerSector, 0);
    if (!readAt(device, info.baseOffset, sector.data(), info.bytesPerSector)) {
        error = "cannot re-read the FAT boot sector";
        return ranges;
    }

    const uint16_t reservedSectors = readU16(sector.data() + 0x0E);
    const uint16_t fatSize16 = readU16(sector.data() + 0x16);
    const uint32_t fatSize32 = readU32(sector.data() + 0x24);
    const uint32_t fatSize = fatSize16 != 0 ? fatSize16 : fatSize32;

    return fatTableFreeRanges(device, info, reservedSectors, fatSize, error);
}

}
