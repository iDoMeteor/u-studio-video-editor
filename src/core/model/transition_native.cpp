#include "core/model/transition_native.h"

#include "core/model/animation.h"
#include "core/model/effect_native.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>

namespace ustudio::core {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<std::string> split(const std::string &text, char separator)
{
    std::vector<std::string> parts;
    std::string part;
    std::istringstream in(text);
    while (std::getline(in, part, separator))
        parts.push_back(part);
    return parts;
}

// "ramp:1,0,0" over a transition `length` frames long: "0=1;<mid>=0;<end>=0".
// Rect values (with spaces) are separated by '|' instead of ','.
std::string expandRamp(const std::string &value, FrameIndex length)
{
    static constexpr std::string_view kRamp = "ramp:";
    if (!value.starts_with(kRamp))
        return value;
    const std::string body = value.substr(kRamp.size());
    const std::vector<std::string> points = split(body, body.find('|') != std::string::npos ? '|' : ',');
    if (points.empty())
        return value;
    if (points.size() == 1)
        return points.front();
    const FrameIndex last = std::max<FrameIndex>(length - 1, 1);
    std::string out;
    for (size_t i = 0; i < points.size(); ++i) {
        const auto at = static_cast<FrameIndex>(
            std::llround(static_cast<double>(last) * static_cast<double>(i) / static_cast<double>(points.size() - 1)));
        if (!out.empty())
            out += ';';
        out += std::to_string(at) + "=" + points[i];
    }
    return out;
}

std::string valueOf(const Param &param, FrameIndex length)
{
    return expandRamp(nativeValue(param.value), length);
}

// "a.<n>.<prop>" -> (n, prop); false for anything else.
bool filterKey(const std::string &name, char side, size_t &index, std::string &prop)
{
    if (name.size() < 5 || name[0] != side || name[1] != '.')
        return false;
    const size_t dot = name.find('.', 2);
    if (dot == std::string::npos || dot == 2)
        return false;
    const std::string number = name.substr(2, dot - 2);
    if (!std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
        number.size() > 2)
        return false;
    index = static_cast<size_t>(std::stoul(number));
    prop = name.substr(dot + 1);
    return !prop.empty();
}

// --- The wipe maps --------------------------------------------------------

using MapFunction = std::function<double(double x, double y)>;

std::map<std::string, MapFunction> mapFunctions(int width, int height)
{
    const double aspect = static_cast<double>(width) / height;
    const double radiusMax = std::hypot(0.5 * aspect, 0.5);
    auto cx = [aspect](double x) { return (x - 0.5) * aspect; };
    auto cy = [](double y) { return y - 0.5; };
    auto star = [=](double x, double y, int points, double inner) {
        const double u = cx(x), v = cy(y);
        const double r = std::hypot(u, v) / radiusMax;
        const double edge = inner + (1 - inner) * (std::cos(points * std::atan2(v, u)) + 1) / 2;
        return std::min(1.0, r / edge);
    };
    // Fixed seed: the same "blocks" map every time, on every machine.
    std::vector<double> blocks(16 * 9);
    std::mt19937 random(4217);
    for (double &b : blocks)
        b = static_cast<double>(random() % 65536) / 65535.0;
    return {
        {"left", [](double x, double) { return x; }},
        {"right", [](double x, double) { return 1 - x; }},
        {"down", [](double, double y) { return y; }},
        {"up", [](double, double y) { return 1 - y; }},
        {"down-right", [](double x, double y) { return (x + y) / 2; }},
        {"down-left", [](double x, double y) { return (1 - x + y) / 2; }},
        {"up-right", [](double x, double y) { return (x + 1 - y) / 2; }},
        {"up-left", [](double x, double y) { return (2 - x - y) / 2; }},
        {"radial", [=](double x, double y) { return std::min(1.0, std::hypot(cx(x), cy(y)) / radiusMax); }},
        {"iris-out", [=](double x, double y) { return 1 - std::min(1.0, std::hypot(cx(x), cy(y)) / radiusMax); }},
        {"clock",
         [=](double x, double y) {
             const double a = std::atan2(cx(x), -cy(y)) / (2 * kPi);
             return a - std::floor(a);
         }},
        {"diamond",
         [=](double x, double y) { return std::min(1.0, (std::abs(cx(x)) / (0.5 * aspect) + std::abs(cy(y)) / 0.5) / 2); }},
        {"barn-door-open", [](double x, double) { return std::abs(x - 0.5) * 2; }},
        {"barn-door-close", [](double x, double) { return 1 - std::abs(x - 0.5) * 2; }},
        {"blinds",
         [](double, double y) {
             const double v = y * 8;
             return v - std::floor(v);
         }},
        {"checker",
         [](double x, double y) {
             const double v = x * 8;
             const bool odd = (static_cast<int>(x * 8) + static_cast<int>(y * 4.5)) % 2 != 0;
             return (v - std::floor(v)) * 0.5 + (odd ? 0.5 : 0.0);
         }},
        {"blocks",
         [blocks](double x, double y) {
             const int row = std::min(8, static_cast<int>(y * 9)), column = std::min(15, static_cast<int>(x * 16));
             return blocks[static_cast<size_t>(row * 16 + column)];
         }},
        {"star", [=](double x, double y) { return star(x, y, 5, 0.45); }},
        {"sparkle", [=](double x, double y) { return star(x, y, 4, 0.25); }},
        {"horn",
         [](double x, double y) {
             return std::clamp((1 - y) * 0.7 + std::abs(x - 0.5) * 1.4 * (1 - y * 0.5), 0.0, 1.0);
         }},
    };
}

} // namespace

