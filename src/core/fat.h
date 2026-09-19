#pragma once

#include "core/device.h"
#include "core/range.h"

#include <cstdint>
#include <string>
#include <vector>

namespace carver {

enum class FatKind { None, Fat12, Fat16, Fat32, ExFat };

struct FatVolumeInfo {
    FatKind kind = FatKind::None;
    uint64_t baseOffset = 0;
    uint32_t bytesPerSector = 0;
    uint32_t bytesPerCluster = 0;
    uint64_t clusterCount = 0;
    uint64_t dataStart = 0;      // first data cluster, relative to baseOffset

    // Geometry needed to follow cluster chains and walk directories. All
    // offsets are relative to baseOffset; add baseOffset to reach the device.
    uint32_t fatOffsetSectors = 0;
    uint32_t fatSizeSectors = 0;
    uint32_t fatCount = 0;
    uint32_t rootEntryCount = 0;   // FAT12/16 only
    uint64_t rootDirStart = 0;     // FAT12/16 fixed root directory, relative
    uint64_t rootDirSectors = 0;   // FAT12/16 only
    uint32_t rootCluster = 0;      // FAT32 only

    uint64_t clusterOffset(uint64_t cluster) const {
        return baseOffset + dataStart + (cluster - 2) * bytesPerCluster;
    }
};

bool parseFatBootSector(const uint8_t* sector, size_t length, FatVolumeInfo& info, std::string& error);

std::vector<ByteRange> fatFreeRanges(RawDevice& device, const FatVolumeInfo& info, std::string& error);

std::string describeFatKind(FatKind kind);

}
