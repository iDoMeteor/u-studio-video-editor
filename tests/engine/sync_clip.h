#pragma once

// The A/V sync test clip from doc 12's M2 box: black, with a one-frame
// white flash on frame 0 of every second and a one-frame 1 kHz beep on
// exactly the same frames, silence elsewhere. Rendered from MLT generators
// (no binary media in the repo) through the same avformat/libx264/AAC path
// renderProject() uses. Shared by tests/engine/test_av_sync.cpp and the
// make_sync_clip tool (for checking it by eye in the editor).

#include <mlt++/Mlt.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace ustudio::testing {

inline int framesPerSecond(Mlt::Profile &profile)
{
    return static_cast<int>(std::lround(profile.fps()));
}

// The beep track, written sample by sample: a 48 kHz stereo 16-bit WAV
// with a 1 kHz tone over exactly the samples of frame 0 of every second
// and zeros elsewhere. Built directly rather than from MLT's tone
// generator because a one-frame tone entry in an Mlt::Playlist renders
// silent (standalone repro, MLT 7.40, 2026-09-24: 1-frame entries silent at
// any in point, 2+ frames fine), and because writing it directly makes the
// intended alignment exact by construction.
inline void writeBeepWav(const std::string &path, int seconds, int fps)
{
    constexpr int kRate = 48000;
    constexpr int kChannels = 2;
    const int samplesPerFrame = kRate / fps;
    const int totalSamples = kRate * seconds;
    std::vector<int16_t> pcm(static_cast<size_t>(totalSamples) * kChannels, 0);
    for (int s = 0; s < seconds; ++s) {
        for (int i = 0; i < samplesPerFrame; ++i) {
            const double t = static_cast<double>(i) / kRate;
            const auto value = static_cast<int16_t>(16000.0 * std::sin(2.0 * std::numbers::pi * 1000.0 * t));
            const size_t at = static_cast<size_t>(s * kRate + i) * kChannels;
            pcm[at] = value;
            pcm[at + 1] = value;
        }
    }
    auto u32 = [](std::ofstream &out, uint32_t v) { out.write(reinterpret_cast<const char *>(&v), 4); };
    auto u16 = [](std::ofstream &out, uint16_t v) { out.write(reinterpret_cast<const char *>(&v), 2); };
    const uint32_t dataBytes = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
    std::ofstream out(path, std::ios::binary);
    out.write("RIFF", 4);
    u32(out, 36 + dataBytes);
    out.write("WAVEfmt ", 8);
    u32(out, 16);
    u16(out, 1); // PCM
    u16(out, kChannels);
    u32(out, kRate);
    u32(out, kRate * kChannels * 2);
    u16(out, kChannels * 2);
    u16(out, 16);
    out.write("data", 4);
    u32(out, dataBytes);
    out.write(reinterpret_cast<const char *>(pcm.data()), dataBytes);
}

// Returns consumer.run()'s result (0 on success; check the file exists too,
// see the README's render notes on avformat's return value).
inline int renderSyncClip(Mlt::Profile &profile, const std::string &path, int seconds)
{
    const int fps = framesPerSecond(profile);
    Mlt::Playlist video(profile);
    for (int s = 0; s < seconds; ++s) {
        Mlt::Producer white(profile, "color:white");
        white.set_in_and_out(0, 0);
        video.append(white);
        Mlt::Producer black(profile, "color:black");
        black.set_in_and_out(0, fps - 2);
        video.append(black);
    }

    const std::string wav = path + ".beep.wav";
    writeBeepWav(wav, seconds, fps);
    Mlt::Producer beep(profile, wav.c_str());
    beep.set_in_and_out(0, seconds * fps - 1);

    // Audio below video with composite + mix, the same shape EngineSync
    // gives an audio track under a video track.
    Mlt::Tractor tractor(profile);
    tractor.set_track(beep, 0);
    tractor.set_track(video, 1);
    std::unique_ptr<Mlt::Field> field(tractor.field());
    Mlt::Transition composite(profile, "composite");
    field->plant_transition(composite, 0, 1);
    Mlt::Transition mix(profile, "mix");
    mix.set("start", 1.0);
    mix.set("sum", 1);
    mix.set("always_active", 1);
    field->plant_transition(mix, 0, 1);

    Mlt::Consumer consumer(profile, "avformat", path.c_str());
    consumer.set("vcodec", "libx264");
    consumer.set("acodec", "aac");
    consumer.set("ar", 48000);
    consumer.set("channels", 2);
    consumer.set("real_time", -1);
    consumer.connect(tractor);
    int result = consumer.run();
    std::remove(wav.c_str());
    return result;
}

} // namespace ustudio::testing
