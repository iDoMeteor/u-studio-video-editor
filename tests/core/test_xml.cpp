#include "doctest.h"

#include "core/xml/reader.h"
#include "core/xml/writer.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

using namespace ustudio::core;

namespace {

namespace fs = std::filesystem;

// A unique path per test case under the scratch build directory -- never
// touches real project files, cleaned up at the end of each test.
struct TempProjectFile
{
    fs::path path;

    explicit TempProjectFile(const std::string &name)
        : path(fs::temp_directory_path() / ("ustudio-xml-test-" + name + ".ustudio"))
    {}
    ~TempProjectFile()
    {
        std::remove(path.string().c_str());
    }
};

AssetId addTestAsset(Model &model, const std::string &path, FrameIndex lengthInFrames = 100'000)
{
    Asset asset;
    asset.path = path;
    asset.displayName = "clip";
    asset.folder = "/b-roll";
    asset.fileFingerprint = "12345:6789";
    asset.info.hasVideo = true;
    asset.info.hasAudio = true;
    asset.info.width = 1920;
    asset.info.height = 1080;
    asset.info.fps = {30, 1};
    asset.info.sampleRate = 48000;
    asset.info.audioChannels = 2;
    asset.info.lengthInSequenceFrames = lengthInFrames;
    asset.info.videoCodec = "h264";
    asset.info.audioCodec = "aac";
    asset.info.container = "mp4";
    asset.status = Asset::Status::Ready;
    return model.addAsset(asset);
}

} // namespace

TEST_CASE("XML round-trip: empty project")
{
    TempProjectFile file("empty");
    Model model = Model::createEmpty();

    std::string error = saveProject(model, file.path.string());
    CHECK(error.empty());

    auto loaded = loadProject(file.path.string());
    REQUIRE(loaded.has_value());
    CHECK(*loaded == model);
}

TEST_CASE("XML round-trip: tracks, clips, and a still-image-style asset")
{
    TempProjectFile file("basic");
    Model model = Model::createEmpty();

    TrackId video = model.addTrack(Track::Kind::Video, 0, "V1");
    TrackId audio = model.addTrack(Track::Kind::Audio, 1, "A1");
    AssetId asset = addTestAsset(model, "/home/user/videos/clip.mp4");

    model.insertClip(video, asset, 0, 0, 99);
    model.insertClip(video, asset, 150, 10, 59);    // gap between the two clips
    model.setTrackFlags(video, false, false, true); // locked
    model.setTrackFlags(audio, true, false, false); // muted; left with no clips -- Model has no
                                                    // mutator yet to flip videoEnabled=false for a
                                                    // clip (invariant 8), that's SplitAudio's job
                                                    // once it lands; this still exercises track-level
                                                    // (kind/flags/name) round-trip on an empty track

    std::string error = saveProject(model, file.path.string());
    REQUIRE(error.empty());

    auto loaded = loadProject(file.path.string());
    REQUIRE(loaded.has_value());
    CHECK(loaded->check().empty());
    CHECK(*loaded == model);
}

TEST_CASE("XML round-trip: relative asset path under the project directory")
{
    TempProjectFile file("relpath");
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");

    // Under the same directory as the project file -- writer.cpp should
    // store this relative to that directory, reader.cpp should resolve it
    // back to the exact same absolute path.
    fs::path assetPath = file.path.parent_path() / "media" / "clip.mp4";
    AssetId asset = addTestAsset(model, assetPath.string());
    model.insertClip(track, asset, 0, 0, 49);

    REQUIRE(saveProject(model, file.path.string()).empty());
    auto loaded = loadProject(file.path.string());
    REQUIRE(loaded.has_value());
    CHECK(loaded->asset(asset).path == assetPath.string());
    CHECK(*loaded == model);
}

TEST_CASE("XML round-trip: generator-shorthand resource is preserved verbatim")
{
    TempProjectFile file("generator");
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model, "color:red");
    model.insertClip(track, asset, 0, 0, 49);

    REQUIRE(saveProject(model, file.path.string()).empty());
    auto loaded = loadProject(file.path.string());
    REQUIRE(loaded.has_value());
    CHECK(loaded->asset(asset).path == "color:red");
}

