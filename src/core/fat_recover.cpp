#include "core/fat_recover.h"

#include "core/recover_common.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace carver {

namespace {

constexpr uint64_t MAX_FAT_TABLE_BYTES = 512ull * 1024ull * 1024ull;
constexpr uint64_t MAX_DIRECTORY_BYTES = 64ull * 1024ull * 1024ull;
constexpr size_t ENTRY_SIZE = 32;

uint16_t readU16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(data[1] << 8);
}

uint32_t readU32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

bool readAt(RawDevice& device, uint64_t offset, uint8_t* buffer, uint32_t length) {
    uint32_t got = 0;
    std::string ignored;
    return device.readAt(offset, buffer, length, got, ignored) && got == length;
}

bool isValidCluster(const FatVolumeInfo& info, uint32_t cluster) {
    return cluster >= 2 && static_cast<uint64_t>(cluster) < info.clusterCount + 2;
}

bool loadFatTable(RawDevice& device, const FatVolumeInfo& info, std::vector<uint8_t>& fat,
                  std::string& error) {
    fat.clear();

    const uint64_t length = static_cast<uint64_t>(info.fatSizeSectors) * info.bytesPerSector;
    if (length == 0 || length > MAX_FAT_TABLE_BYTES) {
        error = "FAT table size is implausible";
        return false;
    }

    fat.resize(static_cast<size_t>(length));

    uint64_t offset = info.baseOffset +
                      static_cast<uint64_t>(info.fatOffsetSectors) * info.bytesPerSector;
    size_t done = 0;
    while (done < fat.size()) {
        const uint32_t want = static_cast<uint32_t>(
            std::min<uint64_t>(fat.size() - done, 1u << 20));
        if (!readAt(device, offset, fat.data() + done, want)) {
            error = "cannot read the FAT table at offset " + std::to_string(offset);
            return false;
        }
        done += want;
        offset += want;
    }

    return true;
}

uint32_t fatEntry(const std::vector<uint8_t>& fat, FatKind kind, uint32_t cluster) {
    if (kind == FatKind::Fat32) {
        const uint64_t at = static_cast<uint64_t>(cluster) * 4;
        return at + 4 <= fat.size() ? (readU32(fat.data() + at) & 0x0FFFFFFFu) : 0x0FFFFFFFu;
    }
    if (kind == FatKind::Fat16) {
        const uint64_t at = static_cast<uint64_t>(cluster) * 2;
        return at + 2 <= fat.size() ? readU16(fat.data() + at) : 0xFFFFu;
    }

    const uint64_t at = cluster + (cluster / 2);
    if (at + 1 >= fat.size()) {
        return 0x0FFFu;
    }
    const uint16_t pair = readU16(fat.data() + at);
    return (cluster % 2 == 0) ? (pair & 0x0FFFu) : (pair >> 4);
}

bool fatEndOfChain(FatKind kind, uint32_t value) {
    switch (kind) {
    case FatKind::Fat12: return value >= 0x0FF8u;
    case FatKind::Fat16: return value >= 0xFFF8u;
    case FatKind::Fat32: return value >= 0x0FFFFFF8u;
    default: return true;
    }
}

bool fatBadCluster(FatKind kind, uint32_t value) {
    switch (kind) {
    case FatKind::Fat12: return value == 0x0FF7u;
    case FatKind::Fat16: return value == 0xFFF7u;
    case FatKind::Fat32: return value == 0x0FFFFFF7u;
    default: return false;
    }
}

std::vector<uint32_t> followChain(const std::vector<uint8_t>& fat, const FatVolumeInfo& info,
                                  uint32_t start) {
    std::vector<uint32_t> chain;
    uint32_t cluster = start;

    while (isValidCluster(info, cluster) && chain.size() < info.clusterCount) {
        chain.push_back(cluster);
        const uint32_t next = fatEntry(fat, info.kind, cluster);
        if (next == 0 || fatEndOfChain(info.kind, next) || fatBadCluster(info.kind, next) ||
            !isValidCluster(info, next)) {
            break;
        }
        cluster = next;
    }

    return chain;
}

