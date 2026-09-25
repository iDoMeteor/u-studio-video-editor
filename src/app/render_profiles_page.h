#pragma once

#include <adwaita.h>

#include <functional>

namespace ustudio::app {

class RenderProfileStore;
class Settings;

// Settings > Render: the global Render threads row, then pick a profile to view; built-ins are read-only, so
// New and Duplicate make an editable copy; Save, Remove, Set as Default.
// `qualityMode` is false when the H.264 encoder has no CRF mode, which the
// page says. The page keeps its own state and only needs `store` and
// `settings` to outlive the dialog (AppWindow owns both).
// `onThreadsChanged` gets the new Render threads percentage (it's already
// saved in `settings`).
AdwPreferencesPage *buildRenderProfilesPage(RenderProfileStore &store, Settings &settings, bool qualityMode,
                                            std::function<void(int percent)> onThreadsChanged);

} // namespace ustudio::app