NativeTransition nativeTransition(const Transition &transition)
{
    NativeTransition out;
    out.video.service = transition.service.empty() ? "luma" : transition.service;
    out.audio.service = "mix";
    std::vector<std::pair<std::string, std::string>> audio;
    std::map<size_t, NativeFilter> tail, head;
    for (const Param &param : transition.params) {
        const std::string &name = param.name;
        const std::string value = valueOf(param, transition.length);
        size_t index = 0;
        std::string prop;
        if (name == "video.service")
            out.video.service = value;
        else if (name == "video.luma")
            out.luma = value;
        else if (name.starts_with("video."))
            out.video.properties.emplace_back(name.substr(6), value);
        else if (name == "audio.service")
            out.audio.service = value;
        else if (name.starts_with("audio."))
            audio.emplace_back(name.substr(6), value);
        else if (filterKey(name, 'a', index, prop) || filterKey(name, 'b', index, prop)) {
            NativeFilter &filter = (name[0] == 'a' ? tail : head)[index];
            if (prop == "service")
                filter.service = value;
            else
                filter.properties.emplace_back(prop, value);
        }
    }
    // The audio crossfade as it has always been (start=-1: "automatic linear
    // crossfade", transition_mix.yml), unless the params say otherwise.
    if (audio.empty() && out.audio.service == "mix")
        out.audio.properties.emplace_back("start", "-1");
    out.audio.properties.insert(out.audio.properties.end(), audio.begin(), audio.end());
    for (auto &[index, filter] : tail)
        if (!filter.service.empty())
            out.tailFilters.push_back(std::move(filter));
    for (auto &[index, filter] : head)
        if (!filter.service.empty())
            out.headFilters.push_back(std::move(filter));
    // A service this build doesn't offer (a drop-in's, the drop-in not
    // here): the whole recipe plays as the plain dissolve, as an unknown
    // wipe map does, never a load failure (doc 15). Its params stay in the
    // model, so a build with the drop-in plays the style again.
    bool offered = transitionServiceAllowed(out.video.service);
    for (const auto *filters : {&out.tailFilters, &out.headFilters})
        for (const NativeFilter &filter : *filters)
            offered = offered && transitionServiceAllowed(filter.service);
    if (!offered) {
        out.video = {"luma", {}};
        out.luma.clear();
        out.tailFilters.clear();
        out.headFilters.clear();
    }
    if (!transitionServiceAllowed(out.audio.service))
        out.audio = {"mix", {{"start", "-1"}}};
    return out;
}

namespace {
std::mutex g_registeredMutex;
std::set<std::string> &registeredServices()
{
    static std::set<std::string> services;
    return services;
}
} // namespace

bool transitionServicesOffered(const Transition &transition)
{
    if (!transition.service.empty() && !transitionServiceAllowed(transition.service))
        return false;
    for (const Param &param : transition.params) {
        const std::string &name = param.name;
        const bool service = name == "video.service" || name == "audio.service" ||
                             ((name.starts_with("a.") || name.starts_with("b.")) && name.ends_with(".service"));
        if (service && !transitionServiceAllowed(valueOf(param, transition.length)))
            return false;
    }
    return true;
}

void registerTransitionService(const std::string &service)
{
    std::lock_guard lock(g_registeredMutex);
    registeredServices().insert(service);
}

