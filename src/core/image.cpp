#include "core/image.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace carver {

namespace {

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

}

bool createImage(RawDevice& source,
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

    HANDLE destination = CreateFileW(utf8ToWide(destinationPath).c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (destination == INVALID_HANDLE_VALUE) {
        error = "cannot create " + destinationPath + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }

    Sha256 hasher;
    if (!hasher.start(error)) {
        CloseHandle(destination);
        DeleteFileW(utf8ToWide(destinationPath).c_str());
        return false;
    }

    const uint64_t chunkSize = std::max<uint64_t>(options.chunkSize, 64 * 1024);
    std::vector<uint8_t> buffer(static_cast<size_t>(chunkSize));

    const uint64_t total = end - start;
    uint64_t position = start;
    const auto began = std::chrono::steady_clock::now();

    ImageProgress state;
    state.bytesTotal = total;

    bool ok = true;

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

        DWORD written = 0;
        if (!WriteFile(destination, buffer.data(), got, &written, nullptr) || written != got) {
            error = "cannot write to " + destinationPath + " (error " + std::to_string(GetLastError()) + ")";
            ok = false;
            break;
        }

        if (!hasher.update(buffer.data(), got, error)) {
            ok = false;
            break;
        }

        position += got;
        result.bytesWritten += written;
        state.bytesDone = result.bytesWritten;

        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        state.bytesPerSecond = elapsed > 0.0 ? static_cast<double>(result.bytesWritten) / elapsed : 0.0;

        if (progress && !progress(state)) {
            result.cancelled = true;
            ok = false;
            error.clear();
            break;
        }
    }

    CloseHandle(destination);

    if (!ok) {
        DeleteFileW(utf8ToWide(destinationPath).c_str());
        result.bytesWritten = 0;
        return false;
    }

    result.sha256 = hasher.finish();
    return true;
}

}
