#pragma once

#include "core/carver.h"
#include "core/ntfs.h"

#include <cstdint>
#include <string>
#include <vector>

namespace carver {

struct IndexName {
    uint64_t mftRecord = 0;
    uint16_t sequence = 0;
    uint64_t parentRecord = 0;
    std::string name;
    std::string path;
    uint64_t size = 0;
    uint8_t nameNamespace = 0;
    bool fromSlack = false;
    NtfsTimestamps times;
};

struct I30Result {
    uint64_t recordsScanned = 0;
    uint64_t directoriesScanned = 0;
    uint64_t liveEntries = 0;
    uint64_t slackEntries = 0;
    bool cancelled = false;
};

I30Result recoverIndexNames(RawDevice& device,
                            const NtfsVolumeInfo& info,
                            uint64_t recordCount,
                            const std::string& outputCsv,
                            const ProgressFn& progress,
                            std::vector<IndexName>& names,
                            std::string& error);

}
