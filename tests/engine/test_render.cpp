#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine/engine_sync.h"
#include "engine/factory_policy.h"

#include <atomic>
#include <thread>
#include <chrono>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace ustudio::core;
using namespace ustudio::engine;

namespace {
namespace fs = std::filesystem;

// Shared across TEST_CASEs, matching FactoryPolicy's own documented
// "exactly one instance per process" contract (tests/engine/
// test_engine_sync.cpp established this pattern first; see its comment).
FactoryPolicy &sharedFactoryPolicy()
{
    static FactoryPolicy policy;
    return policy;
}

// RAII so a scratch output (and any stray "<path>.part") is removed even if
// a REQUIRE below throws (doctest unwinds failures via an exception).
struct RemoveOnExit
{
    fs::path path;
    ~RemoveOnExit()
    {
        std::error_code ec;
        fs::remove(path, ec);
        fs::remove(path.string() + ".part", ec);
    }
};

Model makeShortProject()
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset asset;
    asset.path = "color:red"; // generator, no file (doc 11: no binary media in tests)
    asset.displayName = "color:red";
    asset.info.hasVideo = true;
    asset.info.lengthInSequenceFrames = 100'000;
    AssetId assetId = model.addAsset(asset);
    model.insertClip(track, assetId, 0, 0, 4); // 5 frames -- fast to encode
    return model;
}
} // namespace

TEST_CASE("renderProject: a successful render lands at outputPath with no .part left behind (audit A6)")
{
    sharedFactoryPolicy();
    Model model = makeShortProject();

    fs::path outputPath = fs::temp_directory_path() / "ustudio-render-test-success.mp4";
    RemoveOnExit guard{outputPath};
    std::error_code ec;
    fs::remove(outputPath, ec);
    fs::remove(outputPath.string() + ".part", ec);

    std::string error;
    bool ok = renderProject(model, outputPath.string(), error);
    REQUIRE(ok);
    CHECK(error.empty());
    CHECK(fs::exists(outputPath));
    CHECK_FALSE(fs::exists(outputPath.string() + ".part"));
}

TEST_CASE("renderProject: an unopenable output path leaves no output or .part file (audit A6)")
{
    sharedFactoryPolicy();
    Model model = makeShortProject();

    // A directory that doesn't exist: avformat can't open anything under
    // it. Mlt::Consumer still reports itself valid, and consumer.run()
    // still returns 0 ("success") -- verified empirically, one more MLT
    // return value CLAUDE.md's empirical-knowledge rule says not to trust.
    // The rename step below is what actually catches this: it fails
    // because the .part file was never created, which is the real signal
    // that something went wrong.
    fs::path outputPath = fs::temp_directory_path() / "ustudio-render-test-nonexistent-dir" / "out.mp4";
    RemoveOnExit guard{outputPath};

    std::string error;
    bool ok = renderProject(model, outputPath.string(), error);
    CHECK_FALSE(ok);
    CHECK_FALSE(error.empty());
    CHECK_FALSE(fs::exists(outputPath));
    CHECK_FALSE(fs::exists(outputPath.string() + ".part"));
}

TEST_CASE("renderProject: onProgress fires with the tractor's real total length (enhancement #13)")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset asset;
    asset.path = "color:red"; // generator, no file (doc 11: no binary media in tests)
    asset.displayName = "color:red";
    asset.info.hasVideo = true;
    asset.info.lengthInSequenceFrames = 100'000;
    AssetId assetId = model.addAsset(asset);
    // A longer clip than makeShortProject()'s 5 frames -- still a trivial
    // generator, fast to encode, but long enough that a real render
    // touches more than a single frame, unlike the 5-frame clips above.
    model.insertClip(track, assetId, 0, 0, 59); // 60 frames

    fs::path outputPath = fs::temp_directory_path() / "ustudio-render-test-progress.mp4";
    RemoveOnExit guard{outputPath};
    std::error_code ec;
    fs::remove(outputPath, ec);
    fs::remove(outputPath.string() + ".part", ec);

    std::vector<std::pair<int, int>> calls; // (currentFrame, totalFrames)
    std::string error;
    bool ok = renderProject(model, outputPath.string(), error,
                            [&calls](int currentFrame, int totalFrames) { calls.emplace_back(currentFrame, totalFrames); });
    REQUIRE(ok);
    CHECK(error.empty());

    // The 500ms throttle (engine_sync.cpp's own comment) means a render
    // this short and simple isn't guaranteed to produce more than one
    // callback -- the FIRST frame-show always passes it (lastCall starts
    // at time_point::min()), so at least one is the real guarantee.
    REQUIRE(calls.size() >= 1);
    for (const auto &[currentFrame, totalFrames] : calls) {
        CHECK(totalFrames == 60);
        CHECK(currentFrame >= 0);
        CHECK(currentFrame < totalFrames);
    }
}

TEST_CASE("renderProject: a cancelled render stops promptly and leaves nothing behind (post-M3 audit P2)")
{
    sharedFactoryPolicy();
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset asset;
    asset.path = "noise:"; // costs real encode time, unlike a flat colour
    asset.displayName = "noise";
    asset.info.hasVideo = true;
    asset.info.lengthInSequenceFrames = 100'000;
    model.insertClip(track, model.addAsset(asset), 0, 0, 30 * 60 - 1); // a minute

    fs::path outputPath = fs::temp_directory_path() / "ustudio-render-test-cancel.mp4";
    RemoveOnExit guard{outputPath};
    std::atomic<bool> cancel{false};
    std::atomic<bool> started{false};
    std::string error;
    bool ok = true;
    auto begin = std::chrono::steady_clock::now();
    std::thread render(
        [&] { ok = renderProject(model, outputPath.string(), error, [&](int, int) { started = true; }, &cancel); });
    while (!started && std::chrono::steady_clock::now() - begin < std::chrono::seconds(20))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    cancel = true;
    auto cancelledAt = std::chrono::steady_clock::now();
    render.join();
    double stopMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cancelledAt).count();

    CHECK(started);
    CHECK_FALSE(ok);
    CHECK(error == "Render cancelled");
    CHECK(stopMs < 2000.0);
    CHECK_FALSE(fs::exists(outputPath));
    CHECK_FALSE(fs::exists(outputPath.string() + ".part"));
}
