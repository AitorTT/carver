#include "core/signature.h"

#include "core/device.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace carver {

namespace {

constexpr uint64_t MB = 1024ull * 1024ull;

uint16_t readU16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(data[1] << 8);
}

uint32_t readU32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

uint32_t readU32Big(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

bool validateJpeg(const uint8_t* data, size_t length, size_t index) {
    if (index + 4 > length) {
        return false;
    }

    const uint8_t marker = data[index + 3];
    if (marker >= 0xE0 && marker <= 0xEF) {
        return true;
    }

    switch (marker) {
    case 0xDB:
    case 0xC0:
    case 0xC1:
    case 0xC2:
    case 0xC3:
    case 0xC4:
    case 0xC5:
    case 0xC6:
    case 0xC7:
    case 0xC9:
    case 0xCA:
    case 0xCB:
    case 0xDD:
    case 0xFE:
        return true;
    default:
        return false;
    }
}

bool validateGif(const uint8_t* data, size_t length, size_t index) {
    if (index + 6 > length) {
        return false;
    }
    const uint8_t version = data[index + 4];
    return (version == '7' || version == '9') && data[index + 5] == 'a';
}

bool validateBmp(const uint8_t* data, size_t length, size_t index) {
    if (index + 30 > length) {
        return false;
    }

    if (readU16(data + index + 6) != 0 || readU16(data + index + 8) != 0) {
        return false;
    }

    const uint32_t fileSize = readU32(data + index + 2);
    if (fileSize < 54 || fileSize > 64 * MB) {
        return false;
    }

    const uint32_t pixelOffset = readU32(data + index + 10);
    if (pixelOffset < 26 || pixelOffset >= fileSize) {
        return false;
    }

    const uint32_t dibSize = readU32(data + index + 14);
    if (dibSize != 12 && dibSize != 40 && dibSize != 52 && dibSize != 56 &&
        dibSize != 64 && dibSize != 108 && dibSize != 124) {
        return false;
    }

    const int32_t width = static_cast<int32_t>(readU32(data + index + 18));
    const int32_t height = static_cast<int32_t>(readU32(data + index + 22));
    if (width == 0 || height == 0) {
        return false;
    }
    if (width > 100000 || width < -100000 || height > 100000 || height < -100000) {
        return false;
    }

    if (readU16(data + index + 26) != 1) {
        return false;
    }

    const uint16_t bitsPerPixel = readU16(data + index + 28);
    return bitsPerPixel == 1 || bitsPerPixel == 4 || bitsPerPixel == 8 ||
           bitsPerPixel == 16 || bitsPerPixel == 24 || bitsPerPixel == 32;
}

uint64_t sizeBmp(const uint8_t* data, size_t length, size_t index) {
    if (index + 6 > length) {
        return 0;
    }
    return readU32(data + index + 2);
}

bool validateRiff(const uint8_t* data, size_t length, size_t index, const char* form) {
    if (index + 12 > length) {
        return false;
    }
    const uint32_t riffSize = readU32(data + index + 4);
    if (riffSize < 4 || riffSize > 1024ull * MB) {
        return false;
    }
    return std::memcmp(data + index + 8, form, 4) == 0;
}

bool validateWave(const uint8_t* data, size_t length, size_t index) {
    return validateRiff(data, length, index, "WAVE");
}

bool validateWebp(const uint8_t* data, size_t length, size_t index) {
    return validateRiff(data, length, index, "WEBP");
}

bool validateAvi(const uint8_t* data, size_t length, size_t index) {
    return validateRiff(data, length, index, "AVI ");
}

uint64_t sizeRiff(const uint8_t* data, size_t length, size_t index) {
    if (index + 8 > length) {
        return 0;
    }
    return static_cast<uint64_t>(readU32(data + index + 4)) + 8;
}

bool validatePdf(const uint8_t* data, size_t length, size_t index) {
    if (index + 7 > length) {
        return false;
    }
    const uint8_t major = data[index + 5];
    return (major == '1' || major == '2') && data[index + 6] == '.';
}

bool validateZip(const uint8_t* data, size_t length, size_t index) {
    if (index + 30 > length) {
        return false;
    }

    const uint16_t version = readU16(data + index + 4);
    if (version < 10 || version > 63) {
        return false;
    }

    switch (readU16(data + index + 8)) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
    case 12:
    case 14:
    case 18:
    case 19:
    case 20:
    case 93:
    case 94:
    case 95:
    case 96:
    case 97:
    case 98:
    case 99:
        break;
    default:
        return false;
    }

    if (readU16(data + index + 26) > 4096 || readU16(data + index + 28) > 4096) {
        return false;
    }

    return true;
}

