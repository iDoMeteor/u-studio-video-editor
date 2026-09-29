#pragma once

// Animated layers (doc 16 T6, ADR-021): the check every Lottie file passes
// before ThorVG sees it. Lottie files come from strangers, so this refuses a
// file as a whole, with a reason, when it's too big or too deep, when its
// header is off, when it uses an expression (script), or when it names
// anything outside itself (an image or font file). Std only: a streaming
// JSON scanner, not a general parser.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace ustudio::titles::lottie {

constexpr size_t kMaxBytes = 8u << 20;
constexpr size_t kMaxDepth = 64;
constexpr size_t kMaxValues = 1'000'000;
constexpr size_t kMaxLayers = 1000;
constexpr size_t kMaxAssets = 256;
constexpr double kMaxSeconds = 600.0;
constexpr int kMaxSide = 8192;
constexpr size_t kMaxImageBytes = 16u << 20;

// What a valid file says about itself.
struct Facts
{
    double fr = 0.0, ip = 0.0, op = 0.0; // frame rate; first frame; the frame after the last
    int width = 0, height = 0;
    size_t layers = 0;    // precompositions' included
    bool hasText = false; // a text layer (drawn with the system's fonts)
};

// `json` checked against ADR-021 decision 3. The reason reads after
// "Couldn't add the animation: ".
std::expected<Facts, std::string> check(std::string_view json);

// The animation's frame at title frame `titleFrame` (ADR-021 decision 5):
// `ip` plus the title's time in the animation's frames, times `speed`;
// looping within [ip, op), or held at the last frame (op - 1) once done.
// One expression in one order, so the producer, the designer and the bake
// get the same frame, and draw the same pixels.
double frameAt(double titleFrame, int fpsNum, int fpsDen, const Facts &facts, double speed, bool loop);

} // namespace ustudio::titles::lottie
