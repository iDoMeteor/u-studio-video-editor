#pragma once

// ADR-017: every OS-specific call lives in src/platform/, behind std-only
// interfaces, one implementation file per OS (*_linux.cpp; *_windows.cpp
// with the port). Includes std and OS headers only: no GTK, GLib or MLT.

#include <cstdint>

namespace ustudio::platform {

// This process's id (autosave's owner record, audit A5).
int64_t currentProcessId();

} // namespace ustudio::platform
