#pragma once

// One MLT filter as plain data: what the project writer puts in a <filter>
// and what the engine creates. Shared by the clip transform
// (transform.h, ADR-018) and effects (effect_native.h).

#include <string>
#include <utility>
#include <vector>

namespace ustudio::core {

struct NativeFilter
{
    std::string service;                                         // mlt_service
    std::vector<std::pair<std::string, std::string>> properties; // besides mlt_service, in order
};

} // namespace ustudio::core
