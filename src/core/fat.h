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
    uint64_t dataStart = 0;
};

bool parseFatBootSector(const uint8_t* sector, size_t length, FatVolumeInfo& info, std::string& error);

std::vector<ByteRange> fatFreeRanges(RawDevice& device, const FatVolumeInfo& info, std::string& error);

std::string describeFatKind(FatKind kind);

}
