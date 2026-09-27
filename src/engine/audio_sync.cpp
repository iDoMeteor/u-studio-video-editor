#include "audio_sync.h"

#include "core/log.h"
#include "core/trace.h"
#include "engine/producer_open.h"

#include <mlt++/Mlt.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>

namespace ustudio::engine {

namespace Log = ustudio::core::Log;

std::optional<core::audio::Envelope> decodeEnvelope(const AudioSpan &span, core::Rational fps)
{
    core::trace::Scope trace("audio sync: decode");
    if (span.frames <= 0 || fps.num <= 0 || fps.den <= 0)
        return std::nullopt;
    // The sequence's rate, as WaveformCache: the producer's frame numbers
    // follow its profile, so any other rate would decode the wrong range.
    Mlt::Profile profile;
    profile.set_frame_rate(fps.num, fps.den);
    std::unique_ptr<Mlt::Producer> opened = openProducer(profile, span.resource, ProducerUse::Worker);
    Mlt::Producer &producer = *opened;
    if (!producer.is_valid()) {
        Log::warn("[sync] could not open " + span.resource);
        return std::nullopt;
    }
    // Only the sound is needed; skipping the picture makes the decode far
    // cheaper. Set on the producer itself, as EngineSync::masterProducerFor
    // does (MLT ignores it on a cut). audio_index is -1 when there's none
    // (producer_avformat.yml).
    producer.set("video_index", -1);
    if (producer.property_exists("audio_index") && producer.get_int("audio_index") < 0)
        return std::nullopt;

    constexpr int kRate = 48'000;
    constexpr int kSamplesPerBlock = static_cast<int>(kRate * core::audio::Envelope::kBlockMs / 1000.0);
    core::audio::Envelope envelope;
    envelope.startMs = static_cast<double>(span.timelineStart) * 1000.0 * fps.den / fps.num;
    envelope.values.reserve(static_cast<size_t>(static_cast<double>(span.frames) * 1000.0 * fps.den / fps.num) + 1);
    double blockSum = 0.0;
    int blockCount = 0;
    producer.seek(static_cast<int>(span.sourceIn));
    for (core::FrameIndex i = 0; i < span.frames; ++i) {
        std::unique_ptr<Mlt::Frame> frame(producer.get_frame()); // advances by itself
        if (!frame || !frame->is_valid())
            break;
        mlt_audio_format format = mlt_audio_s16;
        int frequency = kRate;
        int channels = 2;
        int samples = mlt_audio_calculate_frame_samples(static_cast<float>(profile.fps()), frequency,
                                                        static_cast<int64_t>(span.sourceIn + i));
        auto *pcm = static_cast<int16_t *>(frame->get_audio(format, frequency, channels, samples));
        if (!pcm || samples <= 0 || channels <= 0)
            continue;
        for (int s = 0; s < samples; ++s) {
            int mono = 0;
            for (int c = 0; c < channels; ++c)
                mono += pcm[s * channels + c];
            blockSum += std::abs(static_cast<double>(mono) / channels) / 32768.0;
            if (++blockCount == kSamplesPerBlock) {
                envelope.values.push_back(static_cast<float>(blockSum / kSamplesPerBlock));
                blockSum = 0.0;
                blockCount = 0;
            }
        }
    }
    // Silence throughout: a generator (color:) decodes to zeros rather
    // than reporting no audio stream, and nothing can be lined up by it.
    if (envelope.values.empty() ||
        std::all_of(envelope.values.begin(), envelope.values.end(), [](float v) { return v < 1e-6f; }))
        return std::nullopt;
    return envelope;
}

} // namespace ustudio::engine
