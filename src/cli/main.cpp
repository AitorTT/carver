#include "core/carver.h"
#include "core/device.h"
#include "core/device_enum.h"
#include "core/signature.h"

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

    if (args.size() < 2) {
        printUsage();
        return 1;
    }

    carver::CarveOptions options;

    for (size_t index = 2; index < args.size(); ++index) {
        const std::string& flag = args[index];
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

    const uint64_t scanEnd = options.endOffset == 0 ? device.size() : options.endOffset;

    std::cout << "input  : " << inputPath << "\n";
    std::cout << "size   : " << humanBytes(device.size()) << " (" << device.size() << " bytes)\n";
    std::cout << "sector : " << device.sectorSize() << " bytes\n";
    std::cout << "output : " << outputDirectory << "\n";
    std::cout << "range  : " << options.startOffset << " .. " << scanEnd << "\n";
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
