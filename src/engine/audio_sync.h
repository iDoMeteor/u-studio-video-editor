#pragma once

#include "core/audio/align.h"
#include "core/model/frame_time.h"

#include <optional>
#include <string>

namespace ustudio::engine {

namespace core = ustudio::core;

// What "Sync tracks (audio)" needs to know about one clip, copied out of
// the model on the main thread so the decode can run on a pool job.
struct AudioSpan
{
    std::string resource;
    core::FrameIndex sourceIn = 0;       // first source frame to decode
    core::FrameIndex frames = 0;         // how many
    core::FrameIndex timelineStart = 0;  // where sourceIn sits on the timeline
};

// The loudness envelope of `span` (mono mean absolute amplitude per
// Envelope::kBlockMs), placed at its timeline position. Opens its own
// Profile/Producer with the picture stream off, like the waveform cache,
// so it never touches the live graph and is safe on a pool thread.
// Nullopt if the file can't be opened, has no audio, or is silent
// throughout (nothing to line up by).
std::optional<core::audio::Envelope> decodeEnvelope(const AudioSpan &span, core::Rational fps);

} // namespace ustudio::engine
