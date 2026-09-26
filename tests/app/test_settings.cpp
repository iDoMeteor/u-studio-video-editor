#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/settings.h"

// meson.build points this binary at the real compiled schema
// (GSETTINGS_SCHEMA_DIR -> builddir/data) AND at GIO's in-process "memory"
// backend (GSETTINGS_BACKEND=memory), confirmed by a standalone repro
// (2026-09-23) to isolate every read/write from the real system dconf
// database -- writing through the real backend here would pollute
// whichever machine runs this test, exactly what CLAUDE.md's "never touch
// real state" testing rule forbids. See test_settings_missing_schema.cpp
// for the companion degrade-path test (no schema found).
using namespace ustudio::app;

TEST_CASE("Settings: schema found -- defaults match the gschema, get/set round-trips through the memory backend")
{
    Settings settings;
    REQUIRE(settings.isPersistent());

    CHECK(settings.autosaveDelayMinutes() == Settings::kDefaultAutosaveDelayMinutes);
    CHECK(settings.defaultPreviewScale() == Settings::kDefaultPreviewScale);
    CHECK(settings.shuttleMaxSpeed() == doctest::Approx(Settings::kDefaultShuttleMaxSpeed));
    CHECK(settings.recentProjectsMax() == Settings::kDefaultRecentProjectsMax);
    CHECK(settings.workerThreads() == Settings::kDefaultWorkerThreads);

    settings.setAutosaveDelayMinutes(7);
    CHECK(settings.autosaveDelayMinutes() == 7);

    settings.setDefaultPreviewScale("half");
    CHECK(settings.defaultPreviewScale() == "half");

    settings.setShuttleMaxSpeed(16.0);
    CHECK(settings.shuttleMaxSpeed() == doctest::Approx(16.0));

    settings.setRecentProjectsMax(25);
    CHECK(settings.recentProjectsMax() == 25);

    settings.setWorkerThreads(3);
    CHECK(settings.workerThreads() == 3);

    // Settings > Drop-ins: off, off again (no duplicate), on.
    CHECK(settings.disabledDropIns().empty());
    settings.setDropInEnabled("effects", false);
    settings.setDropInEnabled("titles", false);
    settings.setDropInEnabled("effects", false);
    CHECK(settings.disabledDropIns() == std::vector<std::string>{"titles", "effects"});
    settings.setDropInEnabled("titles", true);
    CHECK(settings.disabledDropIns() == std::vector<std::string>{"effects"});

    // M4 G: Help as it was left.
    CHECK(settings.helpOpenSections().empty());
    CHECK(settings.helpTab().empty());
    settings.setHelpOpenSections({"controls:Preview", "releases:0.49.0-beta.1"});
    settings.setHelpTab("releases");
    settings.setHelpScroll({"controls=240", "shortcuts=0"});
    CHECK(settings.helpOpenSections() == std::vector<std::string>{"controls:Preview", "releases:0.49.0-beta.1"});
    CHECK(settings.helpTab() == "releases");
    CHECK(settings.helpScroll() == std::vector<std::string>{"controls=240", "shortcuts=0"});
}
