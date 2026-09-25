// core::audio::align (the "Sync tracks (audio)" matcher): synthetic
// envelopes with known offsets.

#include "doctest.h"

#include "core/audio/align.h"

#include <cmath>
#include <random>

using namespace ustudio::core::audio;

namespace {

// 20 s of irregular bursts, 1 ms blocks: loud stretches of random length at
// random gaps, like speech or music, so exactly one shift lines it up.
std::vector<float> bursts(unsigned seed, size_t ms = 20'000)
{
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> gap(40, 700), burst(30, 400);
    std::uniform_real_distribution<float> level(0.2f, 1.0f), noise(0.0f, 0.02f);
    std::vector<float> v(ms, 0.0f);
    size_t i = 0;
    while (i < ms) {
        i += static_cast<size_t>(gap(rng));
        float l = level(rng);
        for (int k = burst(rng); k > 0 && i < ms; --k, ++i)
            v[i] = l;
    }
    for (float &x : v)
        x += noise(rng);
    return v;
}

} // namespace

TEST_CASE("audio align: finds the shift that lines up two recordings of the same sound")
{
    // The same 20 s of sound; clip b starts 3 s into it but sits on the
    // timeline 237 ms later than it should.
    std::vector<float> sound = bursts(7);
    Envelope a{0.0, sound};
    Envelope b{3'000.0 + 237.0, std::vector<float>(sound.begin() + 3'000, sound.begin() + 15'000)};
    auto result = align(a, b, 5'000.0, 2'000.0);
    REQUIRE(result);
    CHECK(std::abs(result->shiftMs - (-237.0)) <= 1.0);
    CHECK(result->confident);
}

TEST_CASE("audio align: a quieter, noisier recording of the same sound still matches")
{
    std::vector<float> sound = bursts(11);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> hiss(0.0f, 0.08f);
    std::vector<float> other(sound.begin() + 2'000, sound.begin() + 12'000);
    for (float &x : other)
        x = 0.3f * x + hiss(rng); // another mic: lower level, more noise
    Envelope a{0.0, sound};
    Envelope b{2'000.0 - 1'480.0, other}; // placed 1.48 s early
    auto result = align(a, b, 5'000.0, 2'000.0);
    REQUIRE(result);
    CHECK(std::abs(result->shiftMs - 1'480.0) <= 2.0);
    CHECK(result->confident);
}

TEST_CASE("audio align: a sound that repeats every second is reported, not trusted")
{
    // Identical pulses every 1000 ms: shifts a second apart look the same.
    std::vector<float> pulses(20'000, 0.0f);
    for (size_t i = 0; i < pulses.size(); ++i)
        pulses[i] = (i % 1'000) < 100 ? 1.0f : 0.0f;
    Envelope a{0.0, pulses};
    Envelope b{300.0, std::vector<float>(pulses.begin(), pulses.begin() + 10'000)};
    auto result = align(a, b, 5'000.0, 2'000.0);
    REQUIRE(result);
    CHECK_FALSE(result->confident);
}

TEST_CASE("audio align: clips that can't overlap within the search range give nothing")
{
    std::vector<float> sound = bursts(5, 5'000);
    Envelope a{0.0, sound};
    Envelope b{60'000.0, sound}; // a minute later, searching +/- 5 s
    CHECK_FALSE(align(a, b, 5'000.0, 2'000.0));
}
