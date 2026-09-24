// doc 19 MT2, piece 0b: a track with more playlist entries than
// EngineSync::playlistChunkSize() is built from nested sub-playlists, since
// MLT refreshes a whole playlist on every append (O(k^2) to build flat).
// These tests hold the chunked graph to the flat one: same frames and audio
// everywhere, the same verify() verdicts, the same paused-seek, step and
// loop behaviour, the same rendered file.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/model/model.h"
#include "core/model/track_segments.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"
#include "engine/playback_controller.h"

#include <glib.h>
#include <mlt++/Mlt.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace ustudio::core;
using namespace ustudio::engine;

namespace {

FactoryPolicy &sharedFactoryPolicy()
{
    static FactoryPolicy policy;
    return policy;
}

// Restores the process-wide chunk size when a test ends.
struct ChunkSize
{
    explicit ChunkSize(size_t entries)
    {
        EngineSync::setPlaylistChunkSize(entries);
    }
    ~ChunkSize()
    {
        EngineSync::setPlaylistChunkSize(EngineSync::kDefaultPlaylistChunkSize);
    }
};
constexpr size_t kFlat = 1'000'000;

AssetId addAsset(Model &model, const std::string &resource, bool audio)
{
    Asset asset;
    asset.path = resource;
    asset.displayName = resource;
    asset.info.hasVideo = !audio;
    asset.info.hasAudio = audio;
    asset.info.lengthInSequenceFrames = 100'000;
    return model.addAsset(asset);
}

// Distinct solid colours, so a frame says which clip it came from.
const char *const kColours[] = {"color:#ff0000", "color:#00ff00", "color:#0000ff", "color:#ffff00",
                                "color:#ff00ff", "color:#00ffff", "color:#ffffff", "color:#808080"};

// V2 (hidden), V1, A1 (volume 0.5): 40 ten-frame clips each, a 5-frame gap
// after every 5th, and a 3+3 dissolve joining every 3rd butted pair.
struct TestProject
{
    Model model = Model::createEmpty();
    TrackId v1, v2, a1;
};

void fillTrack(Model &model, TrackId track, const std::vector<AssetId> &assets)
{
    FrameIndex pos = 0;
    ClipId previous;
    for (int c = 0; c < 40; ++c) {
        ClipId clip = model.insertClip(track, assets[size_t(c) % assets.size()], pos, 10, 19);
        if (previous.isValid() && c % 3 == 0)
            model.addTransition(track, previous, clip, 3, 3);
        previous = clip;
        pos = model.clip(clip).end();
        if (c % 5 == 4) {
            pos += 5;
            previous = ClipId{};
        }
    }
}

TestProject makeProject()
{
    TestProject p;
    p.v2 = p.model.addTrack(Track::Kind::Video, 0, "V2");
    p.v1 = p.model.addTrack(Track::Kind::Video, 1, "V1");
    p.a1 = p.model.addTrack(Track::Kind::Audio, 2, "A1");
    std::vector<AssetId> colours, reversed, tones;
    for (const char *c : kColours)
        colours.push_back(addAsset(p.model, c, false));
    reversed.assign(colours.rbegin(), colours.rend());
    for (int f : {220, 330, 440, 550, 660})
        tones.push_back(addAsset(p.model, "tone:" + std::to_string(f), true));
    fillTrack(p.model, p.v1, colours);
    fillTrack(p.model, p.v2, reversed);
    fillTrack(p.model, p.a1, tones);
    const Track &v2 = p.model.track(p.v2);
    p.model.setTrackFlags(p.v2, v2.muted, /*hidden=*/true, v2.locked);
    p.model.setTrackVolume(p.a1, 0.5);
    return p;
}

// Playlist entries rebuildTrackPlaylist() makes for a track: a gap is one,
// every segment is one. Returns each transition segment's entry index.
std::vector<size_t> transitionEntryIndices(const Model &model, TrackId track, size_t &entryCount)
{
    std::vector<size_t> indices;
    entryCount = 0;
    FrameIndex cursor = 0;
    for (const TrackSegment &seg : planTrackSegments(model, model.track(track))) {
        if (seg.start > cursor)
            ++entryCount;
        if (seg.kind == TrackSegment::Kind::Transition)
            indices.push_back(entryCount);
        ++entryCount;
        cursor = seg.start + seg.length;
    }
    return indices;
}

uint64_t fnv(const uint8_t *data, size_t size, uint64_t h = 1469598103934665603ull)
{
    for (size_t i = 0; i < size; ++i)
        h = (h ^ data[i]) * 1099511628211ull;
    return h;
}

// Image and audio hash of every frame the tractor plays.
std::vector<uint64_t> frameHashes(Mlt::Tractor &tractor)
{
    std::vector<uint64_t> hashes;
    for (int pos = 0; pos < tractor.get_length(); ++pos) {
        tractor.seek(pos);
        std::unique_ptr<Mlt::Frame> frame(tractor.get_frame());
        mlt_image_format format = mlt_image_rgb;
        int w = 64, h = 36;
        const uint8_t *image = frame->get_image(format, w, h);
        mlt_audio_format audioFormat = mlt_audio_s16;
        int frequency = 48000, channels = 2, samples = mlt_audio_calculate_frame_samples(30000.0f / 1001, 48000, pos);
        const auto *pcm = static_cast<const uint8_t *>(frame->get_audio(audioFormat, frequency, channels, samples));
        uint64_t hash = fnv(image, size_t(w * h * 3));
        hashes.push_back(pcm ? fnv(pcm, size_t(samples * channels * 2), hash) : hash);
    }
    return hashes;
}

std::unique_ptr<Mlt::Producer> trackAt(Mlt::Tractor &tractor, int index)
{
    return std::unique_ptr<Mlt::Producer>(tractor.track(index));
}

// The outer playlist's first entry is a chunk (not a cut of a clip).
bool isChunked(Mlt::Tractor &tractor, int mltIndex)
{
    Mlt::Playlist playlist(*trackAt(tractor, mltIndex));
    std::unique_ptr<Mlt::ClipInfo> info(playlist.clip_info(0));
    return info && info->producer && info->producer->get_int("ustudio.chunk") == 1;
}

template <class Done> bool pumpUntil(Done done, std::chrono::milliseconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_iteration(nullptr, FALSE)) {
        }
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

} // namespace

