#include "stall_monitor.h"

#include "core/log.h"
#include "core/trace.h"

#include <glib.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace ustudio::app::stall {

namespace {

namespace Log = ustudio::core::Log;
using Clock = std::chrono::steady_clock;

constexpr double kStallMs = 16.0;

struct Open
{
    std::string label;
    Clock::time_point start;
};

struct Done
{
    std::string path; // "outer > inner"
    double ms;
};

// Main thread only (the hooks drop calls from any other thread, and the
// poll function runs on the thread iterating the default context: main).
struct State
{
    bool installed = false;
    std::thread::id mainThread;
    GPollFunc originalPoll = nullptr;
    Clock::time_point iterationStart;
    std::vector<Open> open;
    std::vector<Done> done;
};

State &state()
{
    static State s;
    return s;
}

void beginScope(std::string label)
{
    State &s = state();
    if (std::this_thread::get_id() != s.mainThread)
        return; // MT2's engine thread will get its own accounting
    s.open.push_back({std::move(label), Clock::now()});
}

void endScope()
{
    State &s = state();
    if (std::this_thread::get_id() != s.mainThread || s.open.empty())
        return;
    Open closing = std::move(s.open.back());
    s.open.pop_back();
    std::string path;
    for (const Open &outer : s.open)
        path += outer.label + " > ";
    path += closing.label;
    s.done.push_back(
        {std::move(path), std::chrono::duration<double, std::milli>(Clock::now() - closing.start).count()});
}

gint monitoredPoll(GPollFD *fds, guint count, gint timeout)
{
    State &s = state();
    Clock::time_point entered = Clock::now();
    double iterationMs = std::chrono::duration<double, std::milli>(entered - s.iterationStart).count();
    if (iterationMs > kStallMs) {
        // The slowest few named scopes (nested ones carry their parents).
        std::sort(s.done.begin(), s.done.end(), [](const Done &a, const Done &b) { return a.ms > b.ms; });
        std::string what;
        for (size_t i = 0; i < s.done.size() && i < 3; ++i) {
            char ms[32];
            std::snprintf(ms, sizeof ms, " %.1f ms", s.done[i].ms);
            what += (i ? "; " : "") + s.done[i].path + ms;
        }
        if (what.empty())
            what = "nothing marked (add a core::trace::Scope where this happens)";
        char head[64];
        std::snprintf(head, sizeof head, "[stall] main loop blocked %.1f ms: ", iterationMs);
        Log::warn(head + what);
    }
    s.done.clear();
    gint result = s.originalPoll(fds, count, timeout);
    s.iterationStart = Clock::now();
    return result;
}

bool wanted()
{
#ifndef NDEBUG
    return true;
#else
    const char *level = std::getenv("USTUDIO_LOG_LEVEL");
    return level && std::string(level) == "debug";
#endif
}

} // namespace

void install()
{
    State &s = state();
    if (s.installed || !wanted())
        return;
    GMainContext *context = g_main_context_default();
    s.mainThread = std::this_thread::get_id();
    s.originalPoll = g_main_context_get_poll_func(context);
    s.iterationStart = Clock::now();
    g_main_context_set_poll_func(context, monitoredPoll);
    core::trace::setHooks(beginScope, endScope);
    s.installed = true;
    Log::debug("[stall] monitoring main-loop iterations over 16 ms");
}

bool installed()
{
    return state().installed;
}

} // namespace ustudio::app::stall
