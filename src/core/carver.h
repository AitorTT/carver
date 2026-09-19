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
};

struct CarveResult {
    uint64_t filesRecovered = 0;
    uint64_t bytesRecovered = 0;
    uint64_t bytesScanned = 0;
    bool cancelled = false;
};

CarveResult carveDevice(RawDevice& device,
                        const std::string& outputDirectory,
                        const std::vector<Signature>& signatures,
                        const CarveOptions& options,
                        const ProgressFn& progress,
                        std::string& error);

}
