// doc 12, M2 soak: "4K60 source plays at real time with frame dropping at
// preview scale 0.5 on the dev machine; no unbounded memory growth over
// 10 min". Not part of `meson test` (it runs for minutes against the real
// audio device); run by hand:
//   playback_soak <media> <minutes> [clips]
// Builds a timeline of `clips` back-to-back copies of <media> (enough to
// cover <minutes> without looping), plays it through the real
// PlaybackController at PreviewScale::Half, and prints every 10 s: the
// playhead vs. wall-clock position, frames delivered to the UI callback,
// and resident memory.
#include "core/model/model.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"
#include "engine/playback_controller.h"

#include <glib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

using namespace ustudio::core;
using namespace ustudio::engine;

namespace {
long rssKb()
{
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line))
        if (line.rfind("VmRSS:", 0) == 0)
            return std::stol(line.substr(6));
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <media> <minutes> [clips]\n", argv[0]);
        return 2;
    }
    const std::string media = argv[1];
    const int minutes = std::atoi(argv[2]);
    FactoryPolicy policy;
    Model model = Model::createEmpty();
    EngineSync sync(model);

    EngineSync::ProbedMedia probed = sync.probeMedia(media);
    if (probed.length <= 0) {
        std::fprintf(stderr, "could not open %s\n", media.c_str());
        return 1;
    }
    const int fps = static_cast<int>(sync.profile().fps() + 0.5);
    const int needed = minutes * 60 * fps;
    const int clips = argc > 3 ? std::atoi(argv[3]) : static_cast<int>(needed / probed.length + 1);
    Asset asset;
    asset.path = media;
    asset.displayName = "soak";
    asset.info.hasVideo = true;
    asset.info.hasAudio = probed.hasAudio;
    asset.info.lengthInSequenceFrames = probed.length;
    AssetId assetId = model.addAsset(asset);
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    for (int i = 0; i < clips; ++i)
        model.insertClip(track, assetId, static_cast<FrameIndex>(i) * probed.length, 0, probed.length - 1);
    std::printf("timeline: %d x %d frames (%s, source %dx%d @ %d/%d) at sequence %d fps, preview scale Half\n", clips,
                static_cast<int>(probed.length), media.c_str(), probed.width, probed.height, probed.fps.num,
                probed.fps.den, fps);

    PlaybackController controller;
    std::atomic<long> delivered{0};
    controller.setFrameCallback([&](std::vector<uint8_t>, int, int, int) { ++delivered; });
    controller.setTractor(sync.tractorPtr());
    controller.setPreviewScale(PlaybackController::PreviewScale::Half);
    controller.play(1.0);
    std::printf("backend: %s\n", controller.backendName().c_str());
    std::printf("%6s %10s %10s %9s %9s %8s\n", "t(s)", "playhead", "expected", "delivered", "lag(frm)", "RSS(MB)");

    const auto start = std::chrono::steady_clock::now();
    auto nextReport = start + std::chrono::seconds(10);
    const auto end = start + std::chrono::minutes(minutes);
    long rssStart = 0;
    while (std::chrono::steady_clock::now() < end) {
        while (g_main_context_iteration(nullptr, FALSE)) {
        }
        auto now = std::chrono::steady_clock::now();
        if (now >= nextReport) {
            double t = std::chrono::duration<double>(now - start).count();
            long expected = static_cast<long>(t * fps);
            int playhead = controller.currentFrame();
            long rss = rssKb();
            if (rssStart == 0)
                rssStart = rss;
            std::printf("%6.0f %10d %10ld %9ld %9ld %8.1f\n", t, playhead, expected, delivered.load(),
                        expected - playhead, static_cast<double>(rss) / 1024.0);
            std::fflush(stdout);
            nextReport += std::chrono::seconds(10);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    controller.shutdown();
    std::printf("RSS first report -> end: %.1f -> %.1f MB\n", static_cast<double>(rssStart) / 1024.0,
                static_cast<double>(rssKb()) / 1024.0);
    return 0;
}