bool readCluster(RawDevice& device, const FatVolumeInfo& info, uint32_t cluster, uint8_t* out) {
    return readAt(device, info.clusterOffset(cluster), out, info.bytesPerCluster);
}

bool readDirectory(RawDevice& device, const FatVolumeInfo& info, const std::vector<uint8_t>& fat,
                   uint32_t startCluster, std::vector<uint8_t>& data,
                   std::vector<uint32_t>& clusters, std::string& error) {
    data.clear();
    clusters.clear();

    const std::vector<uint32_t> chain = followChain(fat, info, startCluster);
    std::vector<uint8_t> buffer(info.bytesPerCluster);

    for (uint32_t cluster : chain) {
        if (data.size() >= MAX_DIRECTORY_BYTES) {
            break;
        }
        if (!readCluster(device, info, cluster, buffer.data())) {
            error = "cannot read directory cluster " + std::to_string(cluster);
            return false;
        }
        data.insert(data.end(), buffer.begin(), buffer.end());
        clusters.push_back(cluster);
    }

    return true;
}

bool readFixedRoot(RawDevice& device, const FatVolumeInfo& info, std::vector<uint8_t>& data,
                   std::string& error) {
    data.clear();

    const uint64_t length =
        std::min<uint64_t>(static_cast<uint64_t>(info.rootEntryCount) * ENTRY_SIZE,
                           MAX_DIRECTORY_BYTES);
    if (length == 0) {
        return true;
    }

    data.resize(static_cast<size_t>(length));
    uint64_t offset = info.baseOffset + info.rootDirStart;
    size_t done = 0;
    while (done < data.size()) {
        const uint32_t want = static_cast<uint32_t>(
            std::min<uint64_t>(data.size() - done, 1u << 20));
        if (!readAt(device, offset, data.data() + done, want)) {
            error = "cannot read the FAT root directory at offset " + std::to_string(offset);
            return false;
        }
        done += want;
        offset += want;
    }

    return true;
}

// A long name is stored as a run of LFN entries immediately before the 8.3
// entry, in reverse order: walking backwards from the 8.3 entry yields the
// first thirteen characters, then the next, and so on.
std::string longNameOf(const std::vector<uint8_t>& data, size_t index) {
    size_t first = index;
    while (first > 0 && data[(first - 1) * ENTRY_SIZE + 11] == 0x0F) {
        --first;
    }
    if (first == index) {
        return {};
    }

    static const int offsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    std::vector<uint8_t> utf16;
    bool done = false;

    for (size_t entry = index; entry > first && !done;) {
        --entry;
        const uint8_t* raw = data.data() + entry * ENTRY_SIZE;
        for (int slot = 0; slot < 13; ++slot) {
            const uint8_t low = raw[offsets[slot]];
            const uint8_t high = raw[offsets[slot] + 1];
            const uint16_t character = static_cast<uint16_t>(low | (high << 8));
            if (character == 0) {
                done = true;
                break;
            }
            utf16.push_back(low);
            utf16.push_back(high);
        }
    }

    return utf16.empty() ? std::string() : utf16leToUtf8(utf16.data(), utf16.size() / 2);
}

// A deleted entry has its first name byte replaced with 0xE5, so the 8.3 name
// is rebuilt from the remaining seven characters plus the extension.
std::string shortNameOf(const uint8_t* entry, bool deleted) {
    std::string name;
    for (int index = deleted ? 1 : 0; index < 8; ++index) {
        const char character = static_cast<char>(entry[index]);
        if (character == ' ' || character == '\0') {
            break;
        }
        name.push_back(character);
    }

    std::string extension;
    for (int index = 8; index < 11; ++index) {
        const char character = static_cast<char>(entry[index]);
        if (character == ' ' || character == '\0') {
            break;
        }
        extension.push_back(character);
    }

    if (!extension.empty()) {
        name += '.';
        name += extension;
    }

    return name;
}

