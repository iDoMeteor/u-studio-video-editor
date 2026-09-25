// A drop-in that exists only for tests (never installed): it touches every
// integration point as each lands, so each has a real consumer before the
// effects and titles drop-ins do. Built linked-in (the built-in describe
// name) and as modules: the real one, and three that must be refused
// (-DTEST_WRONG_API, -DTEST_WRONG_APP, -DTEST_NO_DESCRIBE).

#include "dropins/api.h"
#include "dropins/dropin_host.h"

#include <string>

namespace {

void contributeFactoryPaths(ustudio::dropins::FactoryPaths *paths)
{
    paths->frei0rPaths.push_back("/testdropin/frei0r");
    paths->mltModuleDirs.push_back("/testdropin/mlt");
}

void registerDropIn(ustudio::dropins::DropInHost *host)
{
    host->log(std::string("[") + TEST_DROPIN_NAME + "] registered in " + host->program());
}

#ifdef TEST_WRONG_API
constexpr int kApi = DROPIN_API_VERSION + 1;
#else
constexpr int kApi = DROPIN_API_VERSION;
#endif
#ifdef TEST_WRONG_APP
constexpr const char *kAppVersion = "0.0.0-elsewhere";
#else
constexpr const char *kAppVersion = USTUDIO_VERSION;
#endif

const UStudioDropInDescription kDescription = {
    kApi,
    TEST_DROPIN_NAME,
    kAppVersion,
    "Exercises every integration point (tests only)",
    &contributeFactoryPaths,
    &registerDropIn,
};

} // namespace

#ifndef TEST_NO_DESCRIBE
extern "C" {
#ifdef TEST_DROPIN_MODULE
__attribute__((visibility("default"))) const UStudioDropInDescription *ustudio_drop_in_describe(void)
#else
const UStudioDropInDescription *ustudio_dropin_testdropin_describe(void)
#endif
{
    return &kDescription;
}
}
#endif
