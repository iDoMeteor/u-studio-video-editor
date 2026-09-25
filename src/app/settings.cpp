#include "settings.h"

#include "core/log.h"

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
Settings::Settings()
{
    GSettingsSchemaSource *source = g_settings_schema_source_get_default();
    if (source == nullptr) {
        core::Log::warn("[settings] No default GSettingsSchemaSource -- GSettings persistence disabled this session");
        return;
    }

    GSettingsSchema *schema = g_settings_schema_source_lookup(source, "com.ustudio.VideoEditor", TRUE);
    if (schema == nullptr) {
        core::Log::warn(
            "[settings] Schema com.ustudio.VideoEditor not found -- falling back to defaults, no persistence "
            "this session. Run via `meson devenv -C builddir`, export "
            "GSETTINGS_SCHEMA_DIR=<builddir>/data, or `meson install` to enable it.");
        return;
    }

    m_settings = g_settings_new_full(schema, nullptr, nullptr);
    g_settings_schema_unref(schema);
}

Settings::~Settings()
{
    if (m_settings != nullptr)
        g_object_unref(m_settings);
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

} // namespace ustudio::app
