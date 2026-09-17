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

    const Project &project() const
    {
        return m_project;
    }
    const Sequence &sequence() const;
    Sequence &mutableSequence();

    bool hasAsset(AssetId) const;
    const Asset &asset(AssetId) const;
    bool hasClip(ClipId) const;
    const Clip &clip(ClipId) const;
    bool hasTrack(TrackId) const;
    const Track &track(TrackId) const;

    // --- Mutators -----------------------------------------------------
    AssetId addAsset(Asset newAsset, std::optional<AssetId> reuseId = std::nullopt);
    void removeAsset(AssetId);

    TrackId addTrack(Track::Kind kind, size_t index, std::string name, std::optional<TrackId> reuseId = std::nullopt);
    void removeTrack(TrackId);
    void setTrackFlags(TrackId, bool muted, bool hidden, bool locked);

    ClipId insertClip(TrackId, AssetId, FrameIndex pos, FrameIndex in, FrameIndex out,
                      std::optional<ClipId> reuseId = std::nullopt);
    void removeClip(ClipId);
    void moveClip(ClipId, TrackId, FrameIndex pos);
    void resizeClip(ClipId, FrameIndex newIn, FrameIndex newOut, FrameIndex newPos);
    // Splits at an absolute track position strictly inside the clip's span.
    // Returns the id of the new right-hand clip; the original clip (now the
    // left half) keeps its id.
    ClipId splitClip(ClipId, FrameIndex at, std::optional<ClipId> reuseRightId = std::nullopt);

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
