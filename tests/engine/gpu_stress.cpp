// GPU pipeline stress (ADR-019; the create_fbo aborts VE Demos found,
// 2026-09-28). `meson test` runs it for 45 s with a fixed seed
// (engine-gpu-stress); for minutes, by hand:
//   SDL_AUDIODRIVER=dummy gpu_stress <seconds> [seed]
// A tour-like project (V1: 1080p clips with dissolves, including one
// between two clips of the same file, one clip proxied; V2: 1344x768@25
// clips fitted and a 1080p PNG still; V3: a scaled, rotated, flipped copy of
// a V1 clip) played through engine::Engine and the real consumer on the GPU
// pipeline, with random play, pause, seek, frame steps, preview scale, the
// proxy toggle and dissolve edits. Every action is printed with the seed, so
// a crash replays. Exit 0: no crash, and frames kept coming; 77 (meson's
// "skipped"): no GPU pipeline on this machine.
#include "core/media/utf8_path.h"
#include "core/model/model.h"
#include "core/model/transform.h"
#include "engine/engine.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"
#include "platform/process.h"

#include <glib.h>
#include <mlt++/Mlt.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>

using namespace ustudio;
using namespace ustudio::core;
using namespace ustudio::engine;
namespace fs = std::filesystem;

namespace {

fs::path scratch()
{
    static const fs::path dir =
        fs::temp_directory_path() / ("ustudio-gpu-stress-" + std::to_string(platform::currentProcessId()));
    fs::create_directories(dir);
    return dir;
}

// `frames` of a moving test pattern at width x height and fps, as H.264,
// or one frame as PNG when `png`.
fs::path generate(const std::string &name, int width, int height, int fps, int frames, bool png = false)
{
    const fs::path path = scratch() / name;
    Profile profile;
    profile.width = width;
    profile.height = height;
    profile.fps = {fps, 1};
    Model model = Model::createEmpty(profile);
    EngineSync sync(model);
    Mlt::Producer producer(sync.profile(), "noise:");
    producer.set_in_and_out(0, frames - 1);
    std::unique_ptr<Mlt::Profile> consumerProfile(producer.profile());
    Mlt::Consumer consumer(*consumerProfile, "avformat", utf8String(path).c_str());
    if (png) {
        consumer.set("vcodec", "png");
        consumer.set("f", "image2");
        consumer.set("update", 1);
    } else {
        consumer.set("vcodec", h264Encoder().c_str());
    }
    consumer.set("real_time", -1);
    consumer.connect(producer);
    consumer.run();
    return path;
}

template <class Done> bool pumpUntil(Done done, std::chrono::milliseconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_iteration(nullptr, FALSE)) {
        }
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

Transform placed(double cx, double cy, double w, double h, double rotation, bool flip)
{
    Transform t;
    t.bounds = Transform::Bounds::None;
    t.x.value = cx;
    t.y.value = cy;
    t.width.value = w;
    t.height.value = h;
    t.rotation.value = rotation;
    t.flipH = flip;
    return t;
}

} // namespace

