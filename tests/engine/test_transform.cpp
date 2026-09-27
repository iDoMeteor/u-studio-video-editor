// ADR-018 (M4 F1): clip transforms in the engine. The picture is placed
// where the model says, the tracks under it show around it (alpha), a
// saved project plays the same in melt and renders the same, dissolves
// still work, and a drag changes the live filters without a rebuild or a
// consumer restart.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/commands/primitives.h"
#include "core/commands/undo_stack.h"
#include "core/media/utf8_path.h"
#include "core/model/transform.h"
#include "core/xml/reader.h"
#include "core/xml/writer.h"
#include "engine/engine.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"
#include "platform/process.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>

using namespace ustudio;
using namespace ustudio::core;
using namespace ustudio::engine;
namespace fs = std::filesystem;

namespace {

FactoryPolicy &sharedFactoryPolicy()
{
    static FactoryPolicy policy;
    return policy;
}

fs::path scratch()
{
    static const fs::path dir =
        fs::temp_directory_path() / ("ustudio-transform-" + std::to_string(platform::currentProcessId()));
    fs::create_directories(dir);
    return dir;
}

// A generated file: `left` over the whole frame, and `right` over its right
// half when given (to see flips and crops).
fs::path generate(const std::string &name, int width, int height, const char *left, const char *right = nullptr)
{
    const fs::path path = scratch() / name;
    if (fs::exists(path))
        return path;
    Profile profile;
    profile.width = width;
    profile.height = height;
    Model model = Model::createEmpty(profile);
    EngineSync sync(model);
    Mlt::Profile &p = sync.profile();
    Mlt::Tractor tractor(p);
    Mlt::Producer base(p, left);
    base.set_in_and_out(0, 59);
    tractor.set_track(base, 0);
    std::unique_ptr<Mlt::Producer> half;
    if (right) {
        half = std::make_unique<Mlt::Producer>(p, right);
        half->set_in_and_out(0, 59);
        tractor.set_track(*half, 1);
        Mlt::Transition composite(p, "composite");
        composite.set(
            "geometry",
            (std::to_string(width / 2) + "/0:" + std::to_string(width / 2) + "x" + std::to_string(height)).c_str());
        composite.set("distort", 1);
        std::unique_ptr<Mlt::Field> field(tractor.field());
        field->plant_transition(composite, 0, 1);
    }
    Mlt::Consumer consumer(p, "avformat", utf8String(path).c_str());
    consumer.set("vcodec", h264Encoder().c_str());
    consumer.set("crf", 10);
    consumer.set("real_time", -1);
    consumer.connect(tractor);
    consumer.run();
    return path;
}

struct Rgb
{
    int r, g, b;
    bool red() const
    {
        return r > 180 && g < 70 && b < 70;
    }
    bool green() const
    {
        return g > 150 && r < 90 && b < 90;
    }
    bool blue() const
    {
        return b > 150 && r < 90 && g < 90;
    }
};

// (x, y) of a 192x108 frame, read from one at the profile's size (1080p
// here): that is what the consumers ask for, and a transformed cut's affine
// filter (use_normalized) always returns the profile's size, so a luma
// asked for a smaller frame gets two sizes and doesn't mix.
Rgb pixelAt(Mlt::Producer &producer, int position, int x, int y)
{
    producer.seek(position);
    std::unique_ptr<Mlt::Frame> frame(producer.get_frame());
    mlt_image_format format = mlt_image_rgb;
    int w = 1920, h = 1080;
    const uint8_t *image = frame->get_image(format, w, h);
    const uint8_t *px = image + (static_cast<size_t>(y * 10 + 5) * 1920 + static_cast<size_t>(x * 10 + 5)) * 3;
    return {px[0], px[1], px[2]};
}

Rgb pixelAt(EngineSync &sync, int position, int x, int y)
{
    return pixelAt(sync.tractor(), position, x, y);
}

// A red lower track and, above it, `media` for 60 frames.
struct Scene
{
    Model model = Model::createEmpty();
    TrackId upper, lower;
    ClipId clip;
    AssetId asset;

