#pragma once

#include "core/carver.h"
#include "core/ntfs.h"

#include <cstdint>
#include <string>
#include <vector>

namespace carver {

struct RecoverOptions {
    uint64_t firstRecord = 1;
    bool includeDirectories = false;
};

struct RecoveredFile {
    uint64_t recordNumber = 0;
    std::string outputName;
    std::string originalName;
    uint64_t size = 0;
    bool resident = false;
    bool clustersStillFree = false;
    bool overwriteRisk = false;
    std::string created;
    std::string modified;
};

struct RecoverResult {
    uint64_t recordsScanned = 0;
    uint64_t deletedFound = 0;
    uint64_t filesWritten = 0;
    uint64_t bytesWritten = 0;
    uint64_t atRisk = 0;
    bool cancelled = false;
};

RecoverResult recoverDeletedFiles(RawDevice& device,
                                  const NtfsVolumeInfo& info,
                                  const std::vector<uint8_t>& bitmap,
                                  const std::string& outputDirectory,
                                  const RecoverOptions& options,
                                  const ProgressFn& progress,
                                  std::vector<RecoveredFile>& index,
                                  std::string& error);

}
