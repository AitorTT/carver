#include "core/recover.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace carver {

namespace {

bool ensureDirectory(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr)) {
        return true;
    }
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool isClusterAllocated(const std::vector<uint8_t>& bitmap, uint64_t cluster) {
    const size_t index = static_cast<size_t>(cluster / 8);
    if (index >= bitmap.size()) {
        return true;
    }
    return ((bitmap[index] >> (cluster % 8)) & 1u) != 0;
}

std::string sanitizeFileName(const std::string& name) {
    std::string cleaned;
    cleaned.reserve(name.size());

    for (unsigned char character : name) {
        const bool invalid = character < 0x20 || character == '<' || character == '>' ||
                             character == ':' || character == '"' || character == '/' ||
                             character == '\\' || character == '|' || character == '?' ||
                             character == '*';
        cleaned.push_back(invalid ? '_' : static_cast<char>(character));
    }

    while (!cleaned.empty() && (cleaned.back() == ' ' || cleaned.back() == '.')) {
        cleaned.pop_back();
    }

    return cleaned.empty() ? std::string("unnamed") : cleaned;
}

bool isReservedDeviceName(const std::string& name) {
    static const char* const reserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };

    std::string stem = name.substr(0, name.find('.'));
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char character) { return static_cast<char>(std::toupper(character)); });

    for (const char* candidate : reserved) {
        if (stem == candidate) {
            return true;
        }
    }
    return false;
}

uint64_t clustersStillFreeFor(const std::vector<uint8_t>& bitmap, const MftFileEntry& entry, bool& anyAllocated) {
    anyAllocated = false;
    uint64_t freeClusters = 0;

    for (const auto& run : entry.runs) {
        if (run.sparse) {
            freeClusters += run.length;
            continue;
        }
        for (uint64_t cluster = run.lcn; cluster < run.lcn + run.length; ++cluster) {
            if (isClusterAllocated(bitmap, cluster)) {
                anyAllocated = true;
            } else {
                ++freeClusters;
            }
        }
    }

    return freeClusters;
}

bool writeEntryData(RawDevice& device, const NtfsVolumeInfo& info, const MftFileEntry& entry,
                    const std::wstring& outputPath, uint64_t& written, std::string& error) {
    written = 0;

    HANDLE output = CreateFileW(outputPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        error = "cannot create output file";
        return false;
    }

    bool ok = true;

    if (entry.residentData) {
        if (!entry.residentContent.empty()) {
            DWORD count = 0;
            if (!WriteFile(output, entry.residentContent.data(),
                           static_cast<DWORD>(entry.residentContent.size()), &count, nullptr)) {
                ok = false;
            } else {
                written += count;
            }
        }
    } else {
        std::vector<uint8_t> buffer(1 << 20);
        uint64_t remaining = entry.logicalSize;

        for (const auto& run : entry.runs) {
            if (remaining == 0) {
                break;
            }

            const uint64_t runBytes = std::min(run.length * info.bytesPerCluster, remaining);

            if (run.sparse) {
                std::fill(buffer.begin(), buffer.end(), 0);
                uint64_t left = runBytes;
                while (left > 0) {
                    const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(left, buffer.size()));
                    DWORD count = 0;
                    if (!WriteFile(output, buffer.data(), chunk, &count, nullptr) || count == 0) {
                        ok = false;
                        break;
                    }
                    written += count;
                    left -= count;
                }
            } else {
                uint64_t offset = info.clusterOffset(run.lcn);
                uint64_t left = runBytes;
                while (left > 0) {
                    const uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(left, buffer.size()));
                    uint32_t got = 0;
                    if (!device.readAt(offset, buffer.data(), chunk, got, error) || got == 0) {
                        ok = false;
                        break;
                    }
                    DWORD count = 0;
                    if (!WriteFile(output, buffer.data(), got, &count, nullptr) || count == 0) {
                        ok = false;
                        break;
                    }
                    written += count;
                    offset += got;
                    left -= got;
                }
            }

            if (!ok) {
                break;
            }
            remaining -= runBytes;
        }
    }

    CloseHandle(output);

    if (!ok || written == 0) {
        DeleteFileW(outputPath.c_str());
        written = 0;
        return false;
    }

    return true;
}

