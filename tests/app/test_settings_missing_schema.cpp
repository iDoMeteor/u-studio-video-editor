#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/settings.h"

// Companion to test_settings.cpp: meson.build runs THIS binary with
// GSETTINGS_SCHEMA_DIR pointed at an empty scratch directory and
// XDG_DATA_DIRS pointed at another empty one (so the system schema source
// -- which is derived entirely from XDG_DATA_DIRS -- finds nothing
// either), confirmed by a standalone repro (2026-09-23) to make
// g_settings_schema_source_lookup() return null without crashing. This is
// the state the app is normally launched in (CLAUDE.md's dev loop runs the
// binary straight from builddir, not via `meson devenv`), so it's the
// common case, not an edge case -- see settings.cpp's own comment.
using namespace ustudio::app;

TEST_CASE("Settings: schema not found -- every getter falls back to its default, every setter is a silent no-op")
{
    Settings settings;
    CHECK_FALSE(settings.isPersistent());

    CHECK(settings.autosaveDelayMinutes() == Settings::kDefaultAutosaveDelayMinutes);
    CHECK(settings.defaultPreviewScale() == Settings::kDefaultPreviewScale);
    CHECK(settings.shuttleMaxSpeed() == doctest::Approx(Settings::kDefaultShuttleMaxSpeed));
    CHECK(settings.recentProjectsMax() == Settings::kDefaultRecentProjectsMax);
    CHECK(settings.workerThreads() == Settings::kDefaultWorkerThreads);

    // No schema behind these -- must not crash, and must leave the
    // defaults exactly as they were (nothing to persist to).
    settings.setAutosaveDelayMinutes(7);
    settings.setDefaultPreviewScale("half");
    settings.setShuttleMaxSpeed(16.0);
    settings.setRecentProjectsMax(25);
    settings.setWorkerThreads(3);

    CHECK(settings.autosaveDelayMinutes() == Settings::kDefaultAutosaveDelayMinutes);
    CHECK(settings.defaultPreviewScale() == Settings::kDefaultPreviewScale);
    CHECK(settings.shuttleMaxSpeed() == doctest::Approx(Settings::kDefaultShuttleMaxSpeed));
    CHECK(settings.recentProjectsMax() == Settings::kDefaultRecentProjectsMax);
    CHECK(settings.workerThreads() == Settings::kDefaultWorkerThreads);
}
