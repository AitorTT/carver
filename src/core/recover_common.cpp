#include "core/recover_common.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace carver {

namespace {

std::string csvField(const std::string& value) {
    if (value.find_first_of(",\"\n") == std::string::npos) {
        return value;
    }
    std::string out = "\"";
    for (char character : value) {
        if (character == '"') {
            out += "\"\"";
        } else {
            out.push_back(character);
        }
    }
    out += "\"";
    return out;
}

}

std::string nameExtension(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) {
        return {};
    }
    return name.substr(dot + 1);
}

std::string sanitizeFileName(const std::string& name) {
    std::string cleaned;
    cleaned.reserve(name.size());

    for (unsigned char character : name) {
        const bool invalid = character < 0x20 || character == '<' || character == '>' ||
                             character == ':' || character == '"' || character == '/' ||
                             character == '\\' || character == '|' || character == '?' ||
                             character == '*';
        cleaned.push_back(invalid ? '_' : static_cast<char>(character));
    }

    while (!cleaned.empty() && (cleaned.back() == ' ' || cleaned.back() == '.')) {
        cleaned.pop_back();
    }

    return cleaned.empty() ? std::string("unnamed") : cleaned;
}

bool isReservedDeviceName(const std::string& name) {
    static const char* const reserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };

    std::string stem = name.substr(0, name.find('.'));
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char character) { return static_cast<char>(std::toupper(character)); });

    for (const char* candidate : reserved) {
        if (stem == candidate) {
            return true;
        }
    }
    return false;
}

void writeRecoveredIndex(const std::wstring& path, const std::vector<RecoveredFile>& index) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }

    std::string text = "output_name,original_name,size,record,resident,clusters_free,overwrite_risk,created,modified\n";
    for (const auto& file : index) {
        text += csvField(file.outputName) + "," +
                csvField(file.originalName) + "," +
                std::to_string(file.size) + "," +
                std::to_string(file.recordNumber) + "," +
                (file.resident ? "yes" : "no") + "," +
                (file.clustersStillFree ? "yes" : "no") + "," +
                (file.overwriteRisk ? "yes" : "no") + "," +
                csvField(file.created) + "," +
                csvField(file.modified) + "\n";
    }

    DWORD written = 0;
    WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(handle);
}

}
