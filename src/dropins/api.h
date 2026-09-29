#pragma once

// The drop-in ABI (ADR-013, ADR-014; doc 15, "Integration points"; doc 17,
// "Distribution"). A drop-in, built in or as a module
// (libustudio-dropin-<name>.so), describes itself with one C function;
// everything else goes through DropInHost's virtual interface, so a module
// needs no symbol from the app beyond what it calls through that host.

namespace ustudio::dropins {
class DropInHost;
} // namespace ustudio::dropins
namespace ustudio::engine {
struct FactoryPaths;
} // namespace ustudio::engine

// Bumped with any change to DropInHost, FactoryPaths, or an integration
// point a drop-in can see. Modules must match it exactly.
// 2: engine::FactoryPaths (IP4); 3: addEngineExtension (IP3); 4: addRenderSubcommand (IP6);
// 5: addShellExtension (IP5); 6: ShellHost::assetChangedOnDisk (IP5);
// 7: ShellHost::projectFolder (IP5); 8: ShellHost::addHeaderButton (IP5);
// 9: ShellHost::seek and playheadMoved (IP5); 10: ShellHost::showInspectorPage (IP5);
// 11: EngineExtension::decorateLane (IP3);
// 12: TimelineOverlayProvider::topLaneHeight, dragged and dragCancelled (IP5);
// 13: ParamChange::block (IP3)
#define DROPIN_API_VERSION 13

extern "C" {

struct UStudioDropInDescription
{
    int apiVersion;          // DROPIN_API_VERSION it was built with
    const char *name;        // "effects", "titles": also its Settings key
    const char *appVersion;  // the app release it was built with (meson's project version)
    const char *description; // one line, for Settings > Drop-ins
    // IP4: before Mlt::Factory::init (may be null).
    void (*contributeFactoryPaths)(ustudio::engine::FactoryPaths *paths);
    // IP3, IP5, IP6: once MLT is up and the host exists.
    void (*registerDropIn)(ustudio::dropins::DropInHost *host);
};

// A module's one export. A built-in drop-in exports
// ustudio_dropin_<name>_describe() instead (listed in the generated
// drop_ins.h), so several can link into one binary.
const UStudioDropInDescription *ustudio_drop_in_describe(void);
typedef const UStudioDropInDescription *(*UStudioDropInDescribeFn)(void);
}
