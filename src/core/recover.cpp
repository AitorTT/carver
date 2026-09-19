#include "core/recover.h"

#include "core/lznt1.h"
#include "core/recover_common.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace carver {

namespace {

bool isClusterAllocated(const std::vector<uint8_t>& bitmap, uint64_t cluster) {
    const size_t index = static_cast<size_t>(cluster / 8);
    if (index >= bitmap.size()) {
        return true;
    }
    return ((bitmap[index] >> (cluster % 8)) & 1u) != 0;
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
    } else if (entry.compressed && entry.compressionUnitClusters > 0 && info.bytesPerCluster > 0 &&
               !entry.runs.empty() &&
               entry.logicalSize <= (1ull << 30)) {
        const uint64_t clusterCount = (entry.logicalSize + info.bytesPerCluster - 1) / info.bytesPerCluster;
        const uint64_t unitSize = static_cast<uint64_t>(entry.compressionUnitClusters) * info.bytesPerCluster;

        std::vector<int64_t> clusterMap(static_cast<size_t>(clusterCount), -1);
        uint64_t vcn = 0;
        for (const auto& run : entry.runs) {
            for (uint64_t index = 0; index < run.length && vcn < clusterCount; ++index, ++vcn) {
                clusterMap[static_cast<size_t>(vcn)] =
                    run.sparse ? -1 : static_cast<int64_t>(run.lcn + index);
            }
            if (vcn >= clusterCount) {
                break;
            }
        }

        std::vector<uint8_t> onDisk(static_cast<size_t>(clusterCount * info.bytesPerCluster), 0);
        for (uint64_t cluster = 0; cluster < clusterCount; ++cluster) {
            const int64_t lcn = clusterMap[static_cast<size_t>(cluster)];
            if (lcn < 0) {
                continue;
            }
            uint32_t got = 0;
            if (!device.readAt(info.clusterOffset(static_cast<uint64_t>(lcn)),
                               onDisk.data() + cluster * info.bytesPerCluster,
                               info.bytesPerCluster, got, error) ||
                got != info.bytesPerCluster) {
                ok = false;
                break;
            }
        }

        std::vector<uint8_t> plain;
        if (ok) {
            const uint64_t unitCount = (entry.logicalSize + unitSize - 1) / unitSize;
            std::vector<bool> unitCompressed(static_cast<size_t>(unitCount), false);
            for (uint64_t unit = 0; unit < unitCount; ++unit) {
                const uint64_t first = unit * entry.compressionUnitClusters;
                const uint64_t last = std::min<uint64_t>(
                    first + entry.compressionUnitClusters, clusterCount);
                uint64_t allocated = 0;
                for (uint64_t cluster = first; cluster < last; ++cluster) {
                    if (clusterMap[static_cast<size_t>(cluster)] >= 0) {
                        ++allocated;
                    }
                }
                unitCompressed[static_cast<size_t>(unit)] = allocated < (last - first);
            }

            if (!lznt1DecompressUnits(onDisk, unitCompressed, unitSize,
                                      entry.logicalSize, plain, error)) {
                ok = false;
            }
        }

        if (ok) {
            uint64_t offset = 0;
            while (offset < plain.size()) {
                const DWORD chunk =
                    static_cast<DWORD>(std::min<uint64_t>(plain.size() - offset, 1u << 20));
                DWORD count = 0;
                if (!WriteFile(output, plain.data() + offset, chunk, &count, nullptr) || count == 0) {
                    ok = false;
                    break;
                }
                written += count;
                offset += count;
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
    if (!ensureDirectoryTree(outputRoot)) {
        error = "cannot create output directory " + outputDirectory;
        return result;
    }

    Progress state;    state.bytesTotal = recordCount;

    uint64_t fileIndex = 0;
    uint64_t processed = 0;

    for (uint64_t recordNumber = options.firstRecord; recordNumber < recordCount; ++recordNumber) {
        MftFileEntry entry;
        std::string ignored;

        if (readMftEntryFull(device, info, recordNumber, entry, ignored)) {
            const std::string extension = nameExtension(entry.name);
            const bool eligible = !entry.inUse &&
                                  entry.baseRecordReference == 0 &&
                                  (options.includeDirectories || !entry.directory) &&
                                  entry.hasData &&
                                  !entry.name.empty() &&
                                  entry.name.front() != '$' &&
                                  extensionSelected(options.onlyExtensions, extension) &&
                                  !extensionSkipped(options.skipExtensions, extension);

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
                bool haveEntry = false;
                if (options.listOnly) {
                    // Nothing is written, so the size has to come from the record.
                    // For a compressed attribute this is the decompressed length,
                    // which is what would be produced on disk.
                    written = entry.logicalSize;
                    haveEntry = written > 0;
                } else {
                    haveEntry = writeEntryData(device, info, entry, outputPath, written, error) && written > 0;
                }

                if (haveEntry) {
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
        state.currentSize = index.empty() ? 0 : index.back().size;

        if (progress && !progress(state)) {
            result.cancelled = true;
            break;
        }
    }

    writeRecoveredIndex(outputRoot + L"\\recovered.csv", index);

    return result;
}

}
