#pragma once

// M4 D: the media browser's badges, as data (no GTK), so what each asset
// shows is tested without a window (tests/app/test_media_badges). Order:
// what it is (IMAGE, SEQUENCE, AUDIO), its resolution, then its state
// (PROBING, FAILED, MISSING) and its proxy's.

#include "core/model/types.h"

#include <optional>
#include <string>
#include <vector>

namespace ustudio::app {

struct MediaBadge
{
    std::string text;
    std::string cssClass; // style.css: badge-kind, badge-resolution, badge-high, badge-info, ...

    bool operator==(const MediaBadge &) const = default;
};

struct ProxyState
{
    std::optional<double> progress; // making one: 0..1
    bool fileExists = false;        // asset.proxyPath is on disk
};

// "4K", "1440p", "1080p", "720p", "SD" for the usual 16:9 and SD sizes;
// "1344×768" otherwise; "" when unknown.
std::string resolutionLabel(int width, int height);

// `sequence`: a picture taller than it is marked badge-high (a proxy would
// help).
std::vector<MediaBadge> mediaBadges(const core::Asset &asset, const core::Profile &sequence, const ProxyState &proxy);

} // namespace ustudio::app
