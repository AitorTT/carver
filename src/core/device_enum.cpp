#include "core/device_enum.h"

#include "core/device.h"

#include <windows.h>
#include <winioctl.h>

#include <cstdlib>
#include <cstring>

namespace carver {

namespace {

std::string describeBusType(STORAGE_BUS_TYPE type) {
    switch (type) {
    case BusTypeScsi: return "SCSI";
    case BusTypeAtapi: return "ATAPI";
    case BusTypeAta: return "ATA";
    case BusType1394: return "IEEE1394";
    case BusTypeSsa: return "SSA";
    case BusTypeFibre: return "Fibre";
    case BusTypeUsb: return "USB";
    case BusTypeRAID: return "RAID";
    case BusTypeiScsi: return "iSCSI";
    case BusTypeSas: return "SAS";
    case BusTypeSata: return "SATA";
    case BusTypeSd: return "SD";
    case BusTypeMmc: return "MMC";
    case BusTypeVirtual: return "Virtual";
    case BusTypeFileBackedVirtual: return "FileBackedVirtual";
    case BusTypeSpaces: return "StorageSpaces";
    case BusTypeNvme: return "NVMe";
    default: return "Unknown";
    }
}

std::string readProductId(const std::vector<uint8_t>& buffer, DWORD returned) {
    const auto* descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
    if (descriptor->ProductIdOffset == 0 || descriptor->ProductIdOffset >= returned) {
        return {};
    }

    const char* product = reinterpret_cast<const char*>(buffer.data() + descriptor->ProductIdOffset);
    std::string model(product);
    while (!model.empty() && (model.back() == ' ' || model.back() == '\0')) {
        model.pop_back();
    }
    return model;
}

}

std::vector<DriveInfo> listPhysicalDrives() {
    std::vector<DriveInfo> drives;

    for (int index = 0; index < 32; ++index) {
        const std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(index);

        HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            continue;
        }

        DriveInfo info;
        info.devicePath = wideToUtf8(path);

        DWORD returned = 0;
        DISK_GEOMETRY_EX geometry{};
        if (DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
                            &geometry, sizeof(geometry), &returned, nullptr)) {
            info.size = static_cast<uint64_t>(geometry.DiskSize.QuadPart);
            info.sectorSize = geometry.Geometry.BytesPerSector;
        }

        STORAGE_PROPERTY_QUERY query{};
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;

        std::vector<uint8_t> buffer(1024);
        if (DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                            buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr)) {
            info.model = readProductId(buffer, returned);
            const auto* descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
            info.busType = describeBusType(descriptor->BusType);
            info.removable = descriptor->RemovableMedia != 0;
        }

        drives.push_back(std::move(info));
        CloseHandle(handle);
    }

    return drives;
}

std::vector<VolumeInfo> listVolumes() {
    std::vector<VolumeInfo> volumes;

    wchar_t roots[512] = {};
    if (GetLogicalDriveStringsW(511, roots) == 0) {
        return volumes;
    }

    for (const wchar_t* root = roots; *root != L'\0'; root += std::wcslen(root) + 1) {
        const UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_REMOTE) {
            continue;
        }

        VolumeInfo info;
        info.mountPoint = wideToUtf8(root);

        std::wstring devicePath = L"\\\\.\\";
        devicePath += root[0];
        devicePath += L':';
        info.devicePath = wideToUtf8(devicePath);

        wchar_t label[256] = {};
        wchar_t fileSystem[64] = {};
        if (GetVolumeInformationW(root, label, 256, nullptr, nullptr, nullptr, fileSystem, 64)) {
            info.label = wideToUtf8(label);
            info.fileSystem = wideToUtf8(fileSystem);
        }

        ULARGE_INTEGER availableToCaller{};
        ULARGE_INTEGER totalBytes{};
        ULARGE_INTEGER totalFree{};
        if (GetDiskFreeSpaceExW(root, &availableToCaller, &totalBytes, &totalFree)) {
            info.size = totalBytes.QuadPart;
            info.free = totalFree.QuadPart;
        }

        info.diskNumber = physicalDiskOfMountPoint(info.mountPoint);

        volumes.push_back(std::move(info));
    }

    return volumes;
}

bool isNtfsVolume(const VolumeInfo& volume) {
    return volume.fileSystem == "NTFS";
}

uint32_t physicalDiskOfDevicePath(const std::string& devicePath) {
    constexpr const char* PHYSICAL_PREFIX = "\\\\.\\PhysicalDrive";

    if (devicePath.rfind(PHYSICAL_PREFIX, 0) == 0) {
        return static_cast<uint32_t>(std::strtoul(devicePath.c_str() + std::strlen(PHYSICAL_PREFIX), nullptr, 10));
    }

    return physicalDiskOfMountPoint(devicePath);
}

uint32_t physicalDiskOfMountPoint(const std::string& mountPoint) {
    const std::string cleaned = mountPoint.empty() ? std::string() : mountPoint;
    if (cleaned.size() < 2 || cleaned[1] != ':') {
        return UNKNOWN_PHYSICAL_DISK;
    }

    std::wstring devicePath = L"\\\\.\\";
    devicePath += static_cast<wchar_t>(std::toupper(static_cast<unsigned char>(cleaned[0])));
    devicePath += L':';

    HANDLE handle = CreateFileW(devicePath.c_str(), 0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return UNKNOWN_PHYSICAL_DISK;
    }

    std::vector<uint8_t> buffer(sizeof(VOLUME_DISK_EXTENTS) + sizeof(DISK_EXTENT) * 16, 0);
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                                    buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr);
    CloseHandle(handle);

    if (!ok) {
        return UNKNOWN_PHYSICAL_DISK;
    }

    const auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
    if (extents->NumberOfDiskExtents == 0) {
        return UNKNOWN_PHYSICAL_DISK;
    }

    return extents->Extents[0].DiskNumber;
}

std::string mountPointOfPath(const std::string& path) {
    if (path.empty()) {
        return {};
    }

    wchar_t buffer[MAX_PATH] = {};
    if (!GetVolumePathNameW(utf8ToWide(path).c_str(), buffer, MAX_PATH)) {
        return {};
    }

    return wideToUtf8(buffer);
}

std::string systemVolumeMountPoint() {
    wchar_t windowsDirectory[MAX_PATH] = {};
    if (GetWindowsDirectoryW(windowsDirectory, MAX_PATH) == 0 || windowsDirectory[0] == L'\0') {
        return {};
    }

    std::wstring mountPoint;
    mountPoint += windowsDirectory[0];
    mountPoint += L":\\";
    return wideToUtf8(mountPoint);
}

}
