#pragma once

// Captions (doc 16, T5): subtitle files (.srt, .vtt) read into cues, cues
// placed on sequence frames, and the one command that imports them as title
// clips. Std only: a subtitle file is untrusted input, read here with the
// limits doc 16 gives.

#include "core/commands/command.h"
#include "core/model/model.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ustudio::titles::captions {

constexpr size_t kMaxBytes = 10u << 20;
constexpr size_t kMaxCues = 20000;
constexpr int64_t kMaxMs = 24ll * 3600 * 1000;

struct Cue
{
    int64_t startMs = 0, endMs = 0;
    // The words, UTF-8, lines joined with '\n'; styled with <b>, <i>, <u>
    // and <c.name> only (what a tags="basic" text layer draws), entities
    // decoded.
    std::string text;
    std::string speaker; // VTT <v Speaker>
    int line = 0;        // where the cue starts in the file (1-based)
    bool top = false;    // placed at the top (VTT line:, SRT {\an8}; T5.2)
};

// T5.2: WebVTT's eight default colour classes, the only colours a caption
// keeps (as <c.name>...</c> in its words). The colour's "#rrggbb" for a
// name, nullptr for anything else.
const char *captionColourHex(std::string_view name);

// `text` without the basic tags a caption keeps (<b>, <i>, <u>, <c.name>
// and their ends): its words, for names and emptiness checks.
std::string captionWords(const std::string &text);

// The name a caption clip gets from its words: their first line, tags gone.
// SetClipFields keeps a caption clip's name following its words while the
// name is still this (the user hasn't renamed the clip).
std::string captionName(const std::string &caption);

struct Parsed
{
    std::vector<Cue> cues;
    size_t skipped = 0;
    int firstSkippedLine = 0; // 0: none
    std::string firstSkippedWhy;
    std::vector<std::string> warnings; // "read as Windows-1252", ...
};

// A subtitle file's bytes: VTT when it starts with "WEBVTT" (after any BOM),
// else SRT. Refused with a reason for doc 16's cases: over the size or cue
// limits, binary content, no usable cue.
std::expected<Parsed, std::string> parse(std::string_view bytes);

// The file's text as UTF-8: UTF-8 (a BOM dropped), UTF-16 with a BOM, else
// Windows-1252 (and a warning). Empty `error` when it's text.
std::string toUtf8(std::string_view bytes, std::string &warning, std::string &error);

// A cue on the timeline: frames at the sequence's rate, and its lane (0 is
// the "Captions" track, 1 "Captions 2", ...): no lane has two cues overlap.
struct Placed
{
    core::FrameIndex position = 0, length = 1;
    size_t lane = 0;
    const Cue *cue = nullptr;
};

// Rounds each time to the nearest frame at `fps` exactly; every cue at least
// one frame; cues that share a time share the frame.
std::vector<Placed> place(const std::vector<Cue> &cues, core::Rational fps);
core::FrameIndex frameAt(int64_t ms, core::Rational fps);

// --- Export (T5.1) -------------------------------------------------------------

enum class Format
{
    Srt,
    Vtt,
};

struct ExportCue
{
    core::FrameIndex start = 0, end = 0; // end: the frame after the last
    std::string text, speaker;
    bool top = false; // the clip's field placement=top
};

// The project's captions: every clip playing a title with a `caption`
// field, on any track, by start (then track order, top first).
std::vector<ExportCue> captionCues(const core::Model &model);

// A frame's time, in milliseconds rounded to the nearest (frameAt()'s
// inverse: frameAt(msAt(f)) == f for any rate up to 1000 fps).
int64_t msAt(core::FrameIndex frame, core::Rational fps);

// The cues as a subtitle file, UTF-8 with \n line ends.
std::string writeSubtitles(const std::vector<ExportCue> &cues, core::Rational fps, Format format);

// The format a file name asks for: .vtt (any case) is VTT, anything else SRT.
Format formatFor(const std::string &path);

// Atomically: a temporary file next to `path`, then a rename over it.
// Empty on success, else the reason.
std::string saveSubtitles(const std::vector<ExportCue> &cues, core::Rational fps, const std::string &path);

// One undo step: the caption title's asset, a track per lane (above the
// others), and a clip per cue with its fields (caption, and speaker when it
// has one). Refused, leaving the model as it was, when any step is.
class ImportCaptions : public core::Command
{
  public:
    // `topTitleAsset`: the title top cues play (T5.2); without one they
    // play `titleAsset`. Either way their clips get placement=top.
    ImportCaptions(core::Asset titleAsset, std::vector<Placed> placed, std::string name,
                   std::optional<core::Asset> topTitleAsset = std::nullopt);
    std::string label() const override
    {
        return "Import " + m_name;
    }
    bool apply(core::Model &model) override;
    void revert(core::Model &model) override;

    // After apply: what was made.
    core::AssetId assetId() const
    {
        return m_asset;
    }
    const std::vector<core::TrackId> &tracks() const
    {
        return m_tracks;
    }
    const std::vector<core::ClipId> &clips() const
    {
        return m_clips;
    }

  private:
    core::Asset m_titleAsset;
    std::optional<core::Asset> m_topTitleAsset;
    std::vector<Placed> m_placed;
    std::vector<Cue> m_cues; // owned copies (Placed points into them)
    std::string m_name;
    std::vector<std::unique_ptr<core::Command>> m_done; // applied steps, in order
    core::AssetId m_asset;
    std::vector<core::TrackId> m_tracks;
    std::vector<core::ClipId> m_clips;
};

} // namespace ustudio::titles::captions
