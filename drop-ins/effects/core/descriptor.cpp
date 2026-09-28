#include "core/descriptor.h"

#include "core/model/animation.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>

namespace ustudio::effects {

namespace {

constexpr ParamKind kKinds[] = {ParamKind::Scalar, ParamKind::Integer, ParamKind::Toggle, ParamKind::Color,
                                ParamKind::Rect,   ParamKind::Choice,  ParamKind::File,   ParamKind::Text};

std::optional<double> toDouble(const std::string &text)
{
    double value = 0.0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr == text.data())
        return std::nullopt;
    return value;
}

bool contains(const std::string &text, const char *part)
{
    return text.find(part) != std::string::npos;
}

std::optional<uint8_t> hexByte(const std::string &text, size_t at)
{
    if (at + 2 > text.size())
        return std::nullopt;
    unsigned value = 0;
    const auto result = std::from_chars(text.data() + at, text.data() + at + 2, value, 16);
    if (result.ec != std::errc() || result.ptr != text.data() + at + 2)
        return std::nullopt;
    return static_cast<uint8_t>(value);
}

// MLT's colour forms (mlt_property_get_color): "0xRRGGBBAA", "#RRGGBB",
// "#AARRGGBB" (alpha first with a '#'), or a packed integer.
core::Color parseColor(const std::string &text)
{
    core::Color color;
    auto channels = [&](size_t at, bool alphaFirst, bool withAlpha) {
        std::optional<uint8_t> b0 = hexByte(text, at), b1 = hexByte(text, at + 2), b2 = hexByte(text, at + 4);
        std::optional<uint8_t> b3 = withAlpha ? hexByte(text, at + 6) : std::optional<uint8_t>(255);
        if (!b0 || !b1 || !b2 || !b3)
            return;
        if (alphaFirst && withAlpha)
            color = {*b1, *b2, *b3, *b0};
        else
            color = {*b0, *b1, *b2, *b3};
    };
    if (text.starts_with("0x") || text.starts_with("0X"))
        channels(2, false, text.size() >= 10);
    else if (text.starts_with("#"))
        channels(1, true, text.size() >= 9);
    else if (std::optional<double> packed = toDouble(text)) {
        const auto value = static_cast<uint32_t>(static_cast<int64_t>(*packed));
        color = {static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 8),
                 static_cast<uint8_t>(value)};
    }
    return color;
}

core::Rect parseRect(const std::string &text)
{
    double parts[4] = {0, 0, 0, 0};
    size_t at = 0;
    for (double &part : parts) {
        while (at < text.size() &&
               (text[at] == ' ' || text[at] == ',' || text[at] == '/' || text[at] == ':' || text[at] == 'x'))
            ++at;
        const auto result = std::from_chars(text.data() + at, text.data() + text.size(), part);
        if (result.ec != std::errc())
            break;
        at = static_cast<size_t>(result.ptr - text.data());
        if (at < text.size() && text[at] == '%')
            ++at;
    }
    return {parts[0], parts[1], parts[2], parts[3]};
}

Json valueToJson(const core::Param::Value &value)
{
    return std::visit(
        [](const auto &v) -> Json {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, double>)
                return Json(Json::Object{{"d", Json(v)}});
            else if constexpr (std::is_same_v<T, int64_t>)
                return Json(Json::Object{{"i", Json(static_cast<double>(v))}});
            else if constexpr (std::is_same_v<T, bool>)
                return Json(Json::Object{{"b", Json(v)}});
            else if constexpr (std::is_same_v<T, std::string>)
                return Json(Json::Object{{"s", Json(v)}});
            else if constexpr (std::is_same_v<T, core::Color>)
                return Json(Json::Object{
                    {"c", Json(Json::Array{Json(int(v.r)), Json(int(v.g)), Json(int(v.b)), Json(int(v.a))})}});
            else
                return Json(Json::Object{{"r", Json(Json::Array{Json(v.x), Json(v.y), Json(v.w), Json(v.h)})}});
        },
        value);
}

core::Param::Value valueFromJson(const Json &json)
{
    if (json.has("i"))
        return static_cast<int64_t>(json["i"].asNumber());
    if (json.has("b"))
        return json["b"].asBool();
    if (json.has("s"))
        return json["s"].asString();
    if (json.has("c")) {
        const Json::Array &c = json["c"].asArray();
        auto channel = [&](size_t i) { return static_cast<uint8_t>(i < c.size() ? c[i].asNumber(255) : 255); };
        return core::Color{channel(0), channel(1), channel(2), channel(3)};
    }
    if (json.has("r")) {
        const Json::Array &r = json["r"].asArray();
        auto at = [&](size_t i) { return i < r.size() ? r[i].asNumber() : 0.0; };
        return core::Rect{at(0), at(1), at(2), at(3)};
    }
    return json["d"].asNumber();
}