bool validateRar(const uint8_t* data, size_t length, size_t index) {
    if (index + 8 > length) {
        return false;
    }
    const uint8_t marker = data[index + 6];
    if (marker == 0x00) {
        return true;
    }
    return marker == 0x01 && data[index + 7] == 0x00;
}

bool validate7z(const uint8_t* data, size_t length, size_t index) {
    if (index + 8 > length) {
        return false;
    }
    return data[index + 6] == 0x00 && data[index + 7] == 0x04;
}

bool validateGzip(const uint8_t* data, size_t length, size_t index) {
    if (index + 12 > length) {
        return false;
    }
    if ((data[index + 3] & 0xE0) != 0) {
        return false;
    }

    const uint8_t extraFlags = data[index + 8];
    if (extraFlags != 0 && extraFlags != 2 && extraFlags != 4) {
        return false;
    }

    const uint8_t operatingSystem = data[index + 9];
    if (operatingSystem > 13 && operatingSystem != 255) {
        return false;
    }

    const uint8_t deflate = data[index + 10];
    if (((deflate >> 1) & 0x03) == 0x03) {
        return false;
    }

    return true;
}

bool validateId3(const uint8_t* data, size_t length, size_t index) {
    if (index + 10 > length) {
        return false;
    }

    const uint8_t major = data[index + 3];
    if (major < 2 || major > 4) {
        return false;
    }
    if (data[index + 4] != 0) {
        return false;
    }

    const uint8_t flags = data[index + 5];
    if (major == 2) {
        if ((flags & 0x3F) != 0) {
            return false;
        }
    } else if ((flags & 0x0F) != 0) {
        return false;
    }

    for (int offset = 6; offset < 10; ++offset) {
        if ((data[index + offset] & 0x80) != 0) {
            return false;
        }
    }

    return true;
}

bool validateTiffLittleEndian(const uint8_t* data, size_t length, size_t index) {
    if (index + 8 > length) {
        return false;
    }
    const uint32_t ifdOffset = readU32(data + index + 4);
    return ifdOffset >= 8 && ifdOffset < (1u << 20);
}

bool validateTiffBigEndian(const uint8_t* data, size_t length, size_t index) {
    if (index + 8 > length) {
        return false;
    }
    const uint32_t ifdOffset = readU32Big(data + index + 4);
    return ifdOffset >= 8 && ifdOffset < (1u << 20);
}

bool validateMp4(const uint8_t* data, size_t length, size_t index) {
    if (index + 12 > length) {
        return false;
    }

    const uint32_t boxSize = readU32(data + index);
    if (boxSize < 8 || boxSize > 4096) {
        return false;
    }

    for (int offset = 8; offset < 12; ++offset) {
        const uint8_t character = data[index + offset];
        if (character < 0x20 || character > 0x7E) {
            return false;
        }
    }

    return true;
}

bool validateElf(const uint8_t* data, size_t length, size_t index) {
    if (index + 7 > length) {
        return false;
    }
    const uint8_t elfClass = data[index + 4];
    const uint8_t elfData = data[index + 5];
    if (elfClass != 1 && elfClass != 2) {
        return false;
    }
    if (elfData != 1 && elfData != 2) {
        return false;
    }
    return data[index + 6] == 1;
}

bool validatePe(const uint8_t* data, size_t length, size_t index) {
    if (index + 64 > length) {
        return false;
    }

    const uint32_t peOffset = readU32(data + index + 0x3C);
    if (peOffset < 0x40 || peOffset > 4096 || (peOffset % 4) != 0) {
        return false;
    }
    if (index + peOffset + 4 > length) {
        return false;
    }

    const uint8_t* signature = data + index + peOffset;
    return signature[0] == 'P' && signature[1] == 'E' && signature[2] == 0 && signature[3] == 0;
}

