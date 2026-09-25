#pragma once

#include <string>

namespace ustudio::core {

// An asset file's identity for relink and cache keys (doc 07): its size
// and modification time in nanoseconds since the Unix epoch,
// "10485760:1757600000123456789" (doc 09's `ustudio:fingerprint`). "" when
// the file can't be read. Cheap (one stat), but it's IO: pool threads for
// imports.
std::string fileFingerprint(const std::string &path);

} // namespace ustudio::core
