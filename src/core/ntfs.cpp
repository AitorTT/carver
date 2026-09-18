#include "core/ntfs.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace carver {

namespace {

constexpr uint32_t ATTRIBUTE_STANDARD_INFORMATION = 0x10u;
constexpr uint32_t ATTRIBUTE_ATTRIBUTE_LIST = 0x20u;
constexpr uint32_t ATTRIBUTE_FILE_NAME = 0x30u;
constexpr uint32_t ATTRIBUTE_END = 0xFFFFFFFFu;
constexpr uint32_t ATTRIBUTE_DATA = 0x80u;
constexpr uint64_t MAX_BITMAP_BYTES = 256ull * 1024ull * 1024ull;

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

uint32_t decodeRecordSizeField(int8_t field, uint32_t bytesPerCluster, uint32_t bytesPerSector) {
    if (field > 0) {
        return static_cast<uint32_t>(field) * bytesPerCluster;
    }
    if (field < 0) {
        const int shift = -field;
        if (shift > 30) {
            return 0;
        }
        return static_cast<uint32_t>(1u) << shift;
    }
    return bytesPerSector;
}

void applyFixups(std::vector<uint8_t>& record, uint32_t bytesPerSector) {
    if (record.size() < 8) {
        return;
    }

    const uint16_t fixupOffset = readU16(record.data() + 0x04);
    const uint16_t fixupCount = readU16(record.data() + 0x06);
    if (fixupOffset == 0 || fixupCount < 2) {
        return;
    }
    if (static_cast<size_t>(fixupOffset) + static_cast<size_t>(fixupCount) * 2 > record.size()) {
        return;
    }

    for (uint16_t index = 1; index < fixupCount; ++index) {
        const size_t sectorTail = static_cast<size_t>(index) * bytesPerSector - 2;
        const size_t replacement = static_cast<size_t>(fixupOffset) + static_cast<size_t>(index) * 2;
        if (sectorTail + 2 > record.size() || replacement + 2 > record.size()) {
            return;
        }
        record[sectorTail] = record[replacement];
        record[sectorTail + 1] = record[replacement + 1];
    }
}

bool readMftRecord(RawDevice& device, const NtfsVolumeInfo& info, uint64_t recordNumber,
                   std::vector<uint8_t>& record, std::string& error) {
    if (info.mftRecordSize == 0) {
        error = "MFT record size is zero";
        return false;
    }

    const uint64_t mftOffset = info.clusterOffset(info.mftCluster);
    const uint64_t recordOffset = mftOffset + recordNumber * info.mftRecordSize;

    record.assign(info.mftRecordSize, 0);

    uint64_t position = recordOffset;
    size_t written = 0;
    while (written < record.size()) {
        const uint32_t want = static_cast<uint32_t>(
            std::min<uint64_t>(record.size() - written, 1u << 20));
        uint32_t got = 0;
        if (!device.readAt(position, record.data() + written, want, got, error) || got == 0) {
            error = "cannot read MFT record " + std::to_string(recordNumber);
            return false;
        }
        written += got;
        position += got;
    }

    if (std::memcmp(record.data(), "FILE", 4) != 0) {
        error = "MFT record " + std::to_string(recordNumber) + " has no FILE signature";
        return false;
    }

    applyFixups(record, info.bytesPerSector);
    return true;
}

bool readExtents(RawDevice& device, const NtfsVolumeInfo& info, const std::vector<DataRun>& runs,
                 uint64_t size, std::vector<uint8_t>& out, std::string& error) {
    out.clear();
    if (size == 0) {
        return true;
    }
    if (size > MAX_BITMAP_BYTES) {
        error = "attribute is too large to buffer (" + std::to_string(size) + " bytes)";
        return false;
    }

    out.reserve(static_cast<size_t>(size));

    uint64_t remaining = size;
    for (const auto& run : runs) {
        if (remaining == 0) {
            break;
        }

        const uint64_t runBytes = run.length * info.bytesPerCluster;
        uint64_t toRead = std::min(runBytes, remaining);

        if (run.sparse) {
            out.insert(out.end(), static_cast<size_t>(toRead), 0);
        } else {
            uint64_t offset = info.clusterOffset(run.lcn);
            while (toRead > 0) {
                const uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(toRead, 1u << 20));
                const size_t base = out.size();
                out.resize(base + chunk);
                uint32_t got = 0;
                if (!device.readAt(offset, out.data() + base, chunk, got, error) || got == 0) {
                    error = "cannot read run data at offset " + std::to_string(offset);
                    return false;
                }
                out.resize(base + got);
                offset += got;
                toRead -= got;
                if (got < chunk) {
                    break;
                }
            }
            if (toRead > 0) {
                out.insert(out.end(), static_cast<size_t>(toRead), 0);
            }
        }

        remaining -= std::min(runBytes, remaining);
    }