uint64_t sizePe(const uint8_t* data, size_t length, size_t index) {
    if (index + 64 > length) {
        return 0;
    }

    const uint32_t peOffset = readU32(data + index + 0x3C);
    if (peOffset < 0x40 || peOffset > 4096) {
        return 0;
    }
    if (index + peOffset + 24 > length) {
        return 0;
    }

    const uint8_t* signature = data + index + peOffset;
    if (signature[0] != 'P' || signature[1] != 'E' || signature[2] != 0 || signature[3] != 0) {
        return 0;
    }

    const uint16_t sectionCount = readU16(data + index + peOffset + 6);
    const uint16_t optionalSize = readU16(data + index + peOffset + 20);
    if (sectionCount == 0 || sectionCount > 96) {
        return 0;
    }

    const size_t sectionTable = index + peOffset + 24 + optionalSize;
    uint64_t end = sectionTable - index;

    for (uint16_t section = 0; section < sectionCount; ++section) {
        const size_t entry = sectionTable + section * 40;
        if (entry + 40 > length) {
            return 0;
        }
        const uint32_t rawSize = readU32(data + entry + 16);
        const uint32_t rawPointer = readU32(data + entry + 20);
        const uint64_t sectionEnd = static_cast<uint64_t>(rawPointer) + rawSize;
        if (sectionEnd > end) {
            end = sectionEnd;
        }
    }

    return end;
}

bool isJpegSegmentMarker(uint8_t marker) {
    if (marker >= 0xE0 && marker <= 0xEF) {
        return true;
    }
    switch (marker) {
    case 0xDB:
    case 0xC0:
    case 0xC1:
    case 0xC2:
    case 0xC3:
    case 0xC4:
    case 0xC5:
    case 0xC6:
    case 0xC7:
    case 0xC9:
    case 0xCA:
    case 0xCB:
    case 0xCC:
    case 0xDC:
    case 0xDD:
    case 0xFE:
        return true;
    default:
        return false;
    }
}

bool validateJpegDevice(RawDevice& device, uint64_t fileStart) {
    std::string ignored;
    uint32_t got = 0;

    uint8_t startOfImage[2];
    if (!device.readAt(fileStart, startOfImage, sizeof(startOfImage), got, ignored) || got < 2) {
        return false;
    }
    if (startOfImage[0] != 0xFF || startOfImage[1] != 0xD8) {
        return false;
    }

    uint64_t position = fileStart + 2;
    int segments = 0;

    while (segments < 6) {
        uint8_t header[4];
        if (!device.readAt(position, header, sizeof(header), got, ignored) || got < 4) {
            return false;
        }
        if (header[0] != 0xFF) {
            return false;
        }

        const uint8_t marker = header[1];
        if (marker == 0xFF || marker == 0x00 || marker == 0x01 || marker == 0xD8) {
            return false;
        }
        if (marker >= 0xD0 && marker <= 0xD7) {
            return false;
        }
        if (marker == 0xD9 || marker == 0xDA) {
            return segments >= 2;
        }
        if (!isJpegSegmentMarker(marker)) {
            return false;
        }

        const uint16_t segmentLength = static_cast<uint16_t>((header[2] << 8) | header[3]);
        if (segmentLength < 4) {
            return false;
        }

        position += 2 + segmentLength;
        ++segments;
    }

    return true;
}

bool scanToNextJpegMarker(RawDevice& device, uint64_t fileStart, uint64_t& relative, uint64_t limitRelative) {
    std::string ignored;
    std::vector<uint8_t> buffer(256 * 1024);

    while (relative < limitRelative) {
        const uint32_t want = static_cast<uint32_t>(
            std::min<uint64_t>(buffer.size(), limitRelative - relative));
        uint32_t got = 0;
        if (!device.readAt(fileStart + relative, buffer.data(), want, got, ignored) || got == 0) {
            return false;
        }

        for (uint32_t index = 0; index + 1 < got; ++index) {
            if (buffer[index] != 0xFF) {
                continue;
            }
            const uint8_t next = buffer[index + 1];
            if (next == 0x00 || next == 0xFF) {
                continue;
            }
            if (next >= 0xD0 && next <= 0xD7) {
                continue;
            }
            relative += index;
            return true;
        }

        if (got < 2) {
            return false;
        }
        relative += got - 1;
        if (got < want) {
            return false;
        }
    }

    return false;
}

