#include "title_document.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace ustudio::titles {

namespace {
int hexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

constexpr std::array<const char *, 14> kPropertyNames = {"x",      "y",        "opacity", "scale",         "rotation",
                                                         "blur",   "tracking", "reveal",  "shift",         "fill-r",
                                                         "fill-g", "fill-b",   "fill-a",  "shadow-opacity"};
static_assert(static_cast<size_t>(Property::ShadowOpacity) + 1 == kPropertyNames.size());
} // namespace

std::optional<Rgba> parseColor(std::string_view text)
{
    if (text.empty() || text[0] != '#')
        return std::nullopt;
    text.remove_prefix(1);
    std::array<int, 8> digits{};
    for (size_t i = 0; i < text.size(); ++i) {
        if (i >= digits.size() || (digits[i] = hexDigit(text[i])) < 0)
            return std::nullopt;
    }
    const auto channel = [&](size_t i) { return (digits[i] * 16 + digits[i + 1]) / 255.0; };
    const auto shortChannel = [&](size_t i) { return (digits[i] * 17) / 255.0; };
    switch (text.size()) {
    case 3:
        return Rgba{shortChannel(0), shortChannel(1), shortChannel(2), 1.0};
    case 6:
        return Rgba{channel(0), channel(2), channel(4), 1.0};
    case 8:
        return Rgba{channel(0), channel(2), channel(4), channel(6)};
    default:
        return std::nullopt;
    }
}

std::string formatColor(const Rgba &colour)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out = "#";
    const auto put = [&](double v) {
        const int byte = static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
        out += kHex[byte >> 4];
        out += kHex[byte & 15];
    };
    put(colour.r);
    put(colour.g);
    put(colour.b);
    if (std::lround(std::clamp(colour.a, 0.0, 1.0) * 255.0) != 255)
        put(colour.a);
    return out;
}

const char *propertyName(Property property)
{
    const auto index = static_cast<size_t>(property);
    return index < kPropertyNames.size() ? kPropertyNames[index] : "opacity";
}

std::optional<Property> propertyFromName(std::string_view name)
{
    for (size_t i = 0; i < kPropertyNames.size(); ++i)
        if (name == kPropertyNames[i])
            return static_cast<Property>(i);
    return std::nullopt;
}

} // namespace ustudio::titles
