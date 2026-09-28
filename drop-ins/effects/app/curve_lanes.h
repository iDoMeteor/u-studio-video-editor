#pragma once

// Curve lanes (doc 15, "Keyframes that feel musical", FX4): under a clip on
// the timeline, one lane per animated value of its effects (and each
// animated mix), drawing the curve the value follows. C shows or hides them
// for the selected clips. Drag a keyframe to move it in time and value (one
// undo step a drag); double-click a lane to add one there.

namespace ustudio::app {
class ShellHost;
}

namespace ustudio::effects {

class Catalog;

// Adds the lanes and the C action to `host` (main thread; once per window).
void addCurveLanes(app::ShellHost &host, Catalog &catalog);

} // namespace ustudio::effects