uint64_t refineJpegSize(RawDevice& device, uint64_t fileStart, uint64_t provisional) {
    static_cast<void>(provisional);
    const uint64_t cap = 512 * MB;
    std::string ignored;
    uint32_t got = 0;

    uint8_t startOfImage[2];
    if (!device.readAt(fileStart, startOfImage, sizeof(startOfImage), got, ignored) || got < 2) {
        return 0;
    }
    if (startOfImage[0] != 0xFF || startOfImage[1] != 0xD8) {
        return 0;
    }

    uint64_t relative = 2;

    while (relative < cap) {
        uint8_t marker[4];
        if (!device.readAt(fileStart + relative, marker, sizeof(marker), got, ignored) || got < 4) {
            return 0;
        }
        if (marker[0] != 0xFF) {
            return 0;
        }

        const uint8_t code = marker[1];
        if (code == 0xD9) {
            return relative + 2;
        }
        if (code == 0xFF) {
            ++relative;
            continue;
        }
        if (code == 0x00 || code == 0xD8) {
            return 0;
        }
        if (code == 0x01 || (code >= 0xD0 && code <= 0xD7)) {
            relative += 2;
            continue;
        }

        const uint16_t segmentLength = static_cast<uint16_t>((marker[2] << 8) | marker[3]);
        if (segmentLength < 2) {
            return 0;
        }

        relative += 2 + segmentLength;

        if (code == 0xDA) {
            if (!scanToNextJpegMarker(device, fileStart, relative, cap)) {
                return 0;
            }
        }
    }

    return 0;
}

uint64_t refinePeSize(RawDevice& device, uint64_t fileStart, uint64_t provisional) {
    if (provisional == 0) {
        return 0;
    }

    std::string ignored;
    uint32_t got = 0;

    uint8_t dosHeader[64];
    if (!device.readAt(fileStart, dosHeader, sizeof(dosHeader), got, ignored) || got < 64) {
        return provisional;
    }
    if (dosHeader[0] != 'M' || dosHeader[1] != 'Z') {
        return provisional;
    }

    const uint32_t peOffset = readU32(dosHeader + 0x3C);
    if (peOffset < 0x40 || peOffset > 4096) {
        return provisional;
    }

    uint8_t fileHeader[20];
    if (!device.readAt(fileStart + peOffset + 4, fileHeader, sizeof(fileHeader), got, ignored) || got < 20) {
        return provisional;
    }

    const uint32_t symbolTablePointer = readU32(fileHeader + 8);
    const uint32_t symbolCount = readU32(fileHeader + 12);
    if (symbolTablePointer == 0 || symbolCount == 0 || symbolCount > 10000000) {
        return provisional;
    }

    const uint64_t symbolTableEnd =
        static_cast<uint64_t>(symbolTablePointer) + static_cast<uint64_t>(symbolCount) * 18;
    if (symbolTableEnd <= provisional) {
        return provisional;
    }

    uint8_t stringTableSizeBytes[4];
    if (!device.readAt(fileStart + symbolTableEnd, stringTableSizeBytes, sizeof(stringTableSizeBytes), got, ignored) ||
        got < 4) {
        return symbolTableEnd;
    }

    const uint32_t stringTableSize = readU32(stringTableSizeBytes);
    if (stringTableSize < 4 || stringTableSize > (64ull << 20)) {
        return symbolTableEnd;
    }

    return symbolTableEnd + stringTableSize;
}

}

