#include "settings.h"

#include "core/log.h"

#include <filesystem>

namespace ustudio::app {

// g_settings_new(schema_id) g_error()s (aborts the process) the moment the
// schema isn't found -- by design, for apps that ship/install their schema
// as a hard dependency. This app is normally launched straight from
// builddir without installing (CLAUDE.md's dev loop), so that's the
// COMMON case here, not an edge case -- g_settings_schema_source_lookup()
// first (which returns null instead of aborting) is what makes the
// degrade-to-defaults path below possible. Confirmed against
// /usr/include/glib-2.0/gio/gsettingsschema.h: lookup() takes a source,
// not a schema id string directly, and returns a new ref the caller owns.
Settings::Settings(const std::string &fallbackSchemaDir)
{
    GSettingsSchemaSource *source = g_settings_schema_source_get_default();
    GSettingsSchema *schema =
        source != nullptr ? g_settings_schema_source_lookup(source, "com.ustudio.VideoEditor", TRUE) : nullptr;
    const char *origin = "installed schemas";

    // Run straight from builddir (the owner's usual launch, and CLAUDE.md's
    // dev loop), nothing installed has our schema -- only
    // GSETTINGS_SCHEMA_DIR or `meson devenv` used to point at it, so
    // settings silently never persisted (2026-09-25). The build compiles it
    // into builddir/data (data/meson.build), found here relative to the
    // executable; an installed or Flatpak schema, checked first, still wins.
    std::error_code ec;
    if (schema == nullptr && !fallbackSchemaDir.empty() &&
        std::filesystem::exists(std::filesystem::path(fallbackSchemaDir) / "gschemas.compiled", ec)) {
        GError *error = nullptr;
        m_fallbackSource =
            g_settings_schema_source_new_from_directory(fallbackSchemaDir.c_str(), source, FALSE, &error);
        if (m_fallbackSource != nullptr) {
            schema = g_settings_schema_source_lookup(m_fallbackSource, "com.ustudio.VideoEditor", FALSE);
            origin = "the build's schema";
        } else {
            core::Log::warn("[settings] Couldn't read schemas in " + fallbackSchemaDir + ": " + error->message);
            g_error_free(error);
        }
    }

    if (schema == nullptr) {
        core::Log::warn(
            "[settings] Schema com.ustudio.VideoEditor not found -- falling back to defaults, no persistence "
            "this session. Run via `meson devenv -C builddir`, export "
            "GSETTINGS_SCHEMA_DIR=<builddir>/data, or `meson install` to enable it.");
        return;
    }
    m_settings = g_settings_new_full(schema, nullptr, nullptr);
    g_settings_schema_unref(schema);
    core::Log::info(std::string("[settings] Using ") + origin +
                    (m_fallbackSource != nullptr ? " (" + fallbackSchemaDir + ")" : std::string()));
}

std::string Settings::builddirSchemaDir()
{
    std::error_code ec;
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec)
        return {};
    const std::filesystem::path dir = exe.parent_path() / ".." / ".." / "data";
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(dir, ec);
    return (ec ? dir : canonical).string();
}

Settings::~Settings()
{
    if (m_settings != nullptr)
        g_object_unref(m_settings);
    if (m_fallbackSource != nullptr)
        g_settings_schema_source_unref(m_fallbackSource);
}

int Settings::autosaveDelayMinutes() const
{
    return m_settings != nullptr ? g_settings_get_int(m_settings, "autosave-delay-minutes")
                                 : kDefaultAutosaveDelayMinutes;
}

void Settings::setAutosaveDelayMinutes(int minutes)
{
    if (m_settings != nullptr)
        g_settings_set_int(m_settings, "autosave-delay-minutes", minutes);
}

bool Settings::getBool(const char *key, bool fallback) const
{
    return m_settings != nullptr ? g_settings_get_boolean(m_settings, key) != FALSE : fallback;
}

void Settings::setBool(const char *key, bool value)
{
    if (m_settings != nullptr)
        g_settings_set_boolean(m_settings, key, value ? TRUE : FALSE);
}

std::string Settings::getString(const char *key, const char *fallback) const
{
    if (m_settings == nullptr)
        return fallback;
    gchar *value = g_settings_get_string(m_settings, key);
    std::string result = value != nullptr ? value : fallback;
    g_free(value);
    return result;
}

