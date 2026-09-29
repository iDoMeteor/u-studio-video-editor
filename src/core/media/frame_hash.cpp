#include "core/media/frame_hash.h"

#include <cstdio>

namespace ustudio::core {

std::string frameHash(const uint8_t *data, size_t size)
{
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

} // namespace ustudio::core
