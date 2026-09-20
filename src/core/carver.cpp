#include "core/carver.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <vector>

namespace carver {

namespace {

bool candidateMatches(const uint8_t* buffer, size_t length, size_t index, const Signature& signature) {
    const size_t headerStart = index + signature.headerOffset;
    if (headerStart + signature.header.size() > length) {
        return false;
    }
    if (std::memcmp(buffer + headerStart, signature.header.data(), signature.header.size()) != 0) {
        return false;
    }
    if (signature.lookahead > 0 && index + signature.lookahead > length) {
        return false;
    }
    if (signature.validate != nullptr && !signature.validate(buffer, length, index)) {
        return false;
    }
    return true;
}

std::string slugify(const std::string& value) {
    std::string result = value;
    for (char& character : result) {
        if (character == ' ' || character == '/' || character == '\\' || character == ':') {
            character = '_';
        }
    }
    return result;
}

constexpr const char* CARVE_STATE_MAGIC = "carver-carve-state";
constexpr uint32_t CARVE_STATE_VERSION = 1;
constexpr uint64_t CARVE_CHECKPOINT_INTERVAL = 64ull * 1024ull * 1024ull;

struct CarveState {
    std::string source;
    std::vector<ByteRange> plan;
    uint64_t scanned = 0;
    uint64_t fileIndex = 0;
    bool present = false;
};

std::string carveStatePath(const std::string& outputDirectory) {
    return outputDirectory + "\\carve-state.txt";
}

std::string trimLine(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool readCarveState(const std::string& path, CarveState& state) {
    state = CarveState{};

    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "rb") != 0 || file == nullptr) {
        return false;
    }

    char line[2048] = {};
    bool magicSeen = false;

    while (fgets(line, sizeof(line), file) != nullptr) {
        const std::string text = trimLine(line);
        if (text.empty()) {
            continue;
        }
        if (!magicSeen) {
            if (text != CARVE_STATE_MAGIC) {
                fclose(file);
                return false;
            }
            magicSeen = true;
            continue;
        }

        const size_t separator = text.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const std::string key = text.substr(0, separator);
        const std::string value = text.substr(separator + 1);
        const unsigned long long number = std::strtoull(value.c_str(), nullptr, 10);

        if (key == "version") {
            if (number != CARVE_STATE_VERSION) {
                fclose(file);
                return false;
            }
        } else if (key == "source") {
            state.source = value;
        } else if (key == "scanned") {
            state.scanned = number;
        } else if (key == "fileIndex") {
            state.fileIndex = number;
        } else if (key.rfind("plan", 0) == 0 && key.size() > 4 &&
                   std::iswdigit(static_cast<wint_t>(key[4])) != 0) {
            const size_t colon = value.find(':');
            if (colon != std::string::npos) {
                ByteRange range;
                range.start = std::strtoull(value.substr(0, colon).c_str(), nullptr, 10);
                range.end = std::strtoull(value.substr(colon + 1).c_str(), nullptr, 10);
                state.plan.push_back(range);
            }
        }
    }

    fclose(file);

    if (!magicSeen) {
        return false;
    }

    state.present = true;
    return true;
}

bool writeCarveState(const std::string& path, const std::string& source,
                     const std::vector<ByteRange>& plan, uint64_t scanned, uint64_t fileIndex) {
    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "wb") != 0 || file == nullptr) {
        return false;
    }

    std::fprintf(file, "%s\n", CARVE_STATE_MAGIC);
    std::fprintf(file, "version=%u\n", CARVE_STATE_VERSION);
    std::fprintf(file, "source=%s\n", source.c_str());
    std::fprintf(file, "scanned=%llu\n", static_cast<unsigned long long>(scanned));
    std::fprintf(file, "fileIndex=%llu\n", static_cast<unsigned long long>(fileIndex));
    std::fprintf(file, "planCount=%llu\n", static_cast<unsigned long long>(plan.size()));
    for (size_t index = 0; index < plan.size(); ++index) {
        std::fprintf(file, "plan%llu=%llu:%llu\n",
                     static_cast<unsigned long long>(index),
                     static_cast<unsigned long long>(plan[index].start),
                     static_cast<unsigned long long>(plan[index].end));
    }

    return std::fclose(file) == 0;
}

