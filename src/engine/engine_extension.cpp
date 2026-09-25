#include "engine/engine_extension.h"

#include <mutex>

namespace ustudio::engine {

namespace {
std::mutex s_mutex;
std::vector<EngineExtensionFactory> &factories()
{
    static std::vector<EngineExtensionFactory> list;
    return list;
}
} // namespace

void attachToCut(Mlt::Producer &cut, Mlt::Filter &filter)
{
    filter.set_in_and_out(cut.get_in(), cut.get_out());
    cut.attach(filter);
}

void registerEngineExtension(EngineExtensionFactory factory)
{
    std::lock_guard lock(s_mutex);
    factories().push_back(std::move(factory));
}

std::vector<std::unique_ptr<EngineExtension>> createEngineExtensions()
{
    std::lock_guard lock(s_mutex);
    std::vector<std::unique_ptr<EngineExtension>> extensions;
    for (const EngineExtensionFactory &factory : factories())
        if (std::unique_ptr<EngineExtension> extension = factory())
            extensions.push_back(std::move(extension));
    return extensions;
}

void clearEngineExtensions()
{
    std::lock_guard lock(s_mutex);
    factories().clear();
}

} // namespace ustudio::engine
