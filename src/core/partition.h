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

struct PartitionResolution {
    uint64_t offset = 0;
    uint64_t size = 0;
    uint32_t index = 0;
    std::string typeName;
    std::string label;
    bool found = false;
    bool autoSelected = false;
};

bool resolveNtfsBase(RawDevice& device,
                     const std::string& selection,
                     PartitionResolution& resolution,
                     std::string& error);

}
