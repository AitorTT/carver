#include "core/image.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace carver {

namespace {

constexpr const char* STATE_MAGIC = "carver-image-state";
constexpr uint32_t STATE_VERSION = 1;
constexpr uint64_t CHECKPOINT_INTERVAL = 64ull * 1024ull * 1024ull;

class Sha256 {
public:
    ~Sha256() {
        reset();
    }

    bool start(std::string& error) {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
            error = "cannot open the SHA-256 provider";
            return false;
        }

        DWORD objectSize = 0;
        DWORD produced = 0;
        if (!BCRYPT_SUCCESS(BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH,
                                              reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
                                              &produced, 0))) {
            error = "cannot size the SHA-256 state";
            reset();
            return false;
        }

        hashObject_.resize(objectSize);
        if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm_, &hash_, hashObject_.data(), objectSize,
                                             nullptr, 0, 0))) {
            error = "cannot create the SHA-256 state";
            reset();
            return false;
        }

        active_ = true;
        return true;
    }

    bool update(const uint8_t* data, size_t length, std::string& error) {
        if (!active_ || length == 0) {
            return true;
        }
        if (!BCRYPT_SUCCESS(BCryptHashData(hash_, const_cast<PUCHAR>(data),
                                           static_cast<ULONG>(length), 0))) {
            error = "cannot hash image data";
            return false;
        }
        return true;
    }

    std::string finish() {
        if (!active_) {
            return {};
        }

        uint8_t digest[32] = {};
        if (!BCRYPT_SUCCESS(BCryptFinishHash(hash_, digest, sizeof(digest), 0))) {
            reset();
            return {};
        }
        reset();

        std::string hex;
        hex.reserve(sizeof(digest) * 2);
        char pair[3] = {};
        for (uint8_t byte : digest) {
            std::snprintf(pair, sizeof(pair), "%02x", byte);
            hex += pair;
        }
        return hex;
    }

private:
    void reset() {
        if (hash_ != nullptr) {
            BCryptDestroyHash(hash_);
            hash_ = nullptr;
        }
        if (algorithm_ != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            algorithm_ = nullptr;
        }
        hashObject_.clear();
        active_ = false;
    }

    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<uint8_t> hashObject_;
    bool active_ = false;
};

struct ImageState {
    std::string source;
    uint64_t rangeStart = 0;
    uint64_t rangeEnd = 0;
    uint64_t bytesDone = 0;
    bool present = false;
};

std::string trim(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool readImageState(const std::string& path, ImageState& state) {
    state = ImageState{};

    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "rb") != 0 || file == nullptr) {
        return false;
    }

    char line[1024] = {};
    bool magicSeen = false;

    while (fgets(line, sizeof(line), file) != nullptr) {
        const std::string text = trim(line);
        if (text.empty()) {
            continue;
        }
        if (!magicSeen) {
            if (text != STATE_MAGIC) {
                fclose(file);
                return false;
            }
            magicSeen = true;
            continue;
        }

        const size_t separator = text.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const std::string key = text.substr(0, separator);
        const std::string value = text.substr(separator + 1);
        const unsigned long long number = std::strtoull(value.c_str(), nullptr, 10);

        if (key == "version") {
            if (number != STATE_VERSION) {
                fclose(file);
                return false;
            }
        } else if (key == "source") {
            state.source = value;
        } else if (key == "rangeStart") {
            state.rangeStart = number;
        } else if (key == "rangeEnd") {
            state.rangeEnd = number;
        } else if (key == "bytesDone") {
            state.bytesDone = number;
        }
    }

    fclose(file);

    if (!magicSeen) {
        return false;
    }

    state.present = true;
    return true;
}