    if (out.size() > size) {
        out.resize(static_cast<size_t>(size));
    }
    return true;
}

void applyStandardInformation(const std::vector<uint8_t>& record, size_t position, MftFileEntry& entry) {
    const uint32_t contentSize = readU32(record.data() + position + 0x10);
    const uint16_t contentOffset = readU16(record.data() + position + 0x14);
    const size_t start = position + contentOffset;
    if (contentSize < 0x20 || start + 0x20 > record.size()) {
        return;
    }

    entry.standard.created = readU64(record.data() + start + 0x00);
    entry.standard.modified = readU64(record.data() + start + 0x08);
    entry.standard.mftModified = readU64(record.data() + start + 0x10);
    entry.standard.accessed = readU64(record.data() + start + 0x18);
}

void applyFileNameAttribute(const std::vector<uint8_t>& record, size_t position, MftFileEntry& entry) {
    const uint32_t contentSize = readU32(record.data() + position + 0x10);
    const uint16_t contentOffset = readU16(record.data() + position + 0x14);
    const size_t start = position + contentOffset;
    if (contentSize < 0x42 || start + 0x42 > record.size()) {
        return;
    }

    const uint8_t characterCount = record[start + 0x40];
    const uint8_t nameSpace = record[start + 0x41];
    if (characterCount == 0 || start + 0x42 + static_cast<size_t>(characterCount) * 2 > record.size()) {
        return;
    }

    const bool haveName = !entry.name.empty();
    const bool currentIsDos = haveName && entry.nameNamespace == 2;
    const bool candidateIsDos = nameSpace == 2;
    if (!haveName || (currentIsDos && !candidateIsDos)) {
        entry.name = utf16leToUtf8(record.data() + start + 0x42, characterCount);
        entry.nameNamespace = nameSpace;
        entry.parentRecord = readU64(record.data() + start + 0x00) & 0x0000FFFFFFFFFFFFull;
        entry.fileName.created = readU64(record.data() + start + 0x08);
        entry.fileName.modified = readU64(record.data() + start + 0x10);
        entry.fileName.mftModified = readU64(record.data() + start + 0x18);
        entry.fileName.accessed = readU64(record.data() + start + 0x20);
        entry.allocatedSize = readU64(record.data() + start + 0x28);
        entry.logicalSize = readU64(record.data() + start + 0x30);
    }
}

void applyDataAttribute(const std::vector<uint8_t>& record, size_t position, uint32_t attributeLength,
                        MftFileEntry& entry) {
    entry.hasData = true;

    const uint8_t nonResident = record[position + 0x08];
    if (nonResident == 0) {
        entry.residentData = true;
        const uint32_t contentSize = readU32(record.data() + position + 0x10);
        const uint16_t contentOffset = readU16(record.data() + position + 0x14);
        const size_t start = position + contentOffset;
        if (start + contentSize <= record.size()) {
            entry.residentContent.assign(record.begin() + start, record.begin() + start + contentSize);
            entry.logicalSize = contentSize;
        }
        return;
    }

    DataExtent extent;
    extent.startingVcn = readU64(record.data() + position + 0x10);
    extent.lastVcn = readU64(record.data() + position + 0x18);
    extent.realSize = readU64(record.data() + position + 0x30);

    const uint16_t runListOffset = readU16(record.data() + position + 0x20);
    const size_t runListStart = position + runListOffset;
    if (runListStart < position + attributeLength) {
        std::string ignored;
        decodeRunList(record.data() + runListStart, position + attributeLength - runListStart,
                      extent.runs, ignored);
    }

    entry.dataExtents.push_back(std::move(extent));
}

