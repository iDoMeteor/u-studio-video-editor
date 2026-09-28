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
    // The words, UTF-8, lines joined with '\n'; styled with <b>, <i> and <u>
    // only (what a tags="basic" text layer draws), entities decoded.
    std::string text;
    std::string speaker; // VTT <v Speaker>
    int line = 0;        // where the cue starts in the file (1-based)
};

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

// One undo step: the caption title's asset, a track per lane (above the
// others), and a clip per cue with its fields (caption, and speaker when it
// has one). Refused, leaving the model as it was, when any step is.
class ImportCaptions : public core::Command
{
  public:
    ImportCaptions(core::Asset titleAsset, std::vector<Placed> placed, std::string name);
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
    std::vector<Placed> m_placed;
    std::vector<Cue> m_cues; // owned copies (Placed points into them)
    std::string m_name;
    std::vector<std::unique_ptr<core::Command>> m_done; // applied steps, in order
    core::AssetId m_asset;
    std::vector<core::TrackId> m_tracks;
    std::vector<core::ClipId> m_clips;
};

} // namespace ustudio::titles::captions
