#include "core/carver.h"
#include "core/device.h"
#include "core/device_enum.h"
#include "core/ntfs.h"
#include "core/recover.h"
#include "core/signature.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void printUsage() {
    std::cout <<
        "carver-cli - signature based file carver\n"
        "\n"
        "usage:\n"
        "  carver-cli --list\n"
        "  carver-cli <input> <output-dir> [options]\n"
        "\n"
        "options:\n"
        "  --start <bytes>   first byte to scan (default 0)\n"
        "  --end <bytes>     last byte to scan (default end of input)\n"
        "  --chunk <bytes>   read chunk size (default 8 MiB)\n"
        "  --free-only       scan only unallocated NTFS clusters, using $Bitmap\n"
        "                    (input must be an NTFS volume, e.g. \\\\.\\D:)\n"
        "  --mft-recover     recover deleted files by name from deleted $MFT records\n"
        "                    (input must be an NTFS volume)\n"
        "  --probe <input>   hex dump raw bytes, to check that a device reads correctly\n"
        "        [--offset <bytes>] [--length <bytes>]\n"
        "\n"
        "input is a disk image file, or a raw device such as\n"
        "\\\\.\\PhysicalDrive2 which requires an elevated shell.\n";
}

std::string humanBytes(uint64_t value) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double size = static_cast<double>(value);
    int unit = 0;
    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        ++unit;
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.1f %s", size, units[unit]);
    return buffer;
}

