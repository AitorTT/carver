#pragma once

#include "core/recover.h"

#include <string>
#include <vector>

namespace carver {

// Helpers shared by the NTFS and FAT deleted-file recoverers.

// The extension a name ends with, without the dot. A dot at the very end or a
// name with no dot at all yields an empty string, matching how extensionSkipped
// treats a missing extension.
std::string nameExtension(const std::string& name);

// Replaces characters that are illegal in a Windows file name and trims
// trailing spaces and dots. Never returns an empty string.
std::string sanitizeFileName(const std::string& name);

// True for names such as CON or COM1 that cannot be created as files.
bool isReservedDeviceName(const std::string& name);

// Writes the recovered.csv index listing every recovered file.
void writeRecoveredIndex(const std::wstring& path, const std::vector<RecoveredFile>& index);

}
