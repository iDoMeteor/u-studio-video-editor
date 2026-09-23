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

std::string Settings::defaultPreviewScale() const
{
    if (m_settings == nullptr)
        return kDefaultPreviewScale;
    gchar *value = g_settings_get_string(m_settings, "default-preview-scale");
    std::string result = value != nullptr ? value : kDefaultPreviewScale;
    g_free(value);
    return result;
}

void Settings::setDefaultPreviewScale(const std::string &scale)
{
    if (m_settings != nullptr)
        g_settings_set_string(m_settings, "default-preview-scale", scale.c_str());
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

} // namespace ustudio::app
