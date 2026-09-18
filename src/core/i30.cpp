#include "core/i30.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace carver {

namespace {

constexpr uint32_t ATTRIBUTE_INDEX_ROOT = 0x90u;
constexpr uint32_t ATTRIBUTE_INDEX_ALLOCATION = 0xA0u;
constexpr uint32_t ATTRIBUTE_END = 0xFFFFFFFFu;

constexpr size_t FILE_NAME_KEY_HEADER = 0x42;
constexpr size_t INDX_HEADER_SIZE = 0x18;
constexpr size_t ROOT_HEADER_OFFSET = 0x10;

uint16_t readU16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(data[1] << 8);
}

uint32_t readU32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t readU64(const uint8_t* data) {
    uint64_t value = 0;
    for (int index = 7; index >= 0; --index) {
        value = (value << 8) | data[index];
    }
    return value;
}

bool parseFileNameKey(const uint8_t* key, size_t keyLength, IndexName& out) {
    if (keyLength < FILE_NAME_KEY_HEADER) {
        return false;
    }

    const uint8_t nameLength = key[0x40];
    const uint8_t nameSpace = key[0x41];
    if (nameLength == 0 || nameSpace > 3) {
        return false;
    }

    const size_t needed = FILE_NAME_KEY_HEADER + static_cast<size_t>(nameLength) * 2;
    if (needed > keyLength) {
        return false;
    }

    for (size_t index = 0; index < nameLength; ++index) {
        const uint16_t character = static_cast<uint16_t>(key[0x42 + index * 2]) |
                                   static_cast<uint16_t>(key[0x43 + index * 2] << 8);
        if (character < 0x20) {
            return false;
        }
    }

    out.name = utf16leToUtf8(key + 0x42, nameLength);
    out.nameNamespace = nameSpace;
    out.parentRecord = readU64(key) & 0x0000FFFFFFFFFFFFull;
    out.size = readU64(key + 0x30);
    out.times.created = readU64(key + 0x08);
    out.times.modified = readU64(key + 0x10);
    out.times.mftModified = readU64(key + 0x18);
    out.times.accessed = readU64(key + 0x20);
    return true;
}

void parseNodeEntries(const std::vector<uint8_t>& node, size_t headerOffset,
                      std::vector<IndexName>& out) {
    if (headerOffset + 16 > node.size()) {
        return;
    }

    const uint8_t* header = node.data() + headerOffset;
    const uint32_t entriesOffset = readU32(header + 0x00);
    const uint32_t entriesSize = readU32(header + 0x04);

    if (entriesOffset < 16) {
        return;
    }

    const size_t base = headerOffset + entriesOffset;
    const size_t limit = std::min<size_t>(base + entriesSize, node.size());
    size_t position = base;

    while (position + 0x10 <= limit) {
        const uint8_t* entry = node.data() + position;
        const uint16_t entryLength = readU16(entry + 0x08);
        const uint16_t keyLength = readU16(entry + 0x0A);
        const uint32_t flags = readU32(entry + 0x0C);

        if (entryLength < 0x10) {
            break;
        }
        if ((flags & 0x02) != 0) {
            break;
        }

        if (keyLength >= FILE_NAME_KEY_HEADER && position + 0x10 + keyLength <= node.size()) {
            IndexName item;
            if (parseFileNameKey(entry + 0x10, keyLength, item)) {
                item.mftRecord = readU64(entry) & 0x0000FFFFFFFFFFFFull;
                item.sequence = readU16(entry + 6);
                out.push_back(std::move(item));
            }
        }

        position += entryLength;
    }
}

void scanSlack(const std::vector<uint8_t>& node, size_t headerOffset, std::vector<IndexName>& out) {
    if (headerOffset + 16 > node.size()) {
        return;
    }

    const uint8_t* header = node.data() + headerOffset;
    const uint32_t entriesOffset = readU32(header + 0x00);
    const uint32_t entriesSize = readU32(header + 0x04);

    const size_t used = headerOffset + entriesOffset + entriesSize;
    if (used >= node.size()) {
        return;
    }

    const size_t limit = node.size() - 8;

    for (size_t position = used; position + 0x10 <= limit; position += 8) {
        const uint8_t* entry = node.data() + position;
        const uint16_t entryLength = readU16(entry + 0x08);
        const uint16_t keyLength = readU16(entry + 0x0A);
        const uint32_t flags = readU32(entry + 0x0C);

        if ((flags & 0x02) != 0) {
            continue;
        }
        if (keyLength < FILE_NAME_KEY_HEADER || entryLength < 0x10) {
            continue;
        }
        if (position + 0x10 + keyLength > node.size()) {
            continue;
        }

        IndexName item;
        if (!parseFileNameKey(entry + 0x10, keyLength, item)) {
            continue;
        }

        item.mftRecord = readU64(entry) & 0x0000FFFFFFFFFFFFull;
        item.sequence = readU16(entry + 6);
        item.fromSlack = true;
        out.push_back(std::move(item));
    }
}

