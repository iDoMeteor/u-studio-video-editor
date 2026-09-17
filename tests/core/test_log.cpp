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
