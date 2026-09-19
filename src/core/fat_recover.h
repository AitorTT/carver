#pragma once

#include "core/fat.h"
#include "core/recover.h"

#include <string>
#include <vector>

namespace carver {

// Recover deleted files from a FAT12/16/32 volume by walking its directory
// entries and reading each deleted file's cluster run. A deleted entry keeps
// its size, start cluster and timestamps; only the first name character is
// overwritten with 0xE5, and a long file name is rebuilt from the preceding
// LFN entries when they are still intact.
//
// exFAT stores directories in a different format and is handled by --free-only
// instead.
RecoverResult recoverDeletedFatFiles(RawDevice& device,
                                     const FatVolumeInfo& info,
                                     const std::string& outputDirectory,
                                     const RecoverOptions& options,
                                     const ProgressFn& progress,
                                     std::vector<RecoveredFile>& index,
                                     std::string& error);

}