TEST_CASE("chunked playlists: every frame and its audio match the flat graph, whatever the chunk size")
{
    sharedFactoryPolicy();
    TestProject p = makeProject();
    size_t entries = 0;
    std::vector<size_t> transitions = transitionEntryIndices(p.model, p.v1, entries);
    REQUIRE(entries > 20);

    // verify() has a known, unrelated blind spot for audio-only generator
    // cuts ("tone:" reports resource "<producer>"; test_engine_sync.cpp's
    // volume test says the same): the chunked graph must give exactly the
    // flat graph's verdict, and the flat one only that.
    std::vector<uint64_t> flat;
    std::vector<std::string> flatProblems;
    {
        ChunkSize size(kFlat);
        EngineSync sync(p.model);
        flatProblems = sync.verify();
        for (const std::string &problem : flatProblems)
            CHECK(problem.find("'<producer>'") != std::string::npos);
        flat = frameHashes(sync.tractor());
    }
    REQUIRE(flat.size() > 300);

    bool dissolveFirstInChunk = false, dissolveLastInChunk = false;
    for (size_t chunk : {2, 3, 4, 5, 7, 8, 13}) {
        CAPTURE(chunk);
        for (size_t t : transitions) {
            dissolveFirstInChunk |= t % chunk == 0 && t > 0;
            dissolveLastInChunk |= t % chunk == chunk - 1;
        }
        ChunkSize size(chunk);
        EngineSync sync(p.model);
        CHECK(sync.verify() == flatProblems);
        // Tracks really are chunked (0 = black; the order is audio below video).
        for (int i = 1; i < sync.tractor().count(); ++i)
            CHECK(isChunked(sync.tractor(), i));
        CHECK(frameHashes(sync.tractor()) == flat);
    }
    // The sizes above put a dissolve sub-tractor both first and last in a chunk.
    CHECK(dissolveFirstInChunk);
    CHECK(dissolveLastInChunk);
}

TEST_CASE("chunked playlists: hide and the volume filter stay on the outer track playlist")
{
    sharedFactoryPolicy();
    TestProject p = makeProject();
    ChunkSize size(4);
    EngineSync sync(p.model);
    bool sawHidden = false, sawVolume = false;
    for (int i = 1; i < sync.tractor().count(); ++i) {
        Mlt::Playlist playlist(*trackAt(sync.tractor(), i));
        REQUIRE(isChunked(sync.tractor(), i));
        if (playlist.get_int("hide") == 1)
            sawHidden = true;
        for (int f = 0; f < playlist.filter_count(); ++f) {
            std::unique_ptr<Mlt::Filter> filter(playlist.filter(f));
            if (std::string(filter->get("mlt_service")) == "volume")
                sawVolume = true;
        }
    }
    CHECK(sawHidden);
    CHECK(sawVolume);
}

