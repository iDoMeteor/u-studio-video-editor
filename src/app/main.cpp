#include <adwaita.h>
#include <gtk/gtk.h>

#include <cstdlib>
#include <string>

#include "app_window.h"
#include "core/log.h"
#include "engine/factory_policy.h"

namespace ustudio::app {
namespace {

void applyStyle(GtkApplication *)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(provider, "/com/ustudio/VideoEditor/style.css");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

void onActivate(GtkApplication *app, gpointer /*userData*/)
{
    core::Log::info("[app] Application activated");
    applyStyle(app);

    // Leaked intentionally: the app has exactly one window for its whole
    // lifetime, and GTK owns/destroys the underlying widget tree on quit.
    auto *window = new AppWindow(app);
    gtk_window_present(GTK_WINDOW(window->widget()));
}

} // namespace
} // namespace ustudio::app

int main(int argc, char **argv)
{
    ustudio::core::Log::init("u-studio-video-editor");
    const char *envLevel = std::getenv("USTUDIO_LOG_LEVEL");
    ustudio::core::Log::info(
        "[app] Starting u Studio Video Editor (log level=" + std::string(envLevel ? envLevel : "info (default)") + ")");

    // Constructed before any window (and before the first MltEngine, which
    // no longer calls Mlt::Factory::init() itself — see factory_policy.h),
    // destroyed after g_application_run() returns: RAII brackets the
    // required init()/close() lifetime automatically.
    ustudio::engine::FactoryPolicy factoryPolicy;

    AdwApplication *app = adw_application_new("com.ustudio.VideoEditor", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(ustudio::app::onActivate), nullptr);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    ustudio::core::Log::info("[app] Exiting with status " + std::to_string(status));
    return status;
}
