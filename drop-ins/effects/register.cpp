// The effects drop-in's one entry point (ADR-013, ADR-014; doc 15): built
// in, it exports ustudio_dropin_effects_describe() (listed in the generated
// drop_ins.h); as a module, ustudio_drop_in_describe().

#include "app/health_scan.h"
#include "core/health.h"
#include "core/log.h"
#include "dropins/api.h"
#include "dropins/dropin_host.h"
#include "engine/effects_extension.h"
#include "engine/plugins.h"
#include "engine/probe.h"
#include "engine/registry.h"

#include <set>
#include <string>

namespace {

using namespace ustudio::effects;

// IP4, before Mlt::Factory::init(): FREI0R_PATH for exactly the plugins that
// may load (engine/plugins.h), and the quarantine the last health scan left.
void contributeFactoryPaths(ustudio::dropins::FactoryPaths *paths)
{
    rememberFrei0rSearchPath();
    HealthFile health = loadHealthFile(healthFilePath());
    // Results for another plugin set or MLT describe other files: ignore
    // them until the scan runs again.
    if (health.fingerprint != registryFingerprint())
        health = {};
    const std::vector<Frei0rPlugin> plugins = findFrei0rPlugins(frei0rSearchDirs());
    const Frei0rCuration curation = curateFrei0r(plugins, health, effectsCacheDir() / "effects-frei0r-qt.json");
    paths->frei0rPaths = curation.paths;
    for (const std::string &why : curation.excluded)
        ustudio::core::Log::info("[effects] frei0r plugin left out: " + why);
    ustudio::core::Log::debug("[effects] " + std::to_string(plugins.size()) + " frei0r plugins found, " +
                              std::to_string(curation.excluded.size()) + " left out");
    std::set<std::string> quarantined;
    for (const auto &[service, record] : health.records)
        if (!record.usable())
            quarantined.insert(service);
    // Known broken in ways a probe can't always catch (the overlays).
    for (const auto &[service, why] : unstableServices(loadOverlays(effectsDataDir() / "overlays")))
        quarantined.insert(service);
    setQuarantinedServices(std::move(quarantined));
}

void registerDropIn(ustudio::dropins::DropInHost *host)
{
    host->log("[effects] registered in " + host->program());
    // IP3: effects on clips, tracks and the sequence, in every graph.
    host->addEngineExtension([] { return makeEffectsExtension(); });
    // IP6: the health and cost probe, and the registry for the editor's cache.
    host->addRenderSubcommand(
        {"probe-effect", "Health and cost of one effect; one JSON line (the effects scan)", &runProbeEffect});
    host->addRenderSubcommand(
        {"effects-registry", "Every effect MLT offers, as one JSON line (the effects scan)", &runEffectsRegistry});
    // IP5: once the window exists, the background health scan (the render
    // tool never calls this).
    host->addShellExtension([](ustudio::app::ShellHost &) { startEditorHealthScan(); });
}

const UStudioDropInDescription kDescription = {
    DROPIN_API_VERSION,      "effects",
    USTUDIO_VERSION,         "Effects: frei0r, FFmpeg and MLT filters on clips, tracks and the sequence",
    &contributeFactoryPaths, &registerDropIn,
};

} // namespace

extern "C" {
#ifdef EFFECTS_DROPIN_MODULE
__attribute__((visibility("default"))) const UStudioDropInDescription *ustudio_drop_in_describe(void)
#else
const UStudioDropInDescription *ustudio_dropin_effects_describe(void)
#endif
{
    return &kDescription;
}
}
