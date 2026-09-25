#pragma once

#include "core/model/types.h"

#include <algorithm>

namespace ustudio::app {

// Doc 15 IP5's frame-to-widget mapper for preview overlays (transform
// handles, mask points): where the sequence's frame lands inside the
// preview widget, which shows it letterboxed at its display aspect
// (GTK_CONTENT_FIT_CONTAIN, centred). Frame coordinates are the profile's
// pixels, (0, 0) top left; widget coordinates are the overlay widget's,
// which covers the preview exactly. Plain arithmetic, no GTK.
struct PreviewMapping
{
    double frameWidth = 0, frameHeight = 0;                         // the profile's pixels
    double imageX = 0, imageY = 0, imageWidth = 0, imageHeight = 0; // the frame, in the widget

    double widgetX(double frameX) const
    {
        return imageX + frameX * imageWidth / frameWidth;
    }
    double widgetY(double frameY) const
    {
        return imageY + frameY * imageHeight / frameHeight;
    }
    double frameX(double widgetXValue) const
    {
        return (widgetXValue - imageX) * frameWidth / imageWidth;
    }
    double frameY(double widgetYValue) const
    {
        return (widgetYValue - imageY) * frameHeight / imageHeight;
    }
    // Widget pixels per frame pixel (hit radii: a 6 px handle is 6 / scale() frame pixels).
    double scale() const
    {
        return imageWidth / frameWidth;
    }
};

// `displayAspect` is the shown image's width / height (the picture's own
// aspect; the profile's DAR when there's none yet). A zero-sized widget or
// frame gives an empty image at the widget's centre (every point maps
// there, nothing divides by zero).
inline PreviewMapping mapPreview(double widgetWidth, double widgetHeight, int frameWidth, int frameHeight,
                                 double displayAspect)
{
    PreviewMapping mapping;
    mapping.frameWidth = std::max(frameWidth, 1);
    mapping.frameHeight = std::max(frameHeight, 1);
    const double aspect = displayAspect > 0.0 ? displayAspect : mapping.frameWidth / mapping.frameHeight;
    const double widgetW = std::max(widgetWidth, 0.0), widgetH = std::max(widgetHeight, 0.0);
    double width = widgetW, height = widgetH;
    if (width > height * aspect)
        width = height * aspect; // pillarboxed: bars left and right
    else
        height = width / aspect; // letterboxed: bars top and bottom
    mapping.imageWidth = std::max(width, 1e-9);
    mapping.imageHeight = std::max(height, 1e-9);
    mapping.imageX = (widgetW - width) / 2.0;
    mapping.imageY = (widgetH - height) / 2.0;
    return mapping;
}

// The profile's display aspect ratio (DAR), or 0 when it has none.
inline double displayAspectOf(const core::Profile &profile)
{
    return profile.dar.num > 0 && profile.dar.den > 0 ? static_cast<double>(profile.dar.num) / profile.dar.den : 0.0;
}

} // namespace ustudio::app
