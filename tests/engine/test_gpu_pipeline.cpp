// ADR-019 G3: the GPU pipeline plays what the CPU one does. Each scene is
// rendered on the CPU graph first (before any glsl.manager exists: one
// would switch even the CPU graph's producers to movit), then on the GPU
// graph with the session's context current on this thread, and the two
// 1080p frames are compared. Skips where there's no GL. One designed
// difference: movit blends in linear light (every RGBA input is
// linearised, mlt_movit_input.cpp), so half-transparent pixels and dissolve
// midpoints are checked against that, not against the CPU's gamma-space
// blend (docs/developer/notes/gpu.md).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/media/utf8_path.h"
#include "core/model/transform.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"
#include "engine/gpu_session.h"
#include "platform/process.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace ustudio;
using namespace ustudio::core;
using namespace ustudio::engine;
namespace fs = std::filesystem;

namespace {

constexpr int kWidth = 1920, kHeight = 1080;

FactoryPolicy &sharedFactoryPolicy()
{
    static FactoryPolicy policy;
    return policy;
}

fs::path scratch()
{
    static const fs::path dir =
        fs::temp_directory_path() / ("ustudio-gpu-pipeline-" + std::to_string(platform::currentProcessId()));
    fs::create_directories(dir);
    return dir;
}

// `left` over the whole frame and `right` over its right half when given.
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

using Image = std::vector<uint8_t>; // RGBA, kWidth x kHeight

Image frameAt(EngineSync &sync, int position)
{
    Mlt::Tractor &tractor = sync.tractor();
    tractor.seek(position);
    std::unique_ptr<Mlt::Frame> frame(tractor.get_frame());
    frame->set("consumer.rescale", "bilinear"); // as the consumers ask
    mlt_image_format format = mlt_image_rgba;
    int w = kWidth, h = kHeight;
    const uint8_t *image = frame->get_image(format, w, h);
    REQUIRE(image);
    REQUIRE(w == kWidth);
    REQUIRE(h == kHeight);
    return Image(image, image + static_cast<size_t>(kWidth) * kHeight * 4);
}

struct Difference
{
    double mean = 0;     // per RGB channel, over the frame
    double outliers = 0; // fraction of pixels with a channel off by more than 16
};

Difference compare(const Image &a, const Image &b)
{
    Difference d;
    size_t off = 0;
    double total = 0;
    for (size_t i = 0; i < a.size(); i += 4) {
        int worst = 0;
        for (size_t c = 0; c < 3; ++c) {
            const int delta = std::abs(a[i + c] - b[i + c]);
            total += delta;
            worst = std::max(worst, delta);
        }
        if (worst > 16)
            ++off;
    }
    const double pixels = static_cast<double>(a.size() / 4);
    d.mean = total / (pixels * 3);
    d.outliers = static_cast<double>(off) / pixels;
    return d;
}

const uint8_t *at(const Image &image, int x, int y)
{
    return image.data() + (static_cast<size_t>(y) * kWidth + static_cast<size_t>(x)) * 4;
}

// BT.709's transfer function and its inverse, on 0-255 values.
double toLinear(double v)
{
    v /= 255.0;
    return v < 0.081 ? v / 4.5 : std::pow((v + 0.099) / 1.099, 1 / 0.45);
}
double fromLinear(double l)
{
    return 255.0 * (l < 0.018 ? 4.5 * l : 1.099 * std::pow(l, 0.45) - 0.099);
}

std::string pixel(const Image &image, int x, int y)
{
    const size_t i = (static_cast<size_t>(y) * kWidth + static_cast<size_t>(x)) * 4;
    return std::to_string(image[i]) + "," + std::to_string(image[i + 1]) + "," + std::to_string(image[i + 2]);
}

// A red lower track and `media` above it for 60 frames.
struct Scene
{
    Model model = Model::createEmpty();
    TrackId upper, lower;
    ClipId clip;

