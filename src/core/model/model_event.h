#pragma once

#include "ids.h"

#include <cstddef>
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
struct EffectChanged
{
    EffectId effect;
};
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
struct BatchBegin
{};
struct BatchEnd
{};

// Emitted synchronously, on the main thread, AFTER Model's state is
// consistent (doc 03) -- listeners never observe a half-applied mutation.
using ModelEvent = std::variant<ClipInserted, ClipRemoved, ClipMoved, ClipResized, ClipFlagsChanged, ClipRenamed,
                                TrackAdded, TrackRemoved, TrackFlagsChanged, TrackVolumeChanged, TrackRenamed,
                                TrackReordered, EffectChanged, TransitionChanged, AssetChanged, SequenceProfileChanged,
                                BatchBegin, BatchEnd>;

} // namespace ustudio::core
