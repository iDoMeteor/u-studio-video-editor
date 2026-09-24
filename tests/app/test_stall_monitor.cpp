#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/stall_monitor.h"
#include "core/trace.h"

#include <glib.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace ustudio;

namespace {

// Busy work, not a sleep: the point is to hold the main loop.
void spin(std::chrono::milliseconds duration)
{
    auto end = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < end) {
    }
}

// Runs `body` with stderr going to a temp file, and returns what was written.
template <class F> std::string captureStderr(F body)
{
    char path[] = "/tmp/ustudio-stall-XXXXXX";
    int fd = mkstemp(path);
    REQUIRE(fd >= 0);
    std::fflush(stderr);
    int saved = dup(2);
    dup2(fd, 2);
    body();
    std::fflush(stderr);
    dup2(saved, 2);
    close(saved);
    close(fd);
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    std::remove(path);
    return text.str();
}

} // namespace

TEST_CASE("Stall monitor: a slow main-loop iteration is logged with the scopes that ran in it")
{
    app::stall::install(); // tests are debug builds, or run with USTUDIO_LOG_LEVEL=debug
    REQUIRE(app::stall::installed());
    REQUIRE(core::trace::enabled());

    std::string log = captureStderr([] {
        GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
        // Iteration 1: fast. Iteration 2: 40 ms inside two nested scopes.
        g_idle_add_once([](gpointer) { core::trace::Scope quick("quick work"); }, nullptr);
        g_timeout_add_once(
            5,
            [](gpointer) {
                core::trace::Scope outer("slow work");
                core::trace::Scope inner([] { return std::string("inner step"); });
                spin(std::chrono::milliseconds(40));
            },
            nullptr);
        g_timeout_add_once(60, [](gpointer data) { g_main_loop_quit(static_cast<GMainLoop *>(data)); }, loop);
        g_main_loop_run(loop);
        g_main_loop_unref(loop);
    });

    INFO(log);
    CHECK(log.find("[stall] main loop blocked") != std::string::npos);
    CHECK(log.find("slow work > inner step") != std::string::npos);
    CHECK(log.find("quick work") == std::string::npos); // fast iterations aren't reported
}

TEST_CASE("Stall monitor: scopes from other threads are ignored")
{
    app::stall::install();
    std::string log = captureStderr([] {
        GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
        GThread *worker = g_thread_new(
            "worker",
            [](gpointer) -> gpointer {
                core::trace::Scope scope("worker job");
                spin(std::chrono::milliseconds(30));
                return nullptr;
            },
            nullptr);
        g_timeout_add_once(
            5,
            [](gpointer) {
                core::trace::Scope scope("main work");
                spin(std::chrono::milliseconds(30));
            },
            nullptr);
        g_timeout_add_once(60, [](gpointer data) { g_main_loop_quit(static_cast<GMainLoop *>(data)); }, loop);
        g_main_loop_run(loop);
        g_thread_join(worker);
        g_main_loop_unref(loop);
    });
    INFO(log);
    CHECK(log.find("main work") != std::string::npos);
    CHECK(log.find("worker job") == std::string::npos);
}
