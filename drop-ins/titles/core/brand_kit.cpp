#include "brand_kit.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <charconv>
#include <memory>

// libxml2's BAD_CAST is a C cast.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"

namespace ustudio::titles {

namespace {
std::string attr(const xmlNode *node, const char *name)
{
    xmlChar *value = xmlGetProp(node, BAD_CAST name);
    std::string out = value ? reinterpret_cast<const char *>(value) : "";
    xmlFree(value);
    return out;
}
} // namespace

const BrandKit::Colour *BrandKit::colour(std::string_view colourName) const
{
    auto it = std::find_if(colours.begin(), colours.end(), [&](const Colour &c) { return c.name == colourName; });
    return it == colours.end() ? nullptr : &*it;
}

std::expected<BrandKit, std::string> parseBrandKit(std::string_view xml)
{
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc(
        xmlReadMemory(xml.data(), static_cast<int>(xml.size()), "brand.xml", nullptr, XML_PARSE_NONET), &xmlFreeDoc);
    if (!doc)
        return std::unexpected("not well-formed XML");
    const xmlNode *root = xmlDocGetRootElement(doc.get());
    if (!root || xmlStrcmp(root->name, BAD_CAST "brand") != 0)
        return std::unexpected("not a brand kit (no <brand> root)");
    BrandKit kit;
    kit.name = attr(root, "name");
    for (const xmlNode *node = root->children; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE)
            continue;
        const std::string element = reinterpret_cast<const char *>(node->name);
        if (element == "colour") {
            auto value = parseColor(attr(node, "value"));
            if (!value)
                return std::unexpected("colour \"" + attr(node, "name") + "\" has no valid value");
            kit.colours.push_back({attr(node, "name"), *value});
        } else if (element == "gradient") {
            Fill fill;
            fill.kind = FillKind::Linear;
            auto from = parseColor(attr(node, "from"));
            auto to = parseColor(attr(node, "to"));
            if (!from || !to)
                return std::unexpected("gradient \"" + attr(node, "name") + "\" needs from and to");
            fill.from = *from;
            fill.to = *to;
            if (auto via = parseColor(attr(node, "via")))
                fill.via = *via;
            const std::string angle = attr(node, "angle");
            std::from_chars(angle.data(), angle.data() + angle.size(), fill.angle);
            kit.gradients.push_back({attr(node, "name"), fill});
        } else if (element == "font") {
            const std::string role = attr(node, "role"), family = attr(node, "family");
            if (role == "display")
                kit.displayFont = family;
            else if (role == "sans")
                kit.sansFont = family;
            else if (role == "mono")
                kit.monoFont = family;
        }
    }
    return kit;
}

bool applyBrand(TitleDocument &doc, const BrandKit &kit)
{
    const TitleDocument before = doc;
    const Layer *largest = nullptr;
    for (const Layer &layer : doc.layers)
        if (layer.kind == LayerKind::Text && (!largest || layer.font.size > largest->font.size))
            largest = &layer;
    const std::string largestId = largest ? largest->id : "";
    const BrandKit::Colour *body = kit.colour("Pink white") ? kit.colour("Pink white") : kit.colour("White");
    const BrandKit::Colour *panel = kit.colour("Ink 700");
    const BrandKit::Colour *outline = kit.colour("Cyan");
    for (Layer &layer : doc.layers) {
        if (layer.kind == LayerKind::Text) {
            const bool hero = layer.id == largestId;
            const std::string &family =
                hero && layer.font.size >= 72 && !kit.displayFont.empty() ? kit.displayFont : kit.sansFont;
            if (!family.empty())
                layer.font.family = family;
            if (hero && !kit.gradients.empty()) {
                const double opacity = layer.fill.opacity;
                layer.fill = kit.gradients.front().fill;
                layer.fill.opacity = opacity;
            } else if (body) {
                layer.fill.kind = FillKind::Solid;
                layer.fill.color = body->value;
            }
        } else if (layer.fill.kind != FillKind::None && layer.shape != ShapeKind::Line && panel) {
            layer.fill.kind = FillKind::Solid;
            layer.fill.color = panel->value;
            layer.fill.opacity = 0.92;
        }
        if (layer.stroke.width > 0.0 && outline)
            layer.stroke.color = outline->value;
    }
    return doc != before;
}

} // namespace ustudio::titles

#pragma GCC diagnostic pop
