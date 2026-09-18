#pragma once

#include "core/device.h"
#include "core/range.h"

#include <cstdint>
#include <functional>
#include <string>

namespace carver {

struct ImageOptions {
    uint64_t startOffset = 0;
    uint64_t endOffset = 0;
    uint64_t chunkSize = 4ull * 1024ull * 1024ull;
    bool resume = false;
    bool force = false;
};

struct ImageProgress {
    uint64_t bytesDone = 0;
    uint64_t bytesTotal = 0;
    double bytesPerSecond = 0.0;
};

using ImageProgressFn = std::function<bool(const ImageProgress&)>;

struct ImageResult {
    uint64_t bytesWritten = 0;
    uint64_t bytesThisRun = 0;
    std::string sha256;
    bool cancelled = false;
    bool resumed = false;
};

std::string imageStatePath(const std::string& destinationPath);

bool imageResumeOffset(const std::string& destinationPath,
                       const std::string& sourcePath,
                       uint64_t rangeStart,
                       uint64_t rangeEnd,
                       uint64_t& offset,
                       std::string& error);

bool createImage(RawDevice& source,
                 const std::string& sourcePath,
                 const std::string& destinationPath,
                 const ImageOptions& options,
                 const ImageProgressFn& progress,
                 ImageResult& result,
                 std::string& error);

}
