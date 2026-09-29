#pragma once

// What the effects drop-in knows about an effect it can offer (doc 15,
// "Descriptor normalisation"): plain data, so the Rack and Browser (app/)
// build their UI without MLT types. engine/registry.cpp fills it from
// Mlt::Repository metadata; curated overlays (data/overlays/*.json) polish it.

#include "core/json.h"
#include "core/model/types.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ustudio::effects {

// One widget per kind (doc 15's table).
enum class ParamKind
{
    Scalar,  // float / frei0r double: draggable number + slider
    Integer, // draggable number
    Toggle,  // switch
    Color,   // swatch + eyedropper
    Rect,    // on-preview handles (FX4) + numbers
    Choice,  // dropdown over `choices`
    File,    // file chooser
    Text,    // entry
};
const char *paramKindName(ParamKind kind);
std::optional<ParamKind> paramKindFromName(const std::string &name);

// frei0r normalises most doubles to 0-1; an overlay maps them to what they
// mean ("Radius 12 px" rather than 0.0468).
struct DisplayMap
{
    double fromMin = 0.0, fromMax = 1.0; // the parameter's own range
    double toMin = 0.0, toMax = 1.0;     // what the user sees
    std::string unit;

    double toDisplay(double value) const;
    double fromDisplay(double shown) const;
    bool operator==(const DisplayMap &) const = default;
};

struct ParamDescriptor
{
    std::string id; // the MLT property name ("0" for frei0r's first parameter)
    std::string title;
    std::string description;
    ParamKind kind = ParamKind::Scalar;
    std::optional<double> minimum, maximum;
    core::Param::Value defaultValue = 0.0;
    bool hasDefault = false; // MLT's metadata (or an overlay) gave one; else the service's own applies
    std::vector<std::string> choices;
    bool animatable = false;
    bool hidden = false; // plumbing (threads, deprecated, wildcard properties); never shown
    std::optional<DisplayMap> display;
    // A file's extensions the chooser offers ("cube"), lower case, no dot;
    // empty: any file.
    std::vector<std::string> extensions;

    bool operator==(const ParamDescriptor &) const = default;
};

enum class MediaKind
{
    Video,
    Audio,
};

struct EffectDescriptor
{
    std::string service; // the MLT service id: the descriptor's key
    std::string family;  // "frei0r", "avfilter", "sox", "ladspa", "mlt"
    std::string name;
    std::string description;
    std::string category;
    std::vector<std::string> tags;
    MediaKind media = MediaKind::Video;
    bool featured = false;
    bool hidden = false;        // plumbing services, and anything an overlay hides
    bool notThreadSafe = false; // MLT serialises it (frei0r's not_thread_safe.txt): slower, still safe
    std::string unstable;       // an overlay's reason to treat it as quarantined whatever the probe says
    std::vector<ParamDescriptor> params;

    bool operator==(const EffectDescriptor &) const = default;
};

// The family of a service id: its prefix before '.', or "mlt" (MLT's own).
std::string familyOf(const std::string &service);

// One parameter as MLT's metadata YAML describes it, read into strings by
// the engine (engine/registry.cpp) so normalisation is pure and testable.
struct RawParam
{
    std::string identifier{};
    std::string title{};
    std::string description{};
    std::string type{};   // "float", "integer", "boolean", "color", "rect", "string", "time", "properties", ""
    std::string widget{}; // "spinner", "checkbox", "combo", "color", "fileopen", ...
    std::optional<double> minimum{}, maximum{};
    std::string defaultValue{};
    std::vector<std::string> values{}; // a string's allowed values
    bool animation = false;            // "animation: yes"
    bool readonly = false;
};

struct RawEffect
{
    std::string service{};
    std::string title{};
    std::string description{};
    std::vector<std::string> tags{}; // "Video", "Audio", ...
    std::vector<RawParam> params{};
};

// The descriptor for `raw`, before overlays.
EffectDescriptor normalise(const RawEffect &raw);
ParamDescriptor normaliseParam(const std::string &family, const RawParam &raw);

// A default for the parameter's kind from MLT's text form ("0.5", "1",
// "0xff0000ff", "#ff0000", "0 0 100 100").
core::Param::Value parseValue(ParamKind kind, const std::string &text);

// Overlays: one JSON object keyed by service id (doc 15, "Curated
// overlays"). Fields: name, category, tags, featured, hidden, description,
// unstable (a reason: known broken in a way the probe can't always catch),
// and params by id with name, hidden, kind (paramKindName(): "file" for a
// text MLT means as a path), extensions (a file's, for its chooser),
// default (a number, bool or string), display {from:[a,b], to:[c,d], unit}.
// Unknown fields are ignored; a malformed overlay changes nothing.
void applyOverlay(EffectDescriptor &descriptor, const Json &overlay);

// Services the overlays mark unstable, with their reasons.
std::map<std::string, std::string> unstableServices(const std::vector<Json> &overlays);

// The registry cache's form (engine/registry.cpp): every field round-trips.
Json toJson(const EffectDescriptor &descriptor);
std::optional<EffectDescriptor> descriptorFromJson(const Json &json);

// A new model effect for `descriptor`: its service and display name, every
// shown parameter that has a default at it (the rest are left to the
// service until set), owner "effects".
core::Effect makeEffect(const EffectDescriptor &descriptor);

// The owner this drop-in writes on its effects (core::Effect::owner).
inline constexpr const char *kOwner = "effects";

} // namespace ustudio::effects