    Scene(const fs::path &media, int width, int height)
    {
        lower = model.addTrack(Track::Kind::Video, 0, "V1");
        upper = model.addTrack(Track::Kind::Video, 0, "V2"); // index 0: the top
        Asset red;
        red.path = "color:red";
        red.info.hasVideo = true;
        red.info.lengthInSequenceFrames = 10'000;
        model.insertClip(lower, model.addAsset(red), 0, 0, 59);
        Asset source;
        source.path = utf8String(media);
        source.info.hasVideo = true;
        source.info.width = width;
        source.info.height = height;
        source.info.lengthInSequenceFrames = 60;
        asset = model.addAsset(source);
        clip = model.insertClip(upper, asset, 0, 0, 59);
    }
};

Transform placed(double cx, double cy, double w, double h, double rotation = 0)
{
    Transform t;
    t.bounds = Transform::Bounds::None;
    t.x.value = cx;
    t.y.value = cy;
    t.width.value = w;
    t.height.value = h;
    t.rotation.value = rotation;
    return t;
}

} // namespace

TEST_CASE("transform: a 1344x768 clip in a 1080p project is fitted and centred by default")
{
    sharedFactoryPolicy();
    Scene scene(generate("blue-1344.mp4", 1344, 768, "color:#0000c0"), 1344, 768);
    EngineSync sync(scene.model);
    CHECK(sync.verify().empty());
    // 1890x1080 at x=15: the red track shows in the 15 px bars either side.
    CHECK(pixelAt(sync, 5, 0, 54).red()); // 0-10 px of the 15 px bar
    CHECK(pixelAt(sync, 5, 191, 54).red());
    CHECK(pixelAt(sync, 5, 96, 54).blue());
    CHECK(pixelAt(sync, 5, 96, 2).blue()); // full height
}

TEST_CASE("transform: read at the profile's size, the compositor fits another aspect without affine")
{
    sharedFactoryPolicy();
    Scene scene(generate("blue-1344.mp4", 1344, 768, "color:#0000c0"), 1344, 768);
    auto fitted = [](auto &&at) {
        CHECK(at(5, 0, 54).red()); // the 15 px bars either side of 1890x1080
        CHECK(at(5, 191, 54).red());
        CHECK(at(5, 96, 54).blue());
        CHECK(at(5, 96, 2).blue());
    };
    {
        EngineSync sync(scene.model, PreviewScale::Full, EngineSync::FrameReads::ProfileSize);
        CHECK(sync.verify().empty());
        fitted([&](int position, int x, int y) { return pixelAt(sync, position, x, y); });
        // No affine filter on the cut: moving it has one to add, so rebuilds.
        int rebuilds = 0, inPlace = 0;
        sync.rebuilt.connect([&] { ++rebuilds; });
        sync.appliedInPlace.connect([&] { ++inPlace; });
        scene.model.setClipTransform(scene.clip, placed(480, 270, 960, 540));
        sync.setProject(scene.model.snapshot());
        CHECK(rebuilds == 1);
        CHECK(inPlace == 0);
        CHECK(pixelAt(sync, 5, 48, 27).blue());
        CHECK(pixelAt(sync, 5, 150, 80).red());
        scene.model.setClipTransform(scene.clip, Transform{});
    }

    // Exports take the same path, at another size too: the graph is built
    // at the export's size (read smaller, the picture ran to the right edge).
    for (int height : {0, 720}) {
        INFO("export height " << height);
        RenderProfile profile = legacyRenderProfile();
        profile.height = height;
        const fs::path out = scratch() / ("fit-" + std::to_string(height) + ".mp4");
        std::string error;
        REQUIRE(renderProject(scene.model, utf8String(out), error, {}, nullptr, profile));
        Mlt::Profile decodeProfile("atsc_1080p_30");
        Mlt::Producer rendered(decodeProfile, utf8String(out).c_str());
        REQUIRE(rendered.is_valid());
        fitted([&](int position, int x, int y) { return pixelAt(rendered, position, x, y); });
    }

    // Into a dissolve the cut keeps its affine filter: luma mixes both
    // pictures at one size, so the incoming 1080p one stays unsqueezed.
    scene.model.resizeClip(scene.clip, 0, 39, 0);
    Asset green;
    green.path = utf8String(generate("green-1080.mp4", 1920, 1080, "color:#00c000"));
    green.info.hasVideo = true;
    green.info.width = 1920;
    green.info.height = 1080;
    green.info.lengthInSequenceFrames = 60;
    const ClipId next = scene.model.insertClip(scene.upper, scene.model.addAsset(green), 40, 10, 49);
    REQUIRE(scene.model.addTransition(scene.upper, scene.clip, next, 10, 10).value != 0);
    EngineSync sync(scene.model, PreviewScale::Full, EngineSync::FrameReads::ProfileSize);
    CHECK(sync.verify().empty());
    fitted([&](int position, int x, int y) { return pixelAt(sync, position, x, y); });
    const Rgb mid = pixelAt(sync, 40, 96, 54);
    CHECK(mid.b > 40);
    CHECK(mid.g > 40);
    const Rgb edge = pixelAt(sync, 40, 0, 54); // the bar: red fading into green
    CHECK(edge.r > 40);
    CHECK(edge.g > 40);
    CHECK(pixelAt(sync, 70, 0, 54).green());
}

