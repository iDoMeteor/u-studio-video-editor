#pragma once

#include "ids.h"

#include <cstddef>
#include <string>
#include <variant>

namespace ustudio::core {

struct ClipInserted
{
    ClipId clip;
};
struct ClipRemoved
{
    ClipId clip;
    TrackId track;
};
struct ClipMoved
{
    ClipId clip;
    TrackId from;
    TrackId to;
};
struct ClipResized
{
    ClipId clip;
};
struct ClipFlagsChanged
{
    ClipId clip;
};
struct ClipRenamed
{
    ClipId clip;
};
// A clip's placement in the frame changed (ADR-018).
struct ClipTransformChanged
{
    ClipId clip;
};
struct TrackAdded
{
    TrackId track;
};
struct TrackRemoved
{
    TrackId track;
    size_t index;
};
struct TrackFlagsChanged
{
    TrackId track;
};
struct TrackRenamed
{
    TrackId track;
};
struct TrackVolumeChanged
{
    TrackId track;
};
struct TrackReordered
{
    TrackId track;
};
// An effect added, removed, moved, enabled or disabled, or its mask
// changed: the graph's shape changes.
struct EffectChanged
{
    EffectId effect;
};
// One parameter (or "mix") of an effect changed value: the engine may apply
// it to the live filter without a rebuild (doc 15, IP3's applyInPlace).
struct EffectParamChanged
{
    EffectId effect;
    std::string param;
};
struct AdjustmentBlockChanged
{
    AdjustmentBlockId block;
};
// A look added to or removed from the bin (never reaches MLT).
struct LooksChanged
{};
// A clip's sourceParams changed (a drop-in-generated clip, e.g. a title).
struct ClipSourceChanged
{
    ClipId clip;
};
struct TransitionAdded
{
    TransitionId transition;
};
struct TransitionRemoved
{
    TransitionId transition;
    TrackId track;
};
// Emitted by Model::retargetTransitionClip() (audit C4: repointing a
// transition's `a`/`b` to a different clip object, e.g. after a split,
// without touching either clip's geometry).
struct TransitionChanged
{
    TransitionId transition;
};
struct AssetChanged
{
    AssetId asset;
};
struct SequenceProfileChanged
{};
struct SequenceBackgroundChanged
{};
// A marker was added, removed, or changed. Markers don't reach MLT, so
// EngineSync ignores this one instead of rebuilding (which would restart
// playback for a timeline annotation).
struct MarkersChanged
{};
struct BatchBegin
{};
struct BatchEnd
{};

// Emitted synchronously, on the main thread, AFTER Model's state is
// consistent (doc 03) -- listeners never observe a half-applied mutation.
using ModelEvent =
    std::variant<ClipInserted, ClipRemoved, ClipMoved, ClipResized, ClipFlagsChanged, ClipRenamed, TrackAdded,
                 TrackRemoved, TrackFlagsChanged, TrackVolumeChanged, TrackRenamed, TrackReordered, EffectChanged,
                 EffectParamChanged, AdjustmentBlockChanged, LooksChanged, ClipSourceChanged, ClipTransformChanged,
                 TransitionAdded, TransitionRemoved, TransitionChanged, AssetChanged, SequenceProfileChanged,
                 SequenceBackgroundChanged, MarkersChanged, BatchBegin, BatchEnd>;

} // namespace ustudio::core
