#pragma once

// The Transitions page (doc 15, "Transitions", FX3): the styles a dissolve
// can play (dissolves, dips, flashes, wipes), each a tile drawn from its own
// shape, for the transition under the playhead. Click a tile to change it
// (one undo step); its settings (softness, reverse) sit above the tiles.
// - T adds the default transition at the cut nearest the playhead on the
//   active track, and opens this page on it.
// - Double-clicking a transition on the timeline opens this page on it.
// The transition the page is about is outlined on the timeline.

#include "core/transitions.h"

#include <vector>

namespace ustudio::app {
class ShellHost;
}

namespace ustudio::effects {

// Adds the page, its action and its timeline outline to `host` (main thread;
// once per window).
void addTransitions(app::ShellHost &host, const std::vector<TransitionRecipe> &recipes);

} // namespace ustudio::effects