bool readWholeAttribute(RawDevice& device, const NtfsVolumeInfo& info, uint64_t recordNumber,
                        uint32_t type, std::vector<uint8_t>& data) {
    std::string ignored;
    return readAttributeData(device, info, recordNumber, type, "$I30", data, ignored);
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

void writeCsv(const std::string& path, const std::vector<IndexName>& names) {
    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "wb") != 0 || file == nullptr) {
        return;
    }

    std::fprintf(file, "path,name,size,mft_record,in_use,source,created,modified\n");
    for (const auto& item : names) {
        std::fprintf(file, "%s,%s,%llu,%llu,%s,%s,%s,%s\n",
                     csvField(item.path).c_str(),
                     csvField(item.name).c_str(),
                     static_cast<unsigned long long>(item.size),
                     static_cast<unsigned long long>(item.mftRecord),
                     item.fromSlack ? "no" : "yes",
                     item.fromSlack ? "index slack" : "index",
                     csvField(fileTimeToString(item.times.created)).c_str(),
                     csvField(fileTimeToString(item.times.modified)).c_str());
    }

    std::fclose(file);
}

}

I30Result recoverIndexNames(RawDevice& device,
                            const NtfsVolumeInfo& info,
                            uint64_t recordCount,
                            const std::string& outputCsv,
                            const ProgressFn& progress,
                            std::vector<IndexName>& names,
                            std::string& error) {
    I30Result result;
    names.clear();
    error.clear();

    struct DirectoryInfo {
        std::string name;
        uint64_t parent = 5;
    };

    std::unordered_map<uint64_t, DirectoryInfo> directories;
    std::vector<uint64_t> directoryRecords;

    Progress state;
    state.bytesTotal = recordCount;

    for (uint64_t recordNumber = 0; recordNumber < recordCount; ++recordNumber) {
        MftFileEntry entry;
        std::string ignored;
        if (!readMftEntryFull(device, info, recordNumber, entry, ignored)) {
            continue;
        }

        result.recordsScanned += 1;

        if (entry.directory && !entry.name.empty() && entry.name.front() != '$') {
            directories[recordNumber] = DirectoryInfo{entry.name, entry.parentRecord};
            directoryRecords.push_back(recordNumber);
        }

        state.bytesScanned = recordNumber + 1;
        if (progress && !progress(state)) {
            result.cancelled = true;
            return result;
        }
    }

    for (uint64_t recordNumber : directoryRecords) {
        std::string path;
        {
            uint64_t current = recordNumber;
            int guard = 0;
            while (current != 5 && guard++ < 64) {
                const auto found = directories.find(current);
                if (found == directories.end()) {
                    path = "<unknown>/" + path;
                    break;
                }
                path = found->second.name + "/" + path;
                current = found->second.parent;
            }
        }
        if (!path.empty() && path.back() != '/') {
            path += "/";
        }

        std::vector<IndexName> found;

        std::vector<uint8_t> root;
        if (readWholeAttribute(device, info, recordNumber, ATTRIBUTE_INDEX_ROOT, root)) {
            parseNodeEntries(root, ROOT_HEADER_OFFSET, found);
            scanSlack(root, ROOT_HEADER_OFFSET, found);
        }

        std::vector<uint8_t> allocation;
        if (readWholeAttribute(device, info, recordNumber, ATTRIBUTE_INDEX_ALLOCATION, allocation) &&
            allocation.size() >= INDX_HEADER_SIZE) {
            const size_t blockSize = info.indexBufferSize != 0 ? info.indexBufferSize : 4096;

            for (size_t offset = 0; offset + INDX_HEADER_SIZE <= allocation.size(); offset += blockSize) {
                std::vector<uint8_t> block(allocation.begin() + offset,
                                           allocation.begin() + std::min(offset + blockSize, allocation.size()));
                if (block.size() < INDX_HEADER_SIZE || std::memcmp(block.data(), "INDX", 4) != 0) {
                    continue;
                }

                applyUpdateSequence(block, info.bytesPerSector);

                parseNodeEntries(block, INDX_HEADER_SIZE, found);
                scanSlack(block, INDX_HEADER_SIZE, found);
            }
        }

        for (auto& item : found) {
            item.path = path + item.name;
            if (item.fromSlack) {
                result.slackEntries += 1;
            }
            names.push_back(std::move(item));
        }

        result.directoriesScanned += 1;
    }

    result.liveEntries = names.size() - result.slackEntries;

    if (result.recordsScanned == 0) {
        error = "no readable MFT records were found, so there are no directory indexes to walk";
        return result;
    }

    std::sort(names.begin(), names.end(), [](const IndexName& left, const IndexName& right) {
        if (left.path != right.path) {
            return left.path < right.path;
        }
        return left.mftRecord < right.mftRecord;
    });

    writeCsv(outputCsv, names);
    return result;
}

}
