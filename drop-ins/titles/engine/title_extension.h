#pragma once

// The titles drop-in in the engine (IP3, doc 16 "Rendering and the MLT
// producer"): every clip of a .ustitle asset gets its own `ustudio_title`
// producer, because clips of one title differ (field values, and the
// length the elastic hold fits).

#include "engine/engine_extension.h"

#include <memory>
#include <string>
#include <vector>

namespace ustudio::titles {

// A `ustudio_title` producer for `path`, `length` frames long, with the
// clip's `field.*` parameters; null if MLT can't make one (the module isn't
// loaded, or the file doesn't read).
std::unique_ptr<Mlt::Producer> makeTitleProducer(Mlt::Profile &profile, const std::string &path,
                                                 core::FrameIndex length, const std::vector<core::Param> &params);

std::unique_ptr<engine::EngineExtension> makeTitleExtension();

} // namespace ustudio::titles
