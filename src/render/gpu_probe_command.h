#pragma once

#include "dropins/render_subcommand.h"

namespace ustudio::render {

// `u-studio-render --gpu-probe` (ADR-019 point 6): engine::probeGpu() in this
// child process, so a GL driver crash never reaches the editor. Prints one
// JSON object:
//   {"status":"ok","renderer":"…","version":"…"}                  exit 0
//   {"status":"error","message":"…","renderer":"…","version":"…"} exit 1
dropins::RenderSubcommand gpuProbeSubcommand();

} // namespace ustudio::render
