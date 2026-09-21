#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/log.h"

using namespace ustudio::core;

TEST_CASE("Log level get/set round-trips without touching the filesystem")
{
    LogLevel original = Log::level();

    Log::setLevel(LogLevel::Debug);
    CHECK(Log::level() == LogLevel::Debug);

    Log::setLevel(LogLevel::None);
    CHECK(Log::level() == LogLevel::None);

    Log::setLevel(original); // leave global state as found
}

TEST_CASE("ScopedTimer logs on destruction and nests safely, at any log level")
{
    LogLevel original = Log::level();

    Log::setLevel(LogLevel::Debug);
    {
        Log::ScopedTimer outer("outer op");
        { Log::ScopedTimer inner("inner op"); } // destructs (and logs) before outer does
    }

    // Filtered out entirely (Debug > the active level) -- must still be
    // safe to construct/destroy, not just a no-op when it happens to log.
    Log::setLevel(LogLevel::None);
    { Log::ScopedTimer filtered("never logged"); }

    Log::setLevel(original);
}
