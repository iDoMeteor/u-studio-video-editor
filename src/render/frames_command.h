#pragma once

#include "dropins/render_subcommand.h"

#include <atomic>

namespace ustudio::render {

// `u-studio-render --frames <project> [--range IN:OUT] [--ffv1 <output>]`
// (M6 groundwork; M5's gate, "preview equals u-studio-render"). Builds the
// project's graph exactly as the editor's preview does (EngineSync at Full,
// frames read at the profile's size, CPU pipeline) and either prints one
// JSON line with a hash of every frame's RGBA pixels, as `--title-frames`
// does:
//   {"width":1920,"height":1080,"fps":"30/1","frames":[{"frame":0,"hash":"9f0c..."},...]}
// or, with --ffv1, renders the range losslessly (FFV1 4:2:2 video, the graph's
// own YUV; PCM audio; Matroska) to <output>, written as <output>.part and renamed on success:
//   {"status":"ok","output":"...","frames":N}
// The range defaults to the whole sequence. `cancelled` stops it between
// frames (hashes) or before the render starts.
dropins::RenderSubcommand framesSubcommand(const std::atomic<bool> *cancelled);

} // namespace ustudio::render
