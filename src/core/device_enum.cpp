#include "core/device_enum.h"

#include "core/device.h"

#include <windows.h>
#include <winioctl.h>

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

}
