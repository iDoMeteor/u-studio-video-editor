#pragma once

// The Effect Rack (doc 15, "Effect Rack", FX2): an inspector page showing
// the effect stack of the selected clip, its track or the whole sequence,
// one card per effect (bypass, name, cost badge, reorder, remove, Mix, and
// a widget per parameter), and an Add button. Every change is a command
// through the host, so it undoes; a slider drag is one undo step.

namespace ustudio::app {
class ShellHost;
}

namespace ustudio::effects {

class Catalog;

// Adds the Rack to `host`'s inspector (main thread; once per window).
void addRack(app::ShellHost &host, Catalog &catalog);

} // namespace ustudio::effects
