#pragma once

// Export from the designer (doc 16, T2d): a title on its own, with alpha
// or flattened, by `u-studio-render --title-export` in a child process (the
// designer has no MLT).

#include <gio/gio.h>

#include <functional>
#include <string>
#include <vector>

namespace ustudio::titles::app {

// The render tool: next to this program, else this build's, else PATH.
std::string renderToolPath();

// Runs the export; `done` gets an empty string on success, else why not.
// Cancelling `cancellable` (a closing window) drops the result unseen.
void exportTitle(const std::string &title, const std::string &output, const std::string &format, double seconds,
                 GCancellable *cancellable, std::function<void(const std::string &error)> done);

} // namespace ustudio::titles::app
