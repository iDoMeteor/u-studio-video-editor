#pragma once

#include "core/model/model.h"

#include <string>

namespace ustudio::core {

// Writes `model` as a .ustudio project file: MLT XML that `melt` and
// u-studio-render can play with no editor involved, carrying everything
// the model needs to round-trip losslessly in ustudio:* properties
// (ADR-004, doc 09). Written by hand (not MLT's own xml consumer, which
// emits implementation detail that churns diffs and can't carry our ids).
//
// Atomic: writes to "<path>.tmp" in the same directory, fsyncs, renames
// over `path`. Returns an empty string on success, an error message
// otherwise; on any failure the target file is left untouched.
std::string saveProject(const Model &model, const std::string &path);

} // namespace ustudio::core
