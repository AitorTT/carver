#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace carver {

// Decompresses a raw LZNT1 stream, which is a sequence of chunks that each
// start with a two byte header. Returns false if the stream is not valid
// LZNT1, which for NTFS means the data was stored uncompressed.
bool lznt1Decompress(const uint8_t* input,
                     size_t inputLength,
                     std::vector<uint8_t>& output,
                     std::string& error);

// Decompresses an NTFS compressed attribute. The on disk stream is split into
// compression units of 'unitSize' bytes; a unit that does not occupy all of its
// clusters is compressed, one that does is stored verbatim.
bool lznt1DecompressUnits(const std::vector<uint8_t>& onDisk,
                          const std::vector<bool>& unitCompressed,
                          uint64_t unitSize,
                          uint64_t logicalSize,
                          std::vector<uint8_t>& output,
                          std::string& error);

}
