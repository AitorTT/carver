#include "core/partition.h"

#include "core/fat.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace carver {

namespace {

constexpr uint32_t MBR_SIGNATURE = 0xAA55u;
constexpr size_t MBR_ENTRY_OFFSET = 0x1BE;
constexpr size_t MBR_ENTRY_SIZE = 16;
constexpr size_t MBR_ENTRY_COUNT = 4;

constexpr uint8_t TYPE_EXTENDED_MIN = 0x05;
constexpr uint8_t TYPE_GPT_PROTECTIVE = 0xEE;

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

bool isExtendedType(uint8_t type) {
    return type == 0x05 || type == 0x0F || type == 0x85;
}

std::string describeMbrType(uint8_t type) {
    switch (type) {
    case 0x00: return "empty";
    case 0x01: return "FAT12";
    case 0x04: return "FAT16 (<32M)";
    case 0x06: return "FAT16";
    case 0x07: return "NTFS / exFAT";
    case 0x0B: return "FAT32";
    case 0x0C: return "FAT32 (LBA)";
    case 0x0E: return "FAT16 (LBA)";
    case 0x11: return "Hidden FAT12";
    case 0x14: return "Hidden FAT16";
    case 0x1B: return "Hidden FAT32";
    case 0x1C: return "Hidden FAT32 (LBA)";
    case 0x1E: return "Hidden FAT16 (LBA)";
    case 0x27: return "Windows recovery";
    case 0x42: return "Windows dynamic disk";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0x8E: return "Linux LVM";
    case 0xA5: return "FreeBSD";
    case 0xAF: return "HFS / HFS+";
    case 0xEE: return "GPT protective";
    case 0xEF: return "EFI system";
    case 0xFD: return "Linux RAID";
    default: {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "type 0x%02X", type);
        return std::string(buffer);
    }
    }
}

bool mbrTypeIsNtfsCandidate(uint8_t type) {
    return type == 0x07;
}

bool guidMatches(const uint8_t* guid, const uint8_t (&pattern)[16]) {
    return std::memcmp(guid, pattern, 16) == 0;
}

std::string describeGptType(const uint8_t* guid, bool& ntfsCandidate) {
    static const uint8_t BASIC_DATA[16] = {0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
                                           0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};
    static const uint8_t EFI_SYSTEM[16] = {0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
                                           0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B};
    static const uint8_t MS_RESERVED[16] = {0x16, 0xE3, 0xC9, 0xE3, 0x5C, 0x0B, 0xB8, 0x4D,
                                            0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE};
    static const uint8_t APPLE_HFS[16] = {0x00, 0x53, 0x46, 0x48, 0x00, 0x00, 0xAA, 0x11,
                                          0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC};
    static const uint8_t LINUX_FS[16] = {0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
                                         0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4};
    static const uint8_t WINDOWS_RECOVERY[16] = {0xA4, 0xBB, 0x94, 0xDE, 0xD1, 0x06, 0x40, 0x4D,
                                                 0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC};

    if (guidMatches(guid, BASIC_DATA)) {
        ntfsCandidate = true;
        return "Basic data (NTFS / exFAT)";
    }
    if (guidMatches(guid, EFI_SYSTEM)) {
        return "EFI system";
    }
    if (guidMatches(guid, MS_RESERVED)) {
        return "Microsoft reserved";
    }
    if (guidMatches(guid, WINDOWS_RECOVERY)) {
        ntfsCandidate = true;
        return "Windows recovery (NTFS)";
    }
    if (guidMatches(guid, APPLE_HFS)) {
        return "Apple HFS+";
    }
    if (guidMatches(guid, LINUX_FS)) {
        return "Linux filesystem";
    }

    static const uint8_t ZERO[16] = {};
    if (guidMatches(guid, ZERO)) {
        return "unused";
    }
    return "unknown";
}

std::string utf16leNameToString(const uint8_t* data, size_t maxBytes) {
    std::wstring wide;
    for (size_t offset = 0; offset + 1 < maxBytes; offset += 2) {
        const uint16_t unit = static_cast<uint16_t>(data[offset]) |
                              static_cast<uint16_t>(data[offset + 1] << 8);
        if (unit == 0) {
            break;
        }
        wide.push_back(static_cast<wchar_t>(unit));
    }
    return wideToUtf8(wide);
}

bool readAt(RawDevice& device, uint64_t offset, uint8_t* buffer, uint32_t length) {
    uint32_t got = 0;
    std::string ignored;
    return device.readAt(offset, buffer, length, got, ignored) && got == length;
}

