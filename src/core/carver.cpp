#include "core/carver.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
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

bool ensureDirectory(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr)) {
        return true;
    }
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

uint64_t carveOne(RawDevice& device,
                  uint64_t startOffset,
                  uint64_t regionEnd,
                  const Signature& signature,
                  uint64_t declaredSize,
                  const std::wstring& outputPath,
                  uint64_t& bytesWritten) {
    bytesWritten = 0;

    HANDLE output = CreateFileW(outputPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        return startOffset + 1;
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

    while (position < limit) {
        const uint32_t want = static_cast<uint32_t>(std::min<uint64_t>(buffer.size(), limit - position));
        uint32_t got = 0;
        std::string ignored;
        if (!device.readAt(position, buffer.data(), want, got, ignored) || got == 0) {
            endOffset = position;
            finished = true;
            break;
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
        if (!WriteFile(output, buffer.data(), writeLength, &written, nullptr)) {
            break;
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

    CloseHandle(output);

    if (bytesWritten == 0) {
        DeleteFileW(outputPath.c_str());
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

    const uint64_t deviceSize = device.size();
    const uint64_t start = std::min(options.startOffset, deviceSize);
    const uint64_t end = (options.endOffset == 0 || options.endOffset > deviceSize)
                             ? deviceSize
                             : options.endOffset;

    if (start >= end) {
        error = "scan range is empty";
        return result;
    }

    const std::wstring outputRoot = utf8ToWide(outputDirectory);
    if (!ensureDirectory(outputRoot)) {
        error = "cannot create output directory " + outputDirectory;
        return result;
    }

    const uint64_t chunkSize = std::max<uint64_t>(options.chunkSize, 64 * 1024);
    const size_t overlap = std::max<size_t>(
        std::max(maxHeaderLength(signatures), maxLookahead(signatures)), 1);

    std::vector<uint8_t> buffer(static_cast<size_t>(chunkSize));

    Progress state;
    state.bytesTotal = end - start;

    uint64_t position = start;
    uint64_t fileIndex = 0;

    while (position < end) {
        const uint32_t want = static_cast<uint32_t>(std::min<uint64_t>(buffer.size(), end - position));
        uint32_t got = 0;
        if (!device.readAt(position, buffer.data(), want, got, error)) {
            return result;
        }
        if (got == 0) {
            break;
        }

        const Signature* found = nullptr;
        size_t foundIndex = 0;

        for (const auto& signature : signatures) {
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
                const uint64_t endOffset = carveOne(device, carveStart, end, *found, declared, outputPath, written);

                if (written > 0) {
                    result.filesRecovered += 1;
                    result.bytesRecovered += written;
                    state.currentOutput = baseName;
                } else {
                    state.currentOutput.clear();
                }

                state.currentType = found->name;
                state.filesRecovered = result.filesRecovered;
                state.bytesRecovered = result.bytesRecovered;

                const uint64_t next = std::max<uint64_t>(endOffset, carveStart + 1);
                position = next > position ? next : position + 1;
            }
        } else {
            position += (got > overlap) ? got - overlap : got;
            if (got < want) {
                break;
            }
        }

        result.bytesScanned = position - start;
        state.bytesScanned = result.bytesScanned;

        if (progress && !progress(state)) {
            result.cancelled = true;
            return result;
        }
    }

    if (progress) {
        state.bytesScanned = result.bytesScanned;
        progress(state);
    }

    return result;
}

}
