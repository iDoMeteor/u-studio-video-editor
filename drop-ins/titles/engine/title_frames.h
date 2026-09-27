#pragma once

// IP6: `u-studio-render --title-frames <project> <frame>...`. Builds the
// project's graph the way the editor's preview and export do (EngineSync,
// with every drop-in's extension) inside the render tool's process, and
// prints one JSON line with a hash of each frame's pixels:
//   {"width":1920,"height":1080,"frames":[{"frame":0,"hash":"9f0c..."}]}
// T1's check that a title plays the same in the editor and through the
// render tool (doc 16, T1 acceptance); M6 replaces it with real renders.

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace ustudio::titles {

int runTitleFrames(const std::vector<std::string> &args, std::ostream &out);

// FNV-1a over bytes, as the hashes above are printed (16 hex digits).
std::string frameHash(const uint8_t *data, size_t size);

} // namespace ustudio::titles
