#pragma once

// The one image-processing routine the titles renderer writes (doc 16):
// three box-blur passes each way, which approximate a Gaussian. Cairo has
// no blur.

#include <cstdint>

namespace ustudio::titles {

// Blurs an 8-bit single-channel image in place (Cairo's A8: `stride` bytes
// per row). Three passes of radius r give variance r(r + 1), so r is the
// nearest whole number to `sigma`; sigma below 0.5 does nothing.
void blurAlpha(uint8_t *data, int width, int height, int stride, double sigma);

// The same for Cairo's ARGB32 (premultiplied, 4 bytes a pixel): every
// channel blurred alike, so colour and coverage stay consistent.
void blurArgb(uint8_t *data, int width, int height, int stride, double sigma);

} // namespace ustudio::titles
