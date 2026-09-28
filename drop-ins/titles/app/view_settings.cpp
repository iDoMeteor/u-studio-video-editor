#include "view_settings.h"

#include "core/log.h"

#include <glib.h>

#include <string>

namespace ustudio::titles::app {

namespace {
constexpr const char *kGroup = "view";

std::string settingsPath()
{
    char *path = g_build_filename(g_get_user_config_dir(), "ustudio", "titles.ini", nullptr);
    std::string out = path;
    g_free(path);
    return out;
}
} // namespace

ViewSettings loadViewSettings()
{
    ViewSettings settings;
    GKeyFile *file = g_key_file_new();
    if (g_key_file_load_from_file(file, settingsPath().c_str(), G_KEY_FILE_NONE, nullptr)) {
        gchar *backdrop = g_key_file_get_string(file, kGroup, "backdrop", nullptr);
        if (backdrop && std::string(backdrop) == "colour")
            settings.backdrop = Backdrop::Colour;
        g_free(backdrop);
        gchar *colour = g_key_file_get_string(file, kGroup, "colour", nullptr);
        GdkRGBA parsed;
        if (colour && gdk_rgba_parse(&parsed, colour))
            settings.colour = parsed;
        g_free(colour);
        GError *error = nullptr;
        const gboolean guides = g_key_file_get_boolean(file, kGroup, "guides", &error);
        if (!error)
            settings.guides = guides;
        g_clear_error(&error);
    }
    g_key_file_free(file);
    return settings;
}

void saveViewSettings(const ViewSettings &settings)
{
    GKeyFile *file = g_key_file_new();
    g_key_file_set_string(file, kGroup, "backdrop", settings.backdrop == Backdrop::Colour ? "colour" : "checkerboard");
    gchar *colour = gdk_rgba_to_string(&settings.colour);
    g_key_file_set_string(file, kGroup, "colour", colour);
    g_free(colour);
    g_key_file_set_boolean(file, kGroup, "guides", settings.guides);
    const std::string path = settingsPath();
    gchar *dir = g_path_get_dirname(path.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    GError *error = nullptr;
    if (!g_key_file_save_to_file(file, path.c_str(), &error)) {
        core::Log::warn(std::string("[titles] view settings not saved: ") + (error ? error->message : "?"));
        g_clear_error(&error);
    }
    g_key_file_free(file);
}

} // namespace ustudio::titles::app
