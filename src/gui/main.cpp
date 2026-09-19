#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "core/carver.h"
#include "core/device.h"
#include "core/device_enum.h"
#include "core/ntfs.h"
#include "core/recover.h"
#include "core/signature.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_renderTarget = nullptr;
HWND g_window = nullptr;
bool g_resizePending = false;

enum class SourceKind { Volume, PhysicalDrive, ImageFile };
enum class Mode { CarveWhole, CarveFree, MftRecover };

struct Enumeration {
    std::mutex mutex;
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<bool> finished{false};
    std::atomic<int> generation{0};
    std::vector<carver::DriveInfo> drives;
    std::vector<carver::VolumeInfo> volumes;

    void start() {
        if (worker.joinable()) {
            worker.join();
        }
        running.store(true);
        finished.store(false);
        worker = std::thread([this] {
            std::vector<carver::DriveInfo> foundDrives = carver::listPhysicalDrives();
            std::vector<carver::VolumeInfo> foundVolumes = carver::listVolumes();
            {
                std::lock_guard<std::mutex> lock(mutex);
                drives = std::move(foundDrives);
                volumes = std::move(foundVolumes);
            }
            generation.fetch_add(1);
            finished.store(true);
            running.store(false);
        });
    }

    void join() {
        if (worker.joinable()) {
            worker.join();
        }
    }
};

struct Job {
    std::mutex mutex;
    std::thread worker;
    std::atomic<bool> cancel{false};
    bool running = false;
    bool finished = false;
    std::string error;
    std::vector<std::string> log;

    carver::Progress progress;
    uint64_t filesWritten = 0;
    uint64_t bytesWritten = 0;
    uint64_t atRisk = 0;
    uint64_t recordsScanned = 0;
    uint64_t deletedFound = 0;
    uint64_t loggedFiles = 0;

    void addLine(const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex);
        log.push_back(line);
        if (log.size() > 500) {
            log.erase(log.begin(), log.begin() + 100);
        }
    }
};

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

std::wstring buildParameterString(LPWSTR* argv, int argc) {
    std::wstring parameters;
    for (int index = 1; index < argc; ++index) {
        if (!parameters.empty()) {
            parameters += L' ';
        }
        parameters += L'"';
        parameters += argv[index];
        parameters += L'"';
    }
    return parameters;
}

bool relaunchElevated(const std::wstring& parameters) {
    wchar_t modulePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) == 0) {
        return false;
    }

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = modulePath;
    info.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
    info.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&info)) {
        return false;
    }
    if (info.hProcess != nullptr) {
        CloseHandle(info.hProcess);
    }
    return true;
}

std::string describeBusType(const std::string& bus) {
    return bus.empty() ? std::string("unknown") : bus;
}

bool createDeviceD3D(HWND window) {
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 2;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferDesc.RefreshRate.Numerator = 60;
    description.BufferDesc.RefreshRate.Denominator = 1;
    description.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL obtained{};

    const HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &description, &g_swapChain, &g_device, &obtained, &g_context);

    if (result != S_OK) {
        return false;
    }

    ID3D11Texture2D* backBuffer = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer != nullptr) {
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget);
        backBuffer->Release();
    }
    return true;
}

void cleanupRenderTarget() {
    if (g_renderTarget != nullptr) {
        g_renderTarget->Release();
        g_renderTarget = nullptr;
    }
}

void cleanupDeviceD3D() {
    cleanupRenderTarget();
    if (g_swapChain != nullptr) {
        g_swapChain->Release();
        g_swapChain = nullptr;
    }
    if (g_context != nullptr) {
        g_context->Release();
        g_context = nullptr;
    }
    if (g_device != nullptr) {
        g_device->Release();
        g_device = nullptr;
    }
}

void createRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer != nullptr) {
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget);
        backBuffer->Release();
    }
}

LRESULT WINAPI wndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam)) {
        return true;
    }

    switch (message) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_resizePending = true;
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) {
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

std::string browseForFile() {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW options{};
    options.lStructSize = sizeof(options);
    options.hwndOwner = g_window;
    options.lpstrFilter = L"Disk images (*.img;*.dd;*.vhd;*.vhdx;*.iso)\0*.img;*.dd;*.vhd;*.vhdx;*.iso\0All files\0*.*\0";
    options.lpstrFile = path;
    options.nMaxFile = MAX_PATH;
    options.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameW(&options) == TRUE) {
        return carver::wideToUtf8(path);
    }
    return {};
}