std::string csvField(const std::string& value) {
    if (value.find_first_of(",\"\n") == std::string::npos) {
        return value;
    }
    std::string out = "\"";
    for (char character : value) {
        if (character == '"') {
            out += "\"\"";
        } else {
            out.push_back(character);
        }
    }
    out += "\"";
    return out;
}

void writeIndex(const std::wstring& path, const std::vector<RecoveredFile>& index) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }

    std::string text = "output_name,original_name,size,record,resident,clusters_free,overwrite_risk,created,modified\n";
    for (const auto& file : index) {
        text += csvField(file.outputName) + "," +
                csvField(file.originalName) + "," +
                std::to_string(file.size) + "," +
                std::to_string(file.recordNumber) + "," +
                (file.resident ? "yes" : "no") + "," +
                (file.clustersStillFree ? "yes" : "no") + "," +
                (file.overwriteRisk ? "yes" : "no") + "," +
                csvField(file.created) + "," +
                csvField(file.modified) + "\n";
    }

    DWORD written = 0;
    WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(handle);
}

}

RecoverResult recoverDeletedFiles(RawDevice& device,
                                  const NtfsVolumeInfo& info,
                                  const std::vector<uint8_t>& bitmap,
                                  const std::string& outputDirectory,
                                  const RecoverOptions& options,
                                  const ProgressFn& progress,
                                  std::vector<RecoveredFile>& index,
                                  std::string& error) {
    RecoverResult result;
    index.clear();

    uint64_t recordCount = 0;
    if (!getMftRecordCount(device, info, recordCount, error)) {
        return result;
    }

    const std::wstring outputRoot = utf8ToWide(outputDirectory);
    if (!ensureDirectory(outputRoot)) {
        error = "cannot create output directory " + outputDirectory;
        return result;
    }

    Progress state;
    state.bytesTotal = recordCount;

    uint64_t fileIndex = 0;
    uint64_t processed = 0;

    for (uint64_t recordNumber = options.firstRecord; recordNumber < recordCount; ++recordNumber) {
        MftFileEntry entry;
        std::string ignored;

        if (readMftEntryFull(device, info, recordNumber, entry, ignored)) {
            const bool eligible = !entry.inUse &&
                                  entry.baseRecordReference == 0 &&
                                  (options.includeDirectories || !entry.directory) &&
                                  entry.hasData &&
                                  !entry.name.empty() &&
                                  entry.name.front() != '$';

            if (eligible) {
                result.deletedFound += 1;

                bool anyAllocated = false;
                const uint64_t freeClusters = clustersStillFreeFor(bitmap, entry, anyAllocated);

                RecoveredFile recovered;
                recovered.recordNumber = recordNumber;
                recovered.originalName = entry.name;
                recovered.size = entry.logicalSize;
                recovered.resident = entry.residentData;
                recovered.clustersStillFree = entry.residentData || freeClusters > 0;
                recovered.overwriteRisk = !entry.residentData && anyAllocated;
                recovered.created = fileTimeToString(entry.standard.created);
                recovered.modified = fileTimeToString(entry.standard.modified);

                if (recovered.overwriteRisk) {
                    result.atRisk += 1;
                }

                ++fileIndex;
                char prefix[32];
                std::snprintf(prefix, sizeof(prefix), "%06llu", static_cast<unsigned long long>(fileIndex));

                std::string baseName = sanitizeFileName(entry.name);
                if (isReservedDeviceName(baseName)) {
                    baseName = "_" + baseName;
                }
                recovered.outputName = std::string(prefix) + "_" + baseName;

                const std::wstring outputPath = outputRoot + L"\\" + utf8ToWide(recovered.outputName);

                uint64_t written = 0;
                if (writeEntryData(device, info, entry, outputPath, written, error) && written > 0) {
                    recovered.size = written;
                    result.filesWritten += 1;
                    result.bytesWritten += written;
                    index.push_back(recovered);
                }
            }
        }

        processed += 1;
        result.recordsScanned = processed;
        state.bytesScanned = processed;
        state.filesRecovered = result.filesWritten;
        state.bytesRecovered = result.bytesWritten;
        state.currentOutput = index.empty() ? std::string() : index.back().outputName;

        if (progress && !progress(state)) {
            result.cancelled = true;
            break;
        }
    }

    writeIndex(outputRoot + L"\\recovered.csv", index);

    return result;
}

}
