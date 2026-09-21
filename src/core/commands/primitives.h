#pragma once

#include "command.h"
#include "core/model/model.h"

#include <optional>
#include <string>

namespace ustudio::core {

// One primitive command per Model mutator (doc 04's primitive-commands
// table). Composite commands (RippleDelete, InsertAt, ...) are built from
// these inside a Transaction; none are needed until the multi-clip
// timeline UI (M3), so they're not implemented yet.

class AddAsset : public Command
{
  public:
    explicit AddAsset(Asset asset);
    std::string label() const override
    {
        return "Add asset";
    }
    bool apply(Model &) override;
    void revert(Model &) override;
    AssetId assetId() const
    {
        return m_assetId;
    }

  private:
    Asset m_asset;
    AssetId m_assetId;
    bool m_appliedBefore = false;
};

class AddTrack : public Command
{
  public:
    AddTrack(Track::Kind kind, size_t index, std::string name);
    std::string label() const override
    {
        return "Add track";
    }
    bool apply(Model &) override;
    void revert(Model &) override;
    TrackId trackId() const
    {
        return m_trackId;
    }

  private:
    Track::Kind m_kind;
    size_t m_index;
    std::string m_name;
    TrackId m_trackId;
    bool m_appliedBefore = false;
};

// Captures the removed track (minus its clip list, restored separately)
// and every clip that lived on it, so revert() reconstructs both exactly.
class RemoveTrack : public Command
{
  public:
    explicit RemoveTrack(TrackId track);
    std::string label() const override
    {
        return "Remove track";
    }
    bool apply(Model &) override;
    void revert(Model &) override;

  private:
    TrackId m_track;
    Track m_capturedTrack;
    size_t m_capturedIndex = 0;
    std::vector<Clip> m_capturedClips;
};

class MoveTrack : public Command
{
  public:
    MoveTrack(TrackId track, size_t newIndex);
    std::string label() const override
    {
        return "Reorder track";
    }
    bool apply(Model &) override;
    void revert(Model &) override;

  private:
    TrackId m_track;
    size_t m_newIndex;
    size_t m_oldIndex = 0;
};

class SetTrackFlags : public Command
{
  public:
    SetTrackFlags(TrackId track, bool muted, bool hidden, bool locked);
    std::string label() const override
    {
        return "Set track flags";
    }
    bool apply(Model &) override;
    void revert(Model &) override;

  private:
    TrackId m_track;
    bool m_muted, m_hidden, m_locked;
    bool m_oldMuted = false, m_oldHidden = false, m_oldLocked = false;
};

// Deliberately NOT gated by Track::locked (unlike every clip-content
// command below): locking a track must stay reversible from the same UI
// that set it, and toggling mute/hide/lock is a track-level setting, not
// a content edit.
class SetTrackVolume : public Command
{
  public:
    SetTrackVolume(TrackId track, double volume);
    std::string label() const override
    {
        return "Set track volume";
    }
    bool apply(Model &) override;
    void revert(Model &) override;
    // Coalesces a volume-slider drag into one undo step (doc 04's
    // SetParam precedent: "mergeable while dragging").
    bool mergeWith(const Command &next) override;

  private:
    TrackId m_track;
    double m_volume;
    double m_oldVolume = 1.0;
};

class InsertClip : public Command
{
  public:
    InsertClip(TrackId track, AssetId asset, FrameIndex pos, FrameIndex in, FrameIndex out);
    std::string label() const override
    {
        return "Insert clip";
    }
    bool apply(Model &) override;
    void revert(Model &) override;
    ClipId clipId() const
    {
        return m_clipId;
    }

  private:
    TrackId m_track;
    AssetId m_asset;
    FrameIndex m_pos, m_in, m_out;
    ClipId m_clipId;
    bool m_appliedBefore = false;
};

// Captures the full Clip at apply time so revert() restores every field,
// not just the ones InsertClip's forward path sets (doc 04).
class RemoveClip : public Command
{
  public:
    explicit RemoveClip(ClipId clip);
    std::string label() const override
    {
        return "Remove clip";
    }
    bool apply(Model &) override;
    void revert(Model &) override;

  private:
    ClipId m_clip;
    Clip m_captured;
};

class MoveClip : public Command
{
  public:
    MoveClip(ClipId clip, TrackId newTrack, FrameIndex newPos);
    std::string label() const override
    {
        return "Move clip";
    }
    bool apply(Model &) override;
    void revert(Model &) override;

  private:
    ClipId m_clip;
    TrackId m_newTrack;
    FrameIndex m_newPos;
    TrackId m_oldTrack;
    FrameIndex m_oldPos = 0;
    bool m_oldVideoEnabled = true;
};

class ResizeClip : public Command
{
  public:
    ResizeClip(ClipId clip, FrameIndex newIn, FrameIndex newOut, FrameIndex newPos);
    std::string label() const override
    {
        return "Resize clip";
    }
    bool apply(Model &) override;
    void revert(Model &) override;

  private:
    ClipId m_clip;
    FrameIndex m_newIn, m_newOut, m_newPos;
    FrameIndex m_oldIn = 0, m_oldOut = 0, m_oldPos = 0;
};

// Implemented as doc 04 describes: apply splits into left (this clip,
// resized) + right (a new InsertClip-equivalent); revert removes the
// right half and restores the left's original `out`.
class SplitClip : public Command
{
  public:
    SplitClip(ClipId clip, FrameIndex at);
    std::string label() const override
    {
        return "Split clip";
    }
    bool apply(Model &) override;
    void revert(Model &) override;
    ClipId rightId() const
    {
        return m_rightId;
    }

  private:
    ClipId m_clip;
    FrameIndex m_at;
    FrameIndex m_oldOut = 0;
    std::optional<FadeSpec> m_oldFadeOut;
    ClipId m_rightId;
    bool m_appliedBefore = false;
};

// Pulls a clip's audio out to a new, independent clip on an audio track
// (doc 04, doc 06's clip context menu, doc 13 Q2's decided design: a
// normal clip carries its own audio; "split audio" is the explicit
// escape hatch that separates them into two ordinary clips with no
// runtime link -- each can be moved/trimmed independently afterward,
// which is the entire point). Refuses if the clip has no audio to split
// (audioEnabled already false) or is already video-less (videoEnabled
// already false, i.e. it's already an audio-only clip).
//
// Reuses the first existing audio track with room for this clip's exact
// span (in model track order), creating a new one at the end only if
// none has room -- "creating one if needed", doc 04. revert() removes
// only the extracted clip and restores the original's audioEnabled; a
// track created by apply() is deliberately left in place afterward, per
// doc 04's own revert entry for this command (it does not mention
// removing a track), and because doing otherwise would fail if the user
// dropped another clip onto that track before undoing.
class SplitAudio : public Command
{
  public:
    explicit SplitAudio(ClipId clip);
    std::string label() const override
    {
        return "Split audio";
    }
    bool apply(Model &) override;
    void revert(Model &) override;
    ClipId audioClipId() const
    {
        return m_audioClipId;
    }

  private:
    ClipId m_clip;
    ClipId m_audioClipId;
    bool m_appliedBefore = false;
};

} // namespace ustudio::core
