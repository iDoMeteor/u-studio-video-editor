#pragma once

// Opening a title in U Stu Titles from the editor (doc 16, "Editor
// integration"): a separate process, started with GSubprocess (portable,
// ADR-017), on the title's file, over the editor's current frame. The
// editor's file watch brings the saved result back.

#include <gdk/gdk.h>

#include <functional>
#include <string>

namespace ustudio::titles {

// The designer's executable: next to this program (installed together, and
// in the Flatpak's one bundle), else this build's, else from PATH. Empty if
// none is found.
std::string titlesAppPath();

// Starts the designer on `title` with `backdrop` (may be null) behind it.
// Empty on success, else why not.
std::string launchTitlesApp(const std::string &title, GdkTexture *backdrop);

// Tests only: what launchTitlesApp() does instead (null: the real launch).
// While one is set, Edit Title skips rendering the backdrop.
void setTitlesLauncherForTesting(std::function<std::string(const std::string &, GdkTexture *)> launcher);
bool titlesLauncherIsForTesting();

} // namespace ustudio::titles
