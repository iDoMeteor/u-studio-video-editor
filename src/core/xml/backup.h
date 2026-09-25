#pragma once

#include <ctime>
#include <string>

namespace ustudio::core {

// Explicit saves keep a short history of the file they overwrite:
// `<dir>/.ustudio-backups/<stem>-YYYYMMDD-HHMMSS.ustudio`, newest kKeepBackups
// per project. Autosave never calls this -- it writes its own file.
constexpr int kKeepBackups = 5;

// Copies `projectPath` (if it exists) into the backup folder, named by `when`
// in local time, then prunes that project's older backups. Returns an error
// message, empty on success or when there was nothing to back up. Never
// touches `projectPath` itself.
std::string backupBeforeOverwrite(const std::string &projectPath, std::time_t when);

} // namespace ustudio::core
