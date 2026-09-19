#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace carver {

class RawDevice;

using ValidatorFn = bool (*)(const uint8_t* data, size_t length, size_t index);
using SizeFn = uint64_t (*)(const uint8_t* data, size_t length, size_t index);
using RefineFn = uint64_t (*)(RawDevice& device, uint64_t fileStart, uint64_t provisional);
using DeviceValidatorFn = bool (*)(RawDevice& device, uint64_t fileStart);

struct Signature {
    std::string name;
    std::string extension;
    std::vector<uint8_t> header;
    std::vector<uint8_t> footer;
    uint64_t maxSize = 0;
    size_t headerOffset = 0;
    size_t lookahead = 0;
    ValidatorFn validate = nullptr;
    SizeFn declaredSize = nullptr;
    RefineFn refineSize = nullptr;
    DeviceValidatorFn deviceValidate = nullptr;
};

const std::vector<Signature>& defaultSignatures();
size_t maxHeaderLength(const std::vector<Signature>& signatures);
size_t maxLookahead(const std::vector<Signature>& signatures);

// Splits a comma, semicolon or space separated list such as "jpeg, .PNG , pdf"
// into lowercase extensions with the leading dot removed. Empty entries are
// dropped, so an extension-less file can never be skipped by accident. Common
// spellings are folded together, so "jpeg" is stored as "jpg" and "tiff" as "tif".
std::vector<std::string> parseExtensionList(const std::string& text);

// True when 'extension', given with or without a leading dot and in any case,
// appears in a list produced by parseExtensionList.
bool extensionSkipped(const std::vector<std::string>& skipped, const std::string& extension);

// True when an only-list is empty, meaning "every type", or when 'extension'
// appears in it. Used for the opposite of extensionSkipped: recover only these.
bool extensionSelected(const std::vector<std::string>& selected, const std::string& extension);

}
