#pragma once

// The drop-in ABI (ADR-013, ADR-014; doc 15, "Integration points"; doc 17,
// "Distribution"). A drop-in, built in or as a module
// (libustudio-dropin-<name>.so), describes itself with one C function;
// everything else goes through DropInHost's virtual interface, so a module
// needs no symbol from the app beyond what it calls through that host.

namespace ustudio::dropins {
class DropInHost;
struct FactoryPaths;
} // namespace ustudio::dropins

// Bumped with any change to DropInHost, FactoryPaths, or an integration
// point a drop-in can see. Modules must match it exactly.
#define DROPIN_API_VERSION 1

extern "C" {

struct UStudioDropInDescription
{
    int apiVersion;          // DROPIN_API_VERSION it was built with
    const char *name;        // "effects", "titles": also its Settings key
    const char *appVersion;  // the app release it was built with (meson's project version)
    const char *description; // one line, for Settings > Drop-ins
    // IP4: before Mlt::Factory::init (may be null).
    void (*contributeFactoryPaths)(ustudio::dropins::FactoryPaths *paths);
    // IP3, IP5, IP6: once MLT is up and the host exists.
    void (*registerDropIn)(ustudio::dropins::DropInHost *host);
};

// A module's one export. A built-in drop-in exports
// ustudio_dropin_<name>_describe() instead (listed in the generated
// drop_ins.h), so several can link into one binary.
const UStudioDropInDescription *ustudio_drop_in_describe(void);
typedef const UStudioDropInDescription *(*UStudioDropInDescribeFn)(void);
}