    Scene(const std::string &media, int width, int height)
    {
        lower = model.addTrack(Track::Kind::Video, 0, "V1");
        upper = model.addTrack(Track::Kind::Video, 0, "V2"); // index 0: the top
        Asset red;
        red.path = "color:#c02020";
        red.info.hasVideo = true;
        red.info.lengthInSequenceFrames = 10'000;
        model.insertClip(lower, model.addAsset(red), 0, 0, 59);
        Asset source;
        source.path = media;
        source.info.hasVideo = true;
        source.info.width = width;
        source.info.height = height;
        source.info.lengthInSequenceFrames = 60;
        clip = model.insertClip(upper, model.addAsset(source), 0, 0, 59);
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

// The scene's frame at `position` on the CPU graph and on the GPU graph;
// false (after a message) where there's no GL.
bool renderBoth(const Model &model, int position, Image &cpu, Image &gpu)
{
    {
        EngineSync sync(model, PreviewScale::Full, EngineSync::FrameReads::ProfileSize);
        REQUIRE(sync.verify().empty());
        cpu = frameAt(sync, position);
    }
    std::string error;
    std::shared_ptr<GpuSession> session = GpuSession::acquire(error);
    if (!session) {
        MESSAGE("no GPU pipeline here (" << error << "); nothing to compare");
        return false;
    }
    REQUIRE(session->renderThreadStarted()); // this thread renders
    {
        EngineSync sync(model, PreviewScale::Full, EngineSync::FrameReads::ProfileSize);
        sync.setPipeline(EngineSync::Pipeline::Gpu, {});
        CHECK(sync.verify().empty());
        gpu = frameAt(sync, position);
    }
    session->renderThreadStopped();
    return true;
}

// Mean within 2 levels (the GPU path's own error against its source) and
// at most 1% of pixels, the picture's resampled edges, off by more.
void checkSame(const Image &cpu, const Image &gpu)
{
    const Difference d = compare(cpu, gpu);
    INFO("mean " << d.mean << ", outliers " << d.outliers * 100 << "%");
    CHECK(d.mean <= 2.0);
    CHECK(d.outliers <= 0.01);
}

} // namespace

TEST_CASE("GPU pipeline: a moved and scaled clip lands where the CPU puts it")
{
    sharedFactoryPolicy();
    Scene scene(utf8String(generate("blue.mp4", 1920, 1080, "color:#2040c0")), 1920, 1080);
    scene.model.setClipTransform(scene.clip, placed(480, 270, 960, 540));
    Image cpu, gpu;
    if (!renderBoth(scene.model, 5, cpu, gpu))
        return;
    INFO("inside cpu " << pixel(cpu, 480, 270) << " gpu " << pixel(gpu, 480, 270));
    INFO("outside cpu " << pixel(cpu, 1440, 810) << " gpu " << pixel(gpu, 1440, 810));
    checkSame(cpu, gpu);
}

TEST_CASE("GPU pipeline: crop and both flips match")
{
    sharedFactoryPolicy();
    const std::string halves = utf8String(generate("halves.mp4", 1920, 1080, "color:#2040c0", "color:#20c040"));
    for (int variant = 0; variant < 3; ++variant) {
        Scene scene(halves, 1920, 1080);
        Transform t = placed(960, 540, 1280, 720);
        if (variant == 0)
            t.cropLeft.value = 400, t.cropTop.value = 100;
        if (variant == 1)
            t.flipH = true;
        if (variant == 2)
            t.flipV = true;
        scene.model.setClipTransform(scene.clip, t);
        INFO("variant " << variant);
        Image cpu, gpu;
        if (!renderBoth(scene.model, 5, cpu, gpu))
            return;
        checkSame(cpu, gpu);
    }
}

TEST_CASE("GPU pipeline: a rotated clip keeps the CPU chain and matches")
{
    sharedFactoryPolicy();
    Scene scene(utf8String(generate("blue.mp4", 1920, 1080, "color:#2040c0")), 1920, 1080);
    scene.model.setClipTransform(scene.clip, placed(960, 540, 960, 540, 10));
    Image cpu, gpu;
    if (!renderBoth(scene.model, 5, cpu, gpu))
        return;
    checkSame(cpu, gpu);
}

TEST_CASE("GPU pipeline: a 4:3 clip is fitted and centred, the track below showing either side")
{
    sharedFactoryPolicy();
    Scene scene(utf8String(generate("blue-4x3.mp4", 1440, 1080, "color:#2040c0")), 1440, 1080);
    Image cpu, gpu;
    if (!renderBoth(scene.model, 5, cpu, gpu))
        return;
    INFO("bar cpu " << pixel(cpu, 100, 540) << " gpu " << pixel(gpu, 100, 540));
    checkSame(cpu, gpu);
}

TEST_CASE("GPU pipeline: a half-transparent clip blends over the track below")
{
    sharedFactoryPolicy();
    Scene scene("color:0x20c04080", 1920, 1080); // 50% alpha green over red
    scene.model.setClipTransform(scene.clip, placed(960, 540, 960, 540));
    Image cpu, gpu;
    if (!renderBoth(scene.model, 5, cpu, gpu))
        return;
    INFO("blend cpu " << pixel(cpu, 960, 540) << " gpu " << pixel(gpu, 960, 540));
    // The CPU averages the coded values; movit averages light.
    const int green[3] = {0x20, 0xc0, 0x40}, red[3] = {0xc0, 0x20, 0x20};
    for (size_t c = 0; c < 3; ++c) {
        CHECK(std::abs(at(cpu, 960, 540)[c] - (green[c] + red[c]) / 2) <= 3);
        const double linear = fromLinear((toLinear(green[c]) + toLinear(red[c])) / 2);
        CHECK(std::abs(at(gpu, 960, 540)[c] - linear) <= 4);
    }
    // Outside the picture, only the track below: the same.
    CHECK(compare(Image(at(cpu, 0, 0), at(cpu, 0, 300)), Image(at(gpu, 0, 0), at(gpu, 0, 300))).mean <= 2.0);
}

TEST_CASE("GPU pipeline: a dissolve mixes as the CPU's does")
{
    sharedFactoryPolicy();
    Scene scene(utf8String(generate("blue.mp4", 1920, 1080, "color:#2040c0")), 1920, 1080);
    scene.model.resizeClip(scene.clip, 0, 39, 0);
    Asset green;
    green.path = utf8String(generate("green.mp4", 1920, 1080, "color:#20c040"));
    green.info.hasVideo = true;
    green.info.width = 1920;
    green.info.height = 1080;
    green.info.lengthInSequenceFrames = 60;
    const ClipId next = scene.model.insertClip(scene.upper, scene.model.addAsset(green), 40, 10, 49);
    REQUIRE(scene.model.addTransition(scene.upper, scene.clip, next, 10, 10).value != 0);
    // The middle: movit.luma_mix squares the progress so a dissolve in
    // linear light looks even, so it differs from the CPU's there, but
    // every channel lies between the two pictures'.
    Image cpu, gpu;
    if (!renderBoth(scene.model, 40, cpu, gpu))
        return;
    INFO("mid cpu " << pixel(cpu, 960, 540) << " gpu " << pixel(gpu, 960, 540));
    const int blue[3] = {0x20, 0x40, 0xc0}, green2[3] = {0x20, 0xc0, 0x40};
    for (size_t c = 0; c < 3; ++c) {
        const int lo = std::min(blue[c], green2[c]) - 4, hi = std::max(blue[c], green2[c]) + 4;
        CHECK(at(gpu, 960, 540)[c] >= lo);
        CHECK(at(gpu, 960, 540)[c] <= hi);
    }
    CHECK(std::abs(at(gpu, 960, 540)[1] - at(gpu, 960, 540)[2]) > 10); // mid-way, not an end
    // Outside the dissolve both pipelines show the same frame.
    Image cpuAfter, gpuAfter;
    REQUIRE(renderBoth(scene.model, 55, cpuAfter, gpuAfter));
    checkSame(cpuAfter, gpuAfter);
}

TEST_CASE("GPU pipeline: switching back to the CPU reopens everything on the CPU chain")
{
    sharedFactoryPolicy();
    Scene scene(utf8String(generate("blue.mp4", 1920, 1080, "color:#2040c0")), 1920, 1080);
    scene.model.setClipTransform(scene.clip, placed(480, 270, 960, 540));
    Image before;
    {
        EngineSync sync(scene.model, PreviewScale::Full, EngineSync::FrameReads::ProfileSize);
        before = frameAt(sync, 5);
    }
    EngineSync sync(scene.model, PreviewScale::Full, EngineSync::FrameReads::ProfileSize);
    {
        std::string error;
        std::shared_ptr<GpuSession> session = GpuSession::acquire(error);
        if (!session) {
            MESSAGE("no GPU pipeline here (" << error << ")");
            return;
        }
        REQUIRE(session->renderThreadStarted());
        sync.setPipeline(EngineSync::Pipeline::Gpu, {});
        checkSame(before, frameAt(sync, 5));
        session->renderThreadStopped();
    }
    sync.setPipeline(EngineSync::Pipeline::Cpu, {}); // no context current now
    CHECK(sync.verify().empty());
    checkSame(before, frameAt(sync, 5));
    fs::remove_all(scratch());
}

// The file an export wrote, frame `position`, read back at 1080p.
Image exportedFrame(const fs::path &file, int position)
{
    Model check = Model::createEmpty();
    TrackId track = check.addTrack(Track::Kind::Video, 0, "V1");
    Asset rendered;
    rendered.path = utf8String(file);
    rendered.info.hasVideo = true;
    rendered.info.width = 1920;
    rendered.info.height = 1080;
    rendered.info.lengthInSequenceFrames = 60;
    check.insertClip(track, check.addAsset(rendered), 0, 0, 59);
    EngineSync sync(check, PreviewScale::Full, EngineSync::FrameReads::ProfileSize);
    return frameAt(sync, position);
}

// G4 (ADR-019 point 7): with the preview on the GPU, an export on a pool
// thread (as the app's) takes its pipeline and looks like the preview,
// soft edges included (a half-transparent picture, where the CPU and GPU
// differ), within what H.264 changes.
TEST_CASE("GPU pipeline: an export while the preview is on the GPU matches the preview")
{
    sharedFactoryPolicy();
    Scene scene("color:0x20c04080", 1920, 1080); // 50% green over red
    scene.model.setClipTransform(scene.clip, placed(960, 540, 960, 540));
    Image cpu, preview;
    if (!renderBoth(scene.model, 5, cpu, preview))
        return;
    std::string error;
    std::shared_ptr<GpuSession> session = GpuSession::acquire(error); // the preview's
    REQUIRE(session);
    const fs::path out = scratch() / "export-gpu.mp4";
    bool ok = false;
    std::thread worker([&] { ok = renderProject(scene.model, utf8String(out), error); });
    worker.join();
    INFO(error);
    REQUIRE(ok);
    session.reset();
    const Image exported = exportedFrame(out, 5);
    INFO("blend preview " << pixel(preview, 960, 540) << " export " << pixel(exported, 960, 540) << " cpu "
                          << pixel(cpu, 960, 540));
    for (size_t c = 0; c < 3; ++c)
        CHECK(std::abs(at(exported, 960, 540)[c] - at(preview, 960, 540)[c]) <= 3);
    CHECK(compare(preview, exported).mean <= 2.0);
}

// FX3: on the GPU a wipe stays the CPU `luma` (an island in the movit
// graph), so it plays the CPU's wipe frame by frame; a dip is movit's
// dissolve with the CPU brightness filters on its cuts.
namespace {

// Where the incoming green meets the outgoing blue along the middle row: the
// first x from the left that is more blue than green; -1 if none.
int wipeEdge(const Image &image)
{
    for (int x = 0; x < 1920; x += 2)
        if (at(image, x, 540)[2] > at(image, x, 540)[1])
            return x;
    return -1;
}

struct TwoClipScene
{
    Scene scene{utf8String(generate("blue.mp4", 1920, 1080, "color:#2040c0")), 1920, 1080};
    TransitionId transition;
    TwoClipScene()
    {
        scene.model.resizeClip(scene.clip, 0, 39, 0);
        Asset green;
        green.path = utf8String(generate("green.mp4", 1920, 1080, "color:#20c040"));
        green.info.hasVideo = true;
        green.info.width = 1920;
        green.info.height = 1080;
        green.info.lengthInSequenceFrames = 60;
        const ClipId next = scene.model.insertClip(scene.upper, scene.model.addAsset(green), 40, 10, 49);
        transition = scene.model.addTransition(scene.upper, scene.clip, next, 10, 10); // [30, 50)
    }
};

} // namespace

TEST_CASE("GPU pipeline: a wipe plays as the CPU's, frame by frame")
{
    sharedFactoryPolicy();
    TwoClipScene two;
    REQUIRE(two.transition.value != 0);
    two.scene.model.setTransitionRecipe(two.transition, "wipe.left",
                                        {{"video.service", std::string("luma"), {}},
                                         {"video.luma", std::string("left"), {}},
                                         {"video.softness", 0.1, {}}});
    REQUIRE(two.scene.model.check().empty());
    for (int position = 31; position < 50; position += 2) {
        Image cpu, gpu;
        if (!renderBoth(two.scene.model, position, cpu, gpu))
            return;
        INFO("frame " << position << ": cpu edge " << wipeEdge(cpu) << ", gpu edge " << wipeEdge(gpu));
        CHECK(std::abs(wipeEdge(cpu) - wipeEdge(gpu)) <= 16);
        if (position == 41) {
            // Mid-wipe: the incoming green on the left, the blue still right.
            CHECK(at(gpu, 100, 540)[1] > 150);
            CHECK(at(gpu, 1820, 540)[2] > 150);
        }
    }
}

TEST_CASE("GPU pipeline: a dip to black is black in the middle")
{
    sharedFactoryPolicy();
    TwoClipScene two;
    REQUIRE(two.transition.value != 0);
    two.scene.model.setTransitionRecipe(two.transition, "dip-black",
                                        {{"a.0.service", std::string("brightness"), {}},
                                         {"a.0.level", std::string("ramp:1,0,0"), {}},
                                         {"b.0.service", std::string("brightness"), {}},
                                         {"b.0.level", std::string("ramp:0,0,1"), {}}});
    Image cpu, gpu;
    if (!renderBoth(two.scene.model, 40, cpu, gpu))
        return;
    for (size_t c = 0; c < 3; ++c) {
        CHECK(at(gpu, 960, 540)[c] <= 4);
        CHECK(at(cpu, 960, 540)[c] <= 4);
    }
    // Either side of the middle the pictures come back (not a stuck black).
    Image cpuAfter, gpuAfter;
    REQUIRE(renderBoth(two.scene.model, 48, cpuAfter, gpuAfter));
    CHECK(at(gpuAfter, 960, 540)[1] > 100);
}

TEST_CASE("GPU pipeline: the preview leaving the GPU mid-export doesn't stop the export")
{
    sharedFactoryPolicy();
    Scene scene(utf8String(generate("blue.mp4", 1920, 1080, "color:#2040c0")), 1920, 1080);
    scene.model.setClipTransform(scene.clip, placed(480, 270, 960, 540));
    std::string error;
    std::shared_ptr<GpuSession> session = GpuSession::acquire(error);
    if (!session) {
        MESSAGE("no GPU pipeline here (" << error << ")");
        return;
    }
    const fs::path out = scratch() / "export-dropped.mp4";
    bool ok = false;
    std::thread worker([&] { ok = renderProject(scene.model, utf8String(out), error); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    session.reset(); // the preview's reference: the export keeps the session alive
    worker.join();
    INFO(error);
    CHECK(ok);
    CHECK_FALSE(GpuSession::current()); // gone with the export
}

// VE Demos' crash (0.67.1, 2026-09-28): movit's create_fbo asserted
// GL_FRAMEBUFFER_COMPLETE (here: a segfault in glTexSubImage2D, about one run
// in six) on the paused refresh of four tracks at Half. The cause: V3's
// rotated picture-in-picture was a copy of a V1 clip, so both cut from one
// master producer, and MLT's movit keys a chain's inputs by producer; the
// RGBA island at profile size and the YUV cut at source size then shared
// one input. On the GPU pipeline each track, and each clip of a dissolve's
// pair, now has its own master (EngineSync::masterLane()).
namespace {

Model demoTourModel(bool sameAssetDissolve)
{
    const std::string blue = utf8String(generate("blue.mp4", 1920, 1080, "color:#2040c0"));
    const std::string green = utf8String(generate("green.mp4", 1920, 1080, "color:#20c040"));
    const std::string capture = utf8String(generate("capture-1344.mp4", 1344, 768, "color:#c0c020"));
    Model model = Model::createEmpty();
    const TrackId v1 = model.addTrack(Track::Kind::Video, 0, "V1");
    const TrackId v2 = model.addTrack(Track::Kind::Video, 0, "V2");
    const TrackId v3 = model.addTrack(Track::Kind::Video, 0, "V3");
    model.addTrack(Track::Kind::Video, 0, "V4");
    auto asset = [&](const std::string &path, int w, int h) {
        Asset a;
        a.path = path;
        a.info.hasVideo = true;
        a.info.width = w;
        a.info.height = h;
        a.info.lengthInSequenceFrames = 60;
        return model.addAsset(a);
    };
    const AssetId blueAsset = asset(blue, 1920, 1080), greenAsset = asset(green, 1920, 1080);
    const ClipId first = model.insertClip(v1, blueAsset, 0, 0, 59);
    // A dissolve into the same file, or into another one.
    const ClipId second = model.insertClip(v1, sameAssetDissolve ? blueAsset : greenAsset, 60, 0, 59);
    REQUIRE(model.addTransition(v1, first, second, 10, 10).value != 0);
    const AssetId captureAsset = asset(capture, 1344, 768);
    model.insertClip(v2, captureAsset, 0, 0, 59);
    model.insertClip(v2, captureAsset, 60, 0, 59);
    // Copies of V1's clips, scaled, rotated and flipped, as in the tour.
    Transform t = placed(1300, 700, 945, 540, 21);
    t.flipH = true;
    model.setClipTransform(model.insertClip(v3, blueAsset, 0, 0, 59), t);
    model.setClipTransform(model.insertClip(v3, greenAsset, 60, 0, 59), t);
    return model;
}

// The master producers the tractor's cuts come from, per track (the cut's
// parent; a dissolve segment's two cuts both count for its track).
std::vector<std::set<mlt_producer>> mastersPerTrack(EngineSync &sync)
{
    std::vector<std::set<mlt_producer>> result;
    Mlt::Tractor &tractor = sync.tractor();
    for (int t = 0; t < tractor.count(); ++t) {
        std::set<mlt_producer> masters;
        std::unique_ptr<Mlt::Producer> track(tractor.track(t));
        Mlt::Playlist playlist(*track);
        for (int c = 0; playlist.is_valid() && c < playlist.count(); ++c) {
            std::unique_ptr<Mlt::Producer> clip(playlist.get_clip(c));
            if (!clip || clip->is_blank())
                continue;
            const char *service = clip->parent().get("mlt_service");
            if (service && std::string(service) == "tractor") {
                Mlt::Tractor sub(clip->parent());
                for (int s = 0; s < sub.count(); ++s) {
                    std::unique_ptr<Mlt::Producer> cut(sub.track(s));
                    masters.insert(cut->parent().get_producer());
                }
            } else {
                masters.insert(clip->parent().get_producer());
            }
        }
        result.push_back(std::move(masters));
    }
    return result;
}

} // namespace

TEST_CASE("GPU pipeline: no master producer feeds two inputs of one frame")
{
    sharedFactoryPolicy();
    Model model = demoTourModel(true);
    std::string error;
    std::shared_ptr<GpuSession> session = GpuSession::acquire(error);
    if (!session) {
        MESSAGE("no GPU pipeline here (" << error << ")");
        return;
    }
    EngineSync sync(model, PreviewScale::Half, EngineSync::FrameReads::ProfileSize);
    sync.setPipeline(EngineSync::Pipeline::Gpu, {});
    const std::vector<std::set<mlt_producer>> masters = mastersPerTrack(sync);
    for (size_t a = 1; a < masters.size(); ++a)
        for (size_t b = a + 1; b < masters.size(); ++b)
            for (mlt_producer p : masters[a])
                CHECK_MESSAGE(!masters[b].contains(p), "tracks " << a << " and " << b << " share a master");
    // The same-file dissolve on V1 (MLT index 1) cuts from two masters.
    CHECK(masters[1].size() >= 2);

    // The check itself: the CPU pipeline shares masters across tracks, and
    // it finds that.
    EngineSync cpu(model, PreviewScale::Half, EngineSync::FrameReads::ProfileSize);
    const std::vector<std::set<mlt_producer>> shared = mastersPerTrack(cpu);
    bool sharing = false;
    for (mlt_producer p : shared[1])
        sharing = sharing || shared[3].contains(p);
    CHECK(sharing);
}

TEST_CASE("GPU pipeline: the demo tour's four-track graph renders every frame at Half")
{
    sharedFactoryPolicy();
    for (bool sameAssetDissolve : {false, true}) {
        Model model = demoTourModel(sameAssetDissolve);
        std::string error;
        std::shared_ptr<GpuSession> session = GpuSession::acquire(error);
        if (!session) {
            MESSAGE("no GPU pipeline here (" << error << ")");
            return;
        }
        REQUIRE(session->renderThreadStarted());
        for (PreviewScale scale : {PreviewScale::Full, PreviewScale::Half}) {
            EngineSync sync(model, scale, EngineSync::FrameReads::ProfileSize);
            sync.setPipeline(EngineSync::Pipeline::Gpu, {});
            const int w = sync.profile().width(), h = sync.profile().height();
            for (int position = 0; position < 120; ++position) {
                Mlt::Tractor &tractor = sync.tractor();
                tractor.seek(position);
                std::unique_ptr<Mlt::Frame> frame(tractor.get_frame());
                mlt_image_format format = mlt_image_rgba;
                int fw = w, fh = h;
                REQUIRE(frame->get_image(format, fw, fh) != nullptr);
            }
        }
        session->renderThreadStopped();
    }
}