TEST_CASE("XML round-trip: markers and settings")
{
    TempProjectFile file("markers");
    Model model = Model::createEmpty();
    Sequence &seq = model.mutableSequence();
    seq.markers.push_back(Marker{MarkerId{1}, 42, "a \"quoted\" marker\nwith a newline", 3});
    seq.markers.push_back(Marker{MarkerId{2}, 100, "second", 1});

    // Settings live on Project, not Sequence -- go through a fresh Model
    // built directly so both markers and settings are present together.
    Project project = model.project();
    project.settings["preview_scale"] = "0.5";
    project.settings["theme"] = "dark";
    Model withSettings(project);

    REQUIRE(saveProject(withSettings, file.path.string()).empty());
    auto loaded = loadProject(file.path.string());
    REQUIRE(loaded.has_value());
    CHECK(*loaded == withSettings);
    REQUIRE(loaded->sequence().markers.size() == 2);
    CHECK(loaded->sequence().markers[0].text == "a \"quoted\" marker\nwith a newline");
}

TEST_CASE("XML round-trip: track visual order survives even though MLT order differs")
{
    TempProjectFile file("order");
    Model model = Model::createEmpty();
    // Visual (model) order: topVideo, bottomVideo, audio -- MLT order is
    // audio, bottomVideo, topVideo, so this specifically exercises the
    // ustudio:visual_index property, not just accidental agreement.
    model.addTrack(Track::Kind::Video, 0, "V-top");
    model.addTrack(Track::Kind::Video, 1, "V-bottom");
    model.addTrack(Track::Kind::Audio, 2, "A1");

    REQUIRE(saveProject(model, file.path.string()).empty());
    auto loaded = loadProject(file.path.string());
    REQUIRE(loaded.has_value());

    REQUIRE(loaded->sequence().tracks.size() == 3);
    CHECK(loaded->sequence().tracks[0].name == "V-top");
    CHECK(loaded->sequence().tracks[1].name == "V-bottom");
    CHECK(loaded->sequence().tracks[2].name == "A1");
    CHECK(*loaded == model);
}

TEST_CASE("XML round-trip: refuses a file with no ustudio:format_version")
{
    TempProjectFile file("not-ustudio");
    {
        std::ofstream out(file.path);
        out << "<mlt><tractor><track producer=\"black\"/></tractor></mlt>";
    }

    auto loaded = loadProject(file.path.string());
    CHECK_FALSE(loaded.has_value());
}

TEST_CASE("XML round-trip: save does not touch the target file on a bad path")
{
    Model model = Model::createEmpty();
    std::string error = saveProject(model, "/nonexistent-directory-for-this-test/project.ustudio");
    CHECK_FALSE(error.empty());
}

// Same random-edit generator as the other property tests, this time
// checked against a save/open round trip every 50 edits -- the
// combination doc 12's M1 acceptance criteria actually asks for
// ("save -> quit -> open restores the timeline identically").
TEST_CASE("XML property: save/open round trip stays exact across 300 random edits")
{
    TempProjectFile file("property");
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    AssetId asset = addTestAsset(model, "/media/generated.mp4", 1'000'000);

    std::mt19937 rng(99);
    std::vector<ClipId> liveClips;
    FrameIndex nextFreePosition = 0;

    for (int i = 0; i < 300; ++i) {
        std::uniform_int_distribution<int> pickAction(0, liveClips.empty() ? 0 : 2);
        int action = pickAction(rng);

        if (action == 0 || liveClips.empty()) {
            FrameIndex length = 10 + static_cast<FrameIndex>(rng() % 90);
            ClipId clip = model.insertClip(track, asset, nextFreePosition, 0, length - 1);
            nextFreePosition += length;
            liveClips.push_back(clip);
        } else if (action == 1) {
            std::uniform_int_distribution<size_t> pickClip(0, liveClips.size() - 1);
            size_t index = pickClip(rng);
            model.removeClip(liveClips[index]);
            liveClips.erase(liveClips.begin() + static_cast<std::ptrdiff_t>(index));
        } else {
            std::uniform_int_distribution<size_t> pickClip(0, liveClips.size() - 1);
            ClipId clip = liveClips[pickClip(rng)];
            const Clip &current = model.clip(clip);
            if (current.length() > 2) {
                FrameIndex at = current.position + 1 + static_cast<FrameIndex>(rng() % (current.length() - 2));
                liveClips.push_back(model.splitClip(clip, at));
            }
        }

        if (i % 50 != 49)
            continue;

        REQUIRE(saveProject(model, file.path.string()).empty());
        auto loaded = loadProject(file.path.string());
        REQUIRE(loaded.has_value());
        INFO("iteration ", i);
        REQUIRE(*loaded == model);
        REQUIRE(loaded->check().empty());
    }
}