std::vector<PartitionInfo> parseMbr(RawDevice& device, uint32_t sectorSize) {
    std::vector<PartitionInfo> partitions;

    std::vector<uint8_t> sector(sectorSize, 0);
    if (!readAt(device, 0, sector.data(), sectorSize)) {
        return partitions;
    }
    if (readU16(sector.data() + 510) != MBR_SIGNATURE) {
        return partitions;
    }

    uint32_t index = 0;

    for (size_t slot = 0; slot < MBR_ENTRY_COUNT; ++slot) {
        const uint8_t* entry = sector.data() + MBR_ENTRY_OFFSET + slot * MBR_ENTRY_SIZE;
        const uint8_t type = entry[4];
        if (type == 0 || type == TYPE_GPT_PROTECTIVE) {
            continue;
        }

        const uint32_t startLba = readU32(entry + 8);
        const uint32_t sectorCount = readU32(entry + 12);
        if (startLba == 0 || sectorCount == 0) {
            continue;
        }

        if (isExtendedType(type)) {
            PartitionInfo container;
            container.index = 0;
            container.offset = static_cast<uint64_t>(startLba) * sectorSize;
            container.size = static_cast<uint64_t>(sectorCount) * sectorSize;
            container.scheme = "MBR";
            container.typeName = describeMbrType(type) + " (container)";
            container.extendedContainer = true;
            partitions.push_back(container);

            const uint64_t extendedBase = static_cast<uint64_t>(startLba) * sectorSize;
            uint64_t ebrOffset = extendedBase;
            int guard = 0;

            while (guard++ < 128) {
                std::vector<uint8_t> ebr(sectorSize, 0);
                if (!readAt(device, ebrOffset, ebr.data(), sectorSize)) {
                    break;
                }
                if (readU16(ebr.data() + 510) != MBR_SIGNATURE) {
                    break;
                }

                const uint8_t* logical = ebr.data() + MBR_ENTRY_OFFSET;
                const uint8_t logicalType = logical[4];
                const uint32_t logicalStart = readU32(logical + 8);
                const uint32_t logicalCount = readU32(logical + 12);

                if (logicalType != 0 && logicalStart != 0 && logicalCount != 0) {
                    PartitionInfo info;
                    info.index = ++index;
                    info.offset = ebrOffset + static_cast<uint64_t>(logicalStart) * sectorSize;
                    info.size = static_cast<uint64_t>(logicalCount) * sectorSize;
                    info.scheme = "MBR (logical)";
                    info.typeName = describeMbrType(logicalType);
                    info.ntfsCandidate = mbrTypeIsNtfsCandidate(logicalType);
                    partitions.push_back(info);
                }

                const uint8_t* link = ebr.data() + MBR_ENTRY_OFFSET + MBR_ENTRY_SIZE;
                const uint32_t linkStart = readU32(link + 8);
                const uint8_t linkType = link[4];
                if (linkStart == 0 || !isExtendedType(linkType)) {
                    break;
                }
                ebrOffset = extendedBase + static_cast<uint64_t>(linkStart) * sectorSize;
            }
            continue;
        }

        PartitionInfo info;
        info.index = ++index;
        info.offset = static_cast<uint64_t>(startLba) * sectorSize;
        info.size = static_cast<uint64_t>(sectorCount) * sectorSize;
        info.scheme = "MBR";
        info.typeName = describeMbrType(type);
        info.ntfsCandidate = mbrTypeIsNtfsCandidate(type);
        partitions.push_back(info);
    }

    return partitions;
}

std::vector<PartitionInfo> parseGpt(RawDevice& device, uint32_t sectorSize) {
    std::vector<PartitionInfo> partitions;

    std::vector<uint8_t> header(sectorSize, 0);
    if (!readAt(device, sectorSize, header.data(), sectorSize)) {
        return partitions;
    }
    if (std::memcmp(header.data(), "EFI PART", 8) != 0) {
        return partitions;
    }

    const uint64_t entryLba = readU64(header.data() + 0x48);
    const uint32_t entryCount = readU32(header.data() + 0x50);
    const uint32_t entrySize = readU32(header.data() + 0x54);

    if (entryCount == 0 || entryCount > 512 || entrySize < 128 || entrySize > 4096) {
        return partitions;
    }

    const uint64_t tableOffset = entryLba * sectorSize;
    const uint64_t tableBytes = static_cast<uint64_t>(entryCount) * entrySize;
    if (tableBytes > 8ull * 1024 * 1024) {
        return partitions;
    }

    std::vector<uint8_t> table(static_cast<size_t>(tableBytes), 0);
    if (!readAt(device, tableOffset, table.data(), static_cast<uint32_t>(table.size()))) {
        return partitions;
    }

    for (uint32_t slot = 0; slot < entryCount; ++slot) {
        const uint8_t* entry = table.data() + static_cast<size_t>(slot) * entrySize;

        bool ntfsCandidate = false;
        const std::string typeName = describeGptType(entry, ntfsCandidate);
        if (typeName == "unused") {
            continue;
        }

        const uint64_t firstLba = readU64(entry + 0x20);
        const uint64_t lastLba = readU64(entry + 0x28);
        if (lastLba < firstLba) {
            continue;
        }

        PartitionInfo info;
        info.index = slot + 1;
        info.offset = firstLba * sectorSize;
        info.size = (lastLba - firstLba + 1) * sectorSize;
        info.scheme = "GPT";
        info.typeName = typeName;
        info.label = utf16leNameToString(entry + 0x38, 72);
        info.ntfsCandidate = ntfsCandidate;
        partitions.push_back(info);
    }

    return partitions;
}

}

