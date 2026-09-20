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
    auto *ctx = new PostedClosure{token, std::move(fn)};
    g_main_context_invoke_full(
        nullptr, G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
            std::unique_ptr<PostedClosure> owned(static_cast<PostedClosure *>(data));
            if (auto locked = owned->token.lock())
                owned->fn();
            return G_SOURCE_REMOVE;
        },
        ctx, nullptr);
}

} // namespace ustudio::engine
