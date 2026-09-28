// ADR-019: a build with -Dgpu_acceleration_default=off (the Flatpak) installs
// data/gsettings-overrides/ustudio-gpu-off.gschema.override. meson compiles the schema with that
// override into a directory of its own for this test and points
// GSETTINGS_SCHEMA_DIR at it, on GSettings' memory backend (as test_settings).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/settings.h"

using namespace ustudio::app;

TEST_CASE("Settings: the vendor override starts GPU acceleration off, and a saved choice wins")
{
    {
        Settings settings;
        REQUIRE(settings.isPersistent());
        CHECK_FALSE(settings.gpuAcceleration()); // the override's default
        CHECK(settings.hardwareDecode());         // untouched by it
        settings.setGpuAcceleration(true);        // the user switches it on
        CHECK(settings.gpuAcceleration());
    }
    // A later start (a new Settings on the same backend) keeps the user's value.
    Settings again;
    CHECK(again.gpuAcceleration());
}