void parseAttributeListContent(const std::vector<uint8_t>& record, size_t position, MftFileEntry& entry) {
    const uint32_t contentSize = readU32(record.data() + position + 0x10);
    const uint16_t contentOffset = readU16(record.data() + position + 0x14);
    const size_t start = position + contentOffset;
    if (start + contentSize > record.size()) {
        return;
    }

    const size_t end = start + contentSize;
    size_t cursor = start;
    while (cursor + 0x1A <= end) {
        const uint32_t type = readU32(record.data() + cursor);
        if (type == ATTRIBUTE_END) {
            break;
        }

        const uint16_t entryLength = readU16(record.data() + cursor + 0x04);
        if (entryLength < 0x1A || cursor + entryLength > end) {
            break;
        }

        AttributeListEntry item;
        item.type = type;
        item.startingVcn = readU64(record.data() + cursor + 0x08);
        item.baseRecord = readU64(record.data() + cursor + 0x10) & 0x0000FFFFFFFFFFFFull;
        item.attributeId = readU16(record.data() + cursor + 0x18);
        entry.attributeList.push_back(item);

        cursor += entryLength;
    }
}

void buildDataRuns(MftFileEntry& entry) {
    entry.runs.clear();
    if (entry.dataExtents.empty()) {
        return;
    }

    std::stable_sort(entry.dataExtents.begin(), entry.dataExtents.end(),
                     [](const DataExtent& left, const DataExtent& right) {
                         return left.startingVcn < right.startingVcn;
                     });

    uint64_t realSize = 0;
    for (const auto& extent : entry.dataExtents) {
        entry.runs.insert(entry.runs.end(), extent.runs.begin(), extent.runs.end());
        if (extent.startingVcn == 0 && extent.realSize > 0) {
            realSize = extent.realSize;
        } else if (extent.realSize > realSize) {
            realSize = extent.realSize;
        }
    }

    if (realSize > 0) {
        entry.logicalSize = realSize;
    }
}

bool parseMftEntryAttributes(const std::vector<uint8_t>& record, MftFileEntry& entry) {
    if (record.size() < 0x30 || std::memcmp(record.data(), "FILE", 4) != 0) {
        return false;
    }

    entry.sequence = readU16(record.data() + 0x10);
    entry.baseRecordReference = readU64(record.data() + 0x20) & 0x0000FFFFFFFFFFFFull;
    const uint16_t flags = readU16(record.data() + 0x16);
    entry.inUse = (flags & 0x0001) != 0;
    entry.directory = (flags & 0x0002) != 0;

    size_t position = readU16(record.data() + 0x14);

    while (position + 8 <= record.size()) {
        const uint32_t type = readU32(record.data() + position);
        if (type == ATTRIBUTE_END) {
            break;
        }

        const uint32_t attributeLength = readU32(record.data() + position + 4);
        if (attributeLength < 16 || position + attributeLength > record.size()) {
            return false;
        }

        const uint8_t nonResident = record[position + 0x08];
        const uint8_t nameLength = record[position + 0x09];

        if (type == ATTRIBUTE_STANDARD_INFORMATION && nonResident == 0 && nameLength == 0) {
            applyStandardInformation(record, position, entry);
        } else if (type == ATTRIBUTE_ATTRIBUTE_LIST) {
            entry.hasAttributeList = true;
            if (nonResident == 0) {
                parseAttributeListContent(record, position, entry);
            }
        } else if (type == ATTRIBUTE_FILE_NAME && nonResident == 0) {
            applyFileNameAttribute(record, position, entry);
        } else if (type == ATTRIBUTE_DATA && nameLength == 0) {
            applyDataAttribute(record, position, attributeLength, entry);
        }

        position += attributeLength;
    }

    buildDataRuns(entry);
    return true;
}

}

