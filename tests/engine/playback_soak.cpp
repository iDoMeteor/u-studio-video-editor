// doc 12, M2 soak: "4K60 source plays at real time with frame dropping at
// preview scale 0.5 on the dev machine; no unbounded memory growth over
// 10 min". Not part of `meson test` (it runs for minutes against the real
// audio device); run by hand:
//   playback_soak <media> <minutes> [clips]
// Builds a timeline of `clips` back-to-back copies of <media> in a sequence
// whose size and frame rate match the source (the criterion is 4K60
// playback; a default 30 fps sequence would only show every other frame),
// plays it through the real EngineSync + PlaybackController at
// PreviewScale::Half, and prints every 10 s:
//   playhead vs. wall clock; frames the consumer showed; frames the UI
//   callback received; positions skipped between deliveries; the longest
//   main-loop stall; RSS; load average; CPU package temperature; average
//   CPU clock.
// shown < expected: the consumer dropped frames (rendering behind real
// time). delivered < shown: the single-slot hand-off overwrote frames
// because the main loop didn't drain it in time (see "stall"). Load,
// temperature and clock say whether the machine was fit to judge by.
#include "core/model/model.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"
#include "engine/playback_controller.h"

#include <glib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

double loadAverage1()
{
    std::ifstream f("/proc/loadavg");
    double value = 0;
    f >> value;
    return value;
}

// x86_pkg_temp if present, else the first thermal zone; degrees C, or -1.
double packageTempC()
{
    namespace fs = std::filesystem;
    std::error_code ec;
    double fallback = -1;
    for (const auto &zone : fs::directory_iterator("/sys/class/thermal", ec)) {
        if (zone.path().filename().string().rfind("thermal_zone", 0) != 0)
            continue;
        std::ifstream type(zone.path() / "type"), temp(zone.path() / "temp");
        std::string name;
        long milli = 0;
        if (!(type >> name) || !(temp >> milli))
            continue;
        if (name == "x86_pkg_temp")
            return static_cast<double>(milli) / 1000.0;
        if (fallback < 0)
            fallback = static_cast<double>(milli) / 1000.0;
    }
    return fallback;
}

