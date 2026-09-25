#pragma once

#include "render_queue.h"

#include <string>
#include <vector>

namespace ustudio::app::pending_renders {

// Renders that were running or queued when the app quit, kept so the next
// launch can offer to restart them: per job, the project as it was when
// queued (render-<n>.ustudio) and the profile and output (render-<n>.ini).

// $XDG_STATE_HOME/ustudio/pending-renders
std::string directory();

// Replaces what's in `dir` with `jobs`. Returns an error, or "".
std::string save(const std::vector<RenderJob> &jobs, const std::string &dir);

struct Pending
{
    std::string projectPath; // the snapshot, to load
    core::RenderProfile profile;
    std::string outputPath;
};

// In the order they were queued; unreadable entries are skipped.
std::vector<Pending> list(const std::string &dir);

// Removes every pending render's files from `dir`.
void clear(const std::string &dir);

} // namespace ustudio::app::pending_renders
