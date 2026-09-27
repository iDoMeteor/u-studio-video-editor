#pragma once

// A brand kit for titles (doc 16, "Brand kit"): named colours, gradients
// and fonts one click away in the inspector, and "Apply brand", which
// restyles a whole title. The Unicorn Tears kit ships as data/brand.xml.

#include "title_document.h"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace ustudio::titles {

struct BrandKit
{
    struct Colour
    {
        std::string name;
        Rgba value;
    };
    struct Gradient
    {
        std::string name;
        Fill fill; // Linear
    };
    std::string name;
    std::vector<Colour> colours;
    std::vector<Gradient> gradients;
    std::string displayFont, sansFont, monoFont; // families; empty if the kit has none

    const Colour *colour(std::string_view colourName) const;
};

std::expected<BrandKit, std::string> parseBrandKit(std::string_view xml);

// Restyles `doc` in the kit, keeping its layout: the largest text in the
// first gradient and the display font (the sans font when it's under 72 px),
// other text in the sans font and the kit's "Pink white" (else "White"),
// filled shapes in "Ink 700" at 92%, and outlines in "Cyan". Returns
// whether anything changed.
bool applyBrand(TitleDocument &doc, const BrandKit &kit);

} // namespace ustudio::titles
