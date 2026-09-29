#pragma once

// Direct manipulation on the preview (doc 15, "Direct manipulation on the
// preview", FX4), for the Rack's controls:
// - The eyedropper: the next click on the picture picks a colour from the
//   selected clip's own frame there (before its effects: the colour a key
//   or a grade starts from), rendered off the live graph like Compare's.
// - Rect handles: a rectangle parameter drawn over the picture, its body
//   and corners draggable; the changes of one drag are one gesture.
// One overlay per window, shared by the Rack's controls.

#include "core/model/types.h"

#include <cstdint>
#include <functional>

namespace ustudio::app {
class ShellHost;
}

namespace ustudio::effects {

class PreviewTools
{
  public:
    virtual ~PreviewTools() = default;
    // The next click on the picture calls `done` with the colour there;
    // Esc, or another pick, cancels it (done isn't called).
    virtual void pickColour(std::function<void(core::Color)> done) = 0;
    // Handles for `rect` (frame pixels) until hideRect(); a drag calls
    // `changed` with the new rect and the drag's gesture id.
    virtual void showRect(core::Rect rect, std::function<void(core::Rect, uint64_t)> changed) = 0;
    virtual void hideRect() = 0;
    virtual bool showingRect() const = 0;
};

// `host`'s tools, made (and their overlay added) on first use (main thread).
PreviewTools &previewTools(app::ShellHost &host);

} // namespace ustudio::effects