// Mean of every CPU's scaling_cur_freq, in MHz, or -1.
double averageCpuMhz()
{
    long sum = 0, count = 0;
    for (int cpu = 0;; ++cpu) {
        std::ifstream f("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/scaling_cur_freq");
        long khz = 0;
        if (!(f >> khz))
            break;
        sum += khz;
        ++count;
    }
    return count ? static_cast<double>(sum) / static_cast<double>(count) / 1000.0 : -1;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <media> <minutes> [clips]\n", argv[0]);
        return 2;
    }
    // Same as main.cpp: without it SDL (initialised by the sdl2_audio
    // consumer) swallows SIGTERM/SIGINT and the soak can't be stopped.
    g_setenv("SDL_NO_SIGNAL_HANDLERS", "1", FALSE);
    const std::string media = argv[1];
    const int minutes = std::atoi(argv[2]);
    FactoryPolicy policy;

    // Probe once at the default profile to learn the source's size and
    // rate, then build the real sequence to match.
    Profile profile;
    {
        Model probeModel = Model::createEmpty();
        EngineSync probeSync(probeModel);
        EngineSync::ProbedMedia first = probeSync.probeMedia(media);
        if (first.length <= 0) {
            std::fprintf(stderr, "could not open %s\n", media.c_str());
            return 1;
        }
        if (first.width > 0 && first.height > 0) {
            profile.width = first.width;
            profile.height = first.height;
        }
        if (first.fps.num > 0 && first.fps.den > 0)
            profile.fps = first.fps;
    }
    Model model = Model::createEmpty(profile);
    EngineSync sync(model, PreviewScale::Half);
    EngineSync::ProbedMedia probed = sync.probeMedia(media); // length in this sequence's frames

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
    sync.setProject(model.snapshot());
    std::printf("timeline: %d x %d frames (%s, source %dx%d @ %d/%d) in a %dx%d @ %d fps sequence; "
                "playback profile %dx%d (preview Half)\n",
                clips, static_cast<int>(probed.length), media.c_str(), probed.width, probed.height, probed.fps.num,
                probed.fps.den, profile.width, profile.height, fps, sync.profile().width(), sync.profile().height());

    PlaybackController controller;
    std::atomic<long> delivered{0};
    std::atomic<long> skipped{0};
    std::atomic<int> lastPosition{-1};
    std::atomic<int> lastWidth{0}, lastHeight{0};
    controller.setFrameCallback([&](std::vector<uint8_t>, int width, int height, int position) {
        ++delivered;
        lastWidth = width;
        lastHeight = height;
        int previous = lastPosition.exchange(position);
        if (previous >= 0 && position > previous + 1)
            skipped += position - previous - 1;
    });
    controller.setTractor(sync.tractorPtr());
    controller.play(1.0);
    std::printf("backend: %s\n", controller.backendName().c_str());
    std::printf("%5s %8s %8s %6s %6s %6s %5s %6s %7s %5s %5s %6s\n", "t(s)", "playhead", "expected", "shown", "deliv",
                "skip", "lag", "stall", "RSS(MB)", "load", "temp", "MHz");

    // A real GMainLoop, like the app: frames are drained the moment their
    // idle source is posted. (The first version polled the context and
    // slept 5 ms between polls, which overwrote frames in the single-slot
    // hand-off whenever two arrived within one sleep, under-counting
    // delivery once rendering got faster.) Reports and the end of the run
    // are timer sources; stalls are measured as the lateness of a 5 ms
    // timer, i.e. how long the main loop was busy or starved.
    struct State
    {
        PlaybackController *controller;
        int fps;
        std::chrono::steady_clock::time_point start, lastTick;
        long rssStart = 0, shownPrev = 0, deliveredPrev = 0, skippedPrev = 0;
        double maxStallMs = 0;
        std::atomic<long> *delivered, *skipped;
        GMainLoop *loop;
    } state{&controller, fps, std::chrono::steady_clock::now(), std::chrono::steady_clock::now(), 0, 0, 0, 0, 0,
            &delivered, &skipped, g_main_loop_new(nullptr, FALSE)};

    g_timeout_add(
        5,
        [](gpointer data) -> gboolean {
            auto *st = static_cast<State *>(data);
            auto now = std::chrono::steady_clock::now();
            st->maxStallMs = std::max(st->maxStallMs, std::chrono::duration<double, std::milli>(now - st->lastTick).count());
            st->lastTick = now;
            return G_SOURCE_CONTINUE;
        },
        &state);
    g_timeout_add_seconds(
        10,
        [](gpointer data) -> gboolean {
            auto *st = static_cast<State *>(data);
            auto now = std::chrono::steady_clock::now();
            double t = std::chrono::duration<double>(now - st->start).count();
            long expected = static_cast<long>(t * st->fps);
            int playhead = st->controller->currentFrame();
            long rss = rssKb();
            if (st->rssStart == 0)
                st->rssStart = rss;
            long shown = st->controller->frameShowCount(), deliv = st->delivered->load(), skip = st->skipped->load();
            std::printf("%5.0f %8d %8ld %6ld %6ld %6ld %5ld %6.0f %7.1f %5.1f %5.0f %6.0f\n", t, playhead, expected,
                        shown - st->shownPrev, deliv - st->deliveredPrev, skip - st->skippedPrev, expected - playhead,
                        st->maxStallMs, static_cast<double>(rss) / 1024.0, loadAverage1(), packageTempC(),
                        averageCpuMhz());
            std::fflush(stdout);
            st->shownPrev = shown;
            st->deliveredPrev = deliv;
            st->skippedPrev = skip;
            st->maxStallMs = 0;
            return G_SOURCE_CONTINUE;
        },
        &state);
    g_timeout_add_seconds(
        static_cast<guint>(minutes * 60),
        [](gpointer data) -> gboolean {
            g_main_loop_quit(static_cast<State *>(data)->loop);
            return G_SOURCE_REMOVE;
        },
        &state);
    g_main_loop_run(state.loop);
    g_main_loop_unref(state.loop);
    const long rssStart = state.rssStart;

    controller.shutdown();
    std::printf("totals: shown %ld, delivered %ld, skipped positions %ld, last frame %dx%d\n",
                controller.frameShowCount(), delivered.load(), skipped.load(), lastWidth.load(), lastHeight.load());
    std::printf("RSS first report -> end: %.1f -> %.1f MB\n", static_cast<double>(rssStart) / 1024.0,
                static_cast<double>(rssKb()) / 1024.0);
    return 0;
}
