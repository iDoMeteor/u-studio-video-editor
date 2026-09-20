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
    // Stashed on `app` (not just leaked) so onShutdown below can reach its
    // MltEngine and stop the worker thread before Factory::close() runs.
    auto *window = new AppWindow(app);
    g_object_set_data(G_OBJECT(app), "ustudio-window", window);
    gtk_window_present(GTK_WINDOW(window->widget()));
}

// Fires once, synchronously inside g_application_run(), after the main
// loop stops but before it returns -- i.e. still before main()'s
// FactoryPolicy local goes out of scope and calls Mlt::Factory::close().
// This is the hook point that stops MltEngine's worker thread first, so
// Factory::close() never runs concurrently with it (see MltEngine::shutdown).
void onShutdown(GtkApplication *app, gpointer /*userData*/)
{
    if (auto *window = static_cast<AppWindow *>(g_object_get_data(G_OBJECT(app), "ustudio-window")))
        window->prepareForShutdown();
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
    g_signal_connect(app, "shutdown", G_CALLBACK(ustudio::app::onShutdown), nullptr);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    ustudio::core::Log::info("[app] Exiting with status " + std::to_string(status));
    return status;
}
