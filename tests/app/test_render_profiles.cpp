// app::RenderProfileStore against a scratch keyfile.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/render_profiles.h"

#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace ustudio;
using Quality = core::RenderProfile::Quality;

namespace {

struct Scratch
{
    fs::path dir = fs::temp_directory_path() / ("ustudio-render-profiles-" + std::to_string(::getpid()));
    fs::path file = dir / "sub" / "render-profiles.ini";
    Scratch()
    {
        fs::remove_all(dir);
    }
    ~Scratch()
    {
        fs::remove_all(dir);
    }
};

} // namespace

TEST_CASE("RenderProfileStore: no file means just the built-ins")
{
    Scratch scratch;
    app::RenderProfileStore store(scratch.file.string());
    CHECK(store.userProfiles().empty());
    REQUIRE(store.all().size() == 2);
    CHECK(store.find("High quality").has_value());
    CHECK(store.find("High quality")->builtIn);
}

TEST_CASE("RenderProfileStore: save, rename and remove survive a reload")
{
    Scratch scratch;
    {
        app::RenderProfileStore store(scratch.file.string());
        CHECK(store.save({.name = "YouTube 720", .height = 720, .quality = Quality::Good}, "").empty());
        CHECK(
            store
                .save(
                    {.name = "Exact", .quality = Quality::Bitrate, .videoBitrate = 8'000'000, .audioBitrate = 192'000},
                    "")
                .empty());
        // Names are unique, built-ins' too, and bitrates need both values.
        CHECK_FALSE(store.save({.name = "Exact"}, "").empty());
        CHECK_FALSE(store.save({.name = "High quality"}, "").empty());
        CHECK_FALSE(store.save({.name = "Half", .quality = Quality::Bitrate, .videoBitrate = 1000}, "").empty());
    }
    {
        app::RenderProfileStore store(scratch.file.string());
        REQUIRE(store.userProfiles().size() == 2);
        CHECK(store.userProfiles()[0] ==
              core::RenderProfile{.name = "YouTube 720", .height = 720, .quality = Quality::Good});
        CHECK(store.userProfiles()[1].videoBitrate == 8'000'000);
        CHECK(store.userProfiles()[1].audioBitrate == 192'000);

        // Renaming in place keeps the order.
        CHECK(store.save({.name = "YouTube", .height = 1080, .quality = Quality::High}, "YouTube 720").empty());
        CHECK(store.remove("Exact").empty());
    }
    app::RenderProfileStore store(scratch.file.string());
    REQUIRE(store.userProfiles().size() == 1);
    CHECK(store.userProfiles()[0].name == "YouTube");
    CHECK(store.userProfiles()[0].height == 1080);
}

TEST_CASE("RenderProfileStore: a hand-edited bad entry is skipped")
{
    Scratch scratch;
    fs::create_directories(scratch.file.parent_path());
    std::ofstream(scratch.file) << "[Good one]\nquality=max\nheight=0\n\n[Bad one]\nquality=superb\n";
    app::RenderProfileStore store(scratch.file.string());
    REQUIRE(store.userProfiles().size() == 1);
    CHECK(store.userProfiles()[0].name == "Good one");
    CHECK(store.userProfiles()[0].quality == Quality::Max);
}

TEST_CASE("RenderProfileStore: a frame rate round-trips; none means the project's")
{
    Scratch scratch;
    {
        app::RenderProfileStore store(scratch.file.string());
        CHECK(store.save({.name = "NTSC", .frameRate = {30000, 1001}, .quality = Quality::High}, "").empty());
        CHECK(store.save({.name = "Same", .quality = Quality::Good}, "").empty());
    }
    app::RenderProfileStore store(scratch.file.string());
    CHECK(store.find("NTSC")->frameRate == ustudio::core::Rational{30000, 1001});
    CHECK(store.find("Same")->frameRate == ustudio::core::Rational{0, 1});
}
