#pragma once

// Help's Copy Diagnostics (0.49.0-beta.2): what a tester pastes into a bug
// report. Versions, the sandbox, the log folder and the last log lines;
// never the environment or any file's contents (CLAUDE.md). No GTK here,
// so the text is tested (tests/app/test_diagnostics); the window gathers
// the facts.

#include <string>
#include <vector>

namespace ustudio::app {

struct DiagnosticsFacts
{
    std::string appVersion, mltVersion, gtkVersion, adwaitaVersion;
    bool flatpak = false;
    std::string logFolder;
    std::vector<std::string> recentLines; // oldest first
};

std::string formatDiagnostics(const DiagnosticsFacts &facts);

} // namespace ustudio::app
