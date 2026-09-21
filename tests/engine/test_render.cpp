#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine/engine_sync.h"
#include "engine/factory_policy.h"

#include <filesystem>
#include <string>

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