std::string entryName(const std::vector<uint8_t>& data, size_t index, bool deleted) {
    std::string name = longNameOf(data, index);
    if (!name.empty()) {
        return name;
    }
    return shortNameOf(data.data() + index * ENTRY_SIZE, deleted);
}

std::string dosDateTime(uint16_t date, uint16_t time) {
    if (date == 0) {
        return {};
    }

    const int year = ((date >> 9) & 0x7F) + 1980;
    const int month = (date >> 5) & 0x0F;
    const int day = date & 0x1F;
    const int hour = (time >> 11) & 0x1F;
    const int minute = (time >> 5) & 0x3F;
    const int second = (time & 0x1F) * 2;

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
                  year, month, day, hour, minute, second);
    return buffer;
}

uint64_t entryOffsetOf(const FatVolumeInfo& info, const std::vector<uint32_t>& clusters,
                       bool fixedRoot, uint64_t fixedBase, size_t index) {
    const uint64_t byte = static_cast<uint64_t>(index) * ENTRY_SIZE;
    if (fixedRoot) {
        return fixedBase + byte;
    }
    const size_t clusterIndex = static_cast<size_t>(byte / info.bytesPerCluster);
    if (clusterIndex >= clusters.size()) {
        return 0;
    }
    return info.clusterOffset(clusters[clusterIndex]) + byte % info.bytesPerCluster;
}

struct FatCandidate {
    std::string name;
    uint32_t startCluster = 0;
    uint64_t size = 0;
    uint64_t entryOffset = 0;
    uint16_t createdDate = 0;
    uint16_t createdTime = 0;
    uint16_t modifiedDate = 0;
    uint16_t modifiedTime = 0;
};

void scanDirectory(const std::vector<uint8_t>& data, const FatVolumeInfo& info,
                   const std::vector<uint32_t>& clusters, bool fixedRoot, uint64_t fixedBase,
                   std::vector<FatCandidate>& files, std::vector<uint32_t>& childDirectories,
                   uint64_t& entriesScanned) {
    const size_t count = data.size() / ENTRY_SIZE;

    for (size_t index = 0; index < count; ++index) {
        const uint8_t* entry = data.data() + index * ENTRY_SIZE;
        ++entriesScanned;

        const uint8_t firstByte = entry[0];
        if (firstByte == 0x00) {
            break;
        }

        const uint8_t attributes = entry[11];
        const uint32_t startCluster =
            static_cast<uint32_t>(readU16(entry + 26)) |
            (static_cast<uint32_t>(readU16(entry + 20)) << 16);

        if (firstByte == 0xE5) {
            if (attributes == 0x0F || (attributes & 0x08) != 0 || (attributes & 0x10) != 0) {
                continue;
            }

            FatCandidate candidate;
            candidate.name = entryName(data, index, true);
            candidate.startCluster = startCluster;
            candidate.size = readU32(entry + 28);
            candidate.entryOffset = entryOffsetOf(info, clusters, fixedRoot, fixedBase, index);
            candidate.createdTime = readU16(entry + 14);
            candidate.createdDate = readU16(entry + 16);
            candidate.modifiedTime = readU16(entry + 22);
            candidate.modifiedDate = readU16(entry + 24);

            if (isValidCluster(info, candidate.startCluster) && candidate.size > 0) {
                files.push_back(candidate);
            }
        } else if (attributes == 0x0F) {
            continue;
        } else if ((attributes & 0x08) == 0 && (attributes & 0x10) != 0) {
            const std::string name = entryName(data, index, false);
            if (name == "." || name == "..") {
                continue;
            }
            childDirectories.push_back(startCluster);
        }
    }
}

