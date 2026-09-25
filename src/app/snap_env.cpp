#include "snap_env.h"

#include "platform/process.h"

#include <cstdlib>
#include <filesystem>
#include <sstream>

extern char **environ;

namespace ustudio::app {

namespace {

// Paths of any snap, or of this user's snap data.
bool inSnap(const std::string &value, const std::string &home)
{
    return value.starts_with("/snap/") || (!home.empty() && value.starts_with(home + "/snap/"));
}

std::vector<std::string> split(const std::string &value, char separator)
{
    std::vector<std::string> parts;
    std::stringstream stream(value);
    for (std::string part; std::getline(stream, part, separator);)
        parts.push_back(part);
    return parts;
}

// Pointing into a snap: unset.
const char *const kSnapOnly[] = {
    "GIO_MODULE_DIR",         "GTK_PATH", "GTK_EXE_PREFIX",       "GTK_IM_MODULE_FILE", "GDK_PIXBUF_MODULEDIR",
    "GDK_PIXBUF_MODULE_FILE", "LOCPATH",  "GSETTINGS_SCHEMA_DIR", "XDG_DATA_HOME",      "XDG_CONFIG_HOME",
    "XDG_CACHE_HOME",
};
// Search paths: drop the snap entries.
const char *const kSearchPaths[] = {"XDG_DATA_DIRS", "XDG_CONFIG_DIRS", "PATH", "LD_LIBRARY_PATH", "LD_PRELOAD"};
constexpr const char *kOriginalSuffix = "_VSCODE_SNAP_ORIG";

} // namespace

std::vector<EnvChange> snapEnvironmentFixes(const std::map<std::string, std::string> &env, const std::string &home)
{
    std::vector<EnvChange> changes;
    std::map<std::string, bool> handled;
    auto change = [&](const std::string &name, std::optional<std::string> value) {
        if (!handled[name]) {
            handled[name] = true;
            changes.push_back({name, std::move(value)});
        }
    };

    // VS Code's saved originals win.
    const std::string suffix = kOriginalSuffix;
    for (const auto &[name, value] : env) {
        if (name.size() > suffix.size() && name.ends_with(suffix)) {
            const std::string original = name.substr(0, name.size() - suffix.size());
            change(original, value.empty() ? std::nullopt : std::optional<std::string>(value));
            change(name, std::nullopt);
        }
    }
    for (const char *name : kSnapOnly) {
        auto it = env.find(name);
        if (it == env.end() || handled[name])
            continue;
        for (const std::string &part : split(it->second, ':'))
            if (inSnap(part, home)) {
                change(name, std::nullopt);
                break;
            }
    }
    for (const char *name : kSearchPaths) {
        auto it = env.find(name);
        if (it == env.end() || handled[name])
            continue;
        const char separator =
            std::string(name) == "LD_PRELOAD" && it->second.find(':') == std::string::npos ? ' ' : ':';
        std::string kept;
        bool dropped = false;
        for (const std::string &part : split(it->second, separator)) {
            if (inSnap(part, home)) {
                dropped = true;
                continue;
            }
            if (!part.empty())
                kept += (kept.empty() ? "" : std::string(1, separator)) + part;
        }
        if (dropped)
            change(name, kept.empty() ? std::nullopt : std::optional<std::string>(kept));
    }
    for (const auto &[name, value] : env) {
        // SNAP, SNAP_NAME, SNAP_USER_DATA, ...; and the launcher's record of
        // which snap app started it.
        if (name == "SNAP" || name.starts_with("SNAP_"))
            change(name, std::nullopt);
        else if ((name == "GIO_LAUNCHED_DESKTOP_FILE" || name == "BAMF_DESKTOP_FILE_HINT") &&
                 value.starts_with("/var/lib/snapd/"))
            change(name, std::nullopt);
    }
    return changes;
}

std::vector<std::string> scrubSnapEnvironment()
{
    if (platform::executablePath().string().starts_with("/snap/"))
        return {}; // we are the snap: its environment is ours
    std::map<std::string, std::string> env;
    for (char **entry = environ; entry && *entry; ++entry) {
        std::string text = *entry;
        auto eq = text.find('=');
        if (eq != std::string::npos)
            env[text.substr(0, eq)] = text.substr(eq + 1);
    }
    const char *home = std::getenv("HOME");
    std::vector<std::string> names;
    for (const EnvChange &fix : snapEnvironmentFixes(env, home ? home : "")) {
        if (fix.value)
            ::setenv(fix.name.c_str(), fix.value->c_str(), 1);
        else
            ::unsetenv(fix.name.c_str());
        names.push_back(fix.name);
    }
    return names;
}

} // namespace ustudio::app
