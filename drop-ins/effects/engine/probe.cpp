#include "engine/probe.h"

#include "core/model/effect_native.h"
#include "engine/producer_open.h"
#include "engine/registry.h"

#include <mlt++/Mlt.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <ostream>

namespace ustudio::effects {

namespace {

// Service names are MLT identifiers; anything else is refused before it
// reaches MLT.
bool isServiceName(const std::string &name)
{
    return !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
               c == '-';
    });
}

struct Pass
{
    bool pulled = true;    // every frame came back with a picture
    bool allBlack = true;  // every pixel of every frame was 0,0,0
    bool badAudio = false; // a NaN or infinite sample
    double seconds = 0.0;  // pulling frames 1.. (frame 0 warms up)
};

// Pulls `frames` frames through `producer` at `width` x `height` (the
// profile's own, 1080p, unless asked), picture and sound.
Pass pull(Mlt::Producer &producer, int frames, int readWidth = 1920, int readHeight = 1080)
{
    Pass pass;
    std::chrono::steady_clock::time_point start;
    for (int position = 0; position < frames; ++position) {
        if (position == 1)
            start = std::chrono::steady_clock::now();
        producer.seek(position);
        std::unique_ptr<Mlt::Frame> frame(producer.get_frame());
        if (!frame) {
            pass.pulled = false;
            break;
        }
        mlt_image_format format = mlt_image_rgba;
        int width = readWidth, height = readHeight;
        const uint8_t *image = frame->get_image(format, width, height);
        if (!image || width <= 0 || height <= 0) {
            pass.pulled = false;
            break;
        }
        const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
        for (size_t i = 0; pass.allBlack && i < bytes; i += 4)
            if (image[i] || image[i + 1] || image[i + 2])
                pass.allBlack = false;
        mlt_audio_format audioFormat = mlt_audio_float;
        int frequency = 48000, channels = 2, samples = 1600;
        const auto *audio = static_cast<const float *>(frame->get_audio(audioFormat, frequency, channels, samples));
        if (audio && audioFormat == mlt_audio_float)
            for (int i = 0; i < samples * channels; ++i)
                if (!std::isfinite(audio[i]))
                    pass.badAudio = true;
    }
    if (frames > 1)
        pass.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return pass;
}

// A noise: source (picture and sound) at 1080p, `frames` long, on the CPU
// chain whatever the process's GPU state (ADR-019: a worker's producer).
std::unique_ptr<Mlt::Producer> source(Mlt::Profile &profile, int frames, const char *resource = "noise:")
{
    std::unique_ptr<Mlt::Producer> producer = engine::openProducer(profile, resource, engine::ProducerUse::Worker);
    if (!producer || !producer->is_valid())
        return nullptr;
    producer->set("length", frames);
    producer->set_in_and_out(0, frames - 1);
    return producer;
}

// The effect on a fresh source, with `effect`'s parameters.
Pass run(Mlt::Profile &profile, const core::Effect &effect, int frames, int readWidth = 1920, int readHeight = 1080,
         const char *resource = "noise:")
{
    std::unique_ptr<Mlt::Producer> producer = source(profile, frames, resource);
    if (!producer)
        return Pass{false, false, false, 0.0};
    std::vector<std::unique_ptr<Mlt::Filter>> filters;
    for (const core::NativeFilter &native : core::nativeFilters(effect, 0, frames)) {
        auto filter = std::make_unique<Mlt::Filter>(profile, native.service.c_str());
        for (const auto &[name, value] : native.properties)
            filter->set(name.c_str(), value.c_str());
        producer->attach(*filter);
        filters.push_back(std::move(filter));
    }
    return pull(*producer, frames, readWidth, readHeight);
}

// Every shown number at one end of its range.
core::Effect atExtreme(const EffectDescriptor &descriptor, core::Effect effect, bool maximum)
{
    for (const ParamDescriptor &p : descriptor.params) {
        if (p.hidden || (p.kind != ParamKind::Scalar && p.kind != ParamKind::Integer))
            continue;
        const std::optional<double> end = maximum ? p.maximum : p.minimum;
        if (!end)
            continue;
        core::Param param;
        param.name = p.id;
        if (p.kind == ParamKind::Scalar)
            param.value = *end;
        else
            param.value = static_cast<int64_t>(*end);
        auto it = std::find_if(effect.params.begin(), effect.params.end(),
                               [&](const core::Param &existing) { return existing.name == p.id; });
        if (it != effect.params.end())
            *it = param;
        else
            effect.params.push_back(param);
    }
    return effect;
}

} // namespace