bool writeImageState(const std::string& path, const std::string& sourcePath,
                     uint64_t rangeStart, uint64_t rangeEnd, uint64_t bytesDone) {
    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "wb") != 0 || file == nullptr) {
        return false;
    }

    std::fprintf(file, "%s\n", STATE_MAGIC);
    std::fprintf(file, "version=%u\n", STATE_VERSION);
    std::fprintf(file, "source=%s\n", sourcePath.c_str());
    std::fprintf(file, "rangeStart=%llu\n", static_cast<unsigned long long>(rangeStart));
    std::fprintf(file, "rangeEnd=%llu\n", static_cast<unsigned long long>(rangeEnd));
    std::fprintf(file, "bytesDone=%llu\n", static_cast<unsigned long long>(bytesDone));

    const bool ok = fclose(file) == 0;
    return ok;
}

bool fileSize(const std::string& path, uint64_t& size) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(utf8ToWide(path).c_str(), GetFileExInfoStandard, &data)) {
        return false;
    }
    LARGE_INTEGER value{};
    value.LowPart = data.nFileSizeLow;
    value.HighPart = static_cast<LONG>(data.nFileSizeHigh);
    size = static_cast<uint64_t>(value.QuadPart);
    return true;
}

}

std::string imageStatePath(const std::string& destinationPath) {
    return destinationPath + ".carver-state";
}

bool imageResumeOffset(const std::string& destinationPath,
                       const std::string& sourcePath,
                       uint64_t rangeStart,
                       uint64_t rangeEnd,
                       uint64_t& offset,
                       std::string& error) {
    offset = 0;

    ImageState state;
    if (!readImageState(imageStatePath(destinationPath), state)) {
        error = "no usable checkpoint file for " + destinationPath;
        return false;
    }

    if (state.source != sourcePath) {
        error = "the checkpoint was written for a different source (" + state.source + ")";
        return false;
    }
    if (state.rangeStart != rangeStart || state.rangeEnd != rangeEnd) {
        error = "the checkpoint covers a different range";
        return false;
    }

    uint64_t actualSize = 0;
    if (!fileSize(destinationPath, actualSize)) {
        error = "the partial image " + destinationPath + " is missing";
        return false;
    }
    if (actualSize != state.bytesDone) {
        error = "the partial image is " + std::to_string(actualSize) +
                " bytes but the checkpoint records " + std::to_string(state.bytesDone);
        return false;
    }

    offset = state.bytesDone;
    return true;
}

