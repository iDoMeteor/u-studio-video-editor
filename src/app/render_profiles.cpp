#include "render_profiles.h"

#include "core/log.h"

#include <glib.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <utility>

namespace ustudio::app {

namespace {

using Quality = core::RenderProfile::Quality;

constexpr std::array<std::pair<Quality, const char *>, 5> kQualityNames = {{
    {Quality::Draft, "draft"},
    {Quality::Good, "good"},
    {Quality::High, "high"},
    {Quality::Max, "max"},
    {Quality::Bitrate, "bitrate"},
}};

const char *qualityName(Quality quality)
{
    for (const auto &[value, name] : kQualityNames)
        if (value == quality)
            return name;
    return "high";
}

std::optional<Quality> qualityFromName(const std::string &text)
{
    for (const auto &[value, name] : kQualityNames)
        if (text == name)
            return value;
    return std::nullopt;
}

} // namespace

void writeProfile(GKeyFile *file, const char *group, const core::RenderProfile &profile)
{
    g_key_file_set_string(file, group, "quality", qualityName(profile.quality));
    g_key_file_set_integer(file, group, "height", profile.height);
    if (profile.frameRate.num > 0)
        g_key_file_set_string(
            file, group, "frame-rate",
            (std::to_string(profile.frameRate.num) + "/" + std::to_string(profile.frameRate.den)).c_str());
    if (profile.quality == Quality::Bitrate) {
        g_key_file_set_int64(file, group, "video-bitrate", profile.videoBitrate);
        g_key_file_set_int64(file, group, "audio-bitrate", profile.audioBitrate);
    }
}

std::optional<core::RenderProfile> readProfile(GKeyFile *file, const char *group)
{
    gchar *quality = g_key_file_get_string(file, group, "quality", nullptr);
    std::optional<Quality> parsed = qualityFromName(quality ? quality : "");
    g_free(quality);
    if (!parsed)
        return std::nullopt;
    core::RenderProfile profile;
    profile.name = group;
    profile.quality = *parsed;
    profile.height = g_key_file_get_integer(file, group, "height", nullptr);
    // "num/den"; absent (profiles saved before 0.42) means the project's.
    if (gchar *rate = g_key_file_get_string(file, group, "frame-rate", nullptr)) {
        int num = 0, den = 0;
        if (std::sscanf(rate, "%d/%d", &num, &den) == 2 && num > 0 && den > 0)
            profile.frameRate = {num, den};
        g_free(rate);
    }
    profile.videoBitrate = g_key_file_get_int64(file, group, "video-bitrate", nullptr);
    profile.audioBitrate = g_key_file_get_int64(file, group, "audio-bitrate", nullptr);
    return profile;
}

RenderProfileStore::RenderProfileStore(std::string path) : m_path(std::move(path))
{
    GKeyFile *file = g_key_file_new();
    GError *error = nullptr;
    if (!g_key_file_load_from_file(file, m_path.c_str(), G_KEY_FILE_NONE, &error)) {
        if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            core::Log::warn("[render] Couldn't read " + m_path + ": " + error->message);
        g_error_free(error);
        g_key_file_free(file);
        return;
    }
    gchar **groups = g_key_file_get_groups(file, nullptr);
    for (gchar **group = groups; *group; ++group) {
        std::optional<core::RenderProfile> profile = readProfile(file, *group);
        if (!profile || !core::renderProfileNameProblem(profile->name, m_user).empty()) {
            core::Log::warn("[render] Skipping render profile \"" + std::string(*group) + "\" in " + m_path);
            continue;
        }
        m_user.push_back(std::move(*profile));
    }
    g_strfreev(groups);
    g_key_file_free(file);
}

std::string RenderProfileStore::defaultPath()
{
    return (std::filesystem::path(g_get_user_config_dir()) / "ustudio" / "render-profiles.ini").string();
}

std::vector<core::RenderProfile> RenderProfileStore::all() const
{
    std::vector<core::RenderProfile> profiles = core::builtInRenderProfiles();
    profiles.insert(profiles.end(), m_user.begin(), m_user.end());
    return profiles;
}

std::optional<core::RenderProfile> RenderProfileStore::find(const std::string &name) const
{
    for (const core::RenderProfile &profile : all())
        if (profile.name == name)
            return profile;
    return std::nullopt;
}

std::string RenderProfileStore::save(const core::RenderProfile &profile, const std::string &previousName)
{
    std::vector<core::RenderProfile> others;
    for (const core::RenderProfile &existing : m_user)
        if (existing.name != previousName)
            others.push_back(existing);
    if (std::string problem = core::renderProfileNameProblem(profile.name, others); !problem.empty())
        return problem;
    if (profile.quality == Quality::Bitrate && (profile.videoBitrate <= 0 || profile.audioBitrate <= 0))
        return "Give both bitrates.";

    const std::vector<core::RenderProfile> before = m_user;
    core::RenderProfile stored = profile;
    stored.builtIn = false;
    auto it = std::find_if(m_user.begin(), m_user.end(), [&](const core::RenderProfile &p) {
        return !previousName.empty() && p.name == previousName;
    });
    if (it != m_user.end())
        *it = stored;
    else
        m_user.push_back(stored);
    if (std::string error = write(); !error.empty()) {
        m_user = before;
        return error;
    }
    return {};
}

std::string RenderProfileStore::remove(const std::string &name)
{
    const std::vector<core::RenderProfile> before = m_user;
    std::erase_if(m_user, [&](const core::RenderProfile &p) { return p.name == name; });
    if (std::string error = write(); !error.empty()) {
        m_user = before;
        return error;
    }
    return {};
}

std::string RenderProfileStore::write() const
{
    GKeyFile *file = g_key_file_new();
    for (const core::RenderProfile &profile : m_user)
        writeProfile(file, profile.name.c_str(), profile);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(m_path).parent_path(), ec);
    GError *error = nullptr;
    // g_key_file_save_to_file() goes through g_file_set_contents(): a
    // temporary file renamed into place.
    bool ok = g_key_file_save_to_file(file, m_path.c_str(), &error);
    g_key_file_free(file);
    if (ok)
        return {};
    std::string message = "Couldn't save render profiles to " + m_path + ": " + error->message;
    g_error_free(error);
    core::Log::warn("[render] " + message);
    return message;
}

} // namespace ustudio::app
