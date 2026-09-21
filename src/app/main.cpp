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
    // "activate" fires more than once per process: G_APPLICATION_DEFAULT_
    // FLAGS makes this app single-instance, so GApplication re-delivers
    // "activate" to THIS already-running primary instance every time
    // something else tries to launch it again (a second double-click, a
    // second terminal invocation, a launcher re-click) instead of starting
    // a new process -- confirmed from a real session's log, two
    // back-to-back "Application activated" lines with no process restart
    // in between. The previous code unconditionally built a brand new
    // AppWindow (a whole new Model/EngineSync/PlaybackController) on every
    // one of those, leaving the earlier window's PlaybackController alive
    // and orphaned (its pointer overwritten in the "ustudio-window" slot,
    // never stopped) -- a second live sdl2_audio consumer fighting the
    // first one over the same PipeWire/audio-device state in the same
    // process. That's the leading suspect for both a real SIGSEGV inside
    // MLT's SDL2 audio callback and "play doesn't work any more" after
    // re-launching once already running: present the existing window
    // instead, the standard GtkApplication pattern for a single-window
    // app.
    if (auto *existing = static_cast<AppWindow *>(g_object_get_data(G_OBJECT(app), "ustudio-window"))) {
        core::Log::info(
            "[app] Application re-activated with a window already open -- presenting it, not creating another");
        gtk_window_present(GTK_WINDOW(existing->widget()));
        return;
    }

    core::Log::info("[app] Application activated");
    applyStyle(app);

    // Leaked intentionally: the app has exactly one window for its whole
    // lifetime, and GTK owns/destroys the underlying widget tree on quit.
    // Stashed on `app` (not just leaked) so onShutdown below can reach its
    // PlaybackController and stop the Mlt::Consumer before Factory::close()
    // runs, and so the re-activation check above can find it.
    auto *window = new AppWindow(app);
    g_object_set_data(G_OBJECT(app), "ustudio-window", window);
    gtk_window_present(GTK_WINDOW(window->widget()));
}

// Fires once, synchronously inside g_application_run(), after the main
// loop stops but before it returns -- i.e. still before main()'s
// FactoryPolicy local goes out of scope and calls Mlt::Factory::close().
// This is the hook point that stops the Mlt::Consumer first (its own MLT-
// owned thread(s)), so Factory::close() never runs concurrently with it
// (see PlaybackController::shutdown).
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

    // Constructed before any window (and before the first
    // PlaybackController, which never calls Mlt::Factory::init() itself —
    // see factory_policy.h), destroyed after g_application_run() returns:
    // RAII brackets the required init()/close() lifetime automatically.
    ustudio::engine::FactoryPolicy factoryPolicy;

    AdwApplication *app = adw_application_new("com.ustudio.VideoEditor", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(ustudio::app::onActivate), nullptr);
    g_signal_connect(app, "shutdown", G_CALLBACK(ustudio::app::onShutdown), nullptr);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    ustudio::core::Log::info("[app] Exiting with status " + std::to_string(status));
    return status;
}
