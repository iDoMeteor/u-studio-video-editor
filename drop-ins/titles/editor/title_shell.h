#pragma once

// The titles drop-in in the editor window (IP5): .ustitle files import as
// title clips, and a title's file is watched, so saving it (from the
// titles app or a text editor) updates the editor within a second.

#include "app/shell_host.h"

#include <expected>
#include <optional>
#include <string>

namespace ustudio::titles {

void extendShell(app::ShellHost &host);

// The import handler: the title's asset, and a clip of its designed length
// (converted to the sequence's frame rate) when a track is given.
std::expected<std::optional<core::FrameIndex>, std::string> importTitle(app::ShellHost &host, const std::string &path,
                                                                        std::optional<core::TrackId> track,
                                                                        std::optional<core::FrameIndex> position);

} // namespace ustudio::titles
