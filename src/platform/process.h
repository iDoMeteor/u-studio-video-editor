#pragma once

// ADR-017: every OS-specific call lives in src/platform/, behind std-only
// interfaces, one implementation file per OS (*_linux.cpp; *_windows.cpp
// with the port). Includes std and OS headers only: no GTK, GLib or MLT.

#include <cstdint>
#include <filesystem>

namespace ustudio::platform {

// This process's id (autosave's owner record, audit A5).
int64_t currentProcessId();

// This program's own file, resolved ("" if unknown): the schema fallback
// and finding u-studio-render next to the editor.
std::filesystem::path executablePath();
// What executables end in here: "" on Linux, ".exe" on Windows.
const char *executableSuffix();

// Whether a process with this id exists, including one another user owns
// (autosave's owner check). pid <= 0 never does.
bool processExists(int64_t pid);

// When the process started, in an OS-defined unit that's only compared
// for equality (a reused pid starts at another time; autosave, audit A3).
// 0 when unknown (no such process, pid <= 0).
int64_t processStartTime(int64_t pid);

} // namespace ustudio::platform