TEST_CASE("transform: move, scale and rotate place the picture, with the lower track around it")
{
    sharedFactoryPolicy();
    Scene scene(generate("blue-1080.mp4", 1920, 1080, "color:#0000c0"), 1920, 1080);
    // A quarter-size picture centred at (480, 270): the frame's top-left quarter.
    scene.model.setClipTransform(scene.clip, placed(480, 270, 960, 540));
    {
        EngineSync sync(scene.model);
        CHECK(pixelAt(sync, 5, 48, 27).blue());
        CHECK(pixelAt(sync, 5, 150, 80).red());
        CHECK(pixelAt(sync, 5, 100, 20).red());
    }
    // Full frame, rotated 45 degrees about the centre: the corners show red.
    scene.model.setClipTransform(scene.clip, placed(960, 540, 1920, 1080, 45));
    EngineSync sync(scene.model);
    CHECK(pixelAt(sync, 5, 96, 54).blue());
    CHECK(pixelAt(sync, 5, 2, 2).red());
    CHECK(pixelAt(sync, 5, 189, 105).red());
}

TEST_CASE("transform: flip and crop act on the source picture")
{
    sharedFactoryPolicy();
    // Left half green, right half blue.
    const fs::path halves = generate("halves.mp4", 1920, 1080, "color:#00c000", "color:#0000c0");
    Scene scene(halves, 1920, 1080);
    {
        EngineSync sync(scene.model); // identity: no filters
        CHECK(pixelAt(sync, 5, 30, 54).green());
        CHECK(pixelAt(sync, 5, 160, 54).blue());
    }
    Transform flipped;
    flipped.flipH = true;
    scene.model.setClipTransform(scene.clip, flipped);
    {
        EngineSync sync(scene.model);
        CHECK(pixelAt(sync, 5, 30, 54).blue());
        CHECK(pixelAt(sync, 5, 160, 54).green());
    }
    // Crop the green half away: the blue half (960x1080) is fitted, centred.
    Transform cropped;
    cropped.cropLeft.value = 960;
    scene.model.setClipTransform(scene.clip, cropped);
    EngineSync sync(scene.model);
    CHECK(pixelAt(sync, 5, 96, 54).blue());
    CHECK(pixelAt(sync, 5, 20, 54).red()); // pillarboxed: the lower track shows
    CHECK(pixelAt(sync, 5, 172, 54).red());
}

TEST_CASE("transform: the saved project plays and renders exactly as the editor shows it")
{
    sharedFactoryPolicy();
    Scene scene(generate("halves.mp4", 1920, 1080, "color:#00c000", "color:#0000c0"), 1920, 1080);
    Transform t = placed(800, 500, 1100, 620, 20);
    t.cropTop.value = 100;
    t.flipH = true;
    scene.model.setClipTransform(scene.clip, t);
    const fs::path project = scratch() / "transformed.ustudio";
    REQUIRE(saveProject(scene.model, utf8String(project)).empty());
    auto loaded = loadProject(utf8String(project));
    REQUIRE(loaded.has_value());
    CHECK(loaded->clip(scene.clip).transform.get() == t);

    EngineSync sync(scene.model);
    Mlt::Profile &profile = sync.profile();
    Mlt::Producer melt(profile, "xml", utf8String(project).c_str());
    REQUIRE(melt.is_valid());
    // Lossless export of the saved graph, decoded back.
    const fs::path exported = scratch() / "export.mkv";
    {
        Mlt::Consumer consumer(profile, "avformat", utf8String(exported).c_str());
        consumer.set("vcodec", "ffv1");
        consumer.set("pix_fmt", "bgr0");
        consumer.set("an", 1);
        consumer.set("real_time", -1);
        melt.set_in_and_out(0, 29);
        consumer.connect(melt);
        consumer.run();
    }
    Mlt::Producer decoded(profile, utf8String(exported).c_str());
    for (int position : {0, 12, 29}) {
        std::unique_ptr<Mlt::Frame> a, b, c;
        auto image = [&](Mlt::Producer &producer, std::unique_ptr<Mlt::Frame> &hold) {
            producer.seek(position);
            hold.reset(producer.get_frame());
            // As the render and playback consumers scale (rescale=bilinear):
            // a frame pulled bare interpolates rotated edges differently.
            hold->set("consumer.rescale", "bilinear");
            mlt_image_format format = mlt_image_rgb;
            int w = 1920, h = 1080;
            return hold->get_image(format, w, h);
        };
        const uint8_t *editor = image(sync.tractor(), a);
        const uint8_t *played = image(melt, b);
        const uint8_t *rendered = image(decoded, c);
        INFO("frame " << position);
        CHECK(std::memcmp(editor, played, 1920 * 1080 * 3) == 0);
        CHECK(std::memcmp(editor, rendered, 1920 * 1080 * 3) == 0);
    }
}

