#pragma once

// ADR-017: see process.h.

#include <ctime>

namespace ustudio::platform {

// `when` in the user's local time zone. Thread-safe (std::localtime isn't).
std::tm localTime(std::time_t when);

} // namespace ustudio::platform
