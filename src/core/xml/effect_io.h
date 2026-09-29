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

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ustudio::core::xml_detail {

xmlNodePtr addProperty(xmlNodePtr parent, const std::string &name, const std::string &value);
std::optional<std::string> getProperty(xmlNodePtr node, const std::string &name);

// Native properties for one cut: `offset` frames into the owner, `length`
// long (keyframes shifted and clamped to it, core::keyframesForCut()).
// `cutIn`, when given, is the cut's first source frame: MLT counts a cut
// filter's animation from its "in" (engine::attachToCut() says why), so the
// filter gets in/out = the cut's.
void writeEffectFilter(xmlNodePtr parent, const Effect &effect, FrameIndex offset, FrameIndex length, bool withModel,
                       std::optional<FrameIndex> cutIn = std::nullopt);
// Nullopt for a <filter> without ustudio:effect_id (a render-only copy).
std::optional<Effect> readEffectFilter(xmlNodePtr filter);
// Every model <filter> directly under `parent`, in order.
std::vector<Effect> readEffectFilters(xmlNodePtr parent);

// The format version a save writes: the lowest that holds everything in it.
// A record an older build would misread (and lose on its next save) raises
// it, so that build refuses the file instead; a project without such
// records stays readable by it. Outside a save, raising does nothing.
class FormatVersionScope
{
  public:
    explicit FormatVersionScope(int base);
    ~FormatVersionScope();
    FormatVersionScope(const FormatVersionScope &) = delete;
    FormatVersionScope &operator=(const FormatVersionScope &) = delete;
    int needed() const;

  private:
    int *m_previous;
    int m_needed;
};
// 7: clip transform keyframes (older builds drop them).
void requireFormatVersion(int version);

// Keyframes with their easing, for ustudio:* properties:
// "at:value:easing;..." (easing as its number).
std::string encodeKeyframes(const std::vector<Keyframe> &keyframes);
std::vector<Keyframe> decodeKeyframes(const std::string &text);

// A list of parameters as indexed ustudio properties: `<prefix>count`,
// `<prefix>N.name`, `<prefix>N.value`, `<prefix>N.keyframes`.
// While a project is saved or loaded on this thread: its folder. A string
// value naming a file inside it (a LUT in its luts folder) is saved relative
// ("f:luts/grade.cube") and loaded against the folder the project is in
// now, so a moved project keeps its files; the model always holds the
// absolute path (MLT's filters open it themselves).
class ProjectFolderScope
{
  public:
    explicit ProjectFolderScope(const std::filesystem::path &folder);
    ~ProjectFolderScope();
    ProjectFolderScope(const ProjectFolderScope &) = delete;
    ProjectFolderScope &operator=(const ProjectFolderScope &) = delete;

  private:
    std::optional<std::filesystem::path> m_previous;
};

void writeParams(xmlNodePtr parent, const std::string &prefix, const std::vector<Param> &params);
std::vector<Param> readParams(xmlNodePtr parent, const std::string &prefix);

} // namespace ustudio::core::xml_detail
