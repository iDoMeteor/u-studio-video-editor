#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/xml/writer.h"
#include "engine/factory_policy.h"

#include <mlt++/Mlt.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <random>

using namespace ustudio::core;
using namespace ustudio::engine;

namespace {
// RAII so the temp file is removed even if a REQUIRE below throws --
// doctest's REQUIRE failure unwinds via an exception, which would
// otherwise skip a plain std::remove() at the end of the test body.
struct RemoveOnExit
{
    std::filesystem::path path;
    ~RemoveOnExit()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};
} // namespace

// doc 12's M1 acceptance: "melt saved.ustudio (or u-studio-render) plays
// the saved file with no editor". `melt` itself isn't installed on this
// machine, so this exercises the same path melt uses internally: MLT's
// own "xml" producer service loading our writer's output, completely
// independent of core/xml's reader (which is OUR parser, not MLT's).
//
// v1's README documents a real, empirically-confirmed MLT quirk here:
// reloading XML gives back a plain producer (Service::type() ==
// mlt_service_producer_type), not a tractor -- wrapping it as
// Mlt::Tractor to keep editing it fails. That's irrelevant to THIS
// criterion, though: melt only needs to PLAY the file, not re-edit it as
// a C++ Tractor object (doc 09: our reader reconstructs the editable
// model from ustudio:* properties directly; the MLT structure is output,
// never read back in as input). Confirmed below: is_valid() and
// get_frame() both work on the wrapped producer regardless of its
// reported service type.
TEST_CASE("A file our writer saves plays via MLT's own xml producer, independent of our reader")
{
    FactoryPolicy policy;

    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");

    Asset asset;
    asset.path = "color:red";
    asset.info.hasVideo = true;
    asset.info.lengthInSequenceFrames = 100;
    AssetId assetId = model.addAsset(asset);
    model.insertClip(track, assetId, 0, 0, 49);

    // A random suffix, not a fixed name: two concurrent test runs (agent
    // worktrees, or a local run racing CI) could otherwise collide on the
    // same path mid-write (CLAUDE.md's concurrent-session hazard).
    std::random_device rd;
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("ustudio-xml-playback-test-" + std::to_string(rd()) + ".ustudio");
    RemoveOnExit cleanup{path};
    REQUIRE(saveProject(model, path.string()).empty());

    Mlt::Profile profile;
    Mlt::Producer loaded(profile, ("xml:" + path.string()).c_str());
    REQUIRE(loaded.is_valid());
    CHECK(loaded.get_length() == 50);

    std::unique_ptr<Mlt::Frame> frame(loaded.get_frame());
    REQUIRE(frame != nullptr);
    CHECK(frame->is_valid());
}
