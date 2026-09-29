#pragma once

// Animated layers (doc 16 T6, ADR-021): a Lottie file's frame drawn by
// ThorVG into a Cairo surface. The only code in the titles drop-in that
// calls ThorVG. Every file passes lottie::check() before ThorVG sees it,
// and every ThorVG call holds one process-wide lock (ThorVG 1.0.6 crashes
// on concurrent threads even with separate objects: notes/titles.md, T6).
// Built without ThorVG, an animated layer draws nothing and warns.

#include "core/lottie_check.h"

#include <cairo.h>

#include <optional>
#include <set>
#include <string>

namespace ustudio::titles {

// The checked facts of the animation at `path` (cached per thread by path
// and modification time), or nullopt and a warning.
std::optional<lottie::Facts> lottieFacts(const std::string &path, const std::string &shownName,
                                         std::set<std::string> &warnings);

// The animation at `path` at frame `frame` (lottie::frameAt()), drawn into
// a new `width` x `height` ARGB32 surface (the caller owns it), or null and
// a warning. The animation fills the surface: the caller keeps its aspect.
cairo_surface_t *lottieFrame(const std::string &path, const std::string &shownName, double frame, int width, int height,
                             std::set<std::string> &warnings);

} // namespace ustudio::titles