TEST_CASE("transform: split transformed clips render on several threads")
{
    // Every affine filter shares one background producer (EngineSync::
    // applyTransform()); a render pulls frames on several threads at once.
    // Under TSan this is the check that sharing it is safe.
    sharedFactoryPolicy();
    Scene scene(generate("blue-1080.mp4", 1920, 1080, "color:#0000c0"), 1920, 1080);
    scene.model.setClipTransform(scene.clip, placed(480, 270, 960, 540, 5));
    ClipId right = scene.clip;
    for (FrameIndex at : {10, 20, 30, 40, 50})
        right = scene.model.splitClip(right, at);
    const fs::path out = scratch() / "threads.mp4";
    std::string error;
    REQUIRE(renderProject(scene.model, utf8String(out), error, {}, nullptr, core::legacyRenderProfile(), 8));
    Mlt::Profile profile("atsc_1080p_30");
    Mlt::Producer rendered(profile, utf8String(out).c_str());
    REQUIRE(rendered.is_valid());
    for (int position : {5, 25, 55}) {
        INFO("frame " << position);
        CHECK(pixelAt(rendered, position, 48, 27).blue());
        CHECK(pixelAt(rendered, position, 150, 80).red());
    }
}

TEST_CASE("transform: a transformed clip dissolves into the next")
{
    sharedFactoryPolicy();
    Scene scene(generate("blue-1080.mp4", 1920, 1080, "color:#0000c0"), 1920, 1080);
    // Room to extend into the dissolve: a plays source 0-39, b 10-49 (of 60).
    scene.model.resizeClip(scene.clip, 0, 39, 0);
    Asset green;
    green.path = utf8String(generate("green-1080.mp4", 1920, 1080, "color:#00c000"));
    green.info.hasVideo = true;
    green.info.width = 1920;
    green.info.height = 1080;
    green.info.lengthInSequenceFrames = 60;
    const ClipId next = scene.model.insertClip(scene.upper, scene.model.addAsset(green), 40, 10, 49);
    scene.model.setClipTransform(scene.clip, placed(480, 270, 960, 540));
    REQUIRE(scene.model.addTransition(scene.upper, scene.clip, next, 10, 10).value != 0);
    EngineSync sync(scene.model);
    CHECK(sync.verify().empty());
    CHECK(pixelAt(sync, 10, 48, 27).blue());   // before the dissolve: placed
    CHECK(pixelAt(sync, 10, 150, 80).red());   // and the lower track beside it
    const Rgb mid = pixelAt(sync, 40, 48, 27); // halfway: blue fading into green
    CHECK(mid.b > 40);
    CHECK(mid.g > 40);
    CHECK(pixelAt(sync, 70, 150, 80).green()); // after: the next clip, full frame
}

