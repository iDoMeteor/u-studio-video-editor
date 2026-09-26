#include "media_badges.h"

#include <cmath>

namespace ustudio::app {

std::string resolutionLabel(int width, int height)
{
    if (width <= 0 || height <= 0)
        return "";
    const bool wide = std::abs(width * 9 - height * 16) <= 16; // 16:9, give or take a pixel
    if (wide) {
        switch (height) {
        case 4320:
            return "8K";
        case 2160:
            return "4K";
        case 1440:
            return "1440p";
        case 1080:
            return "1080p";
        case 720:
            return "720p";
        default:
            break;
        }
    }
    if (width == 4096 && height == 2160)
        return "4K";
    if ((height == 480 || height == 576) && width <= 1024)
        return "SD";
    return std::to_string(width) + "×" + std::to_string(height);
}

std::vector<MediaBadge> mediaBadges(const core::Asset &asset, const core::Profile &sequence, const ProxyState &proxy)
{
    std::vector<MediaBadge> badges;
    const core::MediaInfo &info = asset.info;
    if (info.isImageSequence)
        badges.push_back({"SEQUENCE", "badge-kind"});
    else if (info.isStillImage)
        badges.push_back({"IMAGE", "badge-kind"});
    else if (info.hasAudio && !info.hasVideo)
        badges.push_back({"AUDIO", "badge-kind"});
    if (info.hasVideo) {
        if (std::string label = resolutionLabel(info.width, info.height); !label.empty())
            badges.push_back(
                {label, sequence.height > 0 && info.height > sequence.height ? "badge-high" : "badge-resolution"});
    }
    switch (asset.status) {
    case core::Asset::Status::Pending:
        badges.push_back({"PROBING", "badge-info"});
        break;
    case core::Asset::Status::Failed:
        badges.push_back({"FAILED", "badge-danger"});
        break;
    case core::Asset::Status::Missing:
        badges.push_back({"MISSING", "badge-danger"});
        return badges; // plays a placeholder: its proxy doesn't matter
    case core::Asset::Status::Ready:
        break;
    }
    if (proxy.progress)
        badges.push_back({"PROXY " + std::to_string(static_cast<int>(*proxy.progress * 100)) + "%", "badge-proxy"});
    else if (!asset.proxyPath.empty())
        badges.push_back(proxy.fileExists ? MediaBadge{"PROXY", "badge-proxy"}
                                          : MediaBadge{"PROXY MISSING", "badge-warning"});
    return badges;
}

} // namespace ustudio::app