bool parseNtfsBootSector(const uint8_t* sector, size_t length, NtfsVolumeInfo& info, std::string& error) {
    if (length < 512) {
        error = "boot sector is shorter than 512 bytes";
        return false;
    }

    if (std::memcmp(sector + 0x03, "NTFS    ", 8) != 0) {
        error = "OEM identifier is not NTFS";
        return false;
    }

    if (sector[0x1FE] != 0x55 || sector[0x1FF] != 0xAA) {
        error = "boot sector signature 0x55AA is missing";
        return false;
    }

    info.bytesPerSector = readU16(sector + 0x0B);
    const uint8_t sectorsPerCluster = sector[0x0D];
    info.totalSectors = readU64(sector + 0x28);
    info.mftCluster = readU64(sector + 0x30);
    info.mftMirrorCluster = readU64(sector + 0x38);

    if (info.bytesPerSector < 256 || info.bytesPerSector > 4096 || (info.bytesPerSector % 256) != 0) {
        error = "implausible bytes-per-sector value";
        return false;
    }
    if (sectorsPerCluster == 0 || sectorsPerCluster > 128) {
        error = "implausible sectors-per-cluster value";
        return false;
    }

    info.bytesPerCluster = static_cast<uint32_t>(info.bytesPerSector) * sectorsPerCluster;
    info.mftRecordSize = decodeRecordSizeField(static_cast<int8_t>(sector[0x40]), info.bytesPerCluster, info.bytesPerSector);
    info.indexBufferSize = decodeRecordSizeField(static_cast<int8_t>(sector[0x41]), info.bytesPerCluster, info.bytesPerSector);

    if (info.mftRecordSize == 0) {
        error = "implausible MFT record size";
        return false;
    }
    if (info.totalSectors == 0) {
        error = "volume reports zero total sectors";
        return false;
    }

    return true;
}

void applyUpdateSequence(std::vector<uint8_t>& buffer, uint32_t bytesPerSector) {
    applyFixups(buffer, bytesPerSector);
}

bool decodeRunList(const uint8_t* data, size_t length, std::vector<DataRun>& runs, std::string& error) {
    runs.clear();

    size_t position = 0;
    int64_t currentLcn = 0;

    while (position < length) {
        const uint8_t header = data[position++];
        if (header == 0x00) {
            return true;
        }

        const int lengthSize = header & 0x0F;
        const int offsetSize = (header >> 4) & 0x0F;

        if (lengthSize == 0) {
            error = "runlist has a zero-length size field";
            return false;
        }
        if (position + static_cast<size_t>(lengthSize) + static_cast<size_t>(offsetSize) > length) {
            error = "runlist is truncated";
            return false;
        }

        uint64_t runLength = 0;
        for (int index = 0; index < lengthSize; ++index) {
            runLength |= static_cast<uint64_t>(data[position + index]) << (8 * index);
        }
        position += static_cast<size_t>(lengthSize);

        DataRun run;
        run.length = runLength;

        if (offsetSize == 0) {
            run.sparse = true;
            run.lcn = 0;
        } else {
            uint64_t raw = 0;
            for (int index = 0; index < offsetSize; ++index) {
                raw |= static_cast<uint64_t>(data[position + index]) << (8 * index);
            }
            if (offsetSize < 8 && (data[position + offsetSize - 1] & 0x80) != 0) {
                raw |= ~0ull << (8 * offsetSize);
            }
            position += static_cast<size_t>(offsetSize);

            currentLcn += static_cast<int64_t>(raw);
            if (currentLcn < 0) {
                error = "runlist produced a negative cluster number";
                return false;
            }
            run.lcn = static_cast<uint64_t>(currentLcn);
        }

        runs.push_back(run);
    }

    return true;
}

std::string utf16leToUtf8(const uint8_t* data, size_t charCount) {
    if (charCount == 0) {
        return {};
    }

    std::wstring wide;
    wide.reserve(charCount);
    for (size_t index = 0; index < charCount; ++index) {
        wide.push_back(static_cast<wchar_t>(data[index * 2] | (static_cast<uint16_t>(data[index * 2 + 1]) << 8)));
    }

    return wideToUtf8(wide);
}