bool samePlan(const std::vector<ByteRange>& left, const std::vector<ByteRange>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        if (left[index].start != right[index].start || left[index].end != right[index].end) {
            return false;
        }
    }
    return true;
}

// The highest NNNNNN_ prefix already used in the output directory, so a manual
// resume offset cannot overwrite an earlier recovered file.
uint64_t highestOutputIndex(const std::string& outputDirectory) {
    const std::wstring pattern = utf8ToWide(outputDirectory) + L"\\*";
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return 0;
    }

    uint64_t highest = 0;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        const std::wstring name = data.cFileName;
        size_t digits = 0;
        while (digits < name.size() && std::iswdigit(static_cast<wint_t>(name[digits])) != 0) {
            ++digits;
        }
        if (digits == 0 || digits >= name.size() || name[digits] != L'_') {
            continue;
        }
        const uint64_t value = std::wcstoull(name.substr(0, digits).c_str(), nullptr, 10);
        if (value > highest) {
            highest = value;
        }
    } while (FindNextFileW(find, &data) != 0);

    FindClose(find);
    return highest;
}

uint64_t carveOne(RawDevice& device,
                  uint64_t startOffset,
                  uint64_t regionEnd,
                  const Signature& signature,
                  uint64_t declaredSize,
                  const std::wstring& outputPath,
                  bool writeToDisk,
                  uint64_t& bytesWritten,
                  uint64_t& readErrors) {
    bytesWritten = 0;

    HANDLE output = INVALID_HANDLE_VALUE;
    if (writeToDisk) {
        output = CreateFileW(outputPath.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE) {
            return startOffset + 1;
        }
    }

    const bool exactLength = declaredSize > 0;
    const uint64_t cap = exactLength ? std::min(declaredSize, signature.maxSize) : signature.maxSize;
    const uint64_t limit = std::min(startOffset + cap, regionEnd);
    const size_t keep = (!exactLength && !signature.footer.empty()) ? signature.footer.size() - 1 : 0;

    std::vector<uint8_t> buffer(256 * 1024);
    std::vector<uint8_t> tail;
    std::vector<uint8_t> search;

    uint64_t position = startOffset;
    uint64_t endOffset = limit;
    bool finished = false;

    const uint32_t sector = device.sectorSize() != 0 ? device.sectorSize() : 512u;

    while (position < limit) {
        const uint32_t want = static_cast<uint32_t>(std::min<uint64_t>(buffer.size(), limit - position));
        uint32_t got = 0;
        std::string ignored;
        if (!device.readAt(position, buffer.data(), want, got, ignored) || got == 0) {
            // The chunk runs into a damaged sector. Re-read it one sector at a
            // time, zero filling the sectors that cannot be read, so the file
            // keeps its length and everything around the damage still lands.
            got = 0;
            while (got < want) {
                const uint32_t step = std::min<uint32_t>(sector, want - got);
                uint32_t one = 0;
                std::string sectorError;
                if (device.readAt(position + got, buffer.data() + got, step, one, sectorError) && one > 0) {
                    got += one;
                    if (one < step) {
                        break;
                    }
                } else {
                    std::memset(buffer.data() + got, 0, step);
                    got += step;
                    readErrors += 1;
                }
            }

            if (got == 0) {
                endOffset = position;
                finished = true;
                break;
            }
        }

        const uint64_t chunkStart = position;
        uint32_t writeLength = got;

        if (keep > 0) {
            search.clear();
            search.reserve(tail.size() + got);
            search.insert(search.end(), tail.begin(), tail.end());
            search.insert(search.end(), buffer.begin(), buffer.begin() + got);

            const size_t footerSize = signature.footer.size();
            for (size_t index = 0; index + footerSize <= search.size(); ++index) {
                if (std::memcmp(search.data() + index, signature.footer.data(), footerSize) != 0) {
                    continue;
                }

                const uint64_t footerStart = chunkStart - tail.size() + index;
                const uint64_t fileEnd = footerStart + footerSize;
                if (fileEnd <= chunkStart) {
                    continue;
                }

                const uint64_t relative = fileEnd - chunkStart;
                if (relative <= got) {
                    writeLength = static_cast<uint32_t>(relative);
                    endOffset = fileEnd;
                    finished = true;
                    break;
                }
            }
        }

        DWORD written = 0;
        if (writeToDisk) {
            if (!WriteFile(output, buffer.data(), writeLength, &written, nullptr)) {
                break;
            }
        } else {
            written = writeLength;
        }
        bytesWritten += written;

        position = chunkStart + got;

        if (finished) {
            break;
        }

        if (keep > 0) {
            const size_t from = got > keep ? got - keep : 0;
            tail.assign(buffer.begin() + from, buffer.begin() + got);
        }
    }

    if (writeToDisk) {
        CloseHandle(output);
    }

    if (bytesWritten == 0) {
        if (writeToDisk) {
            DeleteFileW(outputPath.c_str());
        }
    } else if (endOffset <= startOffset) {
        endOffset = position;
    }

    return endOffset;
}

}

