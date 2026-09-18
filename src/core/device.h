#pragma once

#include <cstdint>
#include <string>

namespace carver {

class RawDevice {
public:
    RawDevice() = default;
    ~RawDevice();

    RawDevice(const RawDevice&) = delete;
    RawDevice& operator=(const RawDevice&) = delete;

    bool open(const std::wstring& path, std::string& error);
    void close();

    bool isOpen() const;
    uint64_t size() const { return size_; }
    uint32_t sectorSize() const { return sectorSize_; }
    const std::wstring& path() const { return path_; }

    bool readAt(uint64_t offset, void* buffer, uint32_t length, uint32_t& bytesRead, std::string& error);

private:
    void* handle_ = nullptr;
    uint64_t size_ = 0;
    uint32_t sectorSize_ = 512;
    std::wstring path_;
};

bool isElevated();

bool ensureDirectoryTree(const std::wstring& path);

std::string wideToUtf8(const std::wstring& value);
std::wstring utf8ToWide(const std::string& value);

}