std::string browseForFolder() {
    wchar_t path[MAX_PATH] = {};
    BROWSEINFOW info{};
    info.hwndOwner = g_window;
    info.lpszTitle = L"Select the folder that will receive recovered files";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST item = SHBrowseForFolderW(&info);
    if (item != nullptr) {
        SHGetPathFromIDListW(item, path);
        CoTaskMemFree(item);
        return carver::wideToUtf8(path);
    }
    return {};
}

void logRecoveredFile(Job& job, const std::string& name, uint64_t size, bool risk) {
    job.addLine("  " + name + "  (" + humanBytes(size) + ")" + (risk ? "  [overwrite risk]" : ""));
}

void runJob(Job& job,
            const std::string& sourcePath,
            const std::string& outputDirectory,
            Mode mode,
            const std::string& skipExtensionText,
            bool listOnly) {
    const std::vector<std::string> skipExtensions = carver::parseExtensionList(skipExtensionText);

    job.running = true;
    job.finished = false;
    job.error.clear();
    job.progress = carver::Progress{};
    job.filesWritten = 0;
    job.bytesWritten = 0;
    job.atRisk = 0;
    job.recordsScanned = 0;
    job.deletedFound = 0;
    job.loggedFiles = 0;

    carver::RawDevice device;
    std::string error;

    job.addLine("opening " + sourcePath);
    if (!device.open(carver::utf8ToWide(sourcePath), error)) {
        job.addLine("error: " + error);
        job.error = error;
        job.running = false;
        job.finished = true;
        return;
    }

    job.addLine("size  : " + humanBytes(device.size()));
    job.addLine("sector: " + std::to_string(device.sectorSize()) + " bytes");
    job.addLine("output: " + outputDirectory);

    const auto onProgress = [&job](const carver::Progress& state) {
        std::string recoveredName;
        uint64_t recoveredSize = 0;
        {
            std::lock_guard<std::mutex> lock(job.mutex);
            job.progress = state;

            // Report each file as it lands, so the log shows progress during the
            // run rather than only in the summary at the end. addLine takes this
            // same mutex, so the call itself has to happen outside the lock.
            if (state.filesRecovered > job.loggedFiles && !state.currentOutput.empty()) {
                job.loggedFiles = state.filesRecovered;
                recoveredName = state.currentOutput;
                recoveredSize = state.currentSize;
            }
        }

        if (!recoveredName.empty()) {
            job.addLine("  " + recoveredName + "  (" + humanBytes(recoveredSize) + ")");
        }

        return !job.cancel.load();
    };

    if (mode == Mode::MftRecover) {
        carver::NtfsVolumeInfo volume;
        std::vector<uint8_t> bootSector(512, 0);
        uint32_t got = 0;

        if (!device.readAt(0, bootSector.data(), 512, got, error) || got < 512) {
            job.error = "cannot read boot sector: " + error;
        } else if (!carver::parseNtfsBootSector(bootSector.data(), got, volume, error)) {
            job.error = "not an NTFS volume: " + error;
        }

        std::vector<uint8_t> bitmap;
        if (job.error.empty() && !carver::readBitmap(device, volume, bitmap, error)) {
            job.error = "cannot read the cluster bitmap: " + error;
        }

        if (job.error.empty()) {
            job.addLine("filesystem: NTFS, " + std::to_string(volume.bytesPerCluster) + " byte clusters");

            carver::RecoverOptions options;
            options.skipExtensions = skipExtensions;
            options.listOnly = listOnly;
            std::vector<carver::RecoveredFile> index;

            const carver::RecoverResult result = carver::recoverDeletedFiles(
                device, volume, bitmap, outputDirectory, options, onProgress, index, error);

            if (!error.empty()) {
                job.error = error;
            }

            job.recordsScanned = result.recordsScanned;
            job.deletedFound = result.deletedFound;
            job.filesWritten = result.filesWritten;
            job.bytesWritten = result.bytesWritten;
            job.atRisk = result.atRisk;

            for (const auto& file : index) {
                logRecoveredFile(job, file.originalName + "  ->  " + file.outputName, file.size, file.overwriteRisk);
            }
        }
    } else {
        carver::CarveOptions options;
        options.skipExtensions = skipExtensions;
        options.listOnly = listOnly;

        if (mode == Mode::CarveFree) {
            carver::NtfsVolumeInfo volume;
            std::vector<uint8_t> bootSector(512, 0);
            uint32_t got = 0;

            if (!device.readAt(0, bootSector.data(), 512, got, error) || got < 512) {
                job.error = "cannot read boot sector: " + error;
            } else if (!carver::parseNtfsBootSector(bootSector.data(), got, volume, error)) {
                job.error = "not an NTFS volume: " + error;
            }

            std::vector<uint8_t> bitmap;
            if (job.error.empty() && !carver::readBitmap(device, volume, bitmap, error)) {
                job.error = "cannot read the cluster bitmap: " + error;
            }

            if (job.error.empty()) {
                options.ranges = carver::freeClusterRanges(bitmap, volume.totalClusters(), volume.bytesPerCluster);
                job.addLine("unallocated extents: " + std::to_string(options.ranges.size()));
                if (options.ranges.empty()) {
                    job.error = "no unallocated clusters found";
                }
            }
        }

        if (job.error.empty()) {
            const auto& signatures = carver::defaultSignatures();
            const carver::CarveResult result =
                carver::carveDevice(device, outputDirectory, signatures, options, onProgress, error);

            if (!error.empty()) {
                job.error = error;
            }
            job.filesWritten = result.filesRecovered;
            job.bytesWritten = result.bytesRecovered;
        }
    }

    if (job.cancel.load()) {
        job.addLine("cancelled");
    } else if (!job.error.empty()) {
        job.addLine("error: " + job.error);
    } else {
        job.addLine((listOnly ? "listed: " : "done: ") + std::to_string(job.filesWritten) +
                    " files, " + humanBytes(job.bytesWritten) +
                    (listOnly ? "  (nothing was written)" : ""));
    }

    job.running = false;
    job.finished = true;
}

