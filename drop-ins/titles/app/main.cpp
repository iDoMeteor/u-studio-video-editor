// u-studio-titles (doc 16, ADR-012): the title designer. GTK4 and
// libadwaita, titlerender for the canvas, no MLT.
//
//   u-studio-titles [FILE.ustitle...] [--backdrop PICTURE]
//
// Each file opens in its own window; with none, an untitled title. The
// editor launches it on a file with its frame at the playhead as the
// backdrop.

#include "titles_window.h"

#include "core/log.h"

#include <adwaita.h>

#include <string>

namespace {

namespace Log = ustudio::core::Log;
using ustudio::titles::app::TitlesWindow;

std::string g_backdrop; // --backdrop, for the windows this launch opens

void applyStyle()
{
    // The editor's stylesheet: one set of Unicorn Tears tokens for both apps.
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(provider, "/com/ustudio/Titles/style.css");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

void onStartup(GApplication *, gpointer)
{
    // Dark only, as the editor (style.css is a dark palette).
    adw_style_manager_set_color_scheme(adw_style_manager_get_default(), ADW_COLOR_SCHEME_FORCE_DARK);
    applyStyle();
}

// Windows delete themselves when destroyed (TitlesWindow::onDestroy).
void onActivate(GApplication *app, gpointer)
{
    auto *window = new TitlesWindow(GTK_APPLICATION(app), {}, g_backdrop);
    gtk_window_present(window->window());
}

void onOpen(GApplication *app, GFile **files, gint count, const gchar *, gpointer)
{
    for (gint i = 0; i < count; ++i) {
        char *raw = g_file_get_path(files[i]);
        char *utf8 = raw ? g_filename_to_utf8(raw, -1, nullptr, nullptr, nullptr) : nullptr;
        const std::string path = utf8 ? utf8 : "";
        g_free(utf8);
        g_free(raw);
        if (path.empty())
            continue;
        auto *window = new TitlesWindow(GTK_APPLICATION(app), path, g_backdrop);
        gtk_window_present(window->window());
    }
}

gint onLocalOptions(GApplication *, GVariantDict *options, gpointer)
{
    const char *backdrop = nullptr;
    if (g_variant_dict_lookup(options, "backdrop", "^&ay", &backdrop) && backdrop) {
        char *utf8 = g_filename_to_utf8(backdrop, -1, nullptr, nullptr, nullptr);
        g_backdrop = utf8 ? utf8 : "";
        g_free(utf8);
    }
    return -1; // carry on
}

} // namespace

int main(int argc, char **argv)
{
    Log::init("u-studio-titles");
    Log::info("[titles] Starting U Stu Titles");
    // One process per launch: each title stays apart, and a crash in one
    // never takes another's unsaved work.
    AdwApplication *app = adw_application_new(
        "com.ustudio.Titles", static_cast<GApplicationFlags>(G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE));
    g_application_add_main_option(G_APPLICATION(app), "backdrop", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME,
                                  "A picture to design over (the editor's frame)", "PICTURE");
    g_signal_connect(app, "startup", G_CALLBACK(onStartup), nullptr);
    g_signal_connect(app, "activate", G_CALLBACK(onActivate), nullptr);
    g_signal_connect(app, "open", G_CALLBACK(onOpen), nullptr);
    g_signal_connect(app, "handle-local-options", G_CALLBACK(onLocalOptions), nullptr);
    const int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    Log::info("[titles] Exiting with status " + std::to_string(status));
    return status;
}
