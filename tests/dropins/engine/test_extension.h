#pragma once

// The test drop-in's engine layer (IP3's consumer): an EngineExtension that
// uses every hook, the way drop-ins/effects/engine will.
//   decorateCut      attaches each of its effects (owner == its drop-in) to
//                    the cut, keyframes animated for that cut
//   decoratePlaylist, decorateTractor   the same for track and master effects
//   makeProducer     a clip whose sourceParams has a "color" plays that colour
//   makeTransitionSegment   the recipe "hard-cut": the segment is just b's head
//   compositor       "composite" marked as its own, when switched on
//   applyInPlace     "level" and "mix" changes set on its live filters

#include "engine/engine_extension.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ustudio::testdropin {

// What the extensions (of this binary's copy) saw, for tests.
struct ExtensionLog
{
    std::vector<std::pair<core::FrameIndex, core::FrameIndex>> cuts; // offset, length
    int builds = 0, playlists = 0, tractors = 0, compositors = 0, producers = 0, recipes = 0, inPlace = 0;
    std::vector<std::pair<int, std::vector<core::FrameIndex>>> lanes; // decorateLane: lane, its blocks' starts
};
ExtensionLog &extensionLog();
// Off by default: with it off the graph uses "composite", as without drop-ins.
bool &useTestCompositor();

std::unique_ptr<engine::EngineExtension> makeTestExtension(std::string owner);

} // namespace ustudio::testdropin
