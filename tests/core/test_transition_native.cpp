#include "doctest.h"

#include "core/model/model.h"
#include "core/model/transition_native.h"
#include "core/xml/reader.h"
#include "core/xml/writer.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>

using namespace ustudio::core;
namespace fs = std::filesystem;

namespace {

Transition transitionWith(std::vector<Param> params, FrameIndex length = 25)
{
    Transition t;
    t.service = "luma";
    t.length = length;
    t.params = std::move(params);
    return t;
}

std::string property(const NativeFilter &filter, const std::string &name)
{
    for (const auto &[key, value] : filter.properties)
        if (key == name)
            return value;
    return "<unset>";
}

// A folder of its own under the temp dir (maps and projects are written in it).
struct TempDir
{
    fs::path path;
    TempDir()
    {
        std::random_device rd;
        path = fs::temp_directory_path() / ("ustudio-transition-test-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// A model with one dissolve between two clips, and its id.
std::pair<Model, TransitionId> modelWithDissolve()
{
    Model model = Model::createEmpty();
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    Asset asset;
    asset.path = "color:red";
    asset.displayName = "red";
    asset.info.hasVideo = true;
    asset.info.lengthInSequenceFrames = 100'000;
    AssetId id = model.addAsset(asset);
    ClipId a = model.insertClip(track, id, 0, 0, 49);
    ClipId b = model.insertClip(track, id, 50, 10, 59);
    TransitionId t = model.addTransition(track, a, b, 5, 5);
    return {std::move(model), t};
}

} // namespace

TEST_CASE("nativeTransition: empty params are the plain dissolve")
{
    const NativeTransition native = nativeTransition(transitionWith({}));
    CHECK(native.video.service == "luma");
    CHECK(native.video.properties.empty());
    CHECK(native.audio.service == "mix");
    CHECK(property(native.audio, "start") == "-1");
    CHECK(native.tailFilters.empty());
    CHECK(native.headFilters.empty());
    CHECK(native.luma.empty());
}

TEST_CASE("nativeTransition: a wipe's video props, map name and a dip's filters in index order")
{
    const NativeTransition native = nativeTransition(transitionWith({
        {"video.service", std::string("luma"), {}},
        {"video.luma", std::string("radial"), {}},
        {"video.softness", 0.25, {}},
        {"a.1.service", std::string("volume"), {}},
        {"a.0.service", std::string("brightness"), {}},
        {"a.0.level", std::string("ramp:1,0,0"), {}},
        {"b.0.service", std::string("brightness"), {}},
        {"b.0.rgb_only", int64_t{1}, {}},
        {"b.2.level", std::string("0.5"), {}}, // no service: dropped
    }));
    CHECK(native.luma == "radial");
    CHECK(property(native.video, "softness") == "0.25");
    CHECK(property(native.video, "luma") == "<unset>");
    REQUIRE(native.tailFilters.size() == 2);
    CHECK(native.tailFilters[0].service == "brightness");
    CHECK(native.tailFilters[1].service == "volume");
    // 25 frames: points at 0, 12, 24.
    CHECK(property(native.tailFilters[0], "level") == "0=1;12=0;24=0");
    REQUIRE(native.headFilters.size() == 1);
    CHECK(property(native.headFilters[0], "rgb_only") == "1");
}

TEST_CASE("nativeTransition: a ramp follows the transition's length")
{
    const std::vector<Param> params{{"a.0.service", std::string("brightness"), {}},
                                    {"a.0.level", std::string("ramp:1,0"), {}}};
    CHECK(property(nativeTransition(transitionWith(params, 10)).tailFilters[0], "level") == "0=1;9=0");
    CHECK(property(nativeTransition(transitionWith(params, 40)).tailFilters[0], "level") == "0=1;39=0");
}

TEST_CASE("transitionProblem: only allowed services, and never a file property")
{
    CHECK(transitionProblem(transitionWith({})).empty());
    CHECK(transitionProblem(transitionWith({{"video.luma", std::string("no-such-map"), {}}})).empty());
    CHECK_FALSE(transitionProblem(transitionWith({{"video.service", std::string("qtblend"), {}}})).empty());
    CHECK_FALSE(transitionProblem(transitionWith({{"audio.service", std::string("ladspa"), {}}})).empty());
    CHECK_FALSE(transitionProblem(transitionWith({{"b.0.service", std::string("frei0r.glow"), {}}})).empty());
    CHECK_FALSE(transitionProblem(transitionWith({{"video.resource", std::string("/etc/passwd"), {}}})).empty());
    CHECK_FALSE(transitionProblem(transitionWith({{"video.producer.resource", std::string("x"), {}}})).empty());
    CHECK_FALSE(transitionProblem(transitionWith({{"video.factory", std::string("loader"), {}}})).empty());
    Transition hostile = transitionWith({});
    hostile.service = "qtblend";
    CHECK_FALSE(transitionProblem(hostile).empty());
}

TEST_CASE("Model::check() and the reader refuse a transition naming a service outside the allowlist")
{
    auto [model, id] = modelWithDissolve();
    REQUIRE(model.check().empty());
    model.setTransitionRecipe(id, "evil", {{"video.service", std::string("glaxnimate"), {}}});
    REQUIRE(model.check().size() == 1);
    CHECK(model.check()[0].find("not allowed") != std::string::npos);

    // A hand-edited file saying the same doesn't open.
    TempDir dir;
    const fs::path path = dir.path / "hostile.ustudio";
    REQUIRE(saveProject(model, path.string()).empty());
    auto loaded = loadProject(path.string());
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().find("not allowed") != std::string::npos);
}

TEST_CASE("XML round-trip: a wipe's recipe survives, and its map is written beside the project")
{
    auto [model, id] = modelWithDissolve();
    model.setTransitionRecipe(id, "wipe.star",
                              {{"video.service", std::string("luma"), {}},
                               {"video.luma", std::string("star"), {}},
                               {"video.softness", 0.1, {}}});
    TempDir dir;
    const fs::path path = dir.path / "wipe.ustudio";
    REQUIRE(saveProject(model, path.string()).empty());
    auto loaded = loadProject(path.string());
    REQUIRE(loaded.has_value());
    CHECK(*loaded == model);
    CHECK(fs::is_regular_file(dir.path / "ustudio-wipes" / "star.pgm"));

    // Named relative to the project, so the folder can move.
    std::ifstream in(path);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find(">ustudio-wipes/star.pgm<") != std::string::npos);
}

TEST_CASE("writeLumaMap: a 16-bit PGM of every named map, the same every time")
{
    TempDir dir;
    CHECK(lumaMapNames().size() == 20);
    for (const std::string &name : lumaMapNames()) {
        const fs::path file = dir.path / (name + ".pgm");
        REQUIRE(writeLumaMap(name, file, 64, 36));
        std::ifstream in(file, std::ios::binary);
        const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::string header = "P5\n64 36\n65535\n";
        REQUIRE(data.starts_with(header));
        CHECK(data.size() == header.size() + 64 * 36 * 2);
    }
    // Deterministic ("blocks" is seeded): the same bytes on a second write.
    const fs::path first = dir.path / "blocks.pgm", second = dir.path / "again" / "blocks.pgm";
    REQUIRE(writeLumaMap("blocks", second, 64, 36));
    std::ifstream a(first, std::ios::binary), b(second, std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(a), {}) == std::string(std::istreambuf_iterator<char>(b), {}));
    // "left" starts dark on the left (the incoming clip appears there first).
    std::ifstream left(dir.path / "left.pgm", std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(left)), std::istreambuf_iterator<char>());
    const size_t pixels = std::string("P5\n64 36\n65535\n").size();
    CHECK(static_cast<unsigned char>(data[pixels]) < static_cast<unsigned char>(data[pixels + 62 * 2]));
    CHECK_FALSE(writeLumaMap("no-such-map", dir.path / "x.pgm"));
    CHECK_FALSE(fs::exists(dir.path / "x.pgm"));
}
