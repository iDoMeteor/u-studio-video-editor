#pragma once

// The formats a title exports to on its own (doc 16, T2d): shared by the
// designer's Export dialog and `u-studio-render --title-export`, which
// holds the encoder settings for each (engine/title_export.cpp).

#include <string_view>
#include <vector>

namespace ustudio::titles {

struct ExportFormat
{
    const char *name;      // the subcommand's name for it: "prores"
    const char *label;     // for people: "ProRes 4444 (alpha)"
    const char *extension; // "mov"; "" for a folder of PNGs
    bool alpha;            // false: flattened onto the title's background, or black
};

const std::vector<ExportFormat> &exportFormats();
const ExportFormat *exportFormat(std::string_view name);

} // namespace ustudio::titles