TEST_CASE("chunked playlists: verify() still finds a real mismatch inside a chunk")
{
    sharedFactoryPolicy();
    TestProject p = makeProject();
    ChunkSize size(4);
    EngineSync sync(p.model);
    const size_t knownProblems = sync.verify().size(); // the tone: blind spot above

    // Shorten the first clip inside V1's second chunk behind the model's
    // back: the graph no longer matches, and verify() must say so.
    int v1 = -1;
    for (int i = 1; i < sync.tractor().count(); ++i) {
        Mlt::Playlist playlist(*trackAt(sync.tractor(), i));
        if (playlist.get_int("hide") == 0 && playlist.filter_count() == 0) {
            v1 = i;
            break;
        }
    }
    REQUIRE(v1 > 0);
    Mlt::Playlist outer(*trackAt(sync.tractor(), v1));
    std::unique_ptr<Mlt::ClipInfo> info(outer.clip_info(1));
    REQUIRE(info);
    Mlt::Playlist chunk(*info->producer);
    int target = chunk.is_blank(0) ? 1 : 0;
    std::unique_ptr<Mlt::ClipInfo> clip(chunk.clip_info(target));
    REQUIRE(clip);
    chunk.resize_clip(target, clip->frame_in, clip->frame_out - 1);

    std::vector<std::string> problems = sync.verify();
    CHECK(problems.size() > knownProblems);
}

namespace {

// Paused and looping playback on V1 alone (chunked), watching the colour of
// each delivered frame. The clip at a frame is known from the model.
struct Watch
{
    std::mutex mutex;
    std::vector<std::pair<int, uint32_t>> frames; // position, 0xRRGGBB of the first pixel
};

uint32_t expectedColour(const Model &model, TrackId track, FrameIndex pos)
{
    for (const TrackSegment &seg : planTrackSegments(model, model.track(track))) {
        if (seg.kind == TrackSegment::Kind::Clip && pos >= seg.start && pos < seg.start + seg.length) {
            std::string path = model.asset(model.clip(seg.clip).asset).path; // color:#rrggbb
            return static_cast<uint32_t>(std::stoul(path.substr(7), nullptr, 16));
        }
    }
    return 0xFFFFFFFF; // gap or dissolve: not checked
}

bool close(uint32_t a, uint32_t b)
{
    for (int shift : {16, 8, 0}) {
        int d = int((a >> shift) & 0xFF) - int((b >> shift) & 0xFF);
        if (d > 16 || d < -16)
            return false;
    }
    return true;
}

// Frame positions where one chunk ends and the next begins on `track`.
std::vector<FrameIndex> chunkBoundaries(Mlt::Tractor &tractor, int mltIndex)
{
    std::vector<FrameIndex> starts;
    Mlt::Playlist outer(*trackAt(tractor, mltIndex));
    for (int i = 1; i < outer.count(); ++i)
        starts.push_back(outer.clip_start(i));
    return starts;
}

} // namespace

TEST_CASE("chunked playlists: paused seeks and steps show the exact frame at and across chunk boundaries")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    TrackId v1 = model.addTrack(Track::Kind::Video, 0, "V1");
    std::vector<AssetId> colours;
    for (const char *c : kColours)
        colours.push_back(addAsset(model, c, false));
    for (int c = 0; c < 40; ++c)
        model.insertClip(v1, colours[size_t(c) % colours.size()], c * 10, 0, 9);
    ChunkSize size(4);
    EngineSync sync(model);
    REQUIRE(isChunked(sync.tractor(), 1));
    std::vector<FrameIndex> boundaries = chunkBoundaries(sync.tractor(), 1);
    REQUIRE(boundaries.size() >= 5);

    PlaybackController controller;
    Watch watch;
    controller.setFrameCallback([&](std::vector<uint8_t> rgba, int, int, int position) {
        std::lock_guard<std::mutex> lock(watch.mutex);
        uint32_t rgb = rgba.size() >= 3 ? (uint32_t(rgba[0]) << 16) | (uint32_t(rgba[1]) << 8) | rgba[2] : 0;
        watch.frames.emplace_back(position, rgb);
    });
    controller.setTractor(sync.tractorPtr());
    pumpUntil(
        [&] {
            std::lock_guard<std::mutex> lock(watch.mutex);
            return !watch.frames.empty();
        },
        std::chrono::seconds(2));

    auto showsExactly = [&](FrameIndex pos) {
        bool got = pumpUntil(
            [&] {
                std::lock_guard<std::mutex> lock(watch.mutex);
                return !watch.frames.empty() && watch.frames.back().first == pos;
            },
            std::chrono::seconds(2));
        std::lock_guard<std::mutex> lock(watch.mutex);
        CAPTURE(pos);
        REQUIRE(got);
        CHECK(controller.currentFrame() == pos);
        CHECK(close(watch.frames.back().second, expectedColour(model, v1, pos)));
    };

    for (FrameIndex b : boundaries) {
        for (FrameIndex pos : {b - 1, b, b + 1}) {
            controller.seek(static_cast<int>(pos));
            showsExactly(pos);
        }
        // Step across the boundary from its last frame, back and forth.
        controller.seek(static_cast<int>(b - 1));
        showsExactly(b - 1);
        controller.stepFrame(1);
        showsExactly(b);
        controller.stepFrame(-1);
        showsExactly(b - 1);
    }
    controller.shutdown();
}

