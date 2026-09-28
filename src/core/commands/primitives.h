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

// Audit T1 (2026-09-22 follow-up): a dissolve Transition assumes its two
// clips keep the exact geometry AddTransition gave them -- removing,
// moving, resizing, or splitting either one without telling the
// transition leaves it dangling or pointing at the wrong span, which
// crashes the next right-click/drag on that row and makes the saved
// project fail to reload (Model::check() invariant 6/the overlap rule).
// RemoveClip, MoveClip, ResizeClip, SplitClip, and RemoveTrack below each
// strip any transition touching the clip(s) they're about to change
// FIRST (via Model::removeTransition, which un-extends both linked clips
// back to their pre-dissolve geometry) and restore it on revert (via
// Model::addTransition, which re-extends them) -- exactly the pairing
// RemoveTransition::apply/revert already uses, so a moved/resized/split/
// removed clip that had a dissolve simply loses it, the same outcome a
// manual "Remove Transition" then the edit would have produced, instead
// of corrupting the model. SplitAudio is deliberately NOT included: it
// never touches a clip's position/in/out (only its video/audio-enabled
// flags, plus inserting a new clip elsewhere for the extracted audio),
// so a transition on the original clip stays geometrically valid
// through it.
//
// The capture-order rule every one of these follows: `stripTransitions
// InvolvingClip` must run BEFORE capturing whatever "old geometry"
// revert() will restore. A linked clip's on-model geometry already
// includes the transition's extension; capturing it before stripping
// would save the EXTENDED values, and revert()'s later addTransition
// call would then extend them a second time. Capturing after stripping
// saves the base (un-extended) geometry, so restoreClip/resizeClip (base)
// followed by addTransition (re-extends once) reproduces the original
// state exactly -- the same reasoning RemoveTransition::revert already
// relies on for its own single clip pair.

// Points one clip at another asset (with that asset's source params),
// keeping its place, range, effects and transform: a title baked to a
// file (doc 16, "Bake title"). Refused on a locked track, or when the
// clip's range doesn't fit the asset.
class SetClipAsset : public Command
{
  public:
    SetClipAsset(ClipId clip, AssetId asset, std::vector<Param> params, std::string label = "Replace clip source");
    std::string label() const override
    {
        return m_label;
    }
    bool apply(Model &) override;
    void revert(Model &) override;

  private:
    ClipId m_clip;
    AssetId m_asset, m_oldAsset;
    std::vector<Param> m_params, m_oldParams;
    std::string m_label;
};

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

// Removes an asset from the project bin, along with every clip on every
// track that uses it (Model::check() flags a clip whose asset is missing
// as invariant 5, so the two can't be separated). Refuses the whole
// operation, not just some clips, if any referencing clip lives on a
// locked track -- same guard as RemoveClip's own, and for the same
// reason: a locked track refuses removal of its own clips regardless of
// what's driving the removal.
// Where a clip's picture sits (ADR-018). A drag sends one of these per
// pointer move with the same non-zero `gesture`; they merge into one undo
// step. A new gesture (or 0) never merges.
class SetClipTransform : public Command
{
  public:
    SetClipTransform(ClipId clip, Transform transform, uint64_t gesture = 0);
    std::string label() const override
    {
        return "Transform clip";
    }
    bool apply(Model &) override;
    void revert(Model &) override;
    bool mergeWith(const Command &next) override;
    bool isNoOp() const override
    {
        return m_transform == m_old;
    }

  private:
    ClipId m_clip;
    Transform m_transform, m_old;
    uint64_t m_gesture;
};

// Points an asset at another file (the relink dialog, doc 07): its path,
// fingerprint and status, nothing else, so clips, lengths and the rest of
// the project are exactly as they were. Whether the new file is long
// enough for the ranges in use is the caller's check (it needs a probe).
class RelinkAsset : public Command
{
  public:
    // `sequenceBegin`: an image sequence's first file number, when the
    // relinked copy is numbered from elsewhere.
    RelinkAsset(AssetId asset, std::string path, std::string fingerprint,
                std::optional<int> sequenceBegin = std::nullopt);
    std::string label() const override
    {
        return "Relink media";
    }
    bool apply(Model &) override;
    void revert(Model &) override;

