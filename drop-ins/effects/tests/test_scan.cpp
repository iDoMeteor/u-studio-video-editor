// The editor's health scan (doc 15, "Health and cost probe") against the
// real render tool: a frei0r plugin that crashes and one that hangs are
// quarantined with where they failed, a working effect isn't, results
// persist and resume, and this process -- the editor's stand-in -- never
// loads the broken plugins itself.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/health_scan.h"
#include "engine/effects_extension.h"
#include "platform/process.h"

#include <glib.h>

#include <filesystem>

using namespace ustudio;
using namespace ustudio::effects;
namespace fs = std::filesystem;

namespace {

fs::path scratch()
{
    static const fs::path dir =
        fs::temp_directory_path() / ("ustudio-effects-scan-" + std::to_string(platform::currentProcessId()));
    fs::create_directories(dir);
    return dir;
}

HealthScanOptions options(std::vector<std::string> services)
{
    HealthScanOptions o;
    o.renderTool = EFFECTS_RENDER_TOOL;
    o.registryCache = scratch() / "registry.json";
    o.healthFile = scratch() / "effect-health.json";
    o.fingerprint = "test";
    o.deadlineSeconds = 30; // room for a sanitizer build's slowdown; only the hanging plugin waits it out
    o.onlyServices = std::move(services);
    // The broken plugins are the children's whole frei0r search path; the
    // children's caches are the test's.
    o.environment = {{"USTUDIO_FREI0R_SEARCH_PATH", EFFECTS_BROKEN_PLUGIN_DIR},
                     {"XDG_CACHE_HOME", (scratch() / "cache").string()}};
#ifdef EFFECTS_DROPIN_MODULE_DIR
    o.environment.emplace_back("USTUDIO_DROPIN_PATH", EFFECTS_DROPIN_MODULE_DIR);
#endif
    return o;
}

// Runs the scan to the end on a main loop, as the editor would.
HealthFile run(HealthScanOptions o)
{
    GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
    HealthScan scan(std::move(o), [&](const std::string &, const HealthRecord &, bool finished) {
        if (finished)
            g_main_loop_quit(loop);
    });
    scan.start();
    g_main_loop_run(loop); // `finished` always arrives, on this loop
    g_main_loop_unref(loop);
    return scan.results();
}

} // namespace

TEST_CASE("A crashing and a hanging plugin are quarantined; the scan and its host survive")
{
    fs::remove(scratch() / "effect-health.json");
    const HealthFile results = run(options({"frei0r.crashy", "frei0r.hangy", "brightness"}));

    REQUIRE(results.records.size() == 3);
    const HealthRecord crashy = *results.find("frei0r.crashy");
    CHECK(crashy.status == HealthStatus::Crashed);
    CHECK(crashy.reason == "the probe died (defaults)");
    const HealthRecord hangy = *results.find("frei0r.hangy");
    CHECK(hangy.status == HealthStatus::TimedOut);
    CHECK(hangy.reason == "still running at the deadline (defaults)");
    CHECK(results.find("brightness")->usable());

    // Quarantined for this process's graphs at once.
    CHECK(isQuarantined("frei0r.crashy"));
    CHECK(isQuarantined("frei0r.hangy"));
    CHECK(!isQuarantined("brightness"));

    // Saved, and a second scan of the same plugin set resumes: nothing to
    // probe again.
    const HealthFile saved = loadHealthFile(scratch() / "effect-health.json");
    CHECK(saved.fingerprint == "test");
    CHECK(saved.records == results.records);
    CHECK(run(options({"frei0r.crashy", "frei0r.hangy", "brightness"})).records == results.records);

    // Another plugin set starts over.
    HealthScanOptions changed = options({"brightness"});
    changed.fingerprint = "another";
    CHECK(run(changed).records.size() == 1);
}

TEST_CASE("Without a render tool the scan ends at once, quarantining nothing")
{
    HealthScanOptions o = options({"brightness"});
    o.renderTool = (scratch() / "no-such-tool").string();
    o.healthFile = scratch() / "none.json";
    const HealthFile results = run(o);
    // The child never started: nothing recorded, so nothing is held against
    // the effect.
    CHECK(results.records.empty());
}
