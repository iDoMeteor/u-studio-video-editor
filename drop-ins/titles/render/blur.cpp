#include "blur.h"

#include <cmath>
#include <vector>

namespace ustudio::titles {

namespace {
// One box pass over `count` samples `step` apart, through `scratch`. Edges
// are treated as transparent, so a shape's shadow fades out past the image.
void boxPass(uint8_t *line, int count, int step, int radius, std::vector<uint8_t> &scratch)
{
    scratch.resize(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        scratch[static_cast<size_t>(i)] = line[static_cast<ptrdiff_t>(i) * step];
    const int window = 2 * radius + 1;
    int sum = 0;
    for (int i = 0; i < radius && i < count; ++i)
        sum += scratch[static_cast<size_t>(i)];
    for (int i = 0; i < count; ++i) {
        const int enter = i + radius;
        const int leave = i - radius - 1;
        if (enter < count)
            sum += scratch[static_cast<size_t>(enter)];
        if (leave >= 0)
            sum -= scratch[static_cast<size_t>(leave)];
        line[static_cast<ptrdiff_t>(i) * step] = static_cast<uint8_t>((sum + window / 2) / window);
    }
}
} // namespace

void blurArgb(uint8_t *data, int width, int height, int stride, double sigma)
{
    if (sigma < 0.5 || width <= 0 || height <= 0)
        return;
    const int radius = static_cast<int>(std::lround(sigma));
    std::vector<uint8_t> scratch;
    for (int pass = 0; pass < 3; ++pass)
        for (int channel = 0; channel < 4; ++channel) {
            for (int y = 0; y < height; ++y)
                boxPass(data + static_cast<ptrdiff_t>(y) * stride + channel, width, 4, radius, scratch);
            for (int x = 0; x < width; ++x)
                boxPass(data + static_cast<ptrdiff_t>(x) * 4 + channel, height, stride, radius, scratch);
        }
}

void blurAlpha(uint8_t *data, int width, int height, int stride, double sigma)
{
    if (sigma < 0.5 || width <= 0 || height <= 0)
        return;
    const int radius = static_cast<int>(std::lround(sigma));
    std::vector<uint8_t> scratch;
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < height; ++y)
            boxPass(data + static_cast<ptrdiff_t>(y) * stride, width, 1, radius, scratch);
        for (int x = 0; x < width; ++x)
            boxPass(data + x, height, stride, radius, scratch);
    }
}

} // namespace ustudio::titles
