#pragma once

#include <functional>
#include <memory>

namespace ustudio::engine {

// Posts a closure from an MLT-owned thread (the consumer's frame-show
// event, in practice) onto the GLib main thread, guarded by a lifetime
// token (doc 02's MainThreadDispatcher). Replaces this project's earlier
// raw g_idle_add() calls for anything posted from a thread whose lifetime
// isn't tied to the object being posted to.
//
// Lifetime guard: a poster holds a LifetimeToken (a shared_ptr<void> with
// no meaningful payload -- its identity is what matters) and passes a
// weak_ptr to post(). If the token is dead by the time the closure would
// run (the owner was destroyed first), the closure is dropped instead of
// touching freed state. This is the mechanism that makes it safe for
// PlaybackController's consumer-frame-show handler (called on an MLT
// thread this class doesn't control the lifetime of) to post work that
// touches `this`.
class MainThreadDispatcher
{
  public:
    using LifetimeToken = std::shared_ptr<void>;

    static LifetimeToken makeToken()
    {
        return std::make_shared<char>(0);
    }

    // Posts `fn` to run once on the main thread via
    // g_main_context_invoke_full(). Safe to call from any thread. `fn` is
    // dropped without running if `token` is already expired when the main
    // loop gets to it.
    static void post(const std::weak_ptr<void> &token, std::function<void()> fn);
};

} // namespace ustudio::engine