CarveResult carveDevice(RawDevice& device,
                        const std::string& outputDirectory,
                        const std::vector<Signature>& signatures,
                        const CarveOptions& options,
                        const ProgressFn& progress,
                        std::string& error) {
    CarveResult result;

    if (signatures.empty()) {
        error = "no signatures selected";
        return result;
    }

    std::vector<Signature> active;
    active.reserve(signatures.size());
    for (const auto& signature : signatures) {
        if (extensionSelected(options.onlyExtensions, signature.extension) &&
            !extensionSkipped(options.skipExtensions, signature.extension)) {
            active.push_back(signature);
        }
    }
    if (active.empty()) {
        error = options.onlyExtensions.empty() ? "every selected file type was skipped"
                                               : "none of the requested file types matched a signature";
        return result;
    }
    const std::vector<Signature>& candidates = active;

    const uint64_t deviceSize = device.size();

    std::vector<ByteRange> plan;
    if (!options.ranges.empty()) {
        plan.reserve(options.ranges.size());
        for (const auto& requested : options.ranges) {
            const ByteRange clamped{std::min(requested.start, deviceSize), std::min(requested.end, deviceSize)};
            if (clamped.start < clamped.end) {
                plan.push_back(clamped);
            }
        }
    } else {
        const uint64_t begin = std::min(options.startOffset, deviceSize);
        const uint64_t finish = (options.endOffset == 0 || options.endOffset > deviceSize)
                                    ? deviceSize
                                    : options.endOffset;
        if (begin < finish) {
            plan.push_back(ByteRange{begin, finish});
        }
    }

    if (plan.empty()) {
        error = "scan range is empty";
        return result;
    }

    const std::wstring outputRoot = utf8ToWide(outputDirectory);
    if (!ensureDirectoryTree(outputRoot)) {
        error = "cannot create output directory " + outputDirectory;
        return result;
    }

    const std::string statePath = carveStatePath(outputDirectory);
    const std::string sourcePath = wideToUtf8(device.path());

    uint64_t resumeScanned = 0;
    uint64_t fileIndex = 0;

    if (options.resume || options.hasResumeOffset) {
        CarveState saved;
        const bool haveState = readCarveState(statePath, saved);

        if (haveState && !saved.source.empty() && saved.source != sourcePath) {
            error = "the checkpoint was written for a different source (" + saved.source + ")";
            return result;
        }

        if (options.resume) {
            if (!haveState) {
                error = "no usable checkpoint in " + outputDirectory;
                return result;
            }
            if (!samePlan(saved.plan, plan)) {
                error = "the checkpoint covers a different scan range; restart, or pick an "
                        "offset with --resume-from";
                return result;
            }
            resumeScanned = saved.scanned;
            fileIndex = saved.fileIndex;
        } else {
            resumeScanned = options.resumeOffset;
            fileIndex = haveState ? saved.fileIndex : highestOutputIndex(outputDirectory);
        }

        result.resumedFrom = resumeScanned;
    } else {
        // A fresh scan must not be confused by a checkpoint left over from before.
        DeleteFileW(utf8ToWide(statePath).c_str());
    }

    // Turn the resume offset back into a position inside the scan plan.
    uint64_t scannedBefore = 0;
    size_t planIndex = 0;
    uint64_t position = plan.front().start;
    {
        uint64_t accumulated = 0;
        bool placed = false;
        for (size_t index = 0; index < plan.size(); ++index) {
            const uint64_t length = plan[index].end - plan[index].start;
            if (resumeScanned < accumulated + length) {
                planIndex = index;
                position = plan[index].start + (resumeScanned - accumulated);
                scannedBefore = accumulated;
                placed = true;
                break;
            }
            accumulated += length;
        }
        if (!placed) {
            // Everything the plan covers has already been scanned.
            DeleteFileW(utf8ToWide(statePath).c_str());
            result.bytesScanned = accumulated;
            return result;
        }
    }

    const uint64_t chunkSize = std::max<uint64_t>(options.chunkSize, 64 * 1024);
    const size_t overlap = std::max<size_t>(
        std::max(maxHeaderLength(candidates), maxLookahead(candidates)), 1);

    std::vector<uint8_t> buffer(static_cast<size_t>(chunkSize));

    Progress state;
    for (const auto& range : plan) {
        state.bytesTotal += range.end - range.start;
    }

    uint64_t lastCheckpoint = resumeScanned;
    const auto saveCheckpoint = [&](uint64_t scanned) {
        writeCarveState(statePath, sourcePath, plan, scanned, fileIndex);
    };

    const uint32_t sector = device.sectorSize() != 0 ? device.sectorSize() : 512u;
    uint64_t sectorModeEnd = 0;

    while (planIndex < plan.size()) {
        if (position >= plan[planIndex].end) {
            scannedBefore += plan[planIndex].end - plan[planIndex].start;
            ++planIndex;
            sectorModeEnd = 0;
            if (planIndex < plan.size()) {
                position = plan[planIndex].start;
            }
            continue;
        }

        const uint64_t rangeEnd = plan[planIndex].end;
        uint64_t want = std::min<uint64_t>(buffer.size(), rangeEnd - position);
        if (position < sectorModeEnd && want > sector) {
            want = sector;
        }

        uint32_t got = 0;
        bool salvaged = false;
        std::string readError;
        bool readable = device.readAt(position, buffer.data(), static_cast<uint32_t>(want), got, readError);

        if (!readable && want > sector) {
            // The read crossed a damaged sector. Note the window and drop back
            // to single sectors, so the good sectors in it are still scanned.
            if (sectorModeEnd < position + want) {
                sectorModeEnd = std::min<uint64_t>(rangeEnd, position + want);
            }
            const uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(sector, rangeEnd - position));
            uint32_t one = 0;
            std::string sectorError;
            readable = device.readAt(position, buffer.data(), step, one, sectorError) && one > 0;
            got = one;
            salvaged = true;
        }

        if (!readable) {
            // A sector that cannot be read at all: report it, skip it and keep
            // scanning rather than failing the whole run.
            const uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(sector, rangeEnd - position));
            result.readErrors += 1;
            result.bytesSkipped += step;
            if (result.readErrors == 1) {
                result.firstBadOffset = position;
            }
            state.readErrors = result.readErrors;
            state.lastBadOffset = position;
            position += step;

            result.bytesScanned = scannedBefore + (std::min(position, rangeEnd) - plan[planIndex].start);
            state.bytesScanned = result.bytesScanned;
            if (result.bytesScanned - lastCheckpoint >= CARVE_CHECKPOINT_INTERVAL) {
                lastCheckpoint = result.bytesScanned;
                saveCheckpoint(result.bytesScanned);
            }
            if (progress && !progress(state)) {
                result.cancelled = true;
                result.paused = true;
                saveCheckpoint(result.bytesScanned);
                return result;
            }
            continue;
        }

        if (got == 0) {
            position = rangeEnd;
            continue;
        }

        const Signature* found = nullptr;
        size_t foundIndex = 0;

        for (const auto& signature : candidates) {
            if (signature.headerOffset + signature.header.size() > got) {
                continue;
            }
            for (size_t index = 0;
                 index + signature.headerOffset + signature.header.size() <= got;
                 ++index) {
                if (candidateMatches(buffer.data(), got, index, signature)) {
                    if (found == nullptr || index < foundIndex) {
                        found = &signature;
                        foundIndex = index;
                    }
                    break;
                }
            }
        }

        if (found != nullptr) {
            const uint64_t hitOffset = position + foundIndex;
            const uint64_t carveStart = hitOffset >= found->headerOffset
                                            ? hitOffset - found->headerOffset
                                            : hitOffset;

            const bool accepted = found->deviceValidate == nullptr ||
                                  found->deviceValidate(device, carveStart);

            if (!accepted) {
                const uint64_t next = carveStart + 1;
                position = next > position ? next : position + 1;
            } else {
                uint64_t declared = 0;
                if (found->declaredSize != nullptr) {
                    declared = found->declaredSize(buffer.data(), got, foundIndex);
                }
                if (found->refineSize != nullptr) {
                    declared = found->refineSize(device, carveStart, declared);
                }

                ++fileIndex;

                char indexLabel[32];
                std::snprintf(indexLabel, sizeof(indexLabel), "%06llu", static_cast<unsigned long long>(fileIndex));

                const std::string baseName = std::string(indexLabel) + "_" + slugify(found->name) + "." + found->extension;
                const std::wstring outputPath = outputRoot + L"\\" + utf8ToWide(baseName);

                uint64_t written = 0;
                const uint64_t endOffset = carveOne(device, carveStart, rangeEnd, *found, declared,
                                                    outputPath, !options.listOnly, written, result.readErrors);

                if (written > 0) {
                    result.filesRecovered += 1;
                    result.bytesRecovered += written;
                    state.currentOutput = baseName;
                    state.currentSize = written;
                } else {
                    state.currentOutput.clear();
                    state.currentSize = 0;
                }

                state.currentType = found->name;
                state.filesRecovered = result.filesRecovered;
                state.bytesRecovered = result.bytesRecovered;

                const uint64_t next = std::max<uint64_t>(endOffset, carveStart + 1);
                position = next > position ? next : position + 1;
            }
        } else {
            position += (got > overlap) ? got - overlap : got;
            // A short read means the device gave up early only when this was a
            // normal read; a salvaged sector is meant to be shorter than the
            // window, so it must not be mistaken for the end of the range.
            if (!salvaged && got < want) {
                position = rangeEnd;
            }
        }

        result.bytesScanned = scannedBefore + (std::min(position, rangeEnd) - plan[planIndex].start);
        state.bytesScanned = result.bytesScanned;

        if (result.bytesScanned - lastCheckpoint >= CARVE_CHECKPOINT_INTERVAL) {
            lastCheckpoint = result.bytesScanned;
            saveCheckpoint(result.bytesScanned);
        }

        if (progress && !progress(state)) {
            result.cancelled = true;
            result.paused = true;
            saveCheckpoint(result.bytesScanned);
            return result;
        }
    }

    if (progress) {
        state.bytesScanned = result.bytesScanned;
        progress(state);
    }

    // A completed scan has nothing left to resume.
    DeleteFileW(utf8ToWide(statePath).c_str());

    return result;
}

}
