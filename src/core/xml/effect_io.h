#pragma once

// Effects and other IP1 data in the project file (format 5, IP2; doc 15,
// "Persistence"). Internal to core/xml: writer.cpp and reader.cpp share it.
//
// An effect is a <filter> with two sets of properties:
// - native ones MLT reads (mlt_service, each parameter by name -- a
//   keyframed one as an animation string -- and "disable"), so melt plays
//   it without the editor (ADR-004);
// - ustudio:* ones holding the whole model record (typed values, keyframes
//   with easing, mix, mask, owner), which the reader rebuilds from alone.
// Render cuts get native properties only; the record playlists, the
// sequence tractor, and the adjustment-block and look playlists carry both.

#include "core/model/types.h"

#include <libxml/tree.h>

#include <optional>
#include <string>
#include <vector>

namespace ustudio::core::xml_detail {

xmlNodePtr addProperty(xmlNodePtr parent, const std::string &name, const std::string &value);
std::optional<std::string> getProperty(xmlNodePtr node, const std::string &name);

// Native properties for one cut: `offset` frames into the owner, `length`
// long (keyframes shifted and clamped to it, core::keyframesForCut()).
void writeEffectFilter(xmlNodePtr parent, const Effect &effect, FrameIndex offset, FrameIndex length, bool withModel);
// Nullopt for a <filter> without ustudio:effect_id (a render-only copy).
std::optional<Effect> readEffectFilter(xmlNodePtr filter);
// Every model <filter> directly under `parent`, in order.
std::vector<Effect> readEffectFilters(xmlNodePtr parent);

// A list of parameters as indexed ustudio properties: `<prefix>count`,
// `<prefix>N.name`, `<prefix>N.value`, `<prefix>N.keyframes`.
void writeParams(xmlNodePtr parent, const std::string &prefix, const std::vector<Param> &params);
std::vector<Param> readParams(xmlNodePtr parent, const std::string &prefix);

} // namespace ustudio::core::xml_detail
