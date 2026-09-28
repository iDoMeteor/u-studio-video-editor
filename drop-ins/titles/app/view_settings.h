#pragma once

// The titles app's view choices, remembered per user (not part of any
// title): the canvas background behind the title, and whether the safe
// areas show. A GKeyFile in the user config directory (GLib finds it on
// every platform, ADR-017).

#include "canvas.h"

#include <gdk/gdk.h>

namespace ustudio::titles::app {

struct ViewSettings
{
    Backdrop backdrop = Backdrop::Checkerboard; // Image isn't remembered: it's per launch
    GdkRGBA colour{0.0f, 0.0f, 0.0f, 1.0f};
    bool guides = true;
};

ViewSettings loadViewSettings();
void saveViewSettings(const ViewSettings &settings);

} // namespace ustudio::titles::app
