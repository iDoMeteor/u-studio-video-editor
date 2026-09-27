// The titles drop-in's one entry point (ADR-013, ADR-014): built in, it
// exports ustudio_dropin_titles_describe() (listed in the generated
// drop_ins.h); as a module, ustudio_drop_in_describe().

#include "dropins/api.h"
#include "dropins/dropin_host.h"

namespace {

void registerDropIn(ustudio::dropins::DropInHost *host)
{
    host->log("[titles] registered in " + host->program());
}

const UStudioDropInDescription kDescription = {
    DROPIN_API_VERSION,
    "titles",
    USTUDIO_VERSION,
    "Titles: animated text and lower thirds from .ustitle files",
    nullptr,
    &registerDropIn,
};

} // namespace

extern "C" {
#ifdef TITLES_DROPIN_MODULE
__attribute__((visibility("default"))) const UStudioDropInDescription *ustudio_drop_in_describe(void)
#else
const UStudioDropInDescription *ustudio_dropin_titles_describe(void)
#endif
{
    return &kDescription;
}
}
