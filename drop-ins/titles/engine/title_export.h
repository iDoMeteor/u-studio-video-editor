#pragma once

// IP6: `u-studio-render --title-export <title.ustitle> <output> <format>
// [--seconds S | --frames N] [--fps N/D] [--timeline-start F]
// [--field name=value]...`. A title on its own, for OBS and
// other tools (doc 16, T2d; owner requirement 2026-09-27):
//
//   png     a folder of PNGs with alpha (<output>/<name>_00001.png ...)
//   prores  ProRes 4444 with alpha (.mov, yuva444p10le)
//   qtrle   QuickTime Animation with alpha (.mov, argb)
//   webm    VP9 with alpha (.webm, yuva420p)
//   h264    flattened onto the title's background, or black (.mp4)
//
// The title's own frames go straight to the encoder: nothing composites
// them, so the alpha is exactly the renderer's. Written atomically (a
// .part file or folder, renamed when done). Prints one JSON line: the
// output and its frame count, or an error.

#include "core/model/types.h"

#include <atomic>
#include <expected>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace ustudio::titles {

struct TitleExportRequest
{
    std::string title, output;
    std::string format; // exportFormats()' name: "png", "prores", ...
    // Length: the title's designed length when unset (elastic timing fits
    // the title to it, as for a clip).
    std::optional<core::FrameIndex> frames;
    // Rate: the title's own when unset (a bake uses the sequence's).
    std::optional<core::Rational> fps;
    // {{timecode}}'s start, in frames at that rate (a bake: the clip's
    // position minus its in point).
    double timelineStart = 0.0;
    std::vector<core::Param> fields; // "field.<name>"
};

// Writes the title to `request.output` atomically; the frame count, or why
// not. `cancel`, when set during the encode, stops it and leaves nothing.
std::expected<core::FrameIndex, std::string> exportTitle(const TitleExportRequest &request,
                                                         const std::atomic<bool> *cancel = nullptr);

int runTitleExport(const std::vector<std::string> &args, std::ostream &out);

} // namespace ustudio::titles
