#pragma once

#include "dropins/render_subcommand.h"

#include <atomic>

namespace ustudio::render {

// `u-studio-render --proxy <source> <output> [--height N] [--fps n/d]
// [--threads N]` (M4 C, doc 07; ADR-009: the editor runs it as a child
// process). A core subcommand, listed before the drop-ins' so none can take
// its name. Settings arrive as arguments: the tool reads no GSettings, so
// it's stateless and testable. Prints one JSON object per line on `out`:
//   {"progress":0.42}                       while rendering
//   {"status":"ok","output":"<path>"}        exit 0
//   {"status":"error","message":"<why>"}     exit 1 (2: bad arguments)
// `cancel` (set by platform::onTerminationRequest()) stops it; the output's
// .part file is removed.
dropins::RenderSubcommand proxySubcommand(const std::atomic<bool> *cancel);

// A JSON string literal's content: quotes, backslashes and control
// characters escaped (paths are the only strings here).
std::string jsonEscape(const std::string &text);

} // namespace ustudio::render