bool createImage(RawDevice& source,
                 const std::string& sourcePath,
                 const std::string& destinationPath,
                 const ImageOptions& options,
                 const ImageProgressFn& progress,
                 ImageResult& result,
                 std::string& error) {
    result = ImageResult{};

    const uint64_t deviceSize = source.size();
    const uint64_t start = std::min(options.startOffset, deviceSize);
    const uint64_t end = (options.endOffset == 0 || options.endOffset > deviceSize)
                             ? deviceSize
                             : options.endOffset;

    if (start >= end) {
        error = "image range is empty";
        return false;
    }

    const std::string statePath = imageStatePath(destinationPath);
    uint64_t resumeOffset = 0;

    uint64_t destinationSize = 0;
    const bool destinationExists = fileSize(destinationPath, destinationSize);

    if (options.resume) {
        if (!imageResumeOffset(destinationPath, sourcePath, start, end, resumeOffset, error)) {
            return false;
        }
        result.resumed = true;
    } else if (destinationExists && !options.force) {
        error = destinationPath + " already exists. Use --resume to continue an interrupted "
                                  "image, or --force to overwrite it.";
        return false;
    }

    if (resumeOffset >= (end - start)) {
        error = "the checkpoint says the image is already complete";
        return false;
    }

    HANDLE destination = CreateFileW(utf8ToWide(destinationPath).c_str(),
                                     GENERIC_WRITE | GENERIC_READ, 0, nullptr,
                                     options.resume ? OPEN_EXISTING : CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
    if (destination == INVALID_HANDLE_VALUE) {
        error = "cannot open " + destinationPath + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }

    LARGE_INTEGER seek{};
    seek.QuadPart = static_cast<LONGLONG>(resumeOffset);
    if (SetFilePointerEx(destination, seek, nullptr, FILE_BEGIN) == 0) {
        error = "cannot seek in " + destinationPath;
        CloseHandle(destination);
        return false;
    }

    Sha256 hasher;
    if (!hasher.start(error)) {
        CloseHandle(destination);
        return false;
    }

    const uint64_t chunkSize = std::max<uint64_t>(options.chunkSize, 64 * 1024);
    std::vector<uint8_t> buffer(static_cast<size_t>(chunkSize));

    if (resumeOffset > 0) {
        uint64_t remainingSeed = resumeOffset;
        LARGE_INTEGER rewind{};
        rewind.QuadPart = 0;
        SetFilePointerEx(destination, rewind, nullptr, FILE_BEGIN);

        while (remainingSeed > 0) {
            const uint32_t want = static_cast<uint32_t>(
                std::min<uint64_t>(remainingSeed, buffer.size()));
            if (want == 0) {
                error = "cannot re-read the partial image to resume its hash";
                CloseHandle(destination);
                return false;
            }

            DWORD read = 0;
            if (!ReadFile(destination, buffer.data(), want, &read, nullptr) || read == 0) {
                error = "cannot re-read the partial image to resume its hash";
                CloseHandle(destination);
                return false;
            }
            if (!hasher.update(buffer.data(), read, error)) {
                CloseHandle(destination);
                return false;
            }
            remainingSeed -= read;
        }

        SetFilePointerEx(destination, seek, nullptr, FILE_BEGIN);
    }

    const uint64_t total = end - start;
    uint64_t position = start + resumeOffset;
    uint64_t written = resumeOffset;
    uint64_t sinceCheckpoint = 0;

    const auto began = std::chrono::steady_clock::now();

    ImageProgress state;
    state.bytesTotal = total;
    state.bytesDone = written;
    if (progress) {
        progress(state);
    }

    bool ok = true;
    bool cancelled = false;

    while (position < end) {
        const uint32_t want = static_cast<uint32_t>(std::min<uint64_t>(buffer.size(), end - position));
        uint32_t got = 0;
        if (!source.readAt(position, buffer.data(), want, got, error) || got == 0) {
            if (error.empty()) {
                error = "source returned no data at offset " + std::to_string(position);
            }
            ok = false;
            break;
        }

        DWORD produced = 0;
        if (!WriteFile(destination, buffer.data(), got, &produced, nullptr) || produced != got) {
            error = "cannot write to " + destinationPath + " (error " + std::to_string(GetLastError()) + ")";
            ok = false;
            break;
        }

        if (!hasher.update(buffer.data(), got, error)) {
            ok = false;
            break;
        }

        position += got;
        written += produced;
        sinceCheckpoint += produced;

        if (sinceCheckpoint >= CHECKPOINT_INTERVAL) {
            FlushFileBuffers(destination);
            writeImageState(statePath, sourcePath, start, end, written);
            sinceCheckpoint = 0;
        }

        state.bytesDone = written;
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        const uint64_t thisRun = written - resumeOffset;
        state.bytesPerSecond = elapsed > 0.0 ? static_cast<double>(thisRun) / elapsed : 0.0;

        if (progress && !progress(state)) {
            cancelled = true;
            ok = false;
            error.clear();
            break;
        }
    }

    if (!ok) {
        FlushFileBuffers(destination);
        CloseHandle(destination);
        writeImageState(statePath, sourcePath, start, end, written);
        result.bytesWritten = written;
        result.bytesThisRun = written - resumeOffset;
        result.cancelled = cancelled;
        return false;
    }

    FlushFileBuffers(destination);
    CloseHandle(destination);

    result.sha256 = hasher.finish();
    result.bytesWritten = written;
    result.bytesThisRun = written - resumeOffset;

    DeleteFileW(utf8ToWide(statePath).c_str());
    return true;
}

}
