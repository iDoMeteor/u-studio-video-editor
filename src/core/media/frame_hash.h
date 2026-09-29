#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ustudio::core {

// A frame's pixels as 16 hex digits (64-bit FNV-1a over the bytes): what
// `u-studio-render --frames` and the titles drop-in's `--title-frames` print,
// so a render can be checked against the editor's preview hash for hash.
std::string frameHash(const uint8_t *data, size_t size);

} // namespace ustudio::core
