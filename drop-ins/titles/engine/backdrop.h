#pragma once

// What a title is designed over (doc 16, "Canvas"): the editor's frame at
// the playhead without the title itself, so moving a layer in the designer
// leaves no ghost behind. Built on its own graph (EngineSync), as an export
// is: call it on a worker thread, never the main or the engine thread.

#include "core/model/types.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace ustudio::titles {

struct Backdrop
{
    int width = 0, height = 0;
    std::vector<uint8_t> rgba; // straight RGBA at the sequence's size; empty if it couldn't render
};

Backdrop renderBackdrop(std::shared_ptr<const core::Project> project, core::ClipId hidden, core::FrameIndex frame);

} // namespace ustudio::titles