void Settings::setString(const char *key, const std::string &value)
{
    if (m_settings != nullptr)
        g_settings_set_string(m_settings, key, value.c_str());
}

std::string Settings::defaultPreviewScale() const
{
    return getString("default-preview-scale", kDefaultPreviewScale);
}

void Settings::setDefaultPreviewScale(const std::string &scale)
{
    setString("default-preview-scale", scale);
}

double Settings::shuttleMaxSpeed() const
{
    return m_settings != nullptr ? g_settings_get_double(m_settings, "shuttle-max-speed") : kDefaultShuttleMaxSpeed;
}

void Settings::setShuttleMaxSpeed(double speed)
{
    if (m_settings != nullptr)
        g_settings_set_double(m_settings, "shuttle-max-speed", speed);
}

int Settings::recentProjectsMax() const
{
    return m_settings != nullptr ? g_settings_get_int(m_settings, "recent-projects-max") : kDefaultRecentProjectsMax;
}

void Settings::setRecentProjectsMax(int max)
{
    if (m_settings != nullptr)
        g_settings_set_int(m_settings, "recent-projects-max", max);
}

int Settings::workerThreads() const
{
    return m_settings != nullptr ? g_settings_get_int(m_settings, "worker-threads") : kDefaultWorkerThreads;
}

void Settings::setWorkerThreads(int threads)
{
    if (m_settings != nullptr)
        g_settings_set_int(m_settings, "worker-threads", threads);
}

int Settings::cacheJobs() const
{
    return m_settings != nullptr ? g_settings_get_int(m_settings, "cache-jobs") : kDefaultCacheJobs;
}

void Settings::setCacheJobs(int jobs)
{
    if (m_settings != nullptr)
        g_settings_set_int(m_settings, "cache-jobs", jobs);
}

bool Settings::reopenLastProject() const
{
    return getBool("reopen-last-project", true);
}

void Settings::setReopenLastProject(bool reopen)
{
    setBool("reopen-last-project", reopen);
}

bool Settings::snapWhileDragging() const
{
    return getBool("snap-while-dragging", true);
}

void Settings::setSnapWhileDragging(bool snap)
{
    setBool("snap-while-dragging", snap);
}

bool Settings::followPlayhead() const
{
    return getBool("follow-playhead", true);
}

void Settings::setFollowPlayhead(bool follow)
{
    setBool("follow-playhead", follow);
}

bool Settings::showTimelineThumbnails() const
{
    return getBool("show-timeline-thumbnails", true);
}

void Settings::setShowTimelineThumbnails(bool show)
{
    setBool("show-timeline-thumbnails", show);
}

bool Settings::showWaveforms() const
{
    return getBool("show-waveforms", true);
}

void Settings::setShowWaveforms(bool show)
{
    setBool("show-waveforms", show);
}

bool Settings::showHoverPreview() const
{
    return getBool("show-hover-preview", true);
}

void Settings::setShowHoverPreview(bool show)
{
    setBool("show-hover-preview", show);
}

std::string Settings::lastProjectPath() const
{
    return getString("last-project-path", "");
}

void Settings::setLastProjectPath(const std::string &path)
{
    setString("last-project-path", path);
}

std::string Settings::defaultProjectFolder() const
{
    return getString("default-project-folder", "");
}

void Settings::setDefaultProjectFolder(const std::string &folder)
{
    setString("default-project-folder", folder);
}

std::string Settings::defaultExportFolder() const
{
    return getString("default-export-folder", "");
}

void Settings::setDefaultExportFolder(const std::string &folder)
{
    setString("default-export-folder", folder);
}

int Settings::renderThreadsPercent() const
{
    return m_settings != nullptr ? g_settings_get_int(m_settings, "render-threads-percent")
                                 : kDefaultRenderThreadsPercent;
}

void Settings::setRenderThreadsPercent(int percent)
{
    if (m_settings != nullptr)
        g_settings_set_int(m_settings, "render-threads-percent", percent);
}

std::string Settings::defaultRenderProfile() const
{
    return getString("default-render-profile", "High quality");
}

void Settings::setDefaultRenderProfile(const std::string &name)
{
    setString("default-render-profile", name);
}

} // namespace ustudio::app