std::vector<PartitionInfo> parsePartitions(RawDevice& device, std::string& error) {
    std::vector<PartitionInfo> partitions;

    uint32_t sectorSize = device.sectorSize();
    if (sectorSize < 512 || sectorSize > 4096) {
        sectorSize = 512;
    }

    std::vector<uint8_t> sector(sectorSize, 0);
    if (!readAt(device, 0, sector.data(), sectorSize)) {
        error = "cannot read the first sector";
        return partitions;
    }

    if (readU16(sector.data() + 510) != MBR_SIGNATURE) {
        return partitions;
    }

    partitions = parseGpt(device, sectorSize);
    if (!partitions.empty()) {
        return partitions;
    }

    return parseMbr(device, sectorSize);
}

bool quickNtfsCheck(RawDevice& device, uint64_t offset) {
    uint8_t sector[512] = {};
    if (!readAt(device, offset, sector, sizeof(sector))) {
        return false;
    }
    if (std::memcmp(sector + 0x03, "NTFS    ", 8) != 0) {
        return false;
    }
    return sector[0x1FE] == 0x55 && sector[0x1FF] == 0xAA;
}

bool quickFatCheck(RawDevice& device, uint64_t offset) {
    uint8_t sector[512] = {};
    if (!readAt(device, offset, sector, sizeof(sector))) {
        return false;
    }

    FatVolumeInfo info;
    std::string ignored;
    return parseFatBootSector(sector, sizeof(sector), info, ignored) && info.kind != FatKind::None;
}

bool resolveNtfsBase(RawDevice& device,
                     const std::string& selection,
                     PartitionResolution& resolution,
                     std::string& error,
                     bool acceptFat) {
    resolution = PartitionResolution{};

    const auto isVolume = [acceptFat](RawDevice& target, uint64_t offset) {
        if (quickNtfsCheck(target, offset)) {
            return true;
        }
        return acceptFat && quickFatCheck(target, offset);
    };

    if (selection.empty() && isVolume(device, 0)) {
        resolution.offset = 0;
        resolution.size = device.size();
        resolution.found = true;
        return true;
    }

    std::string parseError;
    const std::vector<PartitionInfo> partitions = parsePartitions(device, parseError);

    if (!selection.empty()) {
        const unsigned long wanted = std::strtoul(selection.c_str(), nullptr, 10);
        for (const auto& partition : partitions) {
            if (partition.index == wanted) {
                resolution.offset = partition.offset;
                resolution.size = partition.size;
                resolution.index = partition.index;
                resolution.typeName = partition.typeName;
                resolution.label = partition.label;
                resolution.found = true;
                return true;
            }
        }

        error = "partition " + selection + " not found";
        if (partitions.empty()) {
            error += " (this input has no partition table)";
        }
        return false;
    }

    std::vector<PartitionInfo> candidates;
    for (const auto& partition : partitions) {
        if (partition.index != 0 && isVolume(device, partition.offset)) {
            candidates.push_back(partition);
        }
    }

    if (candidates.size() == 1) {
        const PartitionInfo& only = candidates.front();
        resolution.offset = only.offset;
        resolution.size = only.size;
        resolution.index = only.index;
        resolution.typeName = only.typeName;
        resolution.label = only.label;
        resolution.found = true;
        resolution.autoSelected = true;
        return true;
    }

    if (candidates.size() > 1) {
        error = "this input has " + std::to_string(candidates.size()) +
                (acceptFat ? " candidate volumes; choose one with --partition"
                           : " NTFS partitions; choose one with --partition");
        return false;
    }

    error = acceptFat ? "no NTFS or FAT volume found on this input"
                      : "no NTFS partition found on this input";
    if (partitions.empty()) {
        error += " (and no partition table; a bare volume has none, so point at the volume itself)";
    }
    return false;
}

}
