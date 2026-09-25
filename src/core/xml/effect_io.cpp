#include "core/xml/effect_io.h"

#include "core/model/animation.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <sstream>

// See writer.cpp: BAD_CAST is libxml2's C-style cast.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"

namespace ustudio::core::xml_detail {

namespace {

double toDouble(const std::string &s)
{
    double value = 0.0;
    auto result = std::from_chars(s.data(), s.data() + s.size(), value);
    return result.ec == std::errc{} ? value : 0.0;
}

int64_t toI64(const std::string &s)
{
    return s.empty() ? 0 : std::strtoll(s.c_str(), nullptr, 10);
}

std::vector<std::string> split(const std::string &text, char separator)
{
    std::vector<std::string> parts;
    std::string part;
    std::istringstream in(text);
    while (std::getline(in, part, separator))
        parts.push_back(part);
    return parts;
}

// "d:0.5", "i:3", "b:1", "s:text", "c:r,g,b,a", "r:x,y,w,h".
std::string encodeValue(const Param::Value &value);
Param::Value decodeValue(const std::string &text);

// What MLT reads for a constant value.
std::string nativeValue(const Param::Value &value)
{
    return std::visit(
        [](const auto &v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, double>)
                return formatDouble(v);
            else if constexpr (std::is_same_v<T, int64_t>)
                return std::to_string(v);
            else if constexpr (std::is_same_v<T, bool>)
                return v ? "1" : "0";
            else if constexpr (std::is_same_v<T, std::string>)
                return v;
            else if constexpr (std::is_same_v<T, Color>) {
                char buf[16]; // MLT's colour form: 0xRRGGBBAA
                std::snprintf(buf, sizeof buf, "0x%02x%02x%02x%02x", v.r, v.g, v.b, v.a);
                return buf;
            } else
                return formatDouble(v.x) + " " + formatDouble(v.y) + " " + formatDouble(v.w) + " " + formatDouble(v.h);
        },
        value);
}

std::string encodeValue(const Param::Value &value)
{
    return std::visit(
        [](const auto &v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, double>)
                return "d:" + formatDouble(v);
            else if constexpr (std::is_same_v<T, int64_t>)
                return "i:" + std::to_string(v);
            else if constexpr (std::is_same_v<T, bool>)
                return v ? "b:1" : "b:0";
            else if constexpr (std::is_same_v<T, std::string>)
                return "s:" + v;
            else if constexpr (std::is_same_v<T, Color>)
                return "c:" + std::to_string(v.r) + "," + std::to_string(v.g) + "," + std::to_string(v.b) + "," +
                       std::to_string(v.a);
            else
                return "r:" + formatDouble(v.x) + "," + formatDouble(v.y) + "," + formatDouble(v.w) + "," +
                       formatDouble(v.h);
        },
        value);
}

Param::Value decodeValue(const std::string &text)
{
    if (text.size() < 2 || text[1] != ':')
        return 0.0;
    const std::string body = text.substr(2);
    switch (text[0]) {
    case 'i':
        return static_cast<int64_t>(toI64(body));
    case 'b':
        return body == "1";
    case 's':
        return body;
    case 'c': {
        std::vector<std::string> parts = split(body, ',');
        Color c;
        auto channel = [&](size_t i) { return static_cast<uint8_t>(i < parts.size() ? toI64(parts[i]) : 255); };
        c.r = channel(0);
        c.g = channel(1);
        c.b = channel(2);
        c.a = channel(3);
        return c;
    }
    case 'r': {
        std::vector<std::string> parts = split(body, ',');
        auto at = [&](size_t i) { return i < parts.size() ? toDouble(parts[i]) : 0.0; };
        return Rect{at(0), at(1), at(2), at(3)};
    }
    default:
        return toDouble(body);
    }
}

// "at:value:easing;..." (easing as its number).
std::string encodeKeyframes(const std::vector<Keyframe> &keyframes)
{
    std::string out;
    for (const Keyframe &k : keyframes) {
        if (!out.empty())
            out += ';';
        out += std::to_string(k.at) + ":" + formatDouble(k.value) + ":" + std::to_string(static_cast<int>(k.easing));
    }
    return out;
}

std::vector<Keyframe> decodeKeyframes(const std::string &text)
{
    std::vector<Keyframe> keyframes;
    for (const std::string &item : split(text, ';')) {
        std::vector<std::string> parts = split(item, ':');
        if (parts.size() != 3)
            continue;
        const int64_t easing = toI64(parts[2]);
        keyframes.push_back({toI64(parts[0]), toDouble(parts[1]),
                             easing >= 0 && easing <= static_cast<int64_t>(Easing::BounceInOut)
                                 ? static_cast<Easing>(easing)
                                 : Easing::Linear});
    }
    return keyframes;
}

void writeKeyframed(xmlNodePtr parent, const std::string &name, const KeyframedValue &value)
{
    addProperty(parent, name, formatDouble(value.value));
    if (!value.keyframes.empty())
        addProperty(parent, name + ".keyframes", encodeKeyframes(value.keyframes));
}

KeyframedValue readKeyframed(xmlNodePtr parent, const std::string &name, double fallback)
{
    KeyframedValue value;
    std::optional<std::string> text = getProperty(parent, name);
    value.value = text ? toDouble(*text) : fallback;
    if (std::optional<std::string> keyframes = getProperty(parent, name + ".keyframes"))
        value.keyframes = decodeKeyframes(*keyframes);
    return value;
}

} // namespace