Json stringsToJson(const std::vector<std::string> &strings)
{
    Json::Array out;
    for (const std::string &s : strings)
        out.emplace_back(s);
    return Json(std::move(out));
}

std::vector<std::string> stringsFromJson(const Json &json)
{
    std::vector<std::string> out;
    for (const Json &item : json.asArray())
        if (item.isString())
            out.push_back(item.asString());
    return out;
}

std::optional<double> optionalNumber(const Json &json)
{
    return json.isNumber() ? std::optional(json.asNumber()) : std::nullopt;
}

Json optionalToJson(std::optional<double> value)
{
    return value ? Json(*value) : Json();
}

} // namespace

const char *paramKindName(ParamKind kind)
{
    switch (kind) {
    case ParamKind::Scalar:
        return "scalar";
    case ParamKind::Integer:
        return "integer";
    case ParamKind::Toggle:
        return "toggle";
    case ParamKind::Color:
        return "color";
    case ParamKind::Rect:
        return "rect";
    case ParamKind::Choice:
        return "choice";
    case ParamKind::File:
        return "file";
    case ParamKind::Text:
        return "text";
    }
    return "text";
}

std::optional<ParamKind> paramKindFromName(const std::string &name)
{
    for (ParamKind kind : kKinds)
        if (name == paramKindName(kind))
            return kind;
    return std::nullopt;
}

double DisplayMap::toDisplay(double value) const
{
    if (fromMax == fromMin)
        return toMin;
    return toMin + (value - fromMin) * (toMax - toMin) / (fromMax - fromMin);
}

double DisplayMap::fromDisplay(double shown) const
{
    if (toMax == toMin)
        return fromMin;
    return fromMin + (shown - toMin) * (fromMax - fromMin) / (toMax - toMin);
}

std::string familyOf(const std::string &service)
{
    const size_t dot = service.find('.');
    if (dot == std::string::npos)
        return "mlt";
    const std::string prefix = service.substr(0, dot);
    for (const char *known : {"frei0r", "avfilter", "sox", "ladspa", "lv2", "vst2", "openfx", "movit", "opencv"})
        if (prefix == known)
            return prefix;
    return "mlt";
}

core::Param::Value parseValue(ParamKind kind, const std::string &text)
{
    switch (kind) {
    case ParamKind::Scalar:
        return toDouble(text).value_or(0.0);
    case ParamKind::Integer:
        return static_cast<int64_t>(toDouble(text).value_or(0.0));
    case ParamKind::Toggle:
        return text == "1" || text == "yes" || text == "true" || text == "on";
    case ParamKind::Color:
        return parseColor(text);
    case ParamKind::Rect:
        return parseRect(text);
    case ParamKind::Choice:
    case ParamKind::File:
    case ParamKind::Text:
        return text;
    }
    return text;
}