std::string fileTimeToString(uint64_t fileTime) {
    constexpr uint64_t TICKS_PER_SECOND = 10000000ull;
    constexpr uint64_t EPOCH_DELTA_SECONDS = 11644473600ull;

    if (fileTime == 0 || fileTime < EPOCH_DELTA_SECONDS * TICKS_PER_SECOND) {
        return {};
    }

    const std::time_t seconds = static_cast<std::time_t>(fileTime / TICKS_PER_SECOND - EPOCH_DELTA_SECONDS);
    const std::tm* utc = std::gmtime(&seconds);
    if (utc == nullptr) {
        return {};
    }

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
                  utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
                  utc->tm_hour, utc->tm_min, utc->tm_sec);
    return buffer;
}

bool readMftEntry(RawDevice& device, const NtfsVolumeInfo& info, uint64_t recordNumber,
                  MftFileEntry& entry, std::string& error) {
    entry = MftFileEntry{};
    entry.recordNumber = recordNumber;

    std::vector<uint8_t> record;
    if (!readMftRecord(device, info, recordNumber, record, error)) {
        return false;
    }

    if (!parseMftEntryAttributes(record, entry)) {
        error = "MFT record " + std::to_string(recordNumber) + " could not be parsed";
        return false;
    }

    return true;
}

bool readMftEntryFull(RawDevice& device, const NtfsVolumeInfo& info, uint64_t recordNumber,
                      MftFileEntry& entry, std::string& error) {
    if (!readMftEntry(device, info, recordNumber, entry, error)) {
        return false;
    }

    if (entry.attributeList.empty()) {
        entry.attributeListFollowed = true;
        return true;
    }

    std::vector<uint8_t> externalRecord;
    std::string ignored;
    uint64_t cachedRecord = ~0ull;

    for (const auto& item : entry.attributeList) {
        if (item.baseRecord == recordNumber) {
            continue;
        }
        if (item.type != ATTRIBUTE_FILE_NAME && item.type != ATTRIBUTE_DATA) {
            continue;
        }

        if (item.baseRecord != cachedRecord) {
            if (!readMftRecord(device, info, item.baseRecord, externalRecord, ignored)) {
                continue;
            }
            cachedRecord = item.baseRecord;
        }

        size_t position = readU16(externalRecord.data() + 0x14);
        while (position + 8 <= externalRecord.size()) {
            const uint32_t type = readU32(externalRecord.data() + position);
            if (type == ATTRIBUTE_END) {
                break;
            }

            const uint32_t attributeLength = readU32(externalRecord.data() + position + 4);
            if (attributeLength < 16 || position + attributeLength > externalRecord.size()) {
                break;
            }

            const uint8_t nonResident = externalRecord[position + 0x08];
            const uint8_t nameLength = externalRecord[position + 0x09];
            const uint16_t attributeId = readU16(externalRecord.data() + position + 0x0E);

            if (type == item.type && attributeId == item.attributeId) {
                if (type == ATTRIBUTE_FILE_NAME && nonResident == 0) {
                    applyFileNameAttribute(externalRecord, position, entry);
                } else if (type == ATTRIBUTE_DATA && nameLength == 0) {
                    applyDataAttribute(externalRecord, position, attributeLength, entry);
                }
                break;
            }

            position += attributeLength;
        }
    }

    entry.attributeListFollowed = true;
    buildDataRuns(entry);
    return true;
}

bool getMftRecordCount(RawDevice& device, const NtfsVolumeInfo& info, uint64_t& count, std::string& error) {
    MftFileEntry mft;
    if (!readMftEntry(device, info, 0, mft, error)) {
        error = "cannot read $MFT record: " + error;
        return false;
    }

    if (!mft.hasData || mft.residentData || mft.logicalSize == 0) {
        error = "$MFT has no usable non-resident $DATA attribute";
        return false;
    }
    if (info.mftRecordSize == 0) {
        error = "MFT record size is zero";
        return false;
    }

    count = mft.logicalSize / info.mftRecordSize;
    return true;
}

