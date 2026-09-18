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

struct VolumeInfo {
    std::string devicePath;
    std::string mountPoint;
    std::string label;
    std::string fileSystem;
    uint64_t size = 0;
    uint64_t free = 0;
};

std::vector<DriveInfo> listPhysicalDrives();

std::vector<VolumeInfo> listVolumes();

bool isNtfsVolume(const VolumeInfo& volume);

}