HealthRecord probeEffect(const std::string &service, int frames, const std::function<void(const char *)> &stage)
{
    Mlt::Profile profile("atsc_1080p_30");
    // The process's repository; a second Factory::init() would reset MLT's
    // global settings (mlt_factory.c).
    Mlt::Repository repository(mlt_factory_repository());
    {
        Mlt::Filter filter(profile, service.c_str());
        if (!filter.is_valid())
            return {HealthStatus::Unavailable, "MLT can't create it", 0.0};
    }
    const EffectDescriptor descriptor = describe(repository, service, loadOverlays(effectsDataDir() / "overlays"));
    const core::Effect defaults = makeEffect(descriptor);

    std::unique_ptr<Mlt::Producer> bare = source(profile, frames);
    if (!bare)
        return {HealthStatus::Unavailable, "no noise: source to probe with", 0.0};
    const Pass baseline = pull(*bare, frames);

    stage("defaults");
    const Pass withDefaults = run(profile, defaults, frames);
    if (!withDefaults.pulled)
        return {HealthStatus::BadOutput, "no picture with its defaults", 0.0};
    if (withDefaults.badAudio)
        return {HealthStatus::BadOutput, "NaN audio with its defaults", 0.0};
    // Black from noise with nothing asked of it: broken for this input. (A
    // service meant to make black at its defaults gets an overlay default.)
    if (withDefaults.allBlack && descriptor.media == MediaKind::Video)
        return {HealthStatus::BadOutput, "black picture with its defaults", 0.0};
    const int timed = std::max(frames - 1, 1);
    // To a tenth: the badge needs no more, and the cache stays readable.
    const double ms =
        std::round(std::max(0.0, (withDefaults.seconds - baseline.seconds) * 1000.0 / timed) * 10.0) / 10.0;

    // The ends of every range (a crash here is the parent's to see).
    for (bool maximum : {false, true}) {
        stage(maximum ? "maximums" : "minimums");
        const Pass extreme = run(profile, atExtreme(descriptor, defaults, maximum), std::min(frames, 3));
        if (!extreme.pulled)
            return {HealthStatus::BadOutput, maximum ? "no picture at its maximums" : "no picture at its minimums", ms};
        if (extreme.badAudio)
            return {HealthStatus::BadOutput, maximum ? "NaN audio at its maximums" : "NaN audio at its minimums", ms};
    }
    // Read at half size, as a scaled preview or a thumbnail may: frei0r's
    // 3dflippo writes out of bounds whenever the frame read isn't the
    // profile's size (MLT 7.40, frei0r 2.5.6: a crash at 64x36 to 960x540
    // in a 1080p profile, none at 1920x1080 or in a matching profile). From
    // a colour, which crashed it every time; from noise it ran on corrupted.
    stage("half size");
    const Pass half = run(profile, defaults, std::min(frames, 3), 960, 540, "color:0x808080ff");
    if (!half.pulled)
        return {HealthStatus::BadOutput, "no picture at half size", ms};
    return {HealthStatus::Ok, "", ms};
}

int runProbeEffect(const std::vector<std::string> &args, std::ostream &out)
{
    int frames = 10;
    bool usable = (args.size() == 1 || args.size() == 2) && isServiceName(args[0]);
    if (usable && args.size() == 2) {
        const std::string &text = args[1];
        auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), frames);
        usable = error == std::errc() && end == text.data() + text.size() && frames >= 2 && frames <= 300;
    }
    if (!usable) {
        out << R"({"status":"usage","usage":"--probe-effect <service> [frames 2-300]"})" << "\n";
        return 2;
    }
    // A line before each stage: when the child dies, the parent knows
    // where (core/health.h, parseProbeStageLine()).
    const HealthRecord record = probeEffect(args[0], frames, [&](const char *stage) {
        out << probeStageLine(args[0], stage) << "\n" << std::flush;
    });
    out << probeResultLine(args[0], record) << "\n" << std::flush;
    return record.usable() ? 0 : 1;
}

int runEffectsRegistry(const std::vector<std::string> &args, std::ostream &out)
{
    if (!args.empty()) {
        out << R"({"status":"usage","usage":"--effects-registry"})" << "\n";
        return 2;
    }
    Mlt::Repository repository(mlt_factory_repository());
    const EffectRegistry registry = EffectRegistry::scan(repository, loadOverlays(effectsDataDir() / "overlays"));
    out << toJson(registry.toJson(registryFingerprint())) << "\n" << std::flush;
    return 0;
}

} // namespace ustudio::effects
