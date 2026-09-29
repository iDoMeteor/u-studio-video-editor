// The effects drop-in's one entry point (ADR-013, ADR-014; doc 15): built
// in, it exports ustudio_dropin_effects_describe() (listed in the generated
// drop_ins.h); as a module, ustudio_drop_in_describe().

#include "app/browser.h"
#include "app/catalog.h"
#include "app/compare.h"
#include "app/curve_lanes.h"
#include "app/fx_lane.h"
#include "app/health_scan.h"
#include "app/preview_tools.h"
#include "app/rack.h"
#include "app/transitions_page.h"
#include "core/health.h"
#include "core/log.h"
#include "core/looks.h"
#include "core/transitions.h"
#include "dropins/api.h"
#include "dropins/dropin_host.h"
#include "engine/effects_extension.h"
#include "engine/plugins.h"
#include "engine/probe.h"
#include "engine/registry.h"

#include <glib.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

namespace {

namespace fs = std::filesystem;

using namespace ustudio::effects;

// IP4, before Mlt::Factory::init(): FREI0R_PATH for exactly the plugins that
// may load (engine/plugins.h), and the quarantine the last health scan left.
void contributeFactoryPaths(ustudio::dropins::FactoryPaths *paths)
{
    // A package's own MLT frei0r module (the Flatpak extension; the core
    // app's MLT has none): FactoryPolicy links it into the curated module
    // directory, through the denylist, after MLT's own (a system frei0r
    // module keeps its name). Titles installs its module to the same folder
    // in a plain install: listed once.
    std::error_code ec;
    if (std::filesystem::is_directory(EFFECTS_MLT_INSTALL_DIR, ec) &&
        std::find(paths->mltModuleDirs.begin(), paths->mltModuleDirs.end(), EFFECTS_MLT_INSTALL_DIR) ==
            paths->mltModuleDirs.end())
        paths->mltModuleDirs.push_back(EFFECTS_MLT_INSTALL_DIR);
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
    // LADSPA and VST2 (MLT's jackrack host): the same no-Qt curation, set
    // in the environment here, before Mlt::Factory::init() reads it; VST2
    // only with its experimental preference.
    rememberAudioSearchPaths();
    const ExperimentalFamilies experimental = loadExperimentalFamilies();
    const fs::path audioQtCache = effectsCacheDir() / "effects-audio-qt.json";
    for (const auto &[host, enabled] : {std::pair{AudioHost::Ladspa, true}, {AudioHost::Vst2, experimental.vst2}}) {
        const PluginCuration audio = curateAudioHost(host, enabled, audioQtCache);
        std::string list;
        for (const std::string &dir : audio.paths)
            list += (list.empty() ? "" : std::string(1, G_SEARCHPATH_SEPARATOR)) + dir;
        const char *variable = host == AudioHost::Ladspa ? "LADSPA_PATH" : "VST_PATH";
        g_setenv(variable, list.c_str(), TRUE);
        for (const std::string &why : audio.excluded)
            ustudio::core::Log::info(std::string("[effects] ") + variable + " plugin left out: " + why);
    }
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
    // IP5: once the window exists, the Rack and the background health scan
    // (the render tool never calls this).
    host->addShellExtension([](ustudio::app::ShellHost &shell) {
        static Catalog catalog;
        static bool scanning = false;
        // Compare first: the Rack's Compare button takes its hint.
        addCompare(shell, catalog);
        addRack(shell, catalog);
        addBrowser(shell, catalog);
        addCurveLanes(shell, catalog);
        addFxLane(shell, catalog);
        previewTools(shell); // the eyedropper's and rect handles' overlay, stacked now
        // FX3: the transition styles (small; the drop-in's own data).
        static const std::vector<TransitionRecipe> recipes = loadRecipes((effectsDataDir() / "transitions").string());
        addTransitions(shell, recipes);
        if (scanning)
            return; // one scan per process, however many windows
        scanning = true;
        // What was loaded at start-up (the experimental families), and
        // whether the recommended audio pack is there.
        catalog.experimental = loadExperimentalFamilies();
        const std::vector<Frei0rPlugin> ladspa = audioHostFiles(AudioHost::Ladspa);
        catalog.lspInstalled = std::any_of(ladspa.begin(), ladspa.end(),
                                           [](const Frei0rPlugin &file) { return file.name.starts_with("lsp-plugins"); });
        // Brand Looks (small; the drop-in's own data).
        std::ifstream in(effectsDataDir() / "looks" / "brand.json");
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (std::optional<Json> json = parseJson(text))
            catalog.setBrandLooks(looksFromJson(*json));
        // The badges of effects probed before (small; the registry itself
        // arrives from the scan's thread).
        for (const auto &[service, record] : loadHealthFile(healthFilePath()).records)
            catalog.setHealth(service, record);
        startEditorHealthScan(
            [](const std::string &service, const HealthRecord &record, bool finished) {
                if (!finished)
                    catalog.setHealth(service, record);
            },
            [](std::shared_ptr<const EffectRegistry> registry) { catalog.setRegistry(std::move(registry)); });
    });
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
