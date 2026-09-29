#pragma once

// ADR-017: every OS-specific call lives in src/platform/, behind std-only
// interfaces, one implementation file per OS (*_linux.cpp; *_windows.cpp
// with the port). Includes std and OS headers only: no GTK, GLib or MLT.

#include <atomic>
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

// A child process's cancel (ADR-017 rule 4): the parent asks with
// requestTermination(); the child, which called onTerminationRequest()
// once at startup, sees `flag` set and stops cleanly (a render removes its
// .part file). Linux: SIGTERM, and SIGINT for a terminal's Ctrl+C.
void onTerminationRequest(std::atomic<bool> *flag);
bool requestTermination(int64_t pid);

// Whether a process with this id exists, including one another user owns
// (autosave's owner check). pid <= 0 never does.
bool processExists(int64_t pid);

// When the process started, in an OS-defined unit that's only compared
// for equality (a reused pid starts at another time; autosave, audit A3).
// 0 when unknown (no such process, pid <= 0).
int64_t processStartTime(int64_t pid);

// Whether this process runs inside a Flatpak sandbox (Help's Copy
// Diagnostics). False on other platforms.
bool runningInFlatpak();

// Hands memory the process has freed back to the OS, after a large, short
// job (a worker closing a decoder): the C library otherwise keeps it for
// reuse, so RSS stays at the job's peak. Linux (glibc): malloc_trim(0),
// which returned about 100 MB of a closed 1080p decoder's 180 MB
// (2026-09-29); a musl build would need a guard. Windows: a no-op
// (_heapmin is deprecated for this in the UCRT). Off the main thread only:
// it walks and locks every malloc arena, tens of ms on a big heap.
void releaseFreeMemory();

} // namespace ustudio::platform
