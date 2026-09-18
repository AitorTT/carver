#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace carver {

constexpr uint32_t UNKNOWN_PHYSICAL_DISK = 0xFFFFFFFFu;

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
    uint32_t diskNumber = UNKNOWN_PHYSICAL_DISK;
};

std::vector<DriveInfo> listPhysicalDrives();

std::vector<VolumeInfo> listVolumes();

bool isNtfsVolume(const VolumeInfo& volume);

uint32_t physicalDiskOfDevicePath(const std::string& devicePath);

uint32_t physicalDiskOfMountPoint(const std::string& mountPoint);

std::string mountPointOfPath(const std::string& path);

std::string systemVolumeMountPoint();

}
