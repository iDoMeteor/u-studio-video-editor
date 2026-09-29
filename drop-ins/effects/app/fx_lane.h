#pragma once

// The FX lane (doc 15, "FX lane (adjustment blocks)", FX4): a thin lane
// above the tracks where adjustment blocks live. Drag across it to draw a
// block; click one to edit its effects in the Rack (its scope becomes the
// block); drag a block to move it, its ends to resize it, and the handles at
// its top corners to fade it in and out. A block the Rack moved below a
// track (its "Affects") shows in a lane under that track.

namespace ustudio::app {
class ShellHost;
}

namespace ustudio::effects {

class Catalog;

// Adds the lane to `host`'s timeline (main thread; once per window).
void addFxLane(app::ShellHost &host, Catalog &catalog);

} // namespace ustudio::effects
