#pragma once

#include <atomic>
#include <concepts>
#include <string>

namespace ustudio::core::trace {

// Named scopes for the main-thread stall monitor (doc 19 MT0,
// app/stall_monitor.h): core and engine code marks what it's doing, and the
// app, which owns the main loop, decides what to do with it. std-only, so
// every layer can use it. With no hooks installed (release builds without
// USTUDIO_LOG_LEVEL=debug) a Scope costs one atomic load.

using BeginHook = void (*)(std::string label);
using EndHook = void (*)();

namespace detail {
inline std::atomic<BeginHook> begin{nullptr};
inline std::atomic<EndHook> end{nullptr};
} // namespace detail

// Main thread, at startup; nullptr/nullptr turns it off.
inline void setHooks(BeginHook begin, EndHook end)
{
    detail::end.store(end);
    detail::begin.store(begin);
}

inline bool enabled()
{
    return detail::begin.load(std::memory_order_relaxed) != nullptr;
}

class Scope
{
  public:
    explicit Scope(const char *label)
    {
        if (BeginHook begin = detail::begin.load(std::memory_order_relaxed)) {
            begin(label);
            m_active = true;
        }
    }
    // For a label that costs something to build (a command's label): only
    // called when tracing is on.
    template <std::invocable F>
        requires std::convertible_to<std::invoke_result_t<F>, std::string>
    explicit Scope(F &&label)
    {
        if (BeginHook begin = detail::begin.load(std::memory_order_relaxed)) {
            begin(label());
            m_active = true;
        }
    }
    ~Scope()
    {
        if (m_active) {
            if (EndHook end = detail::end.load(std::memory_order_relaxed))
                end();
        }
    }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

  private:
    bool m_active = false;
};

} // namespace ustudio::core::trace
