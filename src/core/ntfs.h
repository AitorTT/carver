#pragma once

#include "core/device.h"
#include "core/range.h"

#include <cstdint>
#include <string>
#include <vector>

namespace carver {

struct NtfsVolumeInfo {
    uint32_t bytesPerSector = 0;
    uint32_t bytesPerCluster = 0;
    uint64_t totalSectors = 0;
    uint64_t mftCluster = 0;
    uint64_t mftMirrorCluster = 0;
    uint32_t mftRecordSize = 0;
    uint32_t indexBufferSize = 0;

    uint64_t totalClusters() const { return bytesPerCluster == 0 ? 0 : (totalSectors * bytesPerSector) / bytesPerCluster; }
    uint64_t clusterOffset(uint64_t cluster) const { return cluster * bytesPerCluster; }
};

struct DataRun {
    uint64_t lcn = 0;
    uint64_t length = 0;
    bool sparse = false;
};

bool parseNtfsBootSector(const uint8_t* sector, size_t length, NtfsVolumeInfo& info, std::string& error);

bool decodeRunList(const uint8_t* data, size_t length, std::vector<DataRun>& runs, std::string& error);

bool readFileRecordData(RawDevice& device, const NtfsVolumeInfo& info, uint64_t recordNumber,
                        std::vector<uint8_t>& data, std::string& error);

bool readBitmap(RawDevice& device, const NtfsVolumeInfo& info, std::vector<uint8_t>& bitmap, std::string& error);

std::vector<ByteRange> freeClusterRanges(const std::vector<uint8_t>& bitmap, uint64_t totalClusters, uint64_t bytesPerCluster);

}