bool transitionServiceAllowed(const std::string &service)
{
    // Project files are untrusted: only what the recipes use. Never a Qt
    // service (ADR-007), never anything that runs arbitrary code or files.
    static const char *kAllowed[] = {"luma",     "mix",           "composite",  "affine",   "brightness",
                                     "volume",   "movit.luma_mix", "movit.mix", "movit.rect"};
    if (std::find(std::begin(kAllowed), std::end(kAllowed), service) != std::end(kAllowed))
        return true;
    std::lock_guard lock(g_registeredMutex);
    return registeredServices().contains(service);
}

std::string transitionProblem(const Transition &transition)
{
    const NativeTransition native = nativeTransition(transition);
    if (!transitionServiceAllowed(native.video.service))
        return "transition video service not allowed: " + native.video.service;
    if (!transitionServiceAllowed(native.audio.service))
        return "transition audio service not allowed: " + native.audio.service;
    for (const auto *filters : {&native.tailFilters, &native.headFilters})
        for (const NativeFilter &filter : *filters)
            if (!transitionServiceAllowed(filter.service))
                return "transition filter service not allowed: " + filter.service;
    // Nothing in the file may name something for MLT to open: a map comes
    // only from video.luma, by name. `affine`'s filter takes a producer as
    // its `background` and passes `producer.*`/`transition.*` on
    // (filter_affine.yml), so the names are checked on every service, at
    // any depth ("transition.producer.resource").
    auto opensSomething = [](const std::string &name) {
        const std::string leaf = name.substr(name.rfind('.') + 1);
        return leaf == "resource" || leaf == "factory" || leaf == "background" || leaf == "luma" ||
               name.starts_with("producer.") || name.find(".producer.") != std::string::npos;
    };
    for (const NativeFilter *service : {&native.video, &native.audio})
        for (const auto &[name, value] : service->properties)
            if (opensSomething(name))
                return "transition property not allowed: " + name;
    for (const auto *filters : {&native.tailFilters, &native.headFilters})
        for (const NativeFilter &filter : *filters)
            for (const auto &[name, value] : filter.properties)
                if (opensSomething(name))
                    return "transition property not allowed: " + name;
    // An unknown wipe map isn't a problem: it plays as the plain dissolve
    // (doc 15: never a load failure).
    return {};
}

const std::vector<std::string> &lumaMapNames()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (const auto &[name, fn] : mapFunctions(16, 9))
            out.push_back(name);
        return out;
    }();
    return names;
}

std::filesystem::path lumaMapPath(const std::filesystem::path &folder, const std::string &name)
{
    return folder / ("v" + std::to_string(kLumaMapVersion)) / (name + ".pgm");
}

std::vector<uint16_t> lumaMapPixels(const std::string &name, int width, int height)
{
    std::vector<uint16_t> pixels;
    if (width <= 0 || height <= 0)
        return pixels;
    const std::map<std::string, MapFunction> maps = mapFunctions(width, height);
    auto it = maps.find(name);
    if (it == maps.end())
        return pixels;
    pixels.reserve(static_cast<size_t>(width) * static_cast<size_t>(height));
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const double v = std::clamp(it->second((x + 0.5) / width, (y + 0.5) / height), 0.0, 1.0);
            pixels.push_back(static_cast<uint16_t>(std::lround(v * 65535.0)));
        }
    return pixels;
}

bool writeLumaMap(const std::string &name, const std::filesystem::path &file, int width, int height)
{
    std::error_code ec;
    if (std::filesystem::is_regular_file(file, ec))
        return !lumaMapPixels(name, 1, 1).empty();
    const std::vector<uint16_t> pixels = lumaMapPixels(name, width, height);
    if (pixels.empty())
        return false;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::string data = "P5\n" + std::to_string(width) + " " + std::to_string(height) + "\n65535\n";
    data.reserve(data.size() + pixels.size() * 2);
    for (uint16_t sample : pixels) {
        data += static_cast<char>(sample >> 8); // big-endian, as PGM wants
        data += static_cast<char>(sample & 0xFF);
    }
    // A temp file of its own, then a rename: a reader (MLT) never sees half a
    // map, and two processes making the same one (the editor and a render
    // child) don't write into each other's.
    const std::filesystem::path temp = file.string() + ".part" + std::to_string(std::random_device{}());
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    std::ifstream check(temp, std::ios::binary | std::ios::ate);
    if (!check || static_cast<size_t>(check.tellg()) != data.size()) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    check.close();
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

} // namespace ustudio::core
