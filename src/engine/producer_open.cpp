#include "engine/producer_open.h"

#include <mlt++/Mlt.h>

namespace ustudio::engine {

namespace {
constexpr const char *kHardwareDecodeQuery = "\\?hwaccel=";
// 0: no build on this thread (the CPU chain), 1: CPU, 2: GPU.
thread_local int t_graphBuild = 0;
} // namespace

GraphBuildScope::GraphBuildScope(bool gpu) : m_previous(t_graphBuild)
{
    t_graphBuild = gpu ? 2 : 1;
}

GraphBuildScope::~GraphBuildScope()
{
    t_graphBuild = m_previous;
}

std::unique_ptr<Mlt::Producer> openProducer(Mlt::Profile &profile, const std::string &resource, ProducerUse use)
{
    if (use == ProducerUse::Graph)
        use = t_graphBuild == 2 ? ProducerUse::GpuGraph : ProducerUse::CpuGraph;
    // The default loader gives movit's normalisers while a glsl.manager
    // exists; loader-nogl, named as the service, never does. Both attach
    // the same CPU normalisers otherwise (docs/developer/notes/gpu.md).
    const char *service = use == ProducerUse::GpuGraph ? "loader" : "loader-nogl";
    return std::make_unique<Mlt::Producer>(profile, service, resource.c_str());
}

std::string withHardwareDecode(const std::string &path, const std::string &api)
{
    if (api.empty())
        return path;
    return path + kHardwareDecodeQuery + api;
}

std::string withoutHardwareDecode(const std::string &resource)
{
    const size_t at = resource.rfind(kHardwareDecodeQuery);
    return at == std::string::npos ? resource : resource.substr(0, at);
}

} // namespace ustudio::engine
