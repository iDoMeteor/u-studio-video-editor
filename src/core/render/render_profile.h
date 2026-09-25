#pragma once

#include "core/model/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ustudio::core {

// A named set of render choices (Settings > Render). Always MP4, H.264 +
// AAC; the output frame rate is the project's (see encoderSettings()).
struct RenderProfile
{
    enum class Quality
    {
        Draft,
        Good,
        High,
        Max,
        Bitrate, // videoBitrate/audioBitrate as given
    };

    std::string name;
    bool builtIn = false;
    int height = 0; // output height in pixels; 0 = the project's size
    Quality quality = Quality::High;
    int64_t videoBitrate = 0; // bits/s, Quality::Bitrate only
    int64_t audioBitrate = 0;

    bool operator==(const RenderProfile &) const = default;
};

// "High quality" (the default) and "Draft (legacy)", the settings every
// render used before profiles existed.
const std::vector<RenderProfile> &builtInRenderProfiles();
constexpr const char *kDefaultRenderProfileName = "High quality";
const RenderProfile &legacyRenderProfile();

// The heights Settings offers; 0 is "Project".
const std::vector<int> &renderHeights();

// What the encoder is told. crf >= 0 means constant quality (with `preset`);
// otherwise the bitrates apply.
struct EncoderSettings
{
    int width = 0;
    int height = 0;
    int crf = -1;
    std::string preset;
    int64_t videoBitrate = 0;
    int64_t audioBitrate = 0;
};

// `qualityMode` is false when the H.264 encoder has no constant-quality
// mode (OpenH264): the Draft..Max presets then become bitrates scaled to
// the output's pixel rate.
EncoderSettings encoderSettings(const RenderProfile &profile, const Profile &project, bool qualityMode);

// Empty if `name` can be a user profile's name, else why not. `others` are
// the user profiles it must not clash with (built-ins are checked here).
std::string renderProfileNameProblem(const std::string &name, const std::vector<RenderProfile> &others);

} // namespace ustudio::core