ParamDescriptor normaliseParam(const std::string &family, const RawParam &raw)
{
    ParamDescriptor param;
    param.id = raw.identifier;
    param.title = raw.title.empty() ? raw.identifier : raw.title;
    param.description = raw.description;
    param.minimum = raw.minimum;
    param.maximum = raw.maximum;

    const std::string &type = raw.type;
    const std::string &widget = raw.widget;
    if (type == "float")
        param.kind = ParamKind::Scalar;
    else if (type == "integer")
        param.kind = widget == "checkbox"                       ? ParamKind::Toggle
                     : widget == "color"                        ? ParamKind::Color
                     : widget == "combo" && !raw.values.empty() ? ParamKind::Choice
                                                                : ParamKind::Integer;
    else if (type == "boolean")
        param.kind = ParamKind::Toggle;
    else if (type == "color" || widget == "color")
        param.kind = ParamKind::Color;
    else if (type == "rect")
        param.kind = ParamKind::Rect;
    else if (!raw.values.empty())
        param.kind = ParamKind::Choice;
    else if (widget == "fileopen")
        param.kind = ParamKind::File;
    else
        param.kind = ParamKind::Text;
    if (param.kind == ParamKind::Choice)
        param.choices = raw.values;

    // Which ones animate: MLT says so per parameter ("animation: yes");
    // frei0r's module marks every double and colour; avfilter's metadata
    // doesn't say, but its numeric options take keyframes (its YAML notes).
    param.animatable = raw.animation || (family == "avfilter" && (type == "float" || type == "integer"));
    // Keyframes are numbers in the model (core::Keyframe): only scalars
    // animate for now.
    if (param.kind != ParamKind::Scalar)
        param.animatable = false;

    param.hidden = raw.readonly || type == "properties" || contains(raw.identifier, "*") ||
                   contains(raw.identifier, "DEPRECATED") || contains(raw.title, "DEPRECATED") ||
                   raw.identifier.starts_with("_") || raw.identifier == "threads" || raw.identifier == "av.threads" ||
                   // avfilter's "position" is MLT plumbing (which clock the filter sees), not the effect's
                   (family == "avfilter" && raw.identifier == "position") || raw.identifier.empty();

    // A number that isn't one ("nan": frei0r's defish0r reports that for
    // its "Non-Linear scale", which then blacks out the picture; the health
    // probe found it) is no default at all.
    const bool numeric = param.kind == ParamKind::Scalar || param.kind == ParamKind::Integer;
    const std::optional<double> number = numeric ? toDouble(raw.defaultValue) : std::nullopt;
    if (!raw.defaultValue.empty() && (!numeric || (number && std::isfinite(*number)))) {
        param.defaultValue = parseValue(param.kind, raw.defaultValue);
        param.hasDefault = true;
    } else {
        // Something to show until set: the range's low end, or the kind's zero.
        param.defaultValue = parseValue(param.kind, param.minimum ? std::to_string(*param.minimum) : std::string());
    }
    return param;
}

EffectDescriptor normalise(const RawEffect &raw)
{
    EffectDescriptor descriptor;
    descriptor.service = raw.service;
    descriptor.family = familyOf(raw.service);
    descriptor.name = raw.title.empty() ? raw.service : raw.title;
    descriptor.description = raw.description;
    descriptor.tags = raw.tags;
    const bool audio = std::find(raw.tags.begin(), raw.tags.end(), "Audio") != raw.tags.end();
    const bool video = std::find(raw.tags.begin(), raw.tags.end(), "Video") != raw.tags.end();
    descriptor.media = audio && !video ? MediaKind::Audio : MediaKind::Video;
    // Until an overlay says otherwise: the family, split by media.
    descriptor.category = descriptor.media == MediaKind::Audio ? "Audio" : "Video";
    for (const RawParam &rawParam : raw.params)
        descriptor.params.push_back(normaliseParam(descriptor.family, rawParam));
    return descriptor;
}

void applyOverlay(EffectDescriptor &descriptor, const Json &overlay)
{
    if (!overlay.isObject())
        return;
    if (overlay["name"].isString())
        descriptor.name = overlay["name"].asString();
    if (overlay["description"].isString())
        descriptor.description = overlay["description"].asString();
    if (overlay["category"].isString())
        descriptor.category = overlay["category"].asString();
    if (overlay["tags"].isArray())
        descriptor.tags = stringsFromJson(overlay["tags"]);
    if (overlay["featured"].isBool())
        descriptor.featured = overlay["featured"].asBool();
    if (overlay["hidden"].isBool())
        descriptor.hidden = overlay["hidden"].asBool();
    if (overlay["unstable"].isString())
        descriptor.unstable = overlay["unstable"].asString();
    for (const auto &[id, paramOverlay] : overlay["params"].asObject()) {
        auto it = std::find_if(descriptor.params.begin(), descriptor.params.end(),
                               [&](const ParamDescriptor &p) { return p.id == id; });
        if (it == descriptor.params.end() || !paramOverlay.isObject())
            continue;
        if (paramOverlay["name"].isString())
            it->title = paramOverlay["name"].asString();
        if (paramOverlay["hidden"].isBool())
            it->hidden = paramOverlay["hidden"].asBool();
        const Json &def = paramOverlay["default"];
        if (def.isNumber() || def.isBool() || def.isString()) {
            const std::string text = def.isNumber() ? core::formatDouble(def.asNumber())
                                     : def.isBool() ? (def.asBool() ? "1" : "0")
                                                    : def.asString();
            it->defaultValue = parseValue(it->kind, text);
            it->hasDefault = true;
        }
        const Json &display = paramOverlay["display"];
        if (display.isObject()) {
            const Json::Array &from = display["from"].asArray();
            const Json::Array &to = display["to"].asArray();
            if (from.size() == 2 && to.size() == 2)
                it->display = DisplayMap{from[0].asNumber(), from[1].asNumber(), to[0].asNumber(), to[1].asNumber(),
                                         display["unit"].asString()};
        }
    }
}

