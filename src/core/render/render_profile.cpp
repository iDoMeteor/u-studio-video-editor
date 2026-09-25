#include "core/render/render_profile.h"

#include <algorithm>
#include <cmath>

namespace ustudio::core {

namespace {

struct Preset
{
    int crf;
    const char *preset;
    int64_t audioBitrate;
    double bitsPerPixel; // for encoders without a quality mode
};

// libx264's own scale: 18 is about visually lossless, 23 its default.
// Presets trade encode time for size at the same quality.
Preset presetFor(RenderProfile::Quality quality)
{
    switch (quality) {
    case RenderProfile::Quality::Draft:
        return {28, "veryfast", 128000, 0.05};
    case RenderProfile::Quality::Good:
        return {23, "medium", 192000, 0.08};
    case RenderProfile::Quality::High:
    case RenderProfile::Quality::Bitrate:
        return {18, "slow", 192000, 0.12};
    case RenderProfile::Quality::Max:
        return {14, "slower", 256000, 0.2};
    }
    return {18, "slow", 192000, 0.12};
}

int even(double value)
{
    return std::max(2, static_cast<int>(std::lround(value / 2.0)) * 2);
}

} // namespace

const std::vector<RenderProfile> &builtInRenderProfiles()
{
    static const std::vector<RenderProfile> profiles = {
        {.name = kDefaultRenderProfileName, .builtIn = true, .height = 0, .quality = RenderProfile::Quality::High},
        // The bitrates every render used before 0.36 (v1's MltEngine
        // settings), kept exactly.
        {.name = "Draft (legacy)",
         .builtIn = true,
         .height = 0,
         .quality = RenderProfile::Quality::Bitrate,
         .videoBitrate = 922698,
         .audioBitrate = 126422},
    };
    return profiles;
}

const RenderProfile &legacyRenderProfile()
{
    return builtInRenderProfiles()[1];
}

const std::vector<int> &renderHeights()
{
    static const std::vector<int> heights = {0, 2160, 1440, 1080, 720};
    return heights;
}

EncoderSettings encoderSettings(const RenderProfile &profile, const Profile &project, bool qualityMode)
{
    EncoderSettings settings;
    if (profile.height > 0 && profile.height != project.height) {
        // Square pixels at the project's display aspect.
        const double aspect = project.dar.den > 0 ? static_cast<double>(project.dar.num) / project.dar.den
                                                  : static_cast<double>(project.width) / project.height;
        settings.height = even(profile.height);
        settings.width = even(profile.height * aspect);
    }
    const int width = settings.width > 0 ? settings.width : project.width;
    const int height = settings.height > 0 ? settings.height : project.height;

    if (profile.quality == RenderProfile::Quality::Bitrate) {
        settings.videoBitrate = profile.videoBitrate;
        settings.audioBitrate = profile.audioBitrate;
        return settings;
    }
    const Preset preset = presetFor(profile.quality);
    settings.audioBitrate = preset.audioBitrate;
    if (qualityMode) {
        settings.crf = preset.crf;
        settings.preset = preset.preset;
    } else {
        const double fps = project.fps.den > 0 ? static_cast<double>(project.fps.num) / project.fps.den : 30.0;
        settings.videoBitrate = std::llround(preset.bitsPerPixel * width * height * fps);
    }
    return settings;
}

RenderThreads splitRenderThreads(int budget)
{
    budget = std::max(1, budget);
    RenderThreads threads;
    threads.frames = std::clamp(budget / 4, 1, 8);
    threads.encoder = std::max(1, static_cast<int>(std::lround(1.5 * (budget - threads.frames))));
    return threads;
}

int renderThreadBudget(int percent, int hardwareThreads)
{
    return std::max(1, std::max(1, hardwareThreads) * std::clamp(percent, 0, 100) / 100);
}

std::string renderProfileNameProblem(const std::string &name, const std::vector<RenderProfile> &others)
{
    if (name.find_first_not_of(" \t") == std::string::npos)
        return "Give the profile a name.";
    if (name.find_first_of("[]\n\r") != std::string::npos)
        return "A profile name can't contain [ or ].";
    for (const RenderProfile &builtIn : builtInRenderProfiles())
        if (builtIn.name == name)
            return "\"" + name + "\" is a built-in profile's name.";
    for (const RenderProfile &other : others)
        if (other.name == name)
            return "There's already a profile called \"" + name + "\".";
    return {};
}

} // namespace ustudio::core
