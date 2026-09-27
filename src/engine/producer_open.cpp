#include "engine/producer_open.h"

#include <mlt++/Mlt.h>

namespace ustudio::engine {

namespace {
constexpr const char *kHardwareDecodeQuery = "\\?hwaccel=";
}

std::unique_ptr<Mlt::Producer> openProducer(Mlt::Profile &profile, const std::string &resource, ProducerUse use)
{
    // nullptr is the default loader, which is what every site used before
    // ADR-019; both loaders attach the same CPU normalisers while no
    // glsl.manager exists.
    const char *service = use == ProducerUse::Worker ? "loader-nogl" : nullptr;
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