xmlNodePtr addProperty(xmlNodePtr parent, const std::string &name, const std::string &value)
{
    xmlNodePtr prop = xmlNewTextChild(parent, nullptr, BAD_CAST "property", BAD_CAST value.c_str());
    xmlNewProp(prop, BAD_CAST "name", BAD_CAST name.c_str());
    return prop;
}

std::optional<std::string> getProperty(xmlNodePtr node, const std::string &name)
{
    for (xmlNodePtr child = node->children; child; child = child->next) {
        if (child->type != XML_ELEMENT_NODE || xmlStrcmp(child->name, BAD_CAST "property") != 0)
            continue;
        xmlChar *propName = xmlGetProp(child, BAD_CAST "name");
        if (!propName)
            continue;
        bool matches = name == reinterpret_cast<const char *>(propName);
        xmlFree(propName);
        if (!matches)
            continue;
        xmlChar *content = xmlNodeGetContent(child);
        std::string value = content ? reinterpret_cast<const char *>(content) : "";
        if (content)
            xmlFree(content);
        return value;
    }
    return std::nullopt;
}

void writeParams(xmlNodePtr parent, const std::string &prefix, const std::vector<Param> &params)
{
    if (params.empty())
        return;
    addProperty(parent, prefix + "count", std::to_string(params.size()));
    for (size_t i = 0; i < params.size(); ++i) {
        const std::string key = prefix + std::to_string(i) + ".";
        addProperty(parent, key + "name", params[i].name);
        addProperty(parent, key + "value", encodeValue(params[i].value));
        if (!params[i].keyframes.empty())
            addProperty(parent, key + "keyframes", encodeKeyframes(params[i].keyframes));
    }
}

std::vector<Param> readParams(xmlNodePtr parent, const std::string &prefix)
{
    std::vector<Param> params;
    const int64_t count = toI64(getProperty(parent, prefix + "count").value_or("0"));
    for (int64_t i = 0; i < count && i < 10'000; ++i) {
        const std::string key = prefix + std::to_string(i) + ".";
        Param param;
        param.name = getProperty(parent, key + "name").value_or("");
        param.value = decodeValue(getProperty(parent, key + "value").value_or("d:0"));
        if (std::optional<std::string> keyframes = getProperty(parent, key + "keyframes"))
            param.keyframes = decodeKeyframes(*keyframes);
        params.push_back(std::move(param));
    }
    return params;
}

void writeEffectFilter(xmlNodePtr parent, const Effect &effect, FrameIndex offset, FrameIndex length, bool withModel)
{
    xmlNodePtr filter = xmlNewChild(parent, nullptr, BAD_CAST "filter", nullptr);
    addProperty(filter, "mlt_service", effect.service);
    for (const Param &param : effect.params) {
        if (param.name.empty())
            continue;
        if (!param.keyframes.empty() && std::holds_alternative<double>(param.value))
            addProperty(filter, param.name, animationString(keyframesForCut(param.keyframes, offset, length)));
        else
            addProperty(filter, param.name, nativeValue(param.value));
    }
    if (!effect.enabled)
        addProperty(filter, "disable", "1");
    if (!withModel)
        return;
    addProperty(filter, "ustudio:effect_id", std::to_string(effect.id.value));
    addProperty(filter, "ustudio:service", effect.service);
    addProperty(filter, "ustudio:display_name", effect.displayName);
    addProperty(filter, "ustudio:enabled", effect.enabled ? "1" : "0");
    addProperty(filter, "ustudio:owner", effect.owner);
    writeParams(filter, "ustudio:param.", effect.params);
    writeKeyframed(filter, "ustudio:mix", effect.mix);
    if (effect.mask) {
        addProperty(filter, "ustudio:mask.shape", effect.mask->shape);
        addProperty(filter, "ustudio:mask.invert", effect.mask->invert ? "1" : "0");
        writeKeyframed(filter, "ustudio:mask.feather", effect.mask->feather);
        writeParams(filter, "ustudio:mask.param.", effect.mask->params);
    }
}

std::optional<Effect> readEffectFilter(xmlNodePtr filter)
{
    std::optional<std::string> id = getProperty(filter, "ustudio:effect_id");
    if (!id)
        return std::nullopt;
    Effect effect;
    effect.id = EffectId{static_cast<uint64_t>(toI64(*id))};
    effect.service = getProperty(filter, "ustudio:service").value_or(getProperty(filter, "mlt_service").value_or(""));
    effect.displayName = getProperty(filter, "ustudio:display_name").value_or("");
    effect.enabled = getProperty(filter, "ustudio:enabled").value_or("1") == "1";
    effect.owner = getProperty(filter, "ustudio:owner").value_or("");
    effect.params = readParams(filter, "ustudio:param.");
    effect.mix = readKeyframed(filter, "ustudio:mix", 1.0);
    if (std::optional<std::string> shape = getProperty(filter, "ustudio:mask.shape")) {
        EffectMask mask;
        mask.shape = *shape;
        mask.invert = getProperty(filter, "ustudio:mask.invert").value_or("0") == "1";
        mask.feather = readKeyframed(filter, "ustudio:mask.feather", 0.0);
        mask.params = readParams(filter, "ustudio:mask.param.");
        effect.mask = std::move(mask);
    }
    return effect;
}

std::vector<Effect> readEffectFilters(xmlNodePtr parent)
{
    std::vector<Effect> effects;
    for (xmlNodePtr child = parent->children; child; child = child->next)
        if (child->type == XML_ELEMENT_NODE && xmlStrcmp(child->name, BAD_CAST "filter") == 0)
            if (std::optional<Effect> effect = readEffectFilter(child))
                effects.push_back(std::move(*effect));
    return effects;
}

} // namespace ustudio::core::xml_detail

#pragma GCC diagnostic pop