void printDrives() {
    std::cout << "elevated : " << (carver::isElevated() ? "yes" : "no") << "\n\n";

    const auto drives = carver::listPhysicalDrives();
    if (drives.empty()) {
        std::cout << "no physical drives could be opened\n";
        return;
    }

    for (const auto& drive : drives) {
        std::printf("  %-24s %10s  sector %-5u %-8s %s%s\n",
                    drive.devicePath.c_str(),
                    humanBytes(drive.size).c_str(),
                    drive.sectorSize,
                    drive.busType.c_str(),
                    drive.model.c_str(),
                    drive.removable ? "  [removable]" : "");
    }
}

}

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty()) {
        printUsage();
        return 1;
    }

    if (args[0] == "--help" || args[0] == "-h") {
        printUsage();
        return 0;
    }

    if (args[0] == "--list") {
        printDrives();
        return 0;
    }

    if (args[0] == "--probe") {
        std::string path;
        uint64_t offset = 0;
        uint64_t length = 512;

        for (size_t index = 1; index < args.size(); ++index) {
            if (args[index] == "--offset" && index + 1 < args.size()) {
                offset = std::strtoull(args[++index].c_str(), nullptr, 0);
            } else if (args[index] == "--length" && index + 1 < args.size()) {
                length = std::strtoull(args[++index].c_str(), nullptr, 0);
            } else if (path.empty()) {
                path = args[index];
            }
        }

        if (path.empty()) {
            std::cerr << "error: --probe needs an input path\n";
            return 1;
        }

        carver::RawDevice device;
        std::string error;
        if (!device.open(carver::utf8ToWide(path), error)) {
            std::cerr << "error: " << error << "\n";
            return 1;
        }

        std::cout << "input  : " << path << "\n";
        std::cout << "size   : " << humanBytes(device.size()) << " (" << device.size() << " bytes)\n";
        std::cout << "sector : " << device.sectorSize() << " bytes\n\n";

        std::vector<uint8_t> buffer(static_cast<size_t>(std::min<uint64_t>(length, 1u << 20)));
        uint32_t got = 0;
        if (!device.readAt(offset, buffer.data(), static_cast<uint32_t>(buffer.size()), got, error)) {
            std::cerr << "error: " << error << "\n";
            return 1;
        }
        buffer.resize(got);

        for (size_t index = 0; index < buffer.size(); index += 16) {
            std::printf("%08llX  ", static_cast<unsigned long long>(offset + index));
            for (size_t column = 0; column < 16; ++column) {
                if (index + column < buffer.size()) {
                    std::printf("%02X ", buffer[index + column]);
                } else {
                    std::printf("   ");
                }
            }
            std::printf(" |");
            for (size_t column = 0; column < 16 && index + column < buffer.size(); ++column) {
                const uint8_t value = buffer[index + column];
                std::printf("%c", (value >= 0x20 && value < 0x7F) ? static_cast<char>(value) : '.');
            }
            std::printf("|\n");
        }

        std::cout << "\n" << got << " bytes read\n";
        return 0;
    }

    if (args.size() < 2) {
        printUsage();
        return 1;
    }

    carver::CarveOptions options;
    bool freeOnly = false;
    bool mftRecover = false;

    for (size_t index = 2; index < args.size(); ++index) {
        const std::string& flag = args[index];
        if (flag == "--free-only") {
            freeOnly = true;
            continue;
        }
        if (flag == "--mft-recover") {
            mftRecover = true;
            continue;
        }
        if (index + 1 >= args.size()) {
            std::cerr << "error: " << flag << " requires a value\n";
            return 1;
        }
        if (flag == "--start") {
            options.startOffset = std::strtoull(args[++index].c_str(), nullptr, 0);
        } else if (flag == "--end") {
            options.endOffset = std::strtoull(args[++index].c_str(), nullptr, 0);
        } else if (flag == "--chunk") {
            options.chunkSize = std::strtoull(args[++index].c_str(), nullptr, 0);
        } else {
            std::cerr << "error: unknown option " << flag << "\n";
            return 1;
        }
    }

    const std::string inputPath = args[0];
    const std::string outputDirectory = args[1];

    carver::RawDevice device;
    std::string error;
    if (!device.open(carver::utf8ToWide(inputPath), error)) {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    carver::NtfsVolumeInfo volume;
    std::vector<uint8_t> bitmap;

    if (freeOnly || mftRecover) {
        std::vector<uint8_t> bootSector(512, 0);
        uint32_t got = 0;
        if (!device.readAt(0, bootSector.data(), static_cast<uint32_t>(bootSector.size()), got, error) || got < 512) {
            std::cerr << "error: cannot read boot sector: " << error << "\n";
            return 1;
        }
        if (!carver::parseNtfsBootSector(bootSector.data(), got, volume, error)) {
            std::cerr << "error: input is not an NTFS volume: " << error << "\n";
            return 1;
        }
        if (!carver::readBitmap(device, volume, bitmap, error)) {
            std::cerr << "error: cannot read $Bitmap: " << error << "\n";
            return 1;
        }

        std::cout << "filesystem : NTFS, " << volume.bytesPerCluster << " byte clusters\n";
        std::cout << "clusters   : " << volume.totalClusters() << "\n";
        std::cout << "bitmap     : " << bitmap.size() << " bytes\n";
    }

    if (mftRecover) {
        uint64_t recordCount = 0;
        if (!carver::getMftRecordCount(device, volume, recordCount, error)) {
            std::cerr << "error: " << error << "\n";
            return 1;
        }

        std::cout << "mft records: " << recordCount << "\n";
        std::cout << "output     : " << outputDirectory << "\n\n";

        const auto recoverProgress = [](const carver::Progress& state) {
            const double percent = state.bytesTotal == 0
                                        ? 100.0
                                        : (static_cast<double>(state.bytesScanned) /
                                           static_cast<double>(state.bytesTotal)) * 100.0;
            std::printf("\r[%5.1f%%] record %llu  %4llu files  %10s written  %-24s",
                        percent,
                        static_cast<unsigned long long>(state.bytesScanned),
                        static_cast<unsigned long long>(state.filesRecovered),
                        humanBytes(state.bytesRecovered).c_str(),
                        state.currentOutput.c_str());
            std::fflush(stdout);
            return true;
        };

        carver::RecoverOptions recoverOptions;
        std::vector<carver::RecoveredFile> index;

        const carver::RecoverResult recovered = carver::recoverDeletedFiles(
            device, volume, bitmap, outputDirectory, recoverOptions, recoverProgress, index, error);

        std::printf("\n\n");

        if (!error.empty()) {
            std::cerr << "error: " << error << "\n";
        }

        std::cout << "records scanned : " << recovered.recordsScanned << "\n";
        std::cout << "deleted entries : " << recovered.deletedFound << "\n";
        std::cout << "files written   : " << recovered.filesWritten << "\n";
        std::cout << "bytes written   : " << humanBytes(recovered.bytesWritten) << "\n";
        std::cout << "possibly overwritten : " << recovered.atRisk << "\n";
        std::cout << "index           : " << outputDirectory << "\\recovered.csv\n";
        if (recovered.cancelled) {
            std::cout << "cancelled\n";
        }
        return 0;
    }

    if (freeOnly) {
        options.ranges = carver::freeClusterRanges(bitmap, volume.totalClusters(), volume.bytesPerCluster);

        uint64_t freeBytes = 0;
        for (const auto& range : options.ranges) {
            freeBytes += range.end - range.start;
        }

        std::cout << "free       : " << options.ranges.size() << " extents, "
                  << humanBytes(freeBytes) << "\n\n";

        if (options.ranges.empty()) {
            std::cout << "no unallocated clusters found; nothing to scan\n";
            return 0;
        }
    }

    const uint64_t scanEnd = options.endOffset == 0 ? device.size() : options.endOffset;

    std::cout << "input  : " << inputPath << "\n";
    std::cout << "size   : " << humanBytes(device.size()) << " (" << device.size() << " bytes)\n";
    std::cout << "sector : " << device.sectorSize() << " bytes\n";
    std::cout << "output : " << outputDirectory << "\n";
    if (options.ranges.empty()) {
        std::cout << "range  : " << options.startOffset << " .. " << scanEnd << "\n";
    } else {
        std::cout << "range  : unallocated clusters only\n";
    }
    std::cout << "chunk  : " << humanBytes(options.chunkSize) << "\n\n";

    const auto& signatures = carver::defaultSignatures();

    const auto progress = [](const carver::Progress& state) {
        const double percent = state.bytesTotal == 0
                                    ? 100.0
                                    : (static_cast<double>(state.bytesScanned) /
                                       static_cast<double>(state.bytesTotal)) * 100.0;

        std::printf("\r[%5.1f%%] %10s scanned  %4llu files  %10s recovered  %-24s",
                    percent,
                    humanBytes(state.bytesScanned).c_str(),
                    static_cast<unsigned long long>(state.filesRecovered),
                    humanBytes(state.bytesRecovered).c_str(),
                    state.currentOutput.c_str());
        std::fflush(stdout);
        return true;
    };

    const carver::CarveResult result =
        carver::carveDevice(device, outputDirectory, signatures, options, progress, error);

    std::printf("\n\n");

    if (!error.empty()) {
        std::cerr << "error: " << error << "\n";
    }

    std::cout << "files recovered : " << result.filesRecovered << "\n";
    std::cout << "bytes recovered : " << humanBytes(result.bytesRecovered) << "\n";
    std::cout << "bytes scanned   : " << humanBytes(result.bytesScanned) << "\n";
    if (result.cancelled) {
        std::cout << "cancelled\n";
    }

    return 0;
}