  private:
    AssetId m_asset;
    std::string m_path, m_fingerprint;
    std::string m_oldPath, m_oldFingerprint;
    Asset::Status m_oldStatus = Asset::Status::Ready;
    std::optional<int> m_sequenceBegin;
    int m_oldSequenceBegin = 0;
};

class RemoveAsset : public Command
{
  public:
    explicit RemoveAsset(AssetId asset);
    std::string label() const override
    {
        return "Remove from project";
    }
    bool apply(Model &) override;
    void revert(Model &) override;

  private:
    AssetId m_asset;
    Asset m_capturedAsset;
    std::vector<Clip> m_capturedClips;
    // Audit C1: every transition any removed clip was part of, stripped
    // before capture (see this file's top-of-file comment) and restored
    // on revert -- without this, a dissolve's OTHER clip kept its
    // extension while the transition record vanished with the removed
    // one.
    std::vector<Transition> m_capturedTransitions;
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

// Captures the removed track (minus its clip list, restored separately),
// every clip that lived on it, and every transition on it (T1: a
// transition's clips are always on the transition's own track, so
// removing a track can never leave a transition referencing a clip on a
// DIFFERENT, still-live track), so revert() reconstructs all three
// exactly.
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
    std::vector<Transition> m_capturedTransitions;
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

// The sequence's size and rate, for a sequence with no clips yet (its
// first video import, doc 13 R7). With clips it refuses: their positions
// would need retiming (core/model/retime.h), a different command.
class SetSequenceProfile : public Command
{
  public:
    explicit SetSequenceProfile(Profile profile);
    std::string label() const override
    {
        return "Set project format";
    }
    bool apply(Model &) override;
    void revert(Model &) override;

  private:
    Profile m_profile;
    Profile m_oldProfile;
};

// The colour under every track (Sequence::background). Refuses the colour
// it already has.
class SetSequenceBackground : public Command
{
  public:
    explicit SetSequenceBackground(uint32_t rgb);
    std::string label() const override
    {
        return "Set background colour";
    }
    bool apply(Model &) override;
    void revert(Model &) override;

  private:
    uint32_t m_rgb;
    uint32_t m_oldRgb = Sequence::kDefaultBackground;
};

// The sequence's frame rate, with everything already on it moved to the
// same times at the new rate (core::retime(): clips, dissolves, fades,
// keyframes, markers, asset lengths). Refuses the rate it already has, and
// a result Model::check() rejects. Undo restores the sequence and bin as
// they were, exactly.
class ChangeSequenceFrameRate : public Command
{
  public:
    explicit ChangeSequenceFrameRate(Rational fps);
    std::string label() const override
    {
        return "Change project frame rate";
    }
    bool apply(Model &) override;
    void revert(Model &) override;

  private:
    Rational m_fps;
    Sequence m_oldSequence;
    std::vector<Asset> m_oldBin;
};

// Also not gated by Track::locked, for the same reason as SetTrackVolume
// above: a name is a label, not content.
class RenameTrack : public Command
{
  public:
    RenameTrack(TrackId track, std::string name);
    std::string label() const override
    {
        return "Rename track";
    }
    bool apply(Model &) override;
    void revert(Model &) override;

  private:
    TrackId m_track;
    std::string m_name;
    std::string m_oldName;
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
    // Audit C1 (2026-09-22): extendAssetLength() only grows the asset's
    // recorded length, so its own effect has no built-in inverse.
    // m_oldAssetLength/m_setAssetLength are captured around apply()'s
    // own extendAssetLength() call; revert() restores m_oldAssetLength
    // via Model::setAssetLength(), but ONLY when the asset's length is
    // still exactly m_setAssetLength (what THIS apply() set it to) --
    // if some other, later clip has since cut even further into the
    // same asset, that's the true current requirement, and reverting
    // this command must not shrink it out from under that other clip.
    bool m_extendedAsset = false;
    FrameIndex m_oldAssetLength = 0;
    FrameIndex m_setAssetLength = 0;
};

// Captures the full Clip at apply time so revert() restores every field,
// not just the ones InsertClip's forward path sets (doc 04). T1: also
// strips (and, on revert, restores) any transition touching the clip --
// see this file's own top-of-file comment.
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
    std::vector<Transition> m_capturedTransitions;
};