int main(int argc, char **argv)
{
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 120;
    const unsigned seed = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : std::random_device{}();
    g_setenv("SDL_NO_SIGNAL_HANDLERS", "1", FALSE);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::printf("gpu_stress: %d s, seed %u\n", seconds, seed);
    FactoryPolicy policy;

    const std::string a = utf8String(generate("a.mp4", 1920, 1080, 30, 90));
    const std::string b = utf8String(generate("b.mp4", 1920, 1080, 30, 90));
    const std::string aProxy = utf8String(generate("a-proxy.mp4", 960, 540, 30, 90));
    const std::string capture = utf8String(generate("capture.mp4", 1344, 768, 25, 75));
    const std::string still = utf8String(generate("still.png", 1920, 1080, 30, 1, true));

    Model model = Model::createEmpty();
    const TrackId v1 = model.addTrack(Track::Kind::Video, 0, "V1");
    const TrackId v2 = model.addTrack(Track::Kind::Video, 0, "V2");
    const TrackId v3 = model.addTrack(Track::Kind::Video, 0, "V3");
    auto asset = [&](const std::string &path, int w, int h, int length, bool isStill = false,
                     const std::string &proxy = {}) {
        Asset x;
        x.path = path;
        x.proxyPath = proxy;
        x.info.hasVideo = true;
        x.info.width = w;
        x.info.height = h;
        x.info.isStillImage = isStill;
        x.info.lengthInSequenceFrames = length;
        return model.addAsset(x);
    };
    const AssetId aAsset = asset(a, 1920, 1080, 90, false, aProxy), bAsset = asset(b, 1920, 1080, 90);
    const AssetId capAsset = asset(capture, 1344, 768, 90), stillAsset = asset(still, 1920, 1080, 0, true);
    // V1: a b a a b a (two same-file neighbours), dissolves at every join.
    std::vector<ClipId> v1Clips;
    const AssetId order[] = {aAsset, bAsset, aAsset, aAsset, bAsset, aAsset};
    for (int i = 0; i < 6; ++i)
        v1Clips.push_back(model.insertClip(v1, order[i], i * 60, 0, 59));
    for (int i = 0; i + 1 < 6; ++i)
        model.addTransition(v1, v1Clips[static_cast<size_t>(i)], v1Clips[static_cast<size_t>(i + 1)], 8, 8);
    // V2: fitted captures, then the still.
    model.insertClip(v2, capAsset, 0, 0, 59);
    model.insertClip(v2, capAsset, 60, 0, 59);
    model.insertClip(v2, stillAsset, 120, 0, 119);
    // V3: a transformed copy of V1's first clip, and one of b.
    model.setClipTransform(model.insertClip(v3, aAsset, 30, 0, 59), placed(1300, 700, 945, 540, 21, true));
    model.setClipTransform(model.insertClip(v3, bAsset, 200, 0, 59), placed(500, 300, 640, 360, 0, false));

    Engine engine(model.snapshot(), PreviewScale::Auto);
    std::optional<bool> gpuOn;
    engine.gpuChanged.connect([&](bool on, const std::string &detail) {
        gpuOn = on;
        std::printf("gpu %s (%s)\n", on ? "on" : "off", detail.c_str());
    });
    long frames = 0;
    engine.setFrameCallback([&](std::vector<uint8_t>, int, int, int) { ++frames; });
    engine.setGpuPipeline(true, {});
    if (!pumpUntil([&] { return gpuOn.has_value(); }, std::chrono::seconds(20)) || !*gpuOn) {
        std::printf("no GPU pipeline; nothing to stress\n");
        engine.shutdown();
        return 77;
    }

    std::mt19937 rng(seed);
    auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    long actions = 0;
    bool proxies = false, dissolves = true;
    while (std::chrono::steady_clock::now() < end) {
        const int length = std::max(engine.totalFrames(), 1);
        const int what = pick(100);
        ++actions;
        if (what < 30) {
            const int to = pick(length);
            std::printf("[%ld] seek %d\n", actions, to);
            engine.seek(to);
        } else if (what < 50) {
            std::printf("[%ld] play\n", actions);
            engine.play();
        } else if (what < 68) {
            std::printf("[%ld] pause\n", actions);
            engine.pause();
        } else if (what < 78) {
            const int delta = pick(2) ? 1 : -1;
            std::printf("[%ld] step %d\n", actions, delta);
            engine.stepFrame(delta);
        } else if (what < 86) {
            const PreviewScale scale = pick(2) ? PreviewScale::Half : PreviewScale::Full;
            std::printf("[%ld] scale %s\n", actions, scale == PreviewScale::Half ? "half" : "full");
            engine.setPreviewScale(scale);
        } else if (what < 92) {
            proxies = !proxies;
            std::printf("[%ld] proxies %s\n", actions, proxies ? "on" : "off");
            engine.setUseProxies(proxies);
        } else {
            // Drop or restore the dissolves (a rebuild, as Add Transition is).
            dissolves = !dissolves;
            std::printf("[%ld] dissolves %s\n", actions, dissolves ? "on" : "off");
            const auto transitions = model.sequence().transitions;
            for (const auto &t : transitions)
                model.removeTransition(t.id);
            if (dissolves)
                for (int i = 0; i + 1 < 6; ++i)
                    model.addTransition(v1, v1Clips[static_cast<size_t>(i)], v1Clips[static_cast<size_t>(i + 1)], 8, 8);
            engine.publish(model.snapshot());
        }
        // Dwell as a user does: a few frames to a couple of seconds.
        const int dwell = pick(10) == 0 ? 1000 + pick(1500) : 20 + pick(300);
        pumpUntil([] { return false; }, std::chrono::milliseconds(dwell));
    }
    std::printf("done: %ld actions, %ld frames delivered, GPU %s\n", actions, frames,
                engine.gpuPipeline() ? "still on" : "OFF");
    engine.shutdown();
    std::error_code ec;
    fs::remove_all(scratch(), ec);
    return engine.gpuPipeline() && frames > 0 ? 0 : 1;
}
