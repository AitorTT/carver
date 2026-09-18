#include "core/carver.h"
#include "core/device.h"
#include "core/device_enum.h"
#include "core/fat.h"
#include "core/image.h"
#include "core/ntfs.h"
#include "core/partition.h"
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
        "  --image <input> <destination>   write a byte-for-byte copy and SHA-256 hash\n"
        "        [--start <bytes>] [--end <bytes>] [--resume] [--force]\n"
        "  --partitions <input>   list the partitions on a disk or image\n"
        "  --partition <n>        use partition n as the NTFS volume (default: auto)\n"
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
        std::cout << "no physical drives could be opened"
                     " (raw device access requires an elevated process)\n";
    } else {
        for (const auto& drive : drives) {
            std::printf("  %-24s %10s  %-4s sector %-5u %-8s %s%s\n",
                        drive.devicePath.c_str(),
                        humanBytes(drive.size).c_str(),
                        drive.rotational ? "HDD" : "SSD",
                        drive.sectorSize,
                        drive.busType.c_str(),
                        drive.model.c_str(),
                        drive.removable ? "  [removable]" : "");
        }
    }

    const auto volumes = carver::listVolumes();
    std::cout << "\nvolumes:\n";
    if (volumes.empty()) {
        std::cout << "  none\n";
        return;
    }

    for (const auto& volume : volumes) {
        const std::string disk = volume.diskNumber == carver::UNKNOWN_PHYSICAL_DISK
                                     ? std::string("?")
                                     : std::to_string(volume.diskNumber);
        std::printf("  %-5s disk %-3s %-7s %10s  %s  (%s)\n",
                    volume.mountPoint.c_str(),
                    disk.c_str(),
                    volume.fileSystem.empty() ? "-" : volume.fileSystem.c_str(),
                    humanBytes(volume.size).c_str(),
                    volume.label.c_str(),
                    volume.devicePath.c_str());
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

    if (args[0] == "--image") {
        std::string sourcePath;
        std::string destinationPath;
        carver::ImageOptions imageOptions;

        for (size_t index = 1; index < args.size(); ++index) {
            if (args[index] == "--start" && index + 1 < args.size()) {
                imageOptions.startOffset = std::strtoull(args[++index].c_str(), nullptr, 0);
            } else if (args[index] == "--end" && index + 1 < args.size()) {
                imageOptions.endOffset = std::strtoull(args[++index].c_str(), nullptr, 0);
            } else if (args[index] == "--chunk" && index + 1 < args.size()) {
                imageOptions.chunkSize = std::strtoull(args[++index].c_str(), nullptr, 0);
            } else if (args[index] == "--resume") {
                imageOptions.resume = true;
            } else if (args[index] == "--force") {
                imageOptions.force = true;
            } else if (sourcePath.empty()) {
                sourcePath = args[index];
            } else if (destinationPath.empty()) {
                destinationPath = args[index];
            }
        }

        if (sourcePath.empty() || destinationPath.empty()) {
            std::cerr << "error: --image needs a source and a destination\n";
            return 1;
        }

        carver::RawDevice device;
        std::string error;
        if (!device.open(carver::utf8ToWide(sourcePath), error)) {
            std::cerr << "error: " << error << "\n";
            return 1;
        }

        const uint64_t begin = std::min(imageOptions.startOffset, device.size());
        const uint64_t finish = (imageOptions.endOffset == 0 || imageOptions.endOffset > device.size())
                                    ? device.size()
                                    : imageOptions.endOffset;
        if (begin >= finish) {
            std::cerr << "error: image range is empty\n";
            return 1;
        }
        const uint64_t imageBytes = finish - begin;

        const std::string destinationMount = carver::mountPointOfPath(destinationPath);
        if (destinationMount.empty()) {
            std::cerr << "error: cannot resolve the destination volume for " << destinationPath << "\n";
            return 1;
        }

        const uint32_t sourceDisk = carver::physicalDiskOfDevicePath(sourcePath);
        const uint32_t destinationDisk = carver::physicalDiskOfMountPoint(destinationMount);
        const bool sourceIsDevice = sourcePath.rfind("\\\\.\\", 0) == 0;
        if (sourceIsDevice &&
            sourceDisk != carver::UNKNOWN_PHYSICAL_DISK &&
            destinationDisk != carver::UNKNOWN_PHYSICAL_DISK &&
            sourceDisk == destinationDisk) {
            std::cerr << "error: refusing to write the image onto the source disk (disk "
                      << sourceDisk << ").\n"
                      << "       Writing there can overwrite the data being imaged.\n"
                      << "       Choose a destination on a different physical disk.\n";
            return 1;
        }

        uint64_t alreadyDone = 0;
        if (imageOptions.resume) {
            std::string resumeError;
            if (!carver::imageResumeOffset(destinationPath, sourcePath, begin, finish, alreadyDone, resumeError)) {
                std::cerr << "error: cannot resume: " << resumeError << "\n";
                return 1;
            }
            imageOptions.startOffset = begin;
            imageOptions.endOffset = finish;
        }

        uint64_t freeBytes = 0;
        uint64_t totalBytes = 0;
        const uint64_t stillNeeded = imageBytes - alreadyDone;
        if (carver::volumeFreeSpace(destinationMount, freeBytes, totalBytes) && freeBytes < stillNeeded) {
            std::cerr << "error: not enough space on " << destinationMount << " for the remaining "
                      << humanBytes(stillNeeded) << " (" << humanBytes(freeBytes) << " free)\n";
            return 1;
        }

        std::cout << "source : " << sourcePath << "\n";
        std::cout << "size   : " << humanBytes(device.size()) << "\n";
        std::cout << "sector : " << device.sectorSize() << " bytes\n";
        std::cout << "range  : " << begin << " .. " << finish << "  (" << humanBytes(imageBytes) << ")\n";
        std::cout << "dest   : " << destinationPath << "\n";
        std::cout << "free   : " << humanBytes(freeBytes) << " on " << destinationMount << "\n";
        if (alreadyDone > 0) {
            std::cout << "resume : continuing from " << humanBytes(alreadyDone) << " ("
                      << humanBytes(stillNeeded) << " left)\n";
        }
        std::cout << "\n";

        const auto imageProgress = [imageBytes](const carver::ImageProgress& state) {
            static const uint64_t stopAfter = [] {
                const char* value = std::getenv("CARVER_IMAGE_STOP_AFTER");
                return value != nullptr ? std::strtoull(value, nullptr, 10) : 0ull;
            }();

            const double percent = state.bytesTotal == 0
                                        ? 100.0
                                        : (static_cast<double>(state.bytesDone) /
                                           static_cast<double>(state.bytesTotal)) * 100.0;
            const double remaining = state.bytesPerSecond > 0.0
                                         ? static_cast<double>(imageBytes - state.bytesDone) / state.bytesPerSecond
                                         : 0.0;
            std::printf("\r[%5.1f%%] %10s  %9.1f MiB/s  ETA %s        ",
                        percent,
                        humanBytes(state.bytesDone).c_str(),
                        state.bytesPerSecond / (1024.0 * 1024.0),
                        humanBytes(static_cast<uint64_t>(remaining)).c_str());
            std::fflush(stdout);

            if (stopAfter > 0 && state.bytesDone >= stopAfter) {
                return false;
            }
            return true;
        };

        carver::ImageResult imageResult;
        const bool imaged = carver::createImage(device, sourcePath, destinationPath, imageOptions,
                                                imageProgress, imageResult, error);
        std::printf("\n\n");

        if (!imaged) {
            std::cerr << "error: " << (error.empty() ? "imaging cancelled" : error) << "\n";
            if (imageResult.bytesWritten > 0) {
                std::cout << "partial image kept: " << humanBytes(imageResult.bytesWritten)
                          << " written\n";
                std::cout << "resume with    : carver-cli --image \"" << sourcePath << "\" \""
                          << destinationPath << "\" --resume\n";
            }
            return 1;
        }

        std::cout << "bytes written : " << imageResult.bytesWritten << " ("
                  << humanBytes(imageResult.bytesWritten) << ")";
        if (imageResult.resumed) {
            std::cout << ", " << humanBytes(imageResult.bytesThisRun) << " this run";
        }
        std::cout << "\n";
        std::cout << "sha256        : " << imageResult.sha256 << "\n";
        std::cout << "\nverify with   : certutil -hashfile \"" << destinationPath << "\" SHA256\n";
        return 0;
    }

    if (args[0] == "--partitions") {
        if (args.size() < 2) {
            std::cerr << "error: --partitions needs an input\n";
            return 1;
        }

        carver::RawDevice device;
        std::string error;
        if (!device.open(carver::utf8ToWide(args[1]), error)) {
            std::cerr << "error: " << error << "\n";
            return 1;
        }

        const auto partitions = carver::parsePartitions(device, error);
        if (!error.empty()) {
            std::cerr << "error: " << error << "\n";
            return 1;
        }

        std::cout << "input : " << args[1] << "\n";
        std::cout << "size  : " << humanBytes(device.size()) << "\n\n";

        if (partitions.empty()) {
            std::cout << "no partition table found (a bare NTFS volume has none; use it directly)\n";
            return 0;
        }

        std::printf("  %-3s %-14s %-12s %-11s %-26s %s\n", "#", "scheme", "offset", "size", "type", "label");
        for (const auto& partition : partitions) {
            const std::string index = partition.index == 0 ? std::string("-") : std::to_string(partition.index);
            std::printf("  %-3s %-14s %-12s %-11s %-26s %s\n",
                        index.c_str(),
                        partition.scheme.c_str(),
                        humanBytes(partition.offset).c_str(),
                        humanBytes(partition.size).c_str(),
                        partition.typeName.c_str(),
                        partition.label.c_str());
        }

        std::cout << "\n";
        for (const auto& partition : partitions) {
            if (partition.index == 0) {
                continue;
            }
            std::cout << "  partition " << partition.index << " at " << partition.offset
                      << " : NTFS boot sector "
                      << (carver::quickNtfsCheck(device, partition.offset) ? "present" : "not found")
                      << "\n";
        }
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
    std::string partitionSelection;

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
        } else if (flag == "--partition") {
            partitionSelection = args[++index];
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
    carver::FatVolumeInfo fatVolume;
    std::vector<uint8_t> bitmap;
    bool usingNtfs = false;

    uint64_t partitionOffset = 0;
    uint64_t partitionSize = device.size();
    bool partitionResolved = false;

    const bool needsNtfsBase = freeOnly || mftRecover || !partitionSelection.empty();

    if (needsNtfsBase) {
        bool resolvedAtZero = false;

        if (partitionSelection.empty()) {
            std::vector<uint8_t> probe(512, 0);
            uint32_t probeGot = 0;
            std::string probeError;
            carver::NtfsVolumeInfo probeNtfs;
            carver::FatVolumeInfo probeFat;

            if (device.readAt(0, probe.data(), static_cast<uint32_t>(probe.size()), probeGot, probeError) &&
                probeGot == 512) {
                if (carver::parseNtfsBootSector(probe.data(), probeGot, probeNtfs, probeError) ||
                    carver::parseFatBootSector(probe.data(), probeGot, probeFat, probeError)) {
                    partitionOffset = 0;
                    partitionSize = device.size();
                    partitionResolved = true;
                    resolvedAtZero = true;
                }
            }
        }

        if (!resolvedAtZero) {
            carver::PartitionResolution resolution;
            if (!carver::resolveNtfsBase(device, partitionSelection, resolution, error)) {
                std::cerr << "error: " << error << "\n";
                return 1;
            }

            partitionOffset = resolution.offset;
            partitionSize = resolution.size;
            partitionResolved = resolution.found;

            if (resolution.autoSelected) {
                std::cout << "auto-selected partition " << resolution.index << " ("
                          << resolution.typeName << ") at offset " << resolution.offset;
                if (!resolution.label.empty()) {
                    std::cout << "  label \"" << resolution.label << "\"";
                }
                std::cout << "\n";
            }
        }
    }

    if (!partitionSelection.empty() && !needsNtfsBase) {
        options.startOffset = partitionOffset;
        options.endOffset = partitionOffset + partitionSize;
    }

    if (freeOnly || mftRecover) {
        if (!partitionResolved) {
            std::cerr << "error: cannot work out which volume to use\n";
            return 1;
        }

        std::vector<uint8_t> bootSector(512, 0);
        uint32_t got = 0;
        if (!device.readAt(partitionOffset, bootSector.data(), static_cast<uint32_t>(bootSector.size()), got, error) ||
            got < 512) {
            std::cerr << "error: cannot read the boot sector at offset " << partitionOffset << ": " << error << "\n";
            return 1;
        }
        usingNtfs = carver::parseNtfsBootSector(bootSector.data(), got, volume, error);

        if (usingNtfs) {
            volume.baseOffset = partitionOffset;
            if (!carver::readBitmap(device, volume, bitmap, error)) {
                std::cerr << "error: cannot read $Bitmap: " << error << "\n";
                return 1;
            }

            std::cout << "filesystem : NTFS, " << volume.bytesPerCluster << " byte clusters\n";
            std::cout << "clusters   : " << volume.totalClusters() << "\n";
            std::cout << "bitmap     : " << bitmap.size() << " bytes\n";
        } else if (freeOnly) {
            std::string fatError;
            if (!carver::parseFatBootSector(bootSector.data(), got, fatVolume, fatError)) {
                std::cerr << "error: unrecognised volume.\n";
                std::cerr << "       not NTFS : " << error << "\n";
                std::cerr << "       not FAT  : " << fatError << "\n";
                return 1;
            }

            fatVolume.baseOffset = partitionOffset;
            error.clear();
            std::cout << "filesystem : " << carver::describeFatKind(fatVolume.kind)
                      << ", " << fatVolume.bytesPerCluster << " byte clusters\n";
            std::cout << "clusters   : " << fatVolume.clusterCount << "\n";
            std::cout << "allocation : "
                      << (fatVolume.kind == carver::FatKind::ExFat ? "exFAT allocation bitmap"
                                                                   : "FAT table")
                      << "\n";
        } else {
            std::cerr << "error: --mft-recover needs an NTFS volume: " << error << "\n";
            return 1;
        }

        const uint32_t diskNumber = carver::physicalDiskOfDevicePath(inputPath);
        bool rotational = true;
        if (carver::physicalDiskIsRotational(diskNumber, rotational) && !rotational) {
            std::cout << "\n";
            std::cout << "WARNING: this target is on a solid-state drive.\n";
            std::cout << "         Deleted data on SSDs is usually erased by TRIM within seconds of\n";
            std::cout << "         deletion, so recovering deleted files from it is unlikely to work.\n";
            std::cout << "         Signature carving of live data still works.\n";
        }
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
        if (usingNtfs) {
            options.ranges = carver::freeClusterRanges(bitmap, volume.totalClusters(),
                                                      volume.bytesPerCluster, volume.baseOffset);
        } else {
            std::string fatError;
            options.ranges = carver::fatFreeRanges(device, fatVolume, fatError);
            if (!fatError.empty()) {
                std::cerr << "error: " << fatError << "\n";
                return 1;
            }
        }

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
