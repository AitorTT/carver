#include "core/lznt1.h"

#include <algorithm>
#include <cstring>

namespace carver {

namespace {

constexpr size_t CHUNK_SIZE = 4096;
constexpr uint16_t CHUNK_SIGNATURE_MASK = 0x7000;
constexpr uint16_t CHUNK_SIGNATURE = 0x3000;
constexpr uint16_t CHUNK_COMPRESSED_FLAG = 0x8000;

uint16_t readU16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(data[1] << 8);
}

bool isChunkHeader(uint16_t header) {
    return (header & CHUNK_SIGNATURE_MASK) == CHUNK_SIGNATURE;
}

}

bool lznt1Decompress(const uint8_t* input,
                     size_t inputLength,
                     std::vector<uint8_t>& output,
                     std::string& error) {
    output.clear();

    size_t position = 0;

    while (position + 2 <= inputLength) {
        const uint16_t header = readU16(input + position);
        position += 2;

        if (header == 0) {
            break;
        }

        if (!isChunkHeader(header)) {
            error = "not an LZNT1 stream: bad chunk header 0x" +
                    std::to_string(header);
            return false;
        }

        const size_t chunkLength = static_cast<size_t>(header & 0x0FFF) + 1;
        if (position + chunkLength > inputLength) {
            error = "LZNT1 chunk runs past the end of the input";
            return false;
        }

        if ((header & CHUNK_COMPRESSED_FLAG) == 0) {
            output.insert(output.end(), input + position, input + position + chunkLength);
            position += chunkLength;
            continue;
        }

        const size_t chunkEnd = position + chunkLength;
        const size_t chunkStart = output.size();
        size_t produced = 0;

        while (position < chunkEnd && produced < CHUNK_SIZE) {
            const uint8_t flags = input[position++];

            for (int bit = 0; bit < 8; ++bit) {
                if (position >= chunkEnd || produced >= CHUNK_SIZE) {
                    break;
                }

                if ((flags & (1u << bit)) == 0) {
                    output.push_back(input[position++]);
                    ++produced;
                    continue;
                }

                if (position + 2 > chunkEnd) {
                    position = chunkEnd;
                    break;
                }

                const uint16_t token = readU16(input + position);
                position += 2;

                // The tuple splits its 16 bits between the length and the back
                // reference dynamically: the further into the chunk we are, the
                // more bits the offset needs and the fewer are left for the
                // length. At the start of a chunk the split is 4 bits of offset
                // and 12 of length, losing one bit to the offset each time the
                // position halves past 16 bytes.
                size_t shift = 12;
                uint16_t mask = 0x0FFF;
                if (produced > 0) {
                    for (size_t iterator = produced - 1; iterator >= 0x10; iterator >>= 1) {
                        --shift;
                        mask = static_cast<uint16_t>(mask >> 1);
                    }
                }

                const size_t length = static_cast<size_t>(token & mask) + 3;
                const size_t offset = static_cast<size_t>(token >> shift) + 1;

                if (offset > produced) {
                    error = "LZNT1 back reference points before the start of the chunk";
                    return false;
                }

                for (size_t index = 0; index < length && produced < CHUNK_SIZE; ++index) {
                    output.push_back(output[chunkStart + produced - offset]);
                    ++produced;
                }
            }
        }

        position = chunkEnd;
    }

    return true;
}

bool lznt1DecompressUnits(const std::vector<uint8_t>& onDisk,
                          const std::vector<bool>& unitCompressed,
                          uint64_t unitSize,
                          uint64_t logicalSize,
                          std::vector<uint8_t>& output,
                          std::string& error) {
    output.clear();

    if (unitSize == 0) {
        error = "compression unit size is zero";
        return false;
    }

    output.reserve(static_cast<size_t>(std::min<uint64_t>(logicalSize, 64ull * 1024 * 1024)));

    const uint64_t unitCount = unitCompressed.size();
    std::vector<uint8_t> decompressed;

    for (uint64_t unit = 0; unit < unitCount && output.size() < logicalSize; ++unit) {
        const uint64_t start = unit * unitSize;
        if (start >= onDisk.size()) {
            break;
        }

        const uint64_t available = std::min<uint64_t>(unitSize, onDisk.size() - start);
        const uint64_t take = std::min<uint64_t>(available, logicalSize - output.size());

        if (!unitCompressed[static_cast<size_t>(unit)]) {
            output.insert(output.end(), onDisk.begin() + start, onDisk.begin() + start + take);
            continue;
        }

        if (!lznt1Decompress(onDisk.data() + start, static_cast<size_t>(available),
                             decompressed, error)) {
            return false;
        }

        const uint64_t wanted = std::min<uint64_t>(decompressed.size(), logicalSize - output.size());
        output.insert(output.end(), decompressed.begin(), decompressed.begin() + wanted);
    }

    if (output.size() > logicalSize) {
        output.resize(static_cast<size_t>(logicalSize));
    }

    if (output.size() < logicalSize) {
        error = "compressed data produced only " + std::to_string(output.size()) +
                " of " + std::to_string(logicalSize) + " expected bytes";
        return false;
    }

    return true;
}

}
