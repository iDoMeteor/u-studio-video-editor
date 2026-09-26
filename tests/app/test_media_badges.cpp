// M4 D: what the media browser shows for each asset.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/media_badges.h"

using namespace ustudio::core;
using namespace ustudio::app;

namespace {

Asset video(int width, int height)
{
    Asset asset;
    asset.status = Asset::Status::Ready;
    asset.info.hasVideo = asset.info.hasAudio = true;
    asset.info.width = width;
    asset.info.height = height;
    return asset;
}

std::vector<std::string> texts(const std::vector<MediaBadge> &badges)
{
    std::vector<std::string> out;
    for (const MediaBadge &badge : badges)
        out.push_back(badge.text);
    return out;
}

} // namespace

TEST_CASE("media badges: resolution labels for the usual sizes, the size otherwise")
{
    CHECK(resolutionLabel(3840, 2160) == "4K");
    CHECK(resolutionLabel(4096, 2160) == "4K");
    CHECK(resolutionLabel(2560, 1440) == "1440p");
    CHECK(resolutionLabel(1920, 1080) == "1080p");
    CHECK(resolutionLabel(1280, 720) == "720p");
    CHECK(resolutionLabel(720, 480) == "SD");
    CHECK(resolutionLabel(720, 576) == "SD");
    CHECK(resolutionLabel(1344, 768) == "1344×768");
    CHECK(resolutionLabel(1080, 1920) == "1080×1920"); // portrait
    CHECK(resolutionLabel(1440, 1080) == "1440×1080"); // 4:3 at 1080 lines
    CHECK(resolutionLabel(0, 0).empty());
}

TEST_CASE("media badges: kind, resolution against the project, state, proxy")
{
    const Profile hd; // 1920x1080
    CHECK(mediaBadges(video(1920, 1080), hd, {}) == std::vector<MediaBadge>{{"1080p", "badge-resolution"}});
    CHECK(mediaBadges(video(3840, 2160), hd, {}) == std::vector<MediaBadge>{{"4K", "badge-high"}});

    Asset still = video(4000, 3000);
    still.info.isStillImage = true;
    still.info.hasAudio = false;
    CHECK(texts(mediaBadges(still, hd, {})) == std::vector<std::string>{"IMAGE", "4000×3000"});

    Asset audio;
    audio.status = Asset::Status::Ready;
    audio.info.hasAudio = true;
    CHECK(texts(mediaBadges(audio, hd, {})) == std::vector<std::string>{"AUDIO"});

    Asset probing = video(0, 0);
    probing.status = Asset::Status::Pending;
    CHECK(texts(mediaBadges(probing, hd, {})) == std::vector<std::string>{"PROBING"});

    Asset failed = video(1920, 1080);
    failed.status = Asset::Status::Failed;
    CHECK(mediaBadges(failed, hd, {}).back() == MediaBadge{"FAILED", "badge-danger"});

    // Missing: the proxy doesn't show (the placeholder plays).
    Asset missing = video(3840, 2160);
    missing.status = Asset::Status::Missing;
    missing.proxyPath = "/cache/p.mp4";
    CHECK(texts(mediaBadges(missing, hd, {std::nullopt, true})) == std::vector<std::string>{"4K", "MISSING"});

    Asset proxied = video(3840, 2160);
    CHECK(texts(mediaBadges(proxied, hd, {0.42, false})) == std::vector<std::string>{"4K", "PROXY 42%"});
    proxied.proxyPath = "/cache/p.mp4";
    CHECK(mediaBadges(proxied, hd, {std::nullopt, true}).back() == MediaBadge{"PROXY", "badge-proxy"});
    CHECK(mediaBadges(proxied, hd, {std::nullopt, false}).back() == MediaBadge{"PROXY MISSING", "badge-warning"});
}