bool readAttributeData(RawDevice& device, const NtfsVolumeInfo& info, uint64_t recordNumber,
                       uint32_t attributeType, const std::string& attributeName,
                       std::vector<uint8_t>& data, std::string& error) {
    data.clear();

    std::vector<uint8_t> record;
    if (!readMftRecord(device, info, recordNumber, record, error)) {
        return false;
    }

    size_t position = readU16(record.data() + 0x14);

    while (position + 8 <= record.size()) {
        const uint32_t type = readU32(record.data() + position);
        if (type == ATTRIBUTE_END) {
            break;
        }

        const uint32_t attributeLength = readU32(record.data() + position + 4);
        if (attributeLength < 16 || position + attributeLength > record.size()) {
            break;
        }

        const uint8_t nonResident = record[position + 0x08];
        const uint8_t nameLength = record[position + 0x09];
        const uint16_t nameOffset = readU16(record.data() + position + 0x0A);

        bool nameMatches = attributeName.empty() && nameLength == 0;
        if (!nameMatches && !attributeName.empty() && nameLength > 0) {
            const size_t nameStart = position + nameOffset;
            if (nameStart + static_cast<size_t>(nameLength) * 2 <= record.size()) {
                nameMatches = utf16leToUtf8(record.data() + nameStart, nameLength) == attributeName;
            }
        }

        if (type == attributeType && nameMatches) {
            if (nonResident == 0) {
                const uint32_t contentSize = readU32(record.data() + position + 0x10);
                const uint16_t contentOffset = readU16(record.data() + position + 0x14);
                const size_t start = position + contentOffset;
                if (start + contentSize > record.size()) {
                    error = "attribute content is out of bounds";
                    return false;
                }
                data.assign(record.begin() + start, record.begin() + start + contentSize);
                return true;
            }

            const uint16_t runListOffset = readU16(record.data() + position + 0x20);
            const uint64_t realSize = readU64(record.data() + position + 0x30);
            const size_t runListStart = position + runListOffset;

            std::vector<DataRun> runs;
            if (runListStart < position + attributeLength) {
                if (!decodeRunList(record.data() + runListStart, position + attributeLength - runListStart,
                                   runs, error)) {
                    return false;
                }
            }

            return readExtents(device, info, runs, realSize, data, error);
        }

        position += attributeLength;
    }

    error = "attribute not found in record " + std::to_string(recordNumber);
    return false;
}

bool readFileRecordData(RawDevice& device, const NtfsVolumeInfo& info, uint64_t recordNumber,
                        std::vector<uint8_t>& data, std::string& error) {
    data.clear();

    MftFileEntry entry;
    if (!readMftEntryFull(device, info, recordNumber, entry, error)) {
        return false;
    }

    if (!entry.hasData) {
        error = "record " + std::to_string(recordNumber) + " has no unnamed $DATA attribute";
        return false;
    }

    if (entry.residentData) {
        data = entry.residentContent;
        return true;
    }

    return readExtents(device, info, entry.runs, entry.logicalSize, data, error);
}

bool readBitmap(RawDevice& device, const NtfsVolumeInfo& info, std::vector<uint8_t>& bitmap, std::string& error) {
    return readFileRecordData(device, info, 6, bitmap, error);
}

std::vector<ByteRange> freeClusterRanges(const std::vector<uint8_t>& bitmap, uint64_t totalClusters,
                                         uint64_t bytesPerCluster, uint64_t baseOffset) {
    std::vector<ByteRange> ranges;
    if (bytesPerCluster == 0) {
        return ranges;
    }

    const auto toBytes = [bytesPerCluster, baseOffset](uint64_t cluster) {
        return baseOffset + cluster * bytesPerCluster;
    };

    bool inFreeRange = false;
    uint64_t rangeStart = 0;

    for (uint64_t cluster = 0; cluster < totalClusters; ++cluster) {
        const size_t byteIndex = static_cast<size_t>(cluster / 8);
        const bool allocated = byteIndex >= bitmap.size()
                                   ? true
                                   : ((bitmap[byteIndex] >> (cluster % 8)) & 1u) != 0;

        if (!allocated) {
            if (!inFreeRange) {
                rangeStart = cluster;
                inFreeRange = true;
            }
        } else if (inFreeRange) {
            ranges.push_back(ByteRange{toBytes(rangeStart), toBytes(cluster)});
            inFreeRange = false;
        }
    }

    if (inFreeRange) {
        ranges.push_back(ByteRange{toBytes(rangeStart), toBytes(totalClusters)});
    }

    return ranges;
}

}
