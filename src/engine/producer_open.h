#pragma once

// Opens every MLT producer the editor makes (ADR-019, point 4). Two things
// are process-wide in MLT and must not leak into worker producers:
// - once a glsl.manager exists, the default loader gives every producer
//   loaded afterwards the movit (GPU) normalisers, even on a thread with no
//   GL context. Only the "loader-nogl" *service* keeps the CPU chain (the
//   "loader-nogl:" resource prefix gets both chains);
// - hardware decode, if switched on with MLT_AVFORMAT_HWACCEL, applies to
//   every avformat producer. A per-producer `\?hwaccel=` query does not.
// docs/developer/notes/gpu.md has the repros.

#include <memory>
#include <string>

namespace Mlt {
class Producer;
class Profile;
} // namespace Mlt

namespace ustudio::engine {

enum class ProducerUse
{
    Live,   // the playback or export graph
    Worker, // thumbnails, waveforms, probes, audio sync: always CPU, software decode
};

// `resource` as the loader takes it. A Worker producer ignores any GPU state.
std::unique_ptr<Mlt::Producer> openProducer(Mlt::Profile &profile, const std::string &resource, ProducerUse use);

// `path` with avformat's per-producer hardware-decode query appended:
// "<path>\?hwaccel=<api>". For a plain file avformat only splits a query at
// an escaped `\?` (parse_url() in producer_avformat.c), so a literal '?' in
// a file name stays part of it. An empty `api` returns `path` unchanged.
std::string withHardwareDecode(const std::string &path, const std::string &api);

// A producer's resource with that query removed (EngineSync::verify()
// compares resources with asset paths).
std::string withoutHardwareDecode(const std::string &resource);

} // namespace ustudio::engine
