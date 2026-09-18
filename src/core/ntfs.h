#pragma once

#include "core/device.h"
#include "core/range.h"

#include <cstdint>
#include <string>
#include <vector>

namespace carver {

struct NtfsVolumeInfo {
    uint64_t baseOffset = 0;
    uint32_t bytesPerSector = 0;
    uint32_t bytesPerCluster = 0;
    uint64_t totalSectors = 0;
    uint64_t mftCluster = 0;
    uint64_t mftMirrorCluster = 0;
    uint32_t mftRecordSize = 0;
    uint32_t indexBufferSize = 0;

    uint64_t totalClusters() const { return bytesPerCluster == 0 ? 0 : (totalSectors * bytesPerSector) / bytesPerCluster; }
    uint64_t clusterOffset(uint64_t cluster) const { return baseOffset + cluster * bytesPerCluster; }
};

struct DataRun {
    uint64_t lcn = 0;
    uint64_t length = 0;
    bool sparse = false;
};

struct DataExtent {
    uint64_t startingVcn = 0;
    uint64_t lastVcn = 0;
    uint64_t realSize = 0;
    std::vector<DataRun> runs;
};

struct AttributeListEntry {
    uint32_t type = 0;
    uint64_t startingVcn = 0;
    uint64_t baseRecord = 0;
    uint16_t attributeId = 0;
};

struct NtfsTimestamps {
    uint64_t created = 0;
    uint64_t modified = 0;
    uint64_t mftModified = 0;
    uint64_t accessed = 0;
};

struct MftFileEntry {
    uint64_t recordNumber = 0;
    uint64_t baseRecordReference = 0;
    uint16_t sequence = 0;
    bool inUse = false;
    bool directory = false;
    bool hasAttributeList = false;
    bool hasData = false;
    bool residentData = false;
    uint64_t logicalSize = 0;
    uint64_t allocatedSize = 0;
    std::vector<uint8_t> residentContent;
    std::vector<DataRun> runs;
    std::vector<DataExtent> dataExtents;
    std::vector<AttributeListEntry> attributeList;
    bool attributeListFollowed = false;
    std::string name;
    uint8_t nameNamespace = 0;
    uint64_t parentRecord = 0;
    NtfsTimestamps standard;
    NtfsTimestamps fileName;
};

bool parseNtfsBootSector(const uint8_t* sector, size_t length, NtfsVolumeInfo& info, std::string& error);

bool decodeRunList(const uint8_t* data, size_t length, std::vector<DataRun>& runs, std::string& error);

bool readMftEntry(RawDevice& device, const NtfsVolumeInfo& info, uint64_t recordNumber,
                  MftFileEntry& entry, std::string& error);

bool readMftEntryFull(RawDevice& device, const NtfsVolumeInfo& info, uint64_t recordNumber,
                      MftFileEntry& entry, std::string& error);

bool getMftRecordCount(RawDevice& device, const NtfsVolumeInfo& info, uint64_t& count, std::string& error);

std::string utf16leToUtf8(const uint8_t* data, size_t charCount);

std::string fileTimeToString(uint64_t fileTime);

bool readFileRecordData(RawDevice& device, const NtfsVolumeInfo& info, uint64_t recordNumber,
                        std::vector<uint8_t>& data, std::string& error);

bool readBitmap(RawDevice& device, const NtfsVolumeInfo& info, std::vector<uint8_t>& bitmap, std::string& error);

std::vector<ByteRange> freeClusterRanges(const std::vector<uint8_t>& bitmap, uint64_t totalClusters,
                                         uint64_t bytesPerCluster, uint64_t baseOffset = 0);

}
