#pragma once

#include <adwaita.h>

namespace ustudio::app {

class RenderProfileStore;
class Settings;

// Settings > Render: pick a profile to view; built-ins are read-only, so
// New and Duplicate make an editable copy; Save, Remove, Set as Default.
// `qualityMode` is false when the H.264 encoder has no CRF mode, which the
// page says. The page keeps its own state and only needs `store` and
// `settings` to outlive the dialog (AppWindow owns both).
AdwPreferencesPage *buildRenderProfilesPage(RenderProfileStore &store, Settings &settings, bool qualityMode);

} // namespace ustudio::app