bool writeFatFile(RawDevice& device, const FatVolumeInfo& info,
                  const std::vector<uint32_t>& clusters, uint64_t size,
                  const std::wstring& outputPath, uint64_t& written, std::string& error) {
    written = 0;

    HANDLE output = CreateFileW(outputPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        error = "cannot create output file";
        return false;
    }

    std::vector<uint8_t> buffer(info.bytesPerCluster);
    uint64_t remaining = size;
    bool ok = true;

    for (uint32_t cluster : clusters) {
        if (remaining == 0) {
            break;
        }
        if (!readCluster(device, info, cluster, buffer.data())) {
            error = "cannot read cluster " + std::to_string(cluster);
            ok = false;
            break;
        }
        const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(remaining, info.bytesPerCluster));
        DWORD count = 0;
        if (!WriteFile(output, buffer.data(), chunk, &count, nullptr) || count != chunk) {
            error = "cannot write output file";
            ok = false;
            break;
        }
        written += count;
        remaining -= count;
    }

    CloseHandle(output);

    if (!ok || written == 0) {
        DeleteFileW(outputPath.c_str());
        written = 0;
        return false;
    }

    return true;
}

}

RecoverResult recoverDeletedFatFiles(RawDevice& device,
                                     const FatVolumeInfo& info,
                                     const std::string& outputDirectory,
                                     const RecoverOptions& options,
                                     const ProgressFn& progress,
                                     std::vector<RecoveredFile>& index,
                                     std::string& error) {
    RecoverResult result;
    index.clear();

    if (info.kind == FatKind::None) {
        error = "not a FAT volume";
        return result;
    }
    if (info.kind == FatKind::ExFat) {
        error = "exFAT directories are not supported by --fat-recover; use --free-only";
        return result;
    }
    if (info.bytesPerSector == 0 || info.bytesPerCluster == 0 || info.clusterCount == 0) {
        error = "FAT volume geometry is invalid";
        return result;
    }

    std::vector<uint8_t> fat;
    if (!loadFatTable(device, info, fat, error)) {
        return result;
    }

    const std::wstring outputRoot = utf8ToWide(outputDirectory);
    if (!ensureDirectoryTree(outputRoot)) {
        error = "cannot create output directory " + outputDirectory;
        return result;
    }

    struct PendingDirectory {
        uint32_t startCluster = 0;
        bool fixedRoot = false;
    };

    std::vector<bool> visited(static_cast<size_t>(info.clusterCount) + 2, false);
    std::vector<PendingDirectory> queue;
    if (info.kind == FatKind::Fat32) {
        if (!isValidCluster(info, info.rootCluster)) {
            error = "FAT32 root directory cluster is invalid";
            return result;
        }
        visited[info.rootCluster] = true;
        queue.push_back({info.rootCluster, false});
    } else {
        queue.push_back({0, true});
    }

    Progress state;
    state.bytesTotal = info.clusterCount;

    const uint64_t fixedBase = info.baseOffset + info.rootDirStart;
    uint64_t fileIndex = 0;
    uint64_t directoryClusters = 0;
    bool rootDone = false;
    bool stop = false;

    const auto pump = [&]() -> bool {
        state.bytesScanned = std::min<uint64_t>(
            directoryClusters + result.bytesWritten / info.bytesPerCluster, info.clusterCount);
        state.filesRecovered = result.filesWritten;
        state.bytesRecovered = result.bytesWritten;
        state.currentOutput = index.empty() ? std::string() : index.back().outputName;
        state.currentSize = index.empty() ? 0 : index.back().size;
        return !progress || progress(state);
    };

    while (!queue.empty() && !stop) {
        const PendingDirectory directory = queue.back();
        queue.pop_back();

        std::vector<uint8_t> data;
        std::vector<uint32_t> clusters;
        std::string readError;
        const bool readOk = directory.fixedRoot
                                ? readFixedRoot(device, info, data, readError)
                                : readDirectory(device, info, fat, directory.startCluster, data,
                                                clusters, readError);
        if (!readOk) {
            if (!rootDone) {
                error = readError;
                return result;
            }
            rootDone = true;
            continue;
        }
        rootDone = true;

        std::vector<FatCandidate> files;
        std::vector<uint32_t> childDirectories;
        uint64_t entries = 0;
        scanDirectory(data, info, clusters, directory.fixedRoot, fixedBase, files,
                      childDirectories, entries);
        result.recordsScanned += entries;
        directoryClusters += data.size() / info.bytesPerCluster;

        for (uint32_t child : childDirectories) {
            if (!isValidCluster(info, child) || visited[child]) {
                continue;
            }
            visited[child] = true;
            queue.push_back({child, false});
        }

        for (const FatCandidate& candidate : files) {
            const std::string extension = nameExtension(candidate.name);
            if (!extensionSelected(options.onlyExtensions, extension) ||
                extensionSkipped(options.skipExtensions, extension)) {
                continue;
            }

            result.deletedFound += 1;

            const uint64_t needed =
                (candidate.size + info.bytesPerCluster - 1) / info.bytesPerCluster;
            const std::vector<uint32_t> chain = followChain(fat, info, candidate.startCluster);

            std::vector<uint32_t> use;
            bool contiguous = false;
            bool anyAllocated = false;

            if (chain.size() >= needed) {
                use.assign(chain.begin(), chain.begin() + static_cast<size_t>(needed));
            } else {
                contiguous = true;
                for (uint64_t step = 0; step < needed; ++step) {
                    const uint64_t cluster = static_cast<uint64_t>(candidate.startCluster) + step;
                    if (cluster > info.clusterCount + 1) {
                        break;
                    }
                    use.push_back(static_cast<uint32_t>(cluster));
                }
                for (uint32_t cluster : use) {
                    if (fatEntry(fat, info.kind, cluster) != 0) {
                        anyAllocated = true;
                    }
                }
            }

            if (use.empty()) {
                continue;
            }

            RecoveredFile recovered;
            recovered.recordNumber = candidate.entryOffset;
            recovered.originalName = candidate.name;
            recovered.size = candidate.size;
            recovered.resident = false;
            recovered.clustersStillFree = contiguous ? !anyAllocated : true;
            recovered.overwriteRisk = contiguous && anyAllocated;
            recovered.created = dosDateTime(candidate.createdDate, candidate.createdTime);
            recovered.modified = dosDateTime(candidate.modifiedDate, candidate.modifiedTime);

            if (recovered.overwriteRisk) {
                result.atRisk += 1;
            }

            ++fileIndex;
            char prefix[32];
            std::snprintf(prefix, sizeof(prefix), "%06llu",
                          static_cast<unsigned long long>(fileIndex));

            std::string baseName = sanitizeFileName(candidate.name);
            if (isReservedDeviceName(baseName)) {
                baseName = "_" + baseName;
            }
            recovered.outputName = std::string(prefix) + "_" + baseName;

            uint64_t written = 0;
            bool haveEntry = false;
            if (options.listOnly) {
                written = candidate.size;
                haveEntry = written > 0;
            } else {
                const std::wstring outputPath =
                    outputRoot + L"\\" + utf8ToWide(recovered.outputName);
                std::string writeError;
                haveEntry = writeFatFile(device, info, use, candidate.size, outputPath, written,
                                         writeError) &&
                            written > 0;
            }

            if (haveEntry) {
                recovered.size = written;
                result.filesWritten += 1;
                result.bytesWritten += written;
                index.push_back(recovered);
            }

            if (!pump()) {
                result.cancelled = true;
                stop = true;
                break;
            }
        }

        if (!stop && !pump()) {
            result.cancelled = true;
            stop = true;
        }
    }

    writeRecoveredIndex(outputRoot + L"\\recovered.csv", index);

    return result;
}

}
