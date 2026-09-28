#pragma once

// Compare (doc 15, "Compare", FX2): the picture without the selected clip's
// effects, next to the picture with them.
// - Hold \ : the whole picture without them, until the key is let go.
// - Compare (win.effects-compare, the Rack's Compare button): a split over
//   the picture, "before" left of a divider you drag, "after" right of it.
// The "before" frame is rendered off the live graph (engine/frame_renderer.h)
// and drawn over the preview, so nothing is rebuilt and nothing is an edit.
// While playing it shows the frame the playhead was on when it was asked for.

namespace ustudio::app {
class ShellHost;
}

namespace ustudio::effects {

class Catalog;

// Adds compare to `host`'s preview (main thread; once per window).
void addCompare(app::ShellHost &host, Catalog &catalog);

} // namespace ustudio::effects
