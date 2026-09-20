#include "dispatcher.h"

#include <glib.h>

#include <memory>

namespace ustudio::engine {

namespace {
struct PostedClosure
{
    std::weak_ptr<void> token;
    std::function<void()> fn;
};
} // namespace

void MainThreadDispatcher::post(const std::weak_ptr<void> &token, std::function<void()> fn)
{
    // g_idle_add_full(), not g_main_context_invoke_full(): the latter has
    // a documented optimisation where, if nothing currently owns the
    // target context, the CALLING thread acquires it and runs the
    // function immediately, inline -- which for this class defeats the
    // entire point (posting from an MLT consumer thread must never run on
    // that thread). Confirmed empirically: a test that posted from a
    // consumer thread with no GTK/GLib main loop running anywhere in the
    // process (so the default context was never owned) crashed with heap
    // corruption from unsynchronized concurrent access, reproducing
    // 100/100 runs; switching to g_idle_add_full (which always creates a
    // genuine idle source and never runs inline on the poster's thread)
    // fixed it. doc 02 names g_main_context_invoke_full for this; this is
    // the corrected mechanism.
    auto *ctx = new PostedClosure{token, std::move(fn)};
    g_idle_add_full(
        G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
            std::unique_ptr<PostedClosure> owned(static_cast<PostedClosure *>(data));
            if (auto locked = owned->token.lock())
                owned->fn();
            return G_SOURCE_REMOVE;
        },
        ctx, nullptr);
}

} // namespace ustudio::engine
