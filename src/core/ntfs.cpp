#include "core/ntfs.h"

#include <algorithm>
#include <cstring>

namespace carver {

namespace {

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
        const size_t sectorEnd = static_cast<size_t>(index) * bytesPerSector;
        const size_t replacement = static_cast<size_t>(fixupOffset) + static_cast<size_t>(index) * 2;
        if (sectorEnd + 2 > record.size() || replacement + 2 > record.size()) {
            return;
        }
        record[sectorEnd] = record[replacement];
        record[sectorEnd + 1] = record[replacement + 1];
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

bool readFileRecordData(RawDevice& device, const NtfsVolumeInfo& info, uint64_t recordNumber,
                        std::vector<uint8_t>& data, std::string& error) {
    data.clear();

    std::vector<uint8_t> record;
    if (!readMftRecord(device, info, recordNumber, record, error)) {
        return false;
    }

    const size_t firstAttribute = readU16(record.data() + 0x14);
    size_t position = firstAttribute;

    while (position + 8 <= record.size()) {
        const uint32_t type = readU32(record.data() + position);
        if (type == ATTRIBUTE_END) {
            break;
        }

        const uint32_t attributeLength = readU32(record.data() + position + 4);
        if (attributeLength < 16 || position + attributeLength > record.size()) {
            error = "attribute length is invalid";
            return false;
        }

        const uint8_t nonResident = record[position + 0x08];
        const uint8_t nameLength = record[position + 0x09];

        if (type == ATTRIBUTE_DATA && nameLength == 0) {
            if (nonResident == 0) {
                const uint32_t contentSize = readU32(record.data() + position + 0x10);
                const uint16_t contentOffset = readU16(record.data() + position + 0x14);
                const size_t start = position + contentOffset;
                if (start + contentSize > record.size()) {
                    error = "resident data attribute is out of bounds";
                    return false;
                }
                data.assign(record.begin() + start, record.begin() + start + contentSize);
                return true;
            }

            const uint16_t runListOffset = readU16(record.data() + position + 0x20);
            const uint64_t realSize = readU64(record.data() + position + 0x30);
            const size_t runListStart = position + runListOffset;
            if (runListStart >= position + attributeLength) {
                error = "runlist offset is out of bounds";
                return false;
            }

            std::vector<DataRun> runs;
            if (!decodeRunList(record.data() + runListStart, position + attributeLength - runListStart, runs, error)) {
                return false;
            }

            return readExtents(device, info, runs, realSize, data, error);
        }

        position += attributeLength;
    }

    error = "record " + std::to_string(recordNumber) + " has no unnamed $DATA attribute";
    return false;
}

bool readBitmap(RawDevice& device, const NtfsVolumeInfo& info, std::vector<uint8_t>& bitmap, std::string& error) {
    return readFileRecordData(device, info, 6, bitmap, error);
}

std::vector<ByteRange> freeClusterRanges(const std::vector<uint8_t>& bitmap, uint64_t totalClusters,
                                         uint64_t bytesPerCluster) {
    std::vector<ByteRange> ranges;
    if (bytesPerCluster == 0) {
        return ranges;
    }

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
            ranges.push_back(ByteRange{rangeStart * bytesPerCluster, cluster * bytesPerCluster});
            inFreeRange = false;
        }
    }

    if (inFreeRange) {
        ranges.push_back(ByteRange{rangeStart * bytesPerCluster, totalClusters * bytesPerCluster});
    }

    return ranges;
}

}
