#include <adwaita.h>
#include <gtk/gtk.h>

#include <cstdlib>
#include <string>

#include "ui/app_window.h"
#include "ui/style_css.h"
#include "util/log.h"

namespace {

void applyStyle(GtkApplication *)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, kAppStyleCss);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

void onActivate(GtkApplication *app, gpointer /*userData*/)
{
    Log::info("Application activated");
    applyStyle(app);

    // Leaked intentionally: the app has exactly one window for its whole
    // lifetime, and GTK owns/destroys the underlying widget tree on quit.
    auto *window = new AppWindow(app);
    gtk_window_present(GTK_WINDOW(window->widget()));
}

} // namespace

int main(int argc, char **argv)
{
    Log::init("u-studio-video-editor");
    const char *envLevel = std::getenv("USTUDIO_LOG_LEVEL");
    Log::info("Starting u Studio Video Editor (log level=" + std::string(envLevel ? envLevel : "info (default)") + ")");

    AdwApplication *app = adw_application_new("com.ustudio.VideoEditor", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(onActivate), nullptr);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    Log::info("Exiting with status " + std::to_string(status));
    return status;
}
