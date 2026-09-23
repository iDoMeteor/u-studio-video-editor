#pragma once

#include "model_event.h"
#include "signal.h"
#include "types.h"

#include <optional>
#include <string>
#include <vector>

namespace ustudio::core {

// The source of truth for everything the user edits (ADR-003). Pure
// C++23: no GTK, no MLT. Mutators are the ONLY way to change state; each
// assumes the caller (a Command, doc 04) has already validated the
// operation -- Model itself does not refuse, it asserts on programmer
// error (missing id) and leaves invariant enforcement to check().
class Model
{
  public:
    static Model createEmpty(Profile profile = {});
    explicit Model(Project project);

    // Hand-written, not compiler-generated, because `changed` (a Signal)
    // is deliberately non-copyable/non-movable (audit C4): a copy or a
    // freshly move-constructed Model is a new value with its own, empty
    // `changed` -- nothing "comes along" from wherever the source Model's
    // data came from (a render thread's snapshot must never carry the
    // live EngineSync's subscription with it). Assignment, by contrast,
    // touches ONLY m_project and deliberately leaves the target's
    // existing `changed` (and whatever is already subscribed to it, e.g.
    // EngineSync's own connection) untouched -- "Open Project" relies on
    // exactly this to replace the live model's contents without losing
    // the engine's subscription out from under it.
    Model(const Model &other);
    Model &operator=(const Model &other);
    Model(Model &&other) noexcept;
    Model &operator=(Model &&other) noexcept;

    const Project &project() const
    {
        return m_project;
    }
    // Value equality on the project state (doc 04): what a revert-after-
    // apply property test checks -- ignores nothing, including ids.
    bool operator==(const Model &other) const
    {
        return m_project == other.m_project;
    }

    const Sequence &sequence() const;
    Sequence &mutableSequence();

    bool hasAsset(AssetId) const;
    const Asset &asset(AssetId) const;
    bool hasClip(ClipId) const;
    const Clip &clip(ClipId) const;
    bool hasTrack(TrackId) const;
    const Track &track(TrackId) const;
    bool hasTransition(TransitionId) const;
    const Transition &transition(TransitionId) const;
    // Pure query, no mutation: true if [start, end) is free of clips on
    // `trackId`, ignoring `ignoreClip` if given (a clip checking against
    // its own future position). Used by core/commands to validate before
    // apply(), per doc 04's dry-run-query pattern.
    bool isRangeFree(TrackId, FrameIndex start, FrameIndex end, std::optional<ClipId> ignoreClip = std::nullopt) const;

    // --- Mutators -----------------------------------------------------
    AssetId addAsset(Asset newAsset, std::optional<AssetId> reuseId = std::nullopt);
    void removeAsset(AssetId);
    // Raises info.lengthInSequenceFrames to `minimumLength` if it's
    // currently shorter; no-op (not even a notify) otherwise -- never
    // shrinks it. For a boundless asset (isBoundless()), the recorded
    // length is really just "how far any clip has asked to cut from it so
    // far" -- a still image or generator has no real fixed duration, so
    // InsertClip/ResizeClip call this with the clip's own new `out + 1` to
    // keep it truthful for both EngineSync (which sizes the underlying MLT
    // producer from it, doc 13's E3) and the saved project file. Like the
    // other mutators, asserts on an unknown id rather than refusing --
    // callers are expected to have already validated hasAsset().
    void extendAssetLength(AssetId, FrameIndex minimumLength);
    // Unconditional set, unlike extendAssetLength above -- restores an
    // exact previously captured length on revert() (audit C1: apply()'s
    // own extendAssetLength() call for a boundless asset had no inverse,
    // so undo of an import or resize left the asset's recorded length
    // extended, breaking command.h's "revert restores the model bit-for-
    // bit" contract). Same "asserts on an unknown id, callers validate
    // first" contract as every other mutator here.
    void setAssetLength(AssetId, FrameIndex length);

    TrackId addTrack(Track::Kind kind, size_t index, std::string name, std::optional<TrackId> reuseId = std::nullopt);
    void removeTrack(TrackId);
    void setTrackFlags(TrackId, bool muted, bool hidden, bool locked);
    // Reorders a track within the visual stack (drag-to-reorder in the
    // UI). `newIndex` is clamped to the current track count.
    void moveTrack(TrackId, size_t newIndex);
    // Linear scale (0 = silent, 1 = unity, >1 = boost); EngineSync converts
    // to the dB "level" a volume filter takes. Applies to the whole track
    // (doc 03: "audio tracks and the audio part of video tracks").
    void setTrackVolume(TrackId, double volume);
    // Deliberately NOT gated by Track::locked (matches setTrackVolume's own
    // comment): a track's name is a label, not content, so it stays
    // editable the same way toggling the lock itself does.
    void setTrackName(TrackId, std::string name);

