#include "pending_renders.h"

#include "render_profiles.h"

#include "core/log.h"
#include "core/xml/writer.h"

#include <glib.h>

#include <algorithm>
#include <filesystem>
#include <optional>

namespace fs = std::filesystem;

namespace ustudio::app::pending_renders {

namespace {

// "render-<n>.ini" -> n, else nullopt.
std::optional<int> indexOf(const fs::path &path, const std::string &extension)
{
    const std::string name = path.filename().string();
    const std::string prefix = "render-";
    if (!name.starts_with(prefix) || !name.ends_with(extension))
        return std::nullopt;
    const std::string digits = name.substr(prefix.size(), name.size() - prefix.size() - extension.size());
    if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return std::nullopt;
    return std::stoi(digits);
}

} // namespace

std::string directory()
{
    return (fs::path(g_get_user_state_dir()) / "ustudio" / "pending-renders").string();
}

void clear(const std::string &dir)
{
    std::error_code ec;
    for (const fs::directory_entry &entry : fs::directory_iterator(dir, ec))
        if (indexOf(entry.path(), ".ini") || indexOf(entry.path(), ".ustudio"))
            fs::remove(entry.path(), ec);
}

std::string save(const std::vector<RenderJob> &jobs, const std::string &dir)
{
    clear(dir);
    if (jobs.empty())
        return {};
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
        return "Couldn't create " + dir + ": " + ec.message();
    for (size_t i = 0; i < jobs.size(); ++i) {
        const RenderJob &job = jobs[i];
        const std::string stem = (fs::path(dir) / ("render-" + std::to_string(i + 1))).string();
        if (!job.snapshot)
            continue;
        if (std::string error = core::saveProject(core::Model(*job.snapshot), stem + ".ustudio"); !error.empty())
            return error;
        GKeyFile *file = g_key_file_new();
        g_key_file_set_string(file, "render", "output", job.outputPath.c_str());
        g_key_file_set_string(file, "render", "profile", job.profile.name.c_str());
        writeProfile(file, job.profile.name.c_str(), job.profile);
        GError *error = nullptr;
        const bool ok = g_key_file_save_to_file(file, (stem + ".ini").c_str(), &error);
        g_key_file_free(file);
        if (!ok) {
            std::string message = std::string("Couldn't save a pending render: ") + error->message;
            g_error_free(error);
            return message;
        }
    }
    return {};
}

std::vector<Pending> list(const std::string &dir)
{
    std::vector<std::pair<int, Pending>> found;
    std::error_code ec;
    for (const fs::directory_entry &entry : fs::directory_iterator(dir, ec)) {
        std::optional<int> index = indexOf(entry.path(), ".ini");
        if (!index)
            continue;
        Pending pending;
        pending.projectPath = (fs::path(dir) / ("render-" + std::to_string(*index) + ".ustudio")).string();
        GKeyFile *file = g_key_file_new();
        bool usable = g_key_file_load_from_file(file, entry.path().c_str(), G_KEY_FILE_NONE, nullptr) &&
                      fs::is_regular_file(pending.projectPath, ec);
        if (usable) {
            gchar *output = g_key_file_get_string(file, "render", "output", nullptr);
            gchar *profileName = g_key_file_get_string(file, "render", "profile", nullptr);
            std::optional<core::RenderProfile> profile = profileName ? readProfile(file, profileName) : std::nullopt;
            usable = output && *output && profile;
            if (usable) {
                pending.outputPath = output;
                pending.profile = *profile;
                // A built-in keeps its read-only mark.
                for (const core::RenderProfile &builtIn : core::builtInRenderProfiles())
                    if (builtIn.name == profile->name)
                        pending.profile.builtIn = true;
            }
            g_free(output);
            g_free(profileName);
        }
        g_key_file_free(file);
        if (usable)
            found.emplace_back(*index, std::move(pending));
        else
            core::Log::warn("[render] Skipping unreadable pending render " + entry.path().string());
    }
    std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    std::vector<Pending> result;
    for (auto &[index, pending] : found)
        result.push_back(std::move(pending));
    return result;
}

} // namespace ustudio::app::pending_renders
