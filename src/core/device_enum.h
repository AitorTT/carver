#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace carver {

struct DriveInfo {
    std::string devicePath;
    std::string model;
    std::string busType;
    uint64_t size = 0;
    uint32_t sectorSize = 0;
    bool removable = false;
};

std::vector<DriveInfo> listPhysicalDrives();

}
