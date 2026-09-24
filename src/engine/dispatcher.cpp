#include "dispatcher.h"

#include <glib.h>

#include <memory>

// GLib's main-context lock is futex-based and invisible to ThreadSanitizer,
// so without these annotations TSan reports every closure handed from a
// worker to the main thread below as a data race (sanitizer report S5,
// 2026-09-23: dispatcher.cpp, playback_controller.cpp and both caches'
// ready callbacks, all this one hand-off). Only compiled into -Db_sanitize=
// thread builds; GCC defines __SANITIZE_THREAD__ there.
#ifdef __SANITIZE_THREAD__
#include <sanitizer/tsan_interface.h>
#define USTUDIO_TSAN_RELEASE(addr) __tsan_release(addr)
#define USTUDIO_TSAN_ACQUIRE(addr) __tsan_acquire(addr)
#else
#define USTUDIO_TSAN_RELEASE(addr) ((void)(addr))
#define USTUDIO_TSAN_ACQUIRE(addr) ((void)(addr))
#endif

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
    USTUDIO_TSAN_RELEASE(ctx);
    g_idle_add_full(
        G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
            USTUDIO_TSAN_ACQUIRE(data);
            std::unique_ptr<PostedClosure> owned(static_cast<PostedClosure *>(data));
            if (auto locked = owned->token.lock())
                owned->fn();
            return G_SOURCE_REMOVE;
        },
        ctx, nullptr);
}

} // namespace ustudio::engine
