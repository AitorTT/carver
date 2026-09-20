#pragma once

#include "core/device.h"
#include "core/range.h"
#include "core/signature.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace carver {

struct Progress {
    uint64_t bytesScanned = 0;
    uint64_t bytesTotal = 0;
    uint64_t filesRecovered = 0;
    uint64_t bytesRecovered = 0;
    uint64_t currentSize = 0;
    uint64_t readErrors = 0;
    uint64_t lastBadOffset = 0;
    std::string currentType;
    std::string currentOutput;
};

using ProgressFn = std::function<bool(const Progress&)>;

struct CarveOptions {
    uint64_t startOffset = 0;
    uint64_t endOffset = 0;
    uint64_t chunkSize = 8ull * 1024ull * 1024ull;
    std::vector<ByteRange> ranges;
    std::vector<std::string> skipExtensions;
    std::vector<std::string> onlyExtensions;
    bool listOnly = false;

    // Continue a scan that was stopped earlier. With resume set, the saved
    // checkpoint in the output directory decides where to carry on. With a
    // resume offset, the caller names the byte offset directly.
    bool resume = false;
    bool hasResumeOffset = false;
    uint64_t resumeOffset = 0;
};

struct CarveResult {
    uint64_t filesRecovered = 0;
    uint64_t bytesRecovered = 0;
    uint64_t bytesScanned = 0;
    uint64_t readErrors = 0;      // unreadable sectors that had to be skipped
    uint64_t bytesSkipped = 0;    // bytes skipped because they could not be read
    uint64_t firstBadOffset = 0;  // where the first unreadable sector was found
    uint64_t resumedFrom = 0;     // offset this run carried on from, when resuming
    bool paused = false;          // stopped early with a checkpoint kept on disk
    bool cancelled = false;
};

CarveResult carveDevice(RawDevice& device,
                        const std::string& outputDirectory,
                        const std::vector<Signature>& signatures,
                        const CarveOptions& options,
                        const ProgressFn& progress,
                        std::string& error);

}
