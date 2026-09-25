#pragma once

#include <adwaita.h>

#include <functional>

namespace ustudio::dropins {
class DropInRegistry;
} // namespace ustudio::dropins

namespace ustudio::app {

class Settings;

// Settings > Drop-ins (doc 17, "Finding, enabling and trusting drop-ins"):
// the installed drop-ins, each with a switch (saved at once, applied from
// the next start: native code isn't unloaded at runtime); the ones this
// build knows of that aren't installed, with how to get them; and any that
// couldn't be loaded, with why. It never downloads or installs anything.
// `registry` may be null (none this run). `onChanged` runs after a switch
// is saved. `settings` must outlive the dialog.
AdwPreferencesPage *buildDropInsPage(const dropins::DropInRegistry *registry, Settings &settings,
                                     std::function<void()> onChanged);

} // namespace ustudio::app