TEST_CASE("chunked playlists: a loop range across a chunk boundary wraps and plays both sides")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    TrackId v1 = model.addTrack(Track::Kind::Video, 0, "V1");
    std::vector<AssetId> colours;
    for (const char *c : kColours)
        colours.push_back(addAsset(model, c, false));
    for (int c = 0; c < 40; ++c)
        model.insertClip(v1, colours[size_t(c) % colours.size()], c * 10, 0, 9);
    ChunkSize size(4);
    EngineSync sync(model);
    FrameIndex b = chunkBoundaries(sync.tractor(), 1).at(1);

    PlaybackController controller;
    Watch watch;
    controller.setFrameCallback([&](std::vector<uint8_t>, int, int, int position) {
        std::lock_guard<std::mutex> lock(watch.mutex);
        watch.frames.emplace_back(position, 0);
    });
    controller.setTractor(sync.tractorPtr());
    controller.setLoopRange(std::make_pair(static_cast<int>(b - 5), static_cast<int>(b + 5)));
    controller.seek(static_cast<int>(b - 5));
    controller.play(1.0);
    bool wrapped = pumpUntil(
        [&] {
            std::lock_guard<std::mutex> lock(watch.mutex);
            for (size_t i = 1; i < watch.frames.size(); ++i)
                if (watch.frames[i].first < watch.frames[i - 1].first && watch.frames[i - 1].first >= b)
                    return true;
            return false;
        },
        std::chrono::seconds(5));
    controller.pause();
    std::lock_guard<std::mutex> lock(watch.mutex);
    if (!wrapped) {
        std::string seen;
        for (auto [position, colour] : watch.frames)
            seen += std::to_string(position) + " ";
        MESSAGE("loop " << b - 5 << ".." << b + 5 << ", delivered: " << seen);
    }
    REQUIRE(wrapped);
    bool before = false, after = false;
    for (auto [position, colour] : watch.frames) {
        (void)colour;
        if (position < 0)
            continue;
        before |= position < b;
        after |= position >= b;
        // Loop-out is b + 5; the same slack test_playback_controller's flat
        // loop test allows (loop-out + 5) for the wrap-seek landing late under
        // load -- 16-way stress overshot by 2.
        CHECK(position <= b + 10);
    }
    CHECK(before);
    CHECK(after);
    controller.shutdown();
}

TEST_CASE("chunked playlists: a render is frame-for-frame the same as the flat graph's")
{
    sharedFactoryPolicy();
    TestProject p = makeProject();
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / ("ustudio-chunk-render-" + std::to_string(getpid()));
    fs::create_directories(dir);

    auto renderAndHash = [&](size_t chunk, const std::string &name) {
        ChunkSize size(chunk);
        std::string error;
        fs::path out = dir / name;
        REQUIRE(renderProject(p.model, out.string(), error));
        Mlt::Profile profile;
        Mlt::Producer decoded(profile, out.string().c_str());
        REQUIRE(decoded.is_valid());
        std::vector<uint64_t> hashes;
        for (int pos = 0; pos < decoded.get_length(); ++pos) {
            decoded.seek(pos);
            std::unique_ptr<Mlt::Frame> frame(decoded.get_frame());
            mlt_image_format format = mlt_image_rgb;
            int w = 64, h = 36;
            const uint8_t *image = frame->get_image(format, w, h);
            hashes.push_back(fnv(image, size_t(w * h * 3)));
        }
        return hashes;
    };
    std::vector<uint64_t> flat = renderAndHash(kFlat, "flat.mp4");
    std::vector<uint64_t> chunked = renderAndHash(4, "chunked.mp4");
    CHECK(flat.size() > 300);
    CHECK(chunked == flat);
    fs::remove_all(dir);
}