struct UiState {
    SourceKind source = SourceKind::Volume;
    Mode mode = Mode::MftRecover;
    int selectedDrive = -1;
    int selectedVolume = -1;
    char imagePath[512] = {};
    char outputPath[512] = {};
    char skipExtensions[256] = {};
    bool listOnly = true;
    std::vector<carver::DriveInfo> drives;
    std::vector<carver::VolumeInfo> volumes;
    bool selectionInitialised = false;
    int seenGeneration = -1;
    bool autoScroll = true;
};

void buildUi(UiState& ui, Job& job, Enumeration& enumeration) {
    const bool busy = job.running;
    const bool enumerating = enumeration.running.load();

    if (!carver::isElevated()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                           "Not elevated: physical drives cannot be opened. Restart as administrator.");
        ImGui::Separator();
    }

    ImGui::BeginChild("left", ImVec2(430, 0), ImGuiChildFlags_Borders);

    ImGui::TextUnformatted("Source");
    ImGui::Separator();

    ImGui::BeginDisabled(busy);
    if (ImGui::RadioButton("Volume", ui.source == SourceKind::Volume)) {
        ui.source = SourceKind::Volume;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Physical drive", ui.source == SourceKind::PhysicalDrive)) {
        ui.source = SourceKind::PhysicalDrive;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Image file", ui.source == SourceKind::ImageFile)) {
        ui.source = SourceKind::ImageFile;
    }
    ImGui::EndDisabled();

    ImGui::Spacing();

    if (ui.source == SourceKind::ImageFile) {
        ImGui::BeginDisabled(busy);
        ImGui::SetNextItemWidth(-90);
        ImGui::InputText("##image", ui.imagePath, sizeof(ui.imagePath));
        ImGui::SameLine();
        if (ImGui::Button("Browse...", ImVec2(80, 0))) {
            const std::string picked = browseForFile();
            if (!picked.empty()) {
                std::snprintf(ui.imagePath, sizeof(ui.imagePath), "%s", picked.c_str());
            }
        }
        ImGui::EndDisabled();
    } else {
        ImGui::BeginDisabled(busy || enumerating);
        if (ImGui::Button("Refresh")) {
            enumeration.start();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (enumerating) {
            ImGui::TextDisabled("enumerating drives...");
        } else {
            ImGui::TextDisabled("%d volumes, %d drives", static_cast<int>(ui.volumes.size()),
                                static_cast<int>(ui.drives.size()));
        }

        ImGui::BeginChild("targets", ImVec2(0, 210), ImGuiChildFlags_Borders);

        if (ui.source == SourceKind::Volume) {
            if (ui.volumes.empty()) {
                ImGui::TextDisabled(enumerating ? "searching..." : "no volumes found");
            }
            for (int index = 0; index < static_cast<int>(ui.volumes.size()); ++index) {
                const auto& volume = ui.volumes[index];
                char label[320];
                std::snprintf(label, sizeof(label), "%-4s %-5s %-6s %9s  %s", volume.mountPoint.c_str(),
                              volume.rotational ? "HDD" : "SSD",
                              volume.fileSystem.c_str(), humanBytes(volume.size).c_str(),
                              volume.label.c_str());
                if (ImGui::Selectable(label, ui.selectedVolume == index)) {
                    ui.selectedVolume = index;
                }
            }
        } else {
            if (ui.drives.empty()) {
                ImGui::TextDisabled(enumerating ? "searching..." : "no drives found");
            }
            for (int index = 0; index < static_cast<int>(ui.drives.size()); ++index) {
                const auto& drive = ui.drives[index];
                char label[320];
                std::snprintf(label, sizeof(label), "%-22s %-5s %9s  %-7s %s", drive.devicePath.c_str(),
                              drive.rotational ? "HDD" : "SSD", humanBytes(drive.size).c_str(),
                              describeBusType(drive.busType).c_str(), drive.model.c_str());
                if (ImGui::Selectable(label, ui.selectedDrive == index)) {
                    ui.selectedDrive = index;
                }
            }
        }

        ImGui::EndChild();
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Output folder");
    ImGui::Separator();
    ImGui::BeginDisabled(busy);
    ImGui::SetNextItemWidth(-90);
    ImGui::InputText("##output", ui.outputPath, sizeof(ui.outputPath));
    ImGui::SameLine();
    if (ImGui::Button("Browse##output", ImVec2(80, 0))) {
        const std::string picked = browseForFolder();
        if (!picked.empty()) {
            std::snprintf(ui.outputPath, sizeof(ui.outputPath), "%s", picked.c_str());
        }
    }
    ImGui::EndDisabled();

    if (ui.outputPath[0] != '\0') {
        const std::string outputMount = carver::mountPointOfPath(ui.outputPath);
        uint64_t freeBytes = 0;
        uint64_t totalBytes = 0;
        if (!outputMount.empty() && carver::volumeFreeSpace(outputMount, freeBytes, totalBytes)) {
            ImGui::TextDisabled("free space: %s of %s on %s", humanBytes(freeBytes).c_str(),
                                humanBytes(totalBytes).c_str(), outputMount.c_str());
        }
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Mode");
    ImGui::Separator();
    ImGui::BeginDisabled(busy);
    if (ImGui::RadioButton("Recover deleted files by name (NTFS)", ui.mode == Mode::MftRecover)) {
        ui.mode = Mode::MftRecover;
    }
    if (ImGui::RadioButton("Carve signatures from unallocated space (NTFS)", ui.mode == Mode::CarveFree)) {
        ui.mode = Mode::CarveFree;
    }
    if (ImGui::RadioButton("Carve signatures from the whole source", ui.mode == Mode::CarveWhole)) {
        ui.mode = Mode::CarveWhole;
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::TextUnformatted("Skip extensions");
    ImGui::Separator();
    ImGui::BeginDisabled(busy);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##skipext", "e.g. epub, pdf   (empty recovers every type)",
                             ui.skipExtensions, sizeof(ui.skipExtensions));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Case insensitive; a leading dot is optional.\n"
                          "MFT mode matches the original file name, so '.epub' really\n"
                          "skips EPUB books. Carving only sees headers, and EPUB, DOCX,\n"
                          "APK and JAR all share the zip signature, so carving cannot\n"
                           "tell them apart from a zip.");
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::BeginDisabled(busy);
    ImGui::Checkbox("List only, do not write any files", &ui.listOnly);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Logs every file name and size, and still writes recovered.csv,\n"
                          "but copies no data. Handy for sizing up a drive before spending\n"
                          "the disk space. On by default.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    std::string resolvedSource;
    bool targetIsNtfsVolume = false;
    bool targetRotational = true;

    if (ui.source == SourceKind::Volume) {
        if (ui.selectedVolume >= 0 && ui.selectedVolume < static_cast<int>(ui.volumes.size())) {
            const auto& volume = ui.volumes[ui.selectedVolume];
            resolvedSource = volume.devicePath;
            targetIsNtfsVolume = volume.fileSystem == "NTFS";
            targetRotational = volume.rotational;
        }
    } else if (ui.source == SourceKind::PhysicalDrive) {
        if (ui.selectedDrive >= 0 && ui.selectedDrive < static_cast<int>(ui.drives.size())) {
            resolvedSource = ui.drives[ui.selectedDrive].devicePath;
            targetRotational = ui.drives[ui.selectedDrive].rotational;
        }
    } else {
        resolvedSource = ui.imagePath;
    }

    const bool needsNtfsVolume = ui.mode != Mode::CarveWhole;

    if (needsNtfsVolume && !resolvedSource.empty() && !targetIsNtfsVolume) {
        const char* reason = ui.source == SourceKind::PhysicalDrive
                                 ? "this mode needs a volume, not a whole disk"
                                 : "this mode needs an NTFS volume";
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Warning: %s", reason);
        ImGui::Spacing();
    }

    if (ui.mode == Mode::MftRecover && !targetRotational) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                           "Warning: target is a solid-state drive.");
        ImGui::TextWrapped("Deleted data on SSDs is usually erased by TRIM within seconds of "
                           "deletion, so little or nothing may be recoverable. Imaging the drive "
                           "and carving live data still works.");
        ImGui::Spacing();
    }

    bool sameDisk = false;
    if (ui.source != SourceKind::ImageFile && !resolvedSource.empty() && ui.outputPath[0] != '\0') {
        const std::string outputMount = carver::mountPointOfPath(ui.outputPath);
        if (!outputMount.empty()) {
            const uint32_t sourceDisk = carver::physicalDiskOfDevicePath(resolvedSource);
            const uint32_t outputDisk = carver::physicalDiskOfMountPoint(outputMount);
            sameDisk = sourceDisk != carver::UNKNOWN_PHYSICAL_DISK &&
                       outputDisk != carver::UNKNOWN_PHYSICAL_DISK &&
                       sourceDisk == outputDisk;
        }
    }

    if (sameDisk) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                           "Blocked: the output folder is on the same physical disk as the source.");
        ImGui::TextWrapped("Writing recovered files there can overwrite the very clusters being "
                           "recovered. Choose a folder on a different disk.");
        ImGui::Spacing();
    }

    const bool canStart = !busy && !sameDisk && !resolvedSource.empty() && ui.outputPath[0] != '\0';

    if (busy) {
        if (ImGui::Button("Stop", ImVec2(140, 32))) {
            job.cancel.store(true);
        }
    } else {
        ImGui::BeginDisabled(!canStart);
        if (ImGui::Button("Start scan", ImVec2(140, 32))) {
            if (job.worker.joinable()) {
                job.worker.join();
            }
            job.cancel.store(false);
            const std::string source = resolvedSource;
            const std::string output = ui.outputPath;
            const std::string skip = ui.skipExtensions;
            const Mode mode = ui.mode;
            const bool listOnly = ui.listOnly;
            job.worker = std::thread([&job, source, output, mode, skip, listOnly] {
                runJob(job, source, output, mode, skip, listOnly);
            });
        }
        ImGui::EndDisabled();
        if (!canStart) {
            ImGui::SameLine();
            ImGui::TextDisabled("choose a source and an output folder");
        }
    }

    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("right", ImVec2(0, 0), ImGuiChildFlags_Borders);

    carver::Progress snapshot;
    std::vector<std::string> lines;
    bool finished = false;
    {
        std::lock_guard<std::mutex> lock(job.mutex);
        snapshot = job.progress;
        lines = job.log;
        finished = job.finished;
    }

    const bool recordBased = (ui.mode == Mode::MftRecover);

    const double fraction = snapshot.bytesTotal == 0
                                ? (finished ? 1.0 : 0.0)
                                : static_cast<double>(snapshot.bytesScanned) / static_cast<double>(snapshot.bytesTotal);

    char overlay[160];
    if (recordBased) {
        std::snprintf(overlay, sizeof(overlay), "%.1f%%  (%llu of %llu MFT records)",
                      fraction * 100.0,
                      static_cast<unsigned long long>(snapshot.bytesScanned),
                      static_cast<unsigned long long>(snapshot.bytesTotal));
    } else {
        std::snprintf(overlay, sizeof(overlay), "%.1f%%  (%.1f MiB of %.1f MiB)",
                      fraction * 100.0,
                      static_cast<double>(snapshot.bytesScanned) / (1024.0 * 1024.0),
                      static_cast<double>(snapshot.bytesTotal) / (1024.0 * 1024.0));
    }
    ImGui::ProgressBar(static_cast<float>(fraction), ImVec2(-1, 22), overlay);

    if (recordBased) {
        ImGui::Text("files recovered: %llu      bytes written: %s",
                    static_cast<unsigned long long>(job.filesWritten),
                    humanBytes(job.bytesWritten).c_str());
    } else {
        ImGui::Text("files recovered: %llu      bytes written: %s      scanned: %s",
                    static_cast<unsigned long long>(job.filesWritten),
                    humanBytes(job.bytesWritten).c_str(),
                    humanBytes(snapshot.bytesScanned).c_str());
    }

    if (job.recordsScanned > 0) {
        ImGui::Text("mft records: %llu      deleted entries: %llu      possibly overwritten: %llu",
                    static_cast<unsigned long long>(job.recordsScanned),
                    static_cast<unsigned long long>(job.deletedFound),
                    static_cast<unsigned long long>(job.atRisk));
    }

    if (!job.error.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "error: %s", job.error.c_str());
    }

    ImGui::Separator();
    ImGui::Checkbox("auto-scroll log", &ui.autoScroll);
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& line : lines) {
        ImGui::TextUnformatted(line.c_str());
    }
    if (ui.autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::EndChild();
}