TEST_CASE("transform: Auto preview scale plays a timeline with a transformed clip at Half")
{
    sharedFactoryPolicy();
    Scene scene(generate("blue-1080.mp4", 1920, 1080, "color:#0000c0"), 1920, 1080);
    // (fx, fy) as fractions of whatever size the tractor plays at.
    auto pixel = [](EngineSync &sync, double fx, double fy) {
        const int w = sync.profile().width(), h = sync.profile().height();
        sync.tractor().seek(5);
        std::unique_ptr<Mlt::Frame> frame(sync.tractor().get_frame());
        mlt_image_format format = mlt_image_rgb;
        int fw = w, fh = h;
        const uint8_t *image = frame->get_image(format, fw, fh);
        const uint8_t *px =
            image + (static_cast<size_t>(fy * fh) * static_cast<size_t>(fw) + static_cast<size_t>(fx * fw)) * 3;
        return Rgb{px[0], px[1], px[2]};
    };
    EngineSync sync(scene.model, PreviewScale::Auto);
    CHECK(sync.previewFactor() == 1.0); // the default Fit doesn't count
    int rebuilds = 0;
    sync.rebuilt.connect([&] { ++rebuilds; });

    scene.model.setClipTransform(scene.clip, placed(480, 270, 960, 540));
    sync.setProject(scene.model.snapshot());
    CHECK(sync.previewFactor() == 0.5);
    CHECK(sync.profile().width() == 960);
    CHECK(rebuilds == 1);
    CHECK(pixel(sync, 0.25, 0.25).blue()); // placed in the top-left quarter at Half too
    CHECK(pixel(sync, 0.75, 0.75).red());

    scene.model.setClipTransform(scene.clip, placed(1440, 810, 960, 540)); // a drag: in place, still Half
    sync.setProject(scene.model.snapshot());
    CHECK(rebuilds == 1);
    CHECK(pixel(sync, 0.75, 0.75).blue());

    sync.setPreviewScale(PreviewScale::Full); // chosen by hand: honoured
    CHECK(sync.previewFactor() == 1.0);
    sync.setPreviewScale(PreviewScale::Auto);
    CHECK(sync.previewFactor() == 0.5);

    scene.model.setClipTransform(scene.clip, Transform{}); // back to the default: Full again
    sync.setProject(scene.model.snapshot());
    CHECK(sync.previewFactor() == 1.0);

    // The engine's main-thread mirror reports it (the app's "Auto (Half)").
    scene.model.setClipTransform(scene.clip, placed(480, 270, 960, 540));
    Engine engine(scene.model.snapshot(), PreviewScale::Auto);
    REQUIRE(engine.syncForTesting());
    CHECK(engine.previewFactor() == 0.5);
    engine.shutdown();
}

TEST_CASE("transform: a drag is applied in place, without a rebuild or a consumer restart")
{
    sharedFactoryPolicy();
    Scene scene(generate("blue-1080.mp4", 1920, 1080, "color:#0000c0"), 1920, 1080);
    scene.model.setClipTransform(scene.clip, placed(480, 270, 960, 540));
    {
        EngineSync sync(scene.model);
        int rebuilds = 0, inPlace = 0;
        sync.rebuilt.connect([&] { ++rebuilds; });
        sync.appliedInPlace.connect([&] { ++inPlace; });
        scene.model.setClipTransform(scene.clip, placed(1440, 810, 960, 540)); // dragged to the bottom right
        sync.setProject(scene.model.snapshot());
        CHECK(rebuilds == 0);
        CHECK(inPlace == 1);
        CHECK(pixelAt(sync, 5, 144, 81).blue());
        CHECK(pixelAt(sync, 5, 48, 27).red());
        Transform flipped = placed(1440, 810, 960, 540);
        flipped.flipV = true; // a new filter: rebuilt
        scene.model.setClipTransform(scene.clip, flipped);
        sync.setProject(scene.model.snapshot());
        CHECK(rebuilds == 1);
    }

    // Through the engine thread, as the app drives it: twenty pointer moves.
    Engine engine(scene.model.snapshot(), PreviewScale::Full);
    REQUIRE(engine.syncForTesting());
    const int restartsBefore = engine.consumerRestartsForTesting();
    UndoStack undo(scene.model);
    Transform drag = placed(960, 540, 960, 540);
    drag.flipV = true;
    for (int step = 0; step < 20; ++step) {
        drag.x.value = 500 + step * 40;
        REQUIRE(undo.execute(std::make_unique<SetClipTransform>(scene.clip, drag, 7)));
        engine.publish(scene.model.snapshot());
    }
    CHECK(engine.consumerRestartsForTesting() == restartsBefore);
    CHECK(undo.undoLabel() == "Transform clip"); // one step for the whole drag
    engine.shutdown();
    fs::remove_all(scratch());
}
