#pragma once

// Bake Title (doc 16, "Portability"): a title clip rendered to a ProRes
// 4444 file with alpha, at the sequence's rate, with the clip's fields,
// and swapped in for the title in one undoable step. The clip keeps its
// place, range, effects and transform. A baked clip plays in stock `melt`
// and any tool without the titles module; undo brings the live title back.

#include "app/shell_host.h"

#include <string>

namespace ustudio::titles {

void bakeTitleClip(app::ShellHost &host, core::ClipId clip);

// Where a bake of `titlePath` goes: beside it, "<name> (baked).mov", or
// "(baked 2)" and so on when that's taken. Never an existing file.
std::string bakePath(const std::string &titlePath);

} // namespace ustudio::titles
