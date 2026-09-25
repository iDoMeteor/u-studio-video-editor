#include "engine/proxy.h"

#include "core/log.h"
#include "core/model/model.h"
#include "core/model/profile_match.h"
#include "core/render/render_profile.h"
#include "engine/engine_sync.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace ustudio::engine {

namespace Log = core::Log;

bool renderProxy(const ProxyRequest &request, std::string &error, std::function<void(int, int)> onProgress,
                 const std::atomic<bool> *cancel)
{
    core::Profile probeProfile;
    probeProfile.fps = request.fps;
    const EngineSync::ProbedMedia probed = EngineSync::probeMediaFile(probeProfile, request.source);
    if (probed.length <= 0) {
        error = "can't open " + request.source;
        return false;
    }
    if (probed.isStillImage) {
        error = "a still image needs no proxy";
        return false;
    }
    if (probed.width <= 0 || probed.height <= 0) {
        error = "no picture in " + request.source;
        return false;
    }

    // The source alone, on a sequence at its own size and the given rate.
    core::Profile profile;
    profile.width = probed.width;
    profile.height = probed.height;
    profile.fps = request.fps;
    const int divisor = std::gcd(probed.width, probed.height);
    profile.dar = {probed.width / divisor, probed.height / divisor};
    core::Model model = core::Model::createEmpty(profile);
    const core::TrackId track = model.addTrack(core::Track::Kind::Video, 0, "V1");
    core::Asset asset;
    asset.path = request.source;
    asset.info.hasVideo = true;
    asset.info.hasAudio = probed.hasAudio;
    asset.info.lengthInSequenceFrames = probed.length;
    asset.info.width = probed.width;
    asset.info.height = probed.height;
    const core::AssetId id = model.addAsset(asset);
    model.insertClip(track, id, 0, 0, probed.length - 1);

    core::RenderProfile renderProfile;
    renderProfile.name = "Proxy";
    renderProfile.height = request.height > 0 && request.height < probed.height ? request.height : 0;
    renderProfile.quality = core::RenderProfile::Quality::Draft;
    // "g" (the GOP size) passes through to the encoder like crf and preset:
    // a standalone repro (MLT 7.40, libx264, 2026-09-25) gave 5 keyframes in
    // 60 frames with g=15, 1 without.
    const double fps = static_cast<double>(request.fps.num) / std::max(request.fps.den, 1);
    const int gop = std::max(1, static_cast<int>(std::lround(fps / 2)));
    Log::info("[proxy] " + request.source + " -> " + request.output + " (" +
              (renderProfile.height > 0 ? std::to_string(renderProfile.height) + "p" : std::string("source size")) +
              ", " + core::formatFps(request.fps) + " fps)");
    return renderProject(model, request.output, error, std::move(onProgress), cancel, renderProfile,
                         request.threadBudget, {{"g", std::to_string(gop)}});
}

} // namespace ustudio::engine