const std::vector<Signature>& defaultSignatures() {
    static const std::vector<Signature> signatures = {
        Signature{
            .name = "jpg",
            .extension = "jpg",
            .header = {0xFF, 0xD8, 0xFF},
            .footer = {0xFF, 0xD9},
            .maxSize = 64 * MB,
            .headerOffset = 0,
            .lookahead = 8,
            .validate = validateJpeg,
            .declaredSize = nullptr,
            .refineSize = refineJpegSize,
            .deviceValidate = validateJpegDevice,
        },
        Signature{
            .name = "png",
            .extension = "png",
            .header = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A},
            .footer = {0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82},
            .maxSize = 64 * MB,
            .headerOffset = 0,
            .lookahead = 0,
            .validate = nullptr,
            .declaredSize = nullptr,
        },
        Signature{
            .name = "gif",
            .extension = "gif",
            .header = {0x47, 0x49, 0x46, 0x38},
            .footer = {0x00, 0x3B},
            .maxSize = 32 * MB,
            .headerOffset = 0,
            .lookahead = 8,
            .validate = validateGif,
            .declaredSize = nullptr,
        },
        Signature{
            .name = "bmp",
            .extension = "bmp",
            .header = {0x42, 0x4D},
            .footer = {},
            .maxSize = 64 * MB,
            .headerOffset = 0,
            .lookahead = 64,
            .validate = validateBmp,
            .declaredSize = sizeBmp,
        },
        Signature{
            .name = "tiff",
            .extension = "tif",
            .header = {0x49, 0x49, 0x2A, 0x00},
            .footer = {},
            .maxSize = 64 * MB,
            .headerOffset = 0,
            .lookahead = 16,
            .validate = validateTiffLittleEndian,
            .declaredSize = nullptr,
        },
        Signature{
            .name = "tiffbe",
            .extension = "tif",
            .header = {0x4D, 0x4D, 0x00, 0x2A},
            .footer = {},
            .maxSize = 64 * MB,
            .headerOffset = 0,
            .lookahead = 16,
            .validate = validateTiffBigEndian,
            .declaredSize = nullptr,
        },
        Signature{
            .name = "pdf",
            .extension = "pdf",
            .header = {0x25, 0x50, 0x44, 0x46, 0x2D},
            .footer = {0x25, 0x25, 0x45, 0x4F, 0x46},
            .maxSize = 128 * MB,
            .headerOffset = 0,
            .lookahead = 8,
            .validate = validatePdf,
            .declaredSize = nullptr,
        },
        Signature{
            .name = "zip",
            .extension = "zip",
            .header = {0x50, 0x4B, 0x03, 0x04},
            .footer = {},
            .maxSize = 128 * MB,
            .headerOffset = 0,
            .lookahead = 32,
            .validate = validateZip,
            .declaredSize = nullptr,
        },
        Signature{
            .name = "rar",
            .extension = "rar",
            .header = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07},
            .footer = {},
            .maxSize = 128 * MB,
            .headerOffset = 0,
            .lookahead = 10,
            .validate = validateRar,
            .declaredSize = nullptr,
        },
        Signature{
            .name = "7z",
            .extension = "7z",
            .header = {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C},
            .footer = {},
            .maxSize = 128 * MB,
            .headerOffset = 0,
            .lookahead = 8,
            .validate = validate7z,
            .declaredSize = nullptr,
        },
        Signature{
            .name = "gzip",
            .extension = "gz",
            .header = {0x1F, 0x8B, 0x08},
            .footer = {},
            .maxSize = 64 * MB,
            .headerOffset = 0,
            .lookahead = 16,
            .validate = validateGzip,
            .declaredSize = nullptr,
        },
        Signature{
            .name = "mp3",
            .extension = "mp3",
            .header = {0x49, 0x44, 0x33},
            .footer = {},
            .maxSize = 32 * MB,
            .headerOffset = 0,
            .lookahead = 16,
            .validate = validateId3,
            .declaredSize = nullptr,
        },
        Signature{
            .name = "wav",
            .extension = "wav",
            .header = {0x52, 0x49, 0x46, 0x46},
            .footer = {},
            .maxSize = 1024 * MB,
            .headerOffset = 0,
            .lookahead = 16,
            .validate = validateWave,
            .declaredSize = sizeRiff,
        },
        Signature{
            .name = "webp",
            .extension = "webp",
            .header = {0x52, 0x49, 0x46, 0x46},
            .footer = {},
            .maxSize = 64 * MB,
            .headerOffset = 0,
            .lookahead = 16,
            .validate = validateWebp,
            .declaredSize = sizeRiff,
        },
        Signature{
            .name = "avi",
            .extension = "avi",
            .header = {0x52, 0x49, 0x46, 0x46},
            .footer = {},
            .maxSize = 1024 * MB,
            .headerOffset = 0,
            .lookahead = 16,
            .validate = validateAvi,
            .declaredSize = sizeRiff,
        },
        Signature{
            .name = "mp4",
            .extension = "mp4",
            .header = {0x66, 0x74, 0x79, 0x70},
            .footer = {},
            .maxSize = 256 * MB,
            .headerOffset = 4,
            .lookahead = 32,
            .validate = validateMp4,
            .declaredSize = nullptr,
        },
        Signature{
            .name = "sqlite",
            .extension = "sqlite",
            .header = {0x53, 0x51, 0x4C, 0x69, 0x74, 0x65, 0x20, 0x66,
                       0x6F, 0x72, 0x6D, 0x61, 0x74, 0x20, 0x33, 0x00},
            .footer = {},
            .maxSize = 256 * MB,
            .headerOffset = 0,
            .lookahead = 0,
            .validate = nullptr,
            .declaredSize = nullptr,
        },
        Signature{
            .name = "pe",
            .extension = "exe",
            .header = {0x4D, 0x5A},
            .footer = {},
            .maxSize = 128 * MB,
            .headerOffset = 0,
            .lookahead = 8192,
            .validate = validatePe,
            .declaredSize = sizePe,
            .refineSize = refinePeSize,
        },
        Signature{
            .name = "elf",
            .extension = "elf",
            .header = {0x7F, 0x45, 0x4C, 0x46},
            .footer = {},
            .maxSize = 128 * MB,
            .headerOffset = 0,
            .lookahead = 8,
            .validate = validateElf,
            .declaredSize = nullptr,
        },
    };

    return signatures;
}