Json toJson(const EffectDescriptor &descriptor)
{
    Json out;
    out.set("service", descriptor.service);
    out.set("family", descriptor.family);
    out.set("name", descriptor.name);
    out.set("description", descriptor.description);
    out.set("category", descriptor.category);
    out.set("tags", stringsToJson(descriptor.tags));
    out.set("media", descriptor.media == MediaKind::Audio ? "audio" : "video");
    out.set("featured", descriptor.featured);
    out.set("hidden", descriptor.hidden);
    out.set("not_thread_safe", descriptor.notThreadSafe);
    out.set("unstable", descriptor.unstable);
    Json::Array params;
    for (const ParamDescriptor &p : descriptor.params) {
        Json param;
        param.set("id", p.id);
        param.set("title", p.title);
        param.set("description", p.description);
        param.set("kind", paramKindName(p.kind));
        param.set("min", optionalToJson(p.minimum));
        param.set("max", optionalToJson(p.maximum));
        param.set("default", valueToJson(p.defaultValue));
        param.set("has_default", p.hasDefault);
        param.set("choices", stringsToJson(p.choices));
        param.set("animatable", p.animatable);
        param.set("hidden", p.hidden);
        if (p.display) {
            Json display;
            display.set("from", Json(Json::Array{Json(p.display->fromMin), Json(p.display->fromMax)}));
            display.set("to", Json(Json::Array{Json(p.display->toMin), Json(p.display->toMax)}));
            display.set("unit", p.display->unit);
            param.set("display", std::move(display));
        }
        params.push_back(std::move(param));
    }
    out.set("params", Json(std::move(params)));
    return out;
}

std::optional<EffectDescriptor> descriptorFromJson(const Json &json)
{
    if (!json["service"].isString())
        return std::nullopt;
    EffectDescriptor d;
    d.service = json["service"].asString();
    d.family = json["family"].asString(familyOf(d.service));
    d.name = json["name"].asString(d.service);
    d.description = json["description"].asString();
    d.category = json["category"].asString();
    d.tags = stringsFromJson(json["tags"]);
    d.media = json["media"].asString() == "audio" ? MediaKind::Audio : MediaKind::Video;
    d.featured = json["featured"].asBool();
    d.hidden = json["hidden"].asBool();
    d.notThreadSafe = json["not_thread_safe"].asBool();
    d.unstable = json["unstable"].asString();
    for (const Json &item : json["params"].asArray()) {
        ParamDescriptor p;
        p.id = item["id"].asString();
        p.title = item["title"].asString();
        p.description = item["description"].asString();
        p.kind = paramKindFromName(item["kind"].asString()).value_or(ParamKind::Text);
        p.minimum = optionalNumber(item["min"]);
        p.maximum = optionalNumber(item["max"]);
        p.defaultValue = valueFromJson(item["default"]);
        p.hasDefault = item["has_default"].asBool();
        p.choices = stringsFromJson(item["choices"]);
        p.animatable = item["animatable"].asBool();
        p.hidden = item["hidden"].asBool();
        const Json &display = item["display"];
        if (display.isObject()) {
            const Json::Array &from = display["from"].asArray();
            const Json::Array &to = display["to"].asArray();
            if (from.size() == 2 && to.size() == 2)
                p.display = DisplayMap{from[0].asNumber(), from[1].asNumber(), to[0].asNumber(), to[1].asNumber(),
                                       display["unit"].asString()};
        }
        d.params.push_back(std::move(p));
    }
    return d;
}

std::map<std::string, std::string> unstableServices(const std::vector<Json> &overlays)
{
    std::map<std::string, std::string> services;
    for (const Json &overlay : overlays)
        for (const auto &[service, entry] : overlay.asObject())
            if (entry["unstable"].isString())
                services[service] = entry["unstable"].asString();
    return services;
}

core::Effect makeEffect(const EffectDescriptor &descriptor)
{
    core::Effect effect;
    effect.service = descriptor.service;
    effect.displayName = descriptor.name;
    effect.owner = kOwner;
    for (const ParamDescriptor &p : descriptor.params) {
        if (p.hidden || !p.hasDefault)
            continue;
        core::Param param;
        param.name = p.id;
        param.value = p.defaultValue;
        effect.params.push_back(std::move(param));
    }
    return effect;
}

} // namespace ustudio::effects
