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
    // affine's filter opens its background as a producer, and passes
    // producer.* and transition.* on: refused on the cuts and at any depth.
    CHECK_FALSE(transitionProblem(transitionWith({{"a.0.service", std::string("affine"), {}},
                                                  {"a.0.background", std::string("/etc/passwd"), {}}}))
                    .empty());
    CHECK_FALSE(transitionProblem(transitionWith({{"b.0.service", std::string("affine"), {}},
                                                  {"b.0.producer.resource", std::string("x.mp4"), {}}}))
                    .empty());
    CHECK_FALSE(transitionProblem(transitionWith({{"b.0.service", std::string("affine"), {}},
                                                  {"b.0.transition.producer.resource", std::string("x"), {}}}))
                    .empty());
    CHECK_FALSE(transitionProblem(transitionWith({{"audio.resource", std::string("x.wav"), {}}})).empty());
    // A motion recipe's geometry is fine.
    CHECK(transitionProblem(transitionWith({{"a.0.service", std::string("affine"), {}},
                                            {"a.0.transition.rect", std::string("ramp:0% 0% 100% 100%|-100% 0% 100% 100%"), {}}}))
              .empty());
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
    CHECK(fs::is_regular_file(dir.path / "ustudio-wipes" / "v1" / "star.pgm"));

    // Named relative to the project, so the folder can move.
    std::ifstream in(path);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find(">ustudio-wipes/v1/star.pgm<") != std::string::npos);
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

TEST_CASE("lumaMapPath: the generator version is in the path, so a newer generator never reuses an old map")
{
    CHECK(lumaMapPath("/cache/luma", "radial") ==
          fs::path("/cache/luma") / ("v" + std::to_string(kLumaMapVersion)) / "radial.pgm");
    // An existing file at that path is kept as is (the cache hit)...
    TempDir dir;
    const fs::path file = lumaMapPath(dir.path, "left");
    fs::create_directories(file.parent_path());
    std::ofstream(file) << "stale";
    REQUIRE(writeLumaMap("left", file, 8, 8));
    CHECK(fs::file_size(file) == 5);
    // ...so a changed generator must land at another path: a bumped version.
    CHECK(file.parent_path().filename() == "v" + std::to_string(kLumaMapVersion));
    // No temp file is left behind by a write.
    REQUIRE(writeLumaMap("right", lumaMapPath(dir.path, "right"), 8, 8));
    for (const auto &entry : fs::directory_iterator(file.parent_path()))
        CHECK(entry.path().extension() == ".pgm");
}

// FX5 leftover: a file inside the project's folder (a LUT) is saved
// relative to it and found again after the folder moves; the render graph
// keeps the absolute path for melt.
TEST_CASE("XML: an effect's file inside the project's folder follows the project when it moves")
{
    auto [model, id] = modelWithDissolve();
    TempDir dir;
    fs::create_directories(dir.path / "luts");
    std::ofstream(dir.path / "luts" / "grade.cube") << "LUT_3D_SIZE 2\n";
    Effect lut;
    lut.service = "avfilter.lut3d";
    lut.owner = "effects";
    lut.params = {{"av.file", (dir.path / "luts" / "grade.cube").string(), {}}};
    const ClipId clip = model.sequence().tracks[0].clips[0];
    model.addEffect(Model::EffectTarget::clip(clip), lut, 0);
    const fs::path path = dir.path / "moving.ustudio";
    REQUIRE(saveProject(model, path.string()).empty());
    std::ifstream in(path);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find(">f:luts/grade.cube<") != std::string::npos);                          // the model's record
    // An older build would read "f:" as a number and lose the file: format 7.
    CHECK(text.find("ustudio:format_version\">7<") != std::string::npos);
    CHECK(text.find(">" + (dir.path / "luts" / "grade.cube").string() + "<") != std::string::npos); // the render filter

    // The whole folder moved: the LUT is found where it is now.
    TempDir moved;
    fs::copy(dir.path, moved.path, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    auto loaded = loadProject((moved.path / "moving.ustudio").string());
    REQUIRE(loaded.has_value());
    const Effect &back = loaded->clip(clip).effects[0];
    REQUIRE(back.params.size() == 1);
    CHECK(std::get<std::string>(back.params[0].value) == (moved.path / "luts" / "grade.cube").string());
    // A path outside the folder stays absolute.
    auto [other, otherId] = modelWithDissolve();
    lut.params = {{"av.file", std::string("/opt/shared/grade.cube"), {}}};
    other.addEffect(Model::EffectTarget::clip(other.sequence().tracks[0].clips[0]), lut, 0);
    REQUIRE(saveProject(other, path.string()).empty());
    std::ifstream again(path);
    const std::string text2((std::istreambuf_iterator<char>(again)), std::istreambuf_iterator<char>());
    CHECK(text2.find(">s:/opt/shared/grade.cube<") != std::string::npos);
    CHECK(text2.find("ustudio:format_version\">6<") != std::string::npos); // nothing an older build loses
}
