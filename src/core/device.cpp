#include "core/device.h"

#include <windows.h>
#include <winioctl.h>

#include <cstring>

namespace carver {

RawDevice::~RawDevice() {
    close();
}

void RawDevice::close() {
    if (handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
    size_ = 0;
    sectorSize_ = 512;
    path_.clear();
    scratch_.clear();
}

bool RawDevice::isOpen() const {
    return handle_ != nullptr;
}

bool RawDevice::open(const std::wstring& path, std::string& error) {
    close();

    HANDLE handle = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);

    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD lastError = GetLastError();
        error = "cannot open " + wideToUtf8(path) + " (error " + std::to_string(lastError) + ")";
        if (lastError == ERROR_ACCESS_DENIED) {
            error += "; reading raw devices requires an elevated process";
        }
        return false;
    }

    handle_ = handle;
    path_ = path;

    DWORD returned = 0;
    GET_LENGTH_INFORMATION lengthInfo{};
    if (DeviceIoControl(handle, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
                        &lengthInfo, sizeof(lengthInfo), &returned, nullptr)) {
        size_ = static_cast<uint64_t>(lengthInfo.Length.QuadPart);
    } else {
        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(handle, &fileSize)) {
            error = "cannot determine size of " + wideToUtf8(path);
            close();
            return false;
        }
        size_ = static_cast<uint64_t>(fileSize.QuadPart);
    }

    DISK_GEOMETRY_EX geometry{};
    if (DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
                        &geometry, sizeof(geometry), &returned, nullptr)) {
        if (geometry.Geometry.BytesPerSector != 0) {
            sectorSize_ = geometry.Geometry.BytesPerSector;
        }
    }

    return true;
}

bool RawDevice::readAt(uint64_t offset, void* buffer, uint32_t length, uint32_t& bytesRead, std::string& error) {
    bytesRead = 0;

    if (handle_ == nullptr) {
        error = "device is not open";
        return false;
    }

    if (length == 0) {
        return true;
    }

    // A volume or physical disk handle only accepts reads whose offset and length
    // are multiples of the sector size, so round the request outward, read into a
    // sector aligned buffer of our own, then copy back just the window asked for.
    // Reading $Bitmap is what makes this matter: its size is ceil(clusters / 8),
    // which is almost never a whole number of sectors.
    const uint32_t sector = sectorSize_ != 0 ? sectorSize_ : 512u;
    const uint64_t skip = offset % sector;
    const uint64_t alignedStart = offset - skip;
    const uint64_t alignedLength = ((skip + length + sector - 1) / sector) * sector;

    if (alignedLength > 0xFFFFFFFFull) {
        error = "read request is too large at offset " + std::to_string(offset);
        return false;
    }

    const size_t needed = static_cast<size_t>(alignedLength) + sector;
    if (scratch_.size() < needed) {
        scratch_.resize(needed);
    }

    const uintptr_t rawAddress = reinterpret_cast<uintptr_t>(scratch_.data());
    uint8_t* aligned = reinterpret_cast<uint8_t*>(
        (rawAddress + sector - 1) & ~static_cast<uintptr_t>(sector - 1));

    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(alignedStart);
    if (!SetFilePointerEx(static_cast<HANDLE>(handle_), position, nullptr, FILE_BEGIN)) {
        error = "seek failed at offset " + std::to_string(offset) +
                " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }

    DWORD read = 0;
    if (!ReadFile(static_cast<HANDLE>(handle_), aligned, static_cast<DWORD>(alignedLength),
                  &read, nullptr)) {
        error = "read failed at offset " + std::to_string(offset) +
                " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }

    if (static_cast<uint64_t>(read) <= skip) {
        return true;
    }

    uint64_t available = static_cast<uint64_t>(read) - skip;
    if (available > length) {
        available = length;
    }

    std::memcpy(buffer, aligned + skip, static_cast<size_t>(available));
    bytesRead = static_cast<uint32_t>(available);
    return true;
}

bool isElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned);
    CloseHandle(token);

    return ok && elevation.TokenIsElevated != 0;
}

bool ensureDirectoryTree(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }

    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    const DWORD error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
        return false;
    }

    const size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return false;
    }

    const std::wstring parent = path.substr(0, separator);
    if (!parent.empty() && parent.back() != L':') {
        if (!ensureDirectoryTree(parent)) {
            return false;
        }
    }

    if (CreateDirectoryW(path.c_str(), nullptr)) {
        return true;
    }
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }

    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                                        nullptr, 0);
    if (size <= 0) {
        return {};
    }

    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                        result.data(), size);
    return result;
}

}
