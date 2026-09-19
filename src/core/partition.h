#pragma once

#include "core/device.h"

#include <cstdint>
#include <string>
#include <vector>

namespace carver {

struct PartitionInfo {
    uint32_t index = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    std::string scheme;
    std::string typeName;
    std::string label;
    bool ntfsCandidate = false;
    bool extendedContainer = false;
};

std::vector<PartitionInfo> parsePartitions(RawDevice& device, std::string& error);

bool quickNtfsCheck(RawDevice& device, uint64_t offset);

bool quickFatCheck(RawDevice& device, uint64_t offset);

struct PartitionResolution {
    uint64_t offset = 0;
    uint64_t size = 0;
    uint32_t index = 0;
    std::string typeName;
    std::string label;
    bool found = false;
    bool autoSelected = false;
};

// Works out the byte offset of the volume to use. When acceptFat is true a
// FAT12/16/32 or exFAT boot sector is accepted as well as NTFS, so a whole-disk
// image holding a FAT partition can be used by --free-only and --fat-recover.
bool resolveNtfsBase(RawDevice& device,
                     const std::string& selection,
                     PartitionResolution& resolution,
                     std::string& error,
                     bool acceptFat = false);

}