    ClipId insertClip(TrackId, AssetId, FrameIndex pos, FrameIndex in, FrameIndex out,
                      std::optional<ClipId> reuseId = std::nullopt);
    void removeClip(ClipId);
    void moveClip(ClipId, TrackId, FrameIndex pos);
    void resizeClip(ClipId, FrameIndex newIn, FrameIndex newOut, FrameIndex newPos);
    // Splits at an absolute track position strictly inside the clip's span.
    // Returns the id of the new right-hand clip; the original clip (now the
    // left half) keeps its id.
    ClipId splitClip(ClipId, FrameIndex at, std::optional<ClipId> reuseRightId = std::nullopt);
    // Toggles which of a clip's media streams the engine actually plays
    // (EngineSync sets video_index/audio_index=-1 on the cut for whichever
    // is false). Used directly by SplitAudio's two sides -- the extracted
    // audio-only clip and the now video-only original -- but generic
    // enough for a future per-clip mute/hide toggle too.
    void setClipEnabled(ClipId, bool videoEnabled, bool audioEnabled);
    // Sets a clip's fade-out marker directly (std::nullopt = no fade).
    // Currently only SplitClip::revert uses this, to put back the fade a
    // split's own apply() clears from the clip that becomes its left half
    // (audit C5) -- narrow on purpose: there's no fade-editing UI/command
    // yet (doc 13), so this exists to restore a captured value, not to set
    // one from scratch.
    void setClipFadeOut(ClipId, std::optional<FadeSpec> fadeOut);
    // A clip's own label, independent of its asset's -- defaults to the
    // asset's displayName at insertClip() time (so two clips cut from the
    // same source start out looking the same, but can be told apart once
    // renamed). Empty string is a valid value ("no custom name"), not
    // refused. Not gated by Track::locked, for the same reason as
    // setTrackName above.
    void setClipName(ClipId, std::string name);

    // A dissolve between clips `a` (earlier) and `b` (later), already
    // adjacent (a.end() == b.position) on `track`. Grows `a.out` forward by
    // `extendA` and/or pulls `b.in`/`b.position` back by `extendB` (either
    // may be 0, but not both) using each clip's own existing source-media
    // handle room -- see the Transition comment in types.h for why this
    // never moves anything after `b`. The caller (AddTransition) has
    // already validated handle availability and adjacency; this mutator
    // asserts, it doesn't refuse.
    // addTransition/removeTransition are each other's exact inverse (both
    // derive the clip mutation from extendA/extendB, so a command's
    // revert() is just calling the other one with the same arguments/id --
    // see core/commands/primitives.h's AddTransition/RemoveTransition).
    TransitionId addTransition(TrackId track, ClipId a, ClipId b, FrameIndex extendA, FrameIndex extendB,
                               std::optional<TransitionId> reuseId = std::nullopt);
    void removeTransition(TransitionId);

    // Verbatim restore, used by Command::revert paths (core/commands) that
    // captured a full Clip/Track at apply time (e.g. RemoveClip, RemoveTrack)
    // -- unlike insertClip/addTrack, these don't derive any field, they just
    // put back exactly what was captured, under its original id. `track`
    // passed to restoreTrack should have an empty `clips` list; restoreClip
    // repopulates it as each of the track's clips is restored.
    void restoreClip(Clip clipToRestore);
    void restoreTrack(Track trackToRestore, size_t index);

    // Emitted synchronously, main thread only, after state is consistent.
    Signal<const ModelEvent &> changed;
    // Transaction (core/commands) uses this to wrap a group of primitive
    // mutations in BatchBegin/BatchEnd; Model has no batching logic itself.
    void notify(ModelEvent event)
    {
        changed.emit(event);
    }

    // Cheap enough for debug builds after every command (doc 03).
    std::vector<std::string> check() const;

  private:
    Project m_project;

    uint64_t allocateId();
    Sequence &activeSequence();
    const Sequence &activeSequence() const;
    Track &mutableTrack(TrackId);
    Clip &mutableClip(ClipId);
    void sortTrackClips(Track &track);
    void reserveId(uint64_t value);
};

} // namespace ustudio::core
