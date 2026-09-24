// doc 19 MT0: how long Model::snapshot() takes on a big project, to decide
// whether snapshots need structural sharing ("Snapshots, not sharing").
// Not a test: `meson test -C builddir --benchmark core-snapshot`, or run the
// binary directly.

#include "core/model/model.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

using namespace ustudio::core;

int main()
{
    // 5,000 clips over 8 tracks, a dissolve between every other pair of
    // butted clips, 200 markers, a few dozen assets.
    Model model = Model::createEmpty();
    std::vector<AssetId> assets;
    for (int a = 0; a < 40; ++a) {
        Asset asset;
        asset.path = "/media/footage/clip-" + std::to_string(a) + ".mp4";
        asset.displayName = "clip-" + std::to_string(a) + ".mp4";
        asset.info.hasVideo = true;
        asset.info.hasAudio = true;
        asset.info.lengthInSequenceFrames = 1'000'000;
        assets.push_back(model.addAsset(asset));
    }
    constexpr int kTracks = 8, kClips = 5000;
    std::vector<TrackId> tracks;
    for (int t = 0; t < kTracks; ++t)
        tracks.push_back(model.addTrack(t < 5 ? Track::Kind::Video : Track::Kind::Audio, static_cast<size_t>(t),
                                        "T" + std::to_string(t)));
    int dissolves = 0;
    for (int t = 0; t < kTracks; ++t) {
        FrameIndex pos = 0;
        ClipId previous;
        for (int c = 0; c < kClips / kTracks; ++c) {
            FrameIndex in = 1000 + c * 10;
            ClipId clip =
                model.insertClip(tracks[static_cast<size_t>(t)], assets[static_cast<size_t>(c % 40)], pos, in, in + 99);
            if (t >= 5)
                model.setClipEnabled(clip, false, true);
            if (previous.isValid() && c % 2 == 1) {
                model.addTransition(tracks[static_cast<size_t>(t)], previous, clip, 5, 5);
                ++dissolves;
            }
            previous = clip;
            pos = model.clip(clip).end() + (c % 5 == 0 ? 30 : 0);
            if (c % 5 == 0)
                previous = ClipId{}; // a gap: nothing to dissolve into
        }
    }
    for (int m = 0; m < 200; ++m)
        model.addMarker(m * 250, "marker " + std::to_string(m));

    size_t clips = 0;
    for (const Track &t : model.sequence().tracks)
        clips += t.clips.size();

    // Each sample edits first, so every snapshot() is a real copy.
    std::vector<double> ms;
    constexpr int kRuns = 101;
    for (int i = 0; i < kRuns; ++i) {
        model.setTrackVolume(tracks[0], i % 2 ? 0.5 : 1.0);
        auto start = std::chrono::steady_clock::now();
        std::shared_ptr<const Project> snapshot = model.snapshot();
        auto end = std::chrono::steady_clock::now();
        ms.push_back(std::chrono::duration<double, std::milli>(end - start).count());
        if (!snapshot)
            return 1;
    }
    std::sort(ms.begin(), ms.end());
    auto cachedStart = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i)
        (void)model.snapshot();
    double cachedUs =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - cachedStart).count() / 1000.0;
    std::printf("snapshot of %zu clips, %d dissolves, %zu markers, %d tracks: median %.3f ms, p90 %.3f ms, max %.3f ms "
                "(%d runs); cached call %.3f us\n",
                clips, dissolves, model.sequence().markers.size(), kTracks, ms[ms.size() / 2], ms[ms.size() * 9 / 10],
                ms.back(), kRuns, cachedUs);
    return 0;
}
