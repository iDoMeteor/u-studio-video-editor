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

// A CPU graph's producers must stay on the CPU chain even while a GPU
// session lives in the process (an export on a pool thread while the
// preview plays on the GPU, or the preview switched back to the CPU while an
// export still holds the session): only a GPU graph's producers go through
// the default loader.
enum class ProducerUse
{
    Worker,   // thumbnails, waveforms, probes, audio sync: always CPU, software decode
    CpuGraph, // a graph on the CPU pipeline
    GpuGraph, // a graph on the GPU pipeline (ADR-019)
    Graph,    // whichever pipeline this thread is building (GraphBuildScope); the CPU outside one.
              // For drop-ins' producers (EngineExtension::makeProducer()).
};

// `resource` as the loader takes it.
std::unique_ptr<Mlt::Producer> openProducer(Mlt::Profile &profile, const std::string &resource, ProducerUse use);

// While alive, ProducerUse::Graph on this thread means `gpu ? GpuGraph :
// CpuGraph`. EngineSync holds one around every call into a drop-in that
// may open a producer.
class GraphBuildScope
{
  public:
    explicit GraphBuildScope(bool gpu);
    ~GraphBuildScope();
    GraphBuildScope(const GraphBuildScope &) = delete;
    GraphBuildScope &operator=(const GraphBuildScope &) = delete;

  private:
    int m_previous;
};

// `path` with avformat's per-producer hardware-decode query appended:
// "<path>\?hwaccel=<api>". For a plain file avformat only splits a query at
// an escaped `\?` (parse_url() in producer_avformat.c), so a literal '?' in
// a file name stays part of it. An empty `api` returns `path` unchanged.
std::string withHardwareDecode(const std::string &path, const std::string &api);

// A producer's resource with that query removed (EngineSync::verify()
// compares resources with asset paths).
std::string withoutHardwareDecode(const std::string &resource);

} // namespace ustudio::engine
