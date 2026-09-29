#pragma once

// What MLT runs for a transition (doc 15, "Transitions"): the one place
// that turns a core::Transition into MLT services, so the project writer
// (for melt) and EngineSync's dissolve builder play the same thing.
//
// A transition's recipe (the effects drop-in's wipes, dips and flashes) is
// resolved into Transition::params when it's applied, with prefixed names:
//   video.service, video.<prop>   the video transition (default: luma)
//   video.luma                    a generated wipe map's name (writeLumaMap())
//   audio.service, audio.<prop>   the audio transition (default: mix start=-1)
//   a.<n>.service, a.<n>.<prop>   filters on the outgoing clip's tail
//   b.<n>.service, b.<n>.<prop>   filters on the incoming clip's head
// Empty params are exactly the plain dissolve every project had before.
// A value "ramp:v0,v1,...,vn" becomes an animation of those values evenly
// spaced over the transition (so it follows a change of length).
//
// Pure data: the builder applies the sub-tractor's own rules (every
// transition's in/out is [0, length-1]; the Field is owned once) and
// resolves video.luma to a file.

#include "core/model/native_filter.h"
#include "core/model/types.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ustudio::core {

struct NativeTransition
{
    NativeFilter video;                   // service and properties (not in/out, not a_track/b_track)
    NativeFilter audio;
    std::vector<NativeFilter> tailFilters; // on A's tail, in order (a.0, a.1, ...)
    std::vector<NativeFilter> headFilters; // on B's head
    std::string luma;                      // video.luma: the map to set as the video's "resource"; "" for none
};

NativeTransition nativeTransition(const Transition &transition);

// Why a transition's params can't be played ("" when they can): an MLT
// service outside the allowlist, or a file property (project files are
// untrusted input; never a Qt service, ADR-007). An unknown wipe map is
// fine: it plays as the plain dissolve.
std::string transitionProblem(const Transition &transition);
// The services a transition's params may name.
bool transitionServiceAllowed(const std::string &service);

// The wipe maps' generator version: part of every map's path, so a map
// written by an older generator is never reused. Bump it whenever any map
// function changes.
inline constexpr int kLumaMapVersion = 1;
// Where `name`'s map lives under `folder`: <folder>/v<version>/<name>.pgm.
std::filesystem::path lumaMapPath(const std::filesystem::path &folder, const std::string &name);

// The generated wipe maps, by name ("left", "radial", "star", ...).
const std::vector<std::string> &lumaMapNames();
// `name`'s map as `width` x `height` samples (0 = where the incoming clip
// appears first, 65535 = last), row by row; empty for an unknown name. What
// writeLumaMap() writes, and what a picker draws a wipe's shape from.
std::vector<uint16_t> lumaMapPixels(const std::string &name, int width, int height);
// Writes `name`'s map to `file` (16-bit greyscale PGM, P5, maxval 65535; the
// incoming clip appears where the map is darkest first) unless it's there
// already. False for an unknown name or a write failure. Every map is
// procedural and deterministic, so projects carry no binary media.
bool writeLumaMap(const std::string &name, const std::filesystem::path &file, int width = 640, int height = 360);

} // namespace ustudio::core
