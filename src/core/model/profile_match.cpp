#include "core/model/profile_match.h"

#include <algorithm>
#include <cstdio>
#include <numeric>

namespace ustudio::core {

bool sequenceTakesProfileFromMedia(const Project &project)
{
    auto seq = std::find_if(project.sequences.begin(), project.sequences.end(),
                            [&](const Sequence &s) { return s.id == project.activeSequence; });
    if (seq == project.sequences.end() || !seq->clips.empty())
        return false;
    return std::none_of(project.bin.begin(), project.bin.end(), [](const Asset &asset) {
        return asset.info.width > 0 && asset.info.fps.num > 0 && asset.info.fps.den > 0 && !asset.info.isStillImage;
    });
}

Profile profileForMedia(const Profile &current, int width, int height, Rational fps)
{
    Profile profile = current;
    profile.width = width;
    profile.height = height;
    profile.fps = fps;
    profile.sar = {1, 1};
    const int divisor = std::gcd(width, height);
    profile.dar = {width / divisor, height / divisor};
    profile.mltName.clear();
    return profile;
}

std::string formatFps(Rational fps)
{
    if (fps.num <= 0 || fps.den <= 0)
        return "—";
    if (fps.num % fps.den == 0)
        return std::to_string(fps.num / fps.den);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", static_cast<double>(fps.num) / fps.den);
    std::string text = buf;
    while (text.back() == '0')
        text.pop_back();
    return text;
}

std::string frameRateNote(const std::string &fileName, Rational clipFps, Rational projectFps)
{
    if (clipFps.num <= 0 || clipFps.den <= 0 || projectFps.num <= 0 || projectFps.den <= 0)
        return {};
    const long long clip = static_cast<long long>(clipFps.num) * projectFps.den;
    const long long project = static_cast<long long>(projectFps.num) * clipFps.den;
    if (clip == project)
        return {};
    return fileName + " is " + formatFps(clipFps) + " fps; the project is " + formatFps(projectFps) +
           (clip < project ? ", so frames will repeat" : ", so frames will be skipped");
}

} // namespace ustudio::core