int runSelfTest(const std::string& logPath) {
    std::string report;
    int status = 0;

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* className = L"carverSelfTest";

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = wndProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    RegisterClassExW(&windowClass);

    g_window = CreateWindowExW(0, className, L"carver selftest", WS_OVERLAPPEDWINDOW,
                               0, 0, 1280, 800, nullptr, nullptr, instance, nullptr);

    if (g_window == nullptr) {
        report = "FAIL create window\n";
        status = 1;
    } else if (!createDeviceD3D(g_window)) {
        report = "FAIL create D3D11 device and swap chain\n";
        status = 1;
    } else {
        ShowWindow(g_window, SW_HIDE);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui_ImplWin32_Init(g_window);
        ImGui_ImplDX11_Init(g_device, g_context);

        report += "imgui version : " + std::string(IMGUI_VERSION) + "\n";
        report += "d3d device    : ok\n";
        report += "backends      : win32 + dx11 ok\n";

        int framesRendered = 0;
        for (int frame = 0; frame < 3; ++frame) {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            ImGui::Begin("selftest");
            ImGui::Text("frame %d", frame);
            ImGui::ProgressBar(0.5f, ImVec2(-1, 20));
            ImGui::End();

            ImGui::Render();

            const float clear[4] = {0.1f, 0.1f, 0.12f, 1.0f};
            g_context->OMSetRenderTargets(1, &g_renderTarget, nullptr);
            g_context->ClearRenderTargetView(g_renderTarget, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_swapChain->Present(1, 0);

            if (ImGui::GetDrawData() != nullptr && ImGui::GetDrawData()->Valid) {
                ++framesRendered;
            }
        }

        report += "frames drawn  : " + std::to_string(framesRendered) + " of 3\n";

        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        if (framesRendered != 3) {
            report += "FAIL: not all frames produced valid draw data\n";
            status = 1;
        } else {
            report += "RESULT: PASS\n";
        }
    }

    if (status != 0 && report.find("RESULT") == std::string::npos) {
        report += "RESULT: FAIL\n";
    }

    cleanupDeviceD3D();
    if (g_window != nullptr) {
        DestroyWindow(g_window);
    }
    UnregisterClassW(className, instance);

    FILE* file = nullptr;
    if (fopen_s(&file, logPath.c_str(), "w") == 0 && file != nullptr) {
        fwrite(report.data(), 1, report.size(), file);
        fclose(file);
    }

    return status;
}

}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    const std::string selftestFlag = "--selftest";
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    std::string logPath = "carver-gui-selftest.log";
    bool selfTest = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = carver::wideToUtf8(argv[index]);
        if (argument == selftestFlag) {
            selfTest = true;
        } else if (index > 0) {
            logPath = argument;
        }
    }

    const std::wstring parameters = buildParameterString(argv, argc);
    if (argv != nullptr) {
        LocalFree(argv);
    }

    if (selfTest) {
        return runSelfTest(logPath);
    }

    if (!carver::isElevated()) {
        if (relaunchElevated(parameters)) {
            return 0;
        }
        MessageBoxW(nullptr,
                    L"carver needs administrator rights to read physical drives.\n\n"
                    L"The elevated launch was cancelled, so carver will exit.",
                    L"carver", MB_OK | MB_ICONWARNING);
        return 1;
    }

    const wchar_t* className = L"carverWindowClass";
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = wndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.lpszClassName = className;
    RegisterClassExW(&windowClass);

    const std::wstring windowTitle = carver::isElevated()
                                         ? std::wstring(L"carver - file recovery  [administrator]")
                                         : std::wstring(L"carver - file recovery");

    g_window = CreateWindowExW(0, className, windowTitle.c_str(), WS_OVERLAPPEDWINDOW,
                               100, 100, 1180, 780, nullptr, nullptr, instance, nullptr);
    if (g_window == nullptr) {
        cleanupDeviceD3D();
        UnregisterClassW(className, instance);
        return 1;
    }

    if (!createDeviceD3D(g_window)) {
        cleanupDeviceD3D();
        DestroyWindow(g_window);
        UnregisterClassW(className, instance);
        return 1;
    }

    ShowWindow(g_window, SW_SHOWDEFAULT);
    UpdateWindow(g_window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_window);
    ImGui_ImplDX11_Init(g_device, g_context);

    UiState ui;
    Job job;
    Enumeration enumeration;
    enumeration.start();

    bool running = true;
    while (running) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) {
                running = false;
            }
        }
        if (!running) {
            break;
        }

        if (g_resizePending) {
            cleanupRenderTarget();
            g_swapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
            createRenderTarget();
            g_resizePending = false;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("carver", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

        if (enumeration.finished.load() && enumeration.generation.load() != ui.seenGeneration) {
            const int generation = enumeration.generation.load();
            {
                std::lock_guard<std::mutex> lock(enumeration.mutex);
                ui.drives = enumeration.drives;
                ui.volumes = enumeration.volumes;
            }
            ui.seenGeneration = generation;

            if (!ui.selectionInitialised && (!ui.volumes.empty() || !ui.drives.empty())) {
                ui.selectionInitialised = true;
                const std::string systemMount = carver::systemVolumeMountPoint();

                for (int index = 0; index < static_cast<int>(ui.volumes.size()); ++index) {
                    if (carver::isNtfsVolume(ui.volumes[index]) &&
                        ui.volumes[index].mountPoint != systemMount) {
                        ui.selectedVolume = index;
                        break;
                    }
                }
                if (ui.selectedVolume < 0) {
                    for (int index = 0; index < static_cast<int>(ui.volumes.size()); ++index) {
                        if (ui.volumes[index].mountPoint != systemMount) {
                            ui.selectedVolume = index;
                            break;
                        }
                    }
                }
                if (ui.selectedVolume < 0 && !ui.volumes.empty()) {
                    ui.selectedVolume = 0;
                }
                if (ui.selectedVolume < 0 && !ui.drives.empty()) {
                    ui.selectedDrive = 0;
                }
            }
        }

        buildUi(ui, job, enumeration);
        ImGui::End();

        ImGui::Render();

        const float clear[4] = {0.09f, 0.09f, 0.11f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_renderTarget, nullptr);
        g_context->ClearRenderTargetView(g_renderTarget, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0);
    }

    job.cancel.store(true);
    if (job.worker.joinable()) {
        job.worker.join();
    }
    enumeration.join();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanupDeviceD3D();
    DestroyWindow(g_window);
    UnregisterClassW(className, instance);
    return 0;
}
