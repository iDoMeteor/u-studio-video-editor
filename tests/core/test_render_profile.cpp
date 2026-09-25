// core::RenderProfile: built-ins, encoder settings, name checks.

#include "doctest.h"

#include "core/render/render_profile.h"

using namespace ustudio::core;

TEST_CASE("render profiles: the built-ins")
{
    const auto &profiles = builtInRenderProfiles();
    REQUIRE(profiles.size() == 2);
    CHECK(profiles[0].name == kDefaultRenderProfileName);
    CHECK(profiles[0].quality == RenderProfile::Quality::High);
    CHECK(legacyRenderProfile().name == "Draft (legacy)");

    // The legacy profile is exactly the pre-profile settings.
    EncoderSettings legacy = encoderSettings(legacyRenderProfile(), Profile{}, true);
    CHECK(legacy.crf == -1);
    CHECK(legacy.videoBitrate == 922698);
    CHECK(legacy.audioBitrate == 126422);
    CHECK(legacy.width == 0);
    CHECK(legacy.height == 0);
}

TEST_CASE("render profiles: quality presets use CRF, or a bitrate without it")
{
    RenderProfile high = builtInRenderProfiles()[0];
    EncoderSettings crf = encoderSettings(high, Profile{}, true);
    CHECK(crf.crf == 18);
    CHECK(crf.preset == "slow");
    CHECK(crf.videoBitrate == 0);
    CHECK(crf.audioBitrate == 192000);

    // OpenH264: 0.12 bits per pixel at 1920x1080x30.
    EncoderSettings bitrate = encoderSettings(high, Profile{}, false);
    CHECK(bitrate.crf == -1);
    CHECK(bitrate.preset.empty());
    CHECK(bitrate.videoBitrate == 7464960);
}

TEST_CASE("render profiles: a height scales at the project's aspect, in even pixels")
{
    RenderProfile profile{.name = "720", .height = 720, .quality = RenderProfile::Quality::Good};
    EncoderSettings settings = encoderSettings(profile, Profile{}, true);
    CHECK(settings.width == 1280);
    CHECK(settings.height == 720);

    Profile scope;
    scope.width = 2048;
    scope.height = 858;
    scope.dar = {1024, 429};
    settings = encoderSettings(profile, scope, true);
    CHECK(settings.height == 720);
    CHECK(settings.width == 1718); // 720 * 2.387 = 1718.7, rounded to even

    // The project's own height is no scaling at all.
    profile.height = 1080;
    CHECK(encoderSettings(profile, Profile{}, true).width == 0);
}

TEST_CASE("render profiles: names")
{
    std::vector<RenderProfile> mine = {{.name = "YouTube"}};
    CHECK(renderProfileNameProblem("Promo", mine).empty());
    CHECK_FALSE(renderProfileNameProblem("", mine).empty());
    CHECK_FALSE(renderProfileNameProblem("  ", mine).empty());
    CHECK_FALSE(renderProfileNameProblem("YouTube", mine).empty());
    CHECK_FALSE(renderProfileNameProblem("High quality", mine).empty());
    CHECK_FALSE(renderProfileNameProblem("a[b]", mine).empty());
}
