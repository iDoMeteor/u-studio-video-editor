#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/model/model.h"
#include "engine/engine_sync.h"
#include "engine/factory_policy.h"
#include "sync_clip.h"

#include <mlt++/Mlt.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using namespace ustudio::core;
using namespace ustudio::engine;

namespace {

struct RemoveOnExit
{
    std::filesystem::path path;
    ~RemoveOnExit()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

// What a consumer actually shows for one frame: its picture brightness and
// its own audio samples (left channel), taken from the same Mlt::Frame.
struct ShownFrame
{
    int brightness = 0;
    std::vector<int16_t> left;
};

struct Capture
{
    std::mutex mutex;
    std::map<int, ShownFrame> byPosition;
    int width = 0, height = 0;
};

void onFrameShow(mlt_properties, void *self, mlt_event_data data)
{
    auto *capture = static_cast<Capture *>(self);
    Mlt::Frame frame(Mlt::EventData(data).to_frame());
    if (!frame.is_valid())
        return;
    ShownFrame shown;
    mlt_image_format format = mlt_image_rgb;
    int w = capture->width, h = capture->height;
    const uint8_t *image = frame.get_image(format, w, h);
    if (image)
        shown.brightness =
            image[(static_cast<size_t>(h / 2) * static_cast<size_t>(w) + static_cast<size_t>(w / 2)) * 3];
    mlt_audio_format audioFormat = mlt_audio_s16;
    int frequency = 48000, channels = 2;
    int samples = mlt_audio_calculate_frame_samples(30.0f, frequency, frame.get_position());
    const auto *pcm = static_cast<const int16_t *>(frame.get_audio(audioFormat, frequency, channels, samples));
    for (int i = 0; pcm && i < samples; ++i)
        shown.left.push_back(pcm[i * channels]);
    std::lock_guard<std::mutex> lock(capture->mutex);
    capture->byPosition[frame.get_position()] = std::move(shown);
}

} // namespace

// doc 12, M2: "A/V sync: a generated clip with a 1 kHz beep on frame 0 of
// every second and a white flash on the same frames shows no perceptible
// offset (< 1 frame) at 1x; verified by eye and by the null-consumer
// position test." This is the automated half: the clip goes through the
// real import path (probeMedia, a model clip, EngineSync's tractor) and a
// real-time null consumer, and for every flash the beep's onset, measured
// in samples within the frames the consumer actually showed, must fall
// inside the flash frame. The by-eye half is the make_sync_clip tool.
TEST_CASE("A/V sync: the beep starts inside the flash frame, through import and a real-time consumer")
{
    FactoryPolicy policy;
    std::random_device rd;
    std::filesystem::path media =
        std::filesystem::temp_directory_path() / ("ustudio-av-sync-" + std::to_string(rd()) + ".mp4");
    RemoveOnExit cleanup{media};

    Model model = Model::createEmpty();
    EngineSync sync(model);
    const int seconds = 4;
    ustudio::testing::renderSyncClip(sync.profile(), media.string(), seconds);
    REQUIRE(std::filesystem::exists(media));

    EngineSync::ProbedMedia probed = sync.probeMedia(media.string());
    REQUIRE(probed.length > 0);
    REQUIRE(probed.hasAudio);
    Asset asset;
    asset.path = media.string();
    asset.displayName = "sync.mp4";
    asset.info.hasVideo = true;
    asset.info.hasAudio = true;
    asset.info.lengthInSequenceFrames = probed.length;
    AssetId assetId = model.addAsset(asset);
    TrackId track = model.addTrack(Track::Kind::Video, 0, "V1");
    model.insertClip(track, assetId, 0, 0, probed.length - 1);
    sync.setProject(model.snapshot());

    Capture capture;
    capture.width = sync.profile().width();
    capture.height = sync.profile().height();
    Mlt::Consumer consumer(sync.profile(), "null");
    REQUIRE(consumer.is_valid());
    consumer.set("real_time", 1);
    std::unique_ptr<Mlt::Event> event(consumer.listen("consumer-frame-show", &capture, onFrameShow));
    REQUIRE(consumer.connect(sync.tractor()) == 0);
    REQUIRE(consumer.start() == 0);
    const int last = sync.tractor().get_length() - 1;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds + 10);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(capture.mutex);
            if (capture.byPosition.contains(last))
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    consumer.stop();

    std::lock_guard<std::mutex> lock(capture.mutex);
    const int fps = ustudio::testing::framesPerSecond(sync.profile());
    int checked = 0;
    for (int s = 0; s < seconds; ++s) {
        const int flash = s * fps;
        if (!capture.byPosition.contains(flash) || !capture.byPosition.contains(flash + 1))
            continue; // a real-time consumer may drop a frame; judge the ones actually shown
        const ShownFrame &flashFrame = capture.byPosition.at(flash);
        CHECK(flashFrame.brightness > 200);
        CHECK(capture.byPosition.at(flash + 1).brightness < 40);

        // Beep onset: the first sample over a clear threshold, searched from
        // the frame before the flash through the one after, as an offset
        // from the flash frame's first sample (negative = early). |offset| <
        // one frame's samples is the M2 box.
        long offset = 0;
        bool found = false;
        long position = capture.byPosition.contains(flash - 1)
                            ? -static_cast<long>(capture.byPosition.at(flash - 1).left.size())
                            : 0;
        for (int f = std::max(flash - 1, 0); f <= flash + 1 && !found; ++f) {
            auto it = capture.byPosition.find(f);
            if (it == capture.byPosition.end())
                break;
            for (size_t i = 0; i < it->second.left.size(); ++i) {
                if (std::abs(it->second.left[i]) > 4000) {
                    offset = position + static_cast<long>(i);
                    found = true;
                    break;
                }
            }
            position += static_cast<long>(it->second.left.size());
        }
        const long samplesPerFrame = static_cast<long>(flashFrame.left.size());
        INFO("second " << s << ": beep onset " << offset << " samples from the flash frame's start");
        REQUIRE(found);
        MESSAGE("second " << s << ": beep onset " << offset << " samples ("
                          << (1000.0 * static_cast<double>(offset) / 48000.0)
                          << " ms) from the flash frame's first sample; one frame = " << samplesPerFrame);
        CHECK(std::labs(offset) < samplesPerFrame);
        ++checked;
    }
    CHECK(checked >= seconds - 1);
}
