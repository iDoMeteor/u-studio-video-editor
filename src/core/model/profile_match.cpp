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

std::string frameRateSummary(const std::vector<std::pair<std::string, Rational>> &clips, Rational projectFps)
{
    struct Group
    {
        Rational fps;
        int count = 0;
        bool slower = false; // than the project: frames repeat
    };
    std::vector<Group> groups;
    const std::pair<std::string, Rational> *only = nullptr;
    int differing = 0;
    for (const auto &clip : clips) {
        if (frameRateNote(clip.first, clip.second, projectFps).empty())
            continue;
        ++differing;
        only = &clip;
        const long long mine = static_cast<long long>(clip.second.num) * projectFps.den;
        const long long project = static_cast<long long>(projectFps.num) * clip.second.den;
        auto same = std::find_if(groups.begin(), groups.end(), [&](const Group &g) {
            return static_cast<long long>(g.fps.num) * clip.second.den ==
                   static_cast<long long>(clip.second.num) * g.fps.den;
        });
        if (same == groups.end())
            groups.push_back({clip.second, 1, mine < project});
        else
            ++same->count;
    }
    if (differing == 0)
        return {};
    if (differing == 1)
        return frameRateNote(only->first, only->second, projectFps);
    std::sort(groups.begin(), groups.end(), [](const Group &a, const Group &b) {
        return static_cast<long long>(a.fps.num) * b.fps.den < static_cast<long long>(b.fps.num) * a.fps.den;
    });
    const bool mixed = std::any_of(groups.begin(), groups.end(), [&](const Group &g) { return g.slower; }) &&
                       std::any_of(groups.begin(), groups.end(), [&](const Group &g) { return !g.slower; });
    std::string text;
    for (const Group &group : groups) {
        if (!text.empty())
            text += ", ";
        text += std::to_string(group.count) + (group.count == 1 ? " is " : " are ") + formatFps(group.fps) + " fps";
        if (mixed)
            text += group.slower ? " (frames will repeat)" : " (frames will be skipped)";
    }
    text += "; the project is " + formatFps(projectFps);
    if (!mixed)
        text += groups.front().slower ? ", so frames will repeat" : ", so frames will be skipped";
    return text;
}

} // namespace ustudio::core