size_t maxHeaderLength(const std::vector<Signature>& signatures) {
    size_t longest = 0;
    for (const auto& signature : signatures) {
        longest = std::max(longest, signature.headerOffset + signature.header.size());
    }
    return longest;
}

size_t maxLookahead(const std::vector<Signature>& signatures) {
    size_t longest = 0;
    for (const auto& signature : signatures) {
        longest = std::max(longest, signature.lookahead);
    }
    return longest;
}

namespace {

// Lowercase with the dot removed, and common spellings folded together so that
// "jpeg" and "jpg" refer to the same thing in both directions. Without this a
// user asking for "jpeg" would silently match nothing, because the carve
// signature for that format is called "jpg".
std::string canonicalExtension(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && value[start] == '.') {
        ++start;
    }

    std::string result;
    result.reserve(value.size() - start);
    for (size_t index = start; index < value.size(); ++index) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value[index]))));
    }

    if (result == "jpeg" || result == "jpe") {
        return "jpg";
    }
    if (result == "tiff") {
        return "tif";
    }
    if (result == "gzip") {
        return "gz";
    }
    return result;
}

}

std::vector<std::string> parseExtensionList(const std::string& text) {
    std::vector<std::string> extensions;
    std::string current;

    const auto flush = [&extensions, &current]() {
        const std::string normalized = canonicalExtension(current);
        if (!normalized.empty()) {
            extensions.push_back(normalized);
        }
        current.clear();
    };

    for (char character : text) {
        if (character == ',' || character == ';' || character == ' ' || character == '\t') {
            flush();
        } else {
            current.push_back(character);
        }
    }
    flush();

    return extensions;
}

bool extensionSkipped(const std::vector<std::string>& skipped, const std::string& extension) {
    if (skipped.empty()) {
        return false;
    }

    const std::string normalized = canonicalExtension(extension);
    if (normalized.empty()) {
        // An extension-less file is never skipped, so a stray empty entry in the
        // skip list cannot quietly drop files that have no extension at all.
        return false;
    }

    return std::find(skipped.begin(), skipped.end(), normalized) != skipped.end();
}

bool extensionSelected(const std::vector<std::string>& selected, const std::string& extension) {
    if (selected.empty()) {
        return true;
    }

    const std::string normalized = canonicalExtension(extension);
    if (normalized.empty()) {
        return false;
    }

    return std::find(selected.begin(), selected.end(), normalized) != selected.end();
}

}