// T1: strips (and, on revert, restores) any transition touching the clip
// -- see this file's own top-of-file comment.
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
    std::vector<Transition> m_capturedTransitions;
};

// T1: strips (and, on revert, restores) any transition touching the clip
// -- see this file's own top-of-file comment.
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
    std::vector<Transition> m_capturedTransitions;
    // Audit C1 -- see InsertClip's own comment on these three.
    bool m_extendedAsset = false;
    FrameIndex m_oldAssetLength = 0;
    FrameIndex m_setAssetLength = 0;
};

// Implemented as doc 04 describes: apply splits into left (this clip,
// resized) + right (a new InsertClip-equivalent); revert removes the
// right half and restores the left's original `out`. Audit C4: a
// transition on this clip is only stripped if the split point actually
// falls inside ITS OWN overlap region (ambiguous which half it'd
// belong to) -- one far from either edge is left alone instead of
// unconditionally destroying both (T1's original, cruder rule). An
// incoming transition (this clip is `b`) needs no other change, since
// the left half keeps this clip's own id and head geometry; an
// outgoing one (this clip is `a`) is repointed (Model::
// retargetTransitionClip(), no geometry change) from this clip to the
// new right half once it exists, since the left half no longer reaches
// that edge.
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
    std::vector<EffectId> m_rightEffectIds; // the right half's effects, the same on redo
    bool m_appliedBefore = false;
    // Set only when an outgoing transition survived (wasn't stripped)
    // and was repointed from m_clip to m_rightId -- revert() repoints
    // it back before removing m_rightId, or that transition would be
    // left dangling on a clip about to disappear. Invalid (default
    // TransitionId) when there was none to repoint.
    TransitionId m_repointedOutgoingTransition;
    std::vector<Transition> m_capturedTransitions;
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

// Not gated by Track::locked, for the same reason as RenameTrack above.
class RenameClip : public Command
{
  public:
    RenameClip(ClipId clip, std::string name);
    std::string label() const override
    {
        return "Rename clip";
    }
    bool apply(Model &) override;
    void revert(Model &) override;

  private:
    ClipId m_clip;
    std::string m_name;
    std::string m_oldName;
};

// Creates a dissolve between two clips already adjacent on `track`
// (a.end() == b.position), by extending `a`'s out forward by `extendA`
// and/or pulling `b`'s in/position back by `extendB` -- see the
// Transition comment in types.h for why this never moves anything after
// `b`. Refuses if the clips aren't exactly adjacent, either side lacks
// the source-media handle it's asked to use, or the requested combined
// length would exceed either clip's own resulting length (the same bound
// Model::check() enforces on the result).
class AddTransition : public Command
{
  public:
    AddTransition(TrackId track, ClipId a, ClipId b, FrameIndex extendA, FrameIndex extendB);
    std::string label() const override
    {
        return "Add transition";
    }
    bool apply(Model &) override;
    void revert(Model &) override;
    TransitionId transitionId() const
    {
        return m_transitionId;
    }

  private:
    TrackId m_track;
    ClipId m_a, m_b;
    FrameIndex m_extendA, m_extendB;
    TransitionId m_transitionId;
    bool m_appliedBefore = false;
};

// Removes a dissolve, shrinking the two clips it linked back by exactly
// the handle each contributed at creation (Model::removeTransition is
// addTransition's exact inverse) -- correct regardless of how long ago,
// or through what other edits, the transition was created, since the
// split is read from the Transition record itself, not re-derived.
class RemoveTransition : public Command
{
  public:
    explicit RemoveTransition(TransitionId transition);
    std::string label() const override
    {
        return "Remove transition";
    }
    bool apply(Model &) override;
    void revert(Model &) override;

  private:
    TransitionId m_transition;
    Transition m_captured;
};

} // namespace ustudio::core
