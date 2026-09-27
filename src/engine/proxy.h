#pragma once

// M4 C (doc 07, "Proxies"): an asset's stand-in for editing, rendered by
// `u-studio-render --proxy` in a child process (ADR-009), never in the
// editor. Export always uses the originals.

#include "core/model/frame_time.h"

#include <atomic>
#include <functional>
#include <string>

namespace ustudio::engine {

struct ProxyRequest
{
    std::string source;
    std::string output;
    // Output height in pixels, square pixels at the source's aspect; 0 is
    // the source's own size (a "conformed" proxy: constant-rate and easy
    // to decode, for VFR phone or screen footage; doc 12, "Frame rate").
    int height = 540;
    // The sequence's rate: the proxy is constant-rate at it, with the
    // source's duration, so a clip's in/out (sequence frames, mapped by
    // time) fit it exactly.
    core::Rational fps{30, 1};
    int threadBudget = 0; // core::splitRenderThreads(); 0: MLT's defaults
    // An image sequence (M4 E): `source` is its %0Nd pattern, numbered from
    // sequenceBegin for sequenceCount files. Count 0: a media file.
    int sequenceBegin = 0;
    int sequenceCount = 0;
};

// Renders the proxy: the source alone, H.264 at draft quality with a
// keyframe every half second (smooth scrubbing), written to a .part file
// and renamed into place. False with `error` on failure or `cancel`.
// Blocks; call it off the main thread (the render tool does).
bool renderProxy(const ProxyRequest &request, std::string &error,
                 std::function<void(int currentFrame, int totalFrames)> onProgress = {},
                 const std::atomic<bool> *cancel = nullptr);

} // namespace ustudio::engine
