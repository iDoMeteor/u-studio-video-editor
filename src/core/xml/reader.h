#pragma once

#include "core/model/model.h"

#include <expected>
#include <string>

namespace ustudio::core {

// Reads a .ustudio project file written by saveProject() (doc 09). Looks
// only at ustudio:* properties and ids to re-derive the model; the
// MLT-facing structure (producer/playlist/tractor/transition elements) is
// regenerated on save, so it's treated as output, not input, here.
//
// Refuses a project whose ustudio:format_version doesn't match what this
// build writes (doc 09: the reader migrates older versions in code, one
// function per bump -- none exist yet, so any mismatch is refused rather
// than guessed at).
//
// A plain MLT XML file with no ustudio:* properties at all (e.g. a
// kdenlive project) is out of scope here -- that's the M7 importer.
std::expected<Model, std::string> loadProject(const std::string &path);

} // namespace ustudio::core
