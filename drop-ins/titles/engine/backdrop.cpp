#include "backdrop.h"

#include "engine/engine_sync.h"

#include <mlt++/Mlt.h>

namespace ustudio::titles {

Backdrop renderBackdrop(std::shared_ptr<const core::Project> project, core::ClipId hidden, core::FrameIndex frame)
{
    Backdrop out;
    if (!project)
        return out;
    auto copy = std::make_shared<core::Project>(*project);
    for (core::Sequence &sequence : copy->sequences)
        if (auto it = sequence.clips.find(hidden); it != sequence.clips.end())
            it->second.videoEnabled = false;
    engine::EngineSync sync{std::shared_ptr<const core::Project>(copy)};
    Mlt::Tractor &tractor = sync.tractor();
    tractor.seek(static_cast<int>(frame));
    std::unique_ptr<Mlt::Frame> picture(tractor.get_frame());
    if (!picture)
        return out;
    mlt_image_format format = mlt_image_rgba;
    int width = sync.profile().width(), height = sync.profile().height();
    const uint8_t *image = picture->get_image(format, width, height);
    if (!image || format != mlt_image_rgba || width <= 0 || height <= 0)
        return out;
    out.width = width;
    out.height = height;
    out.rgba.assign(image, image + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    return out;
}

} // namespace ustudio::titles
