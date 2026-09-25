#pragma once

#include <cstdint>
#include <functional>

namespace ustudio::core {

// Ids are allocated by Project::nextId (a monotonic counter) and persisted
// in the project file; they are never reused within a document, so
// undo/redo can refer to objects by id across removal and re-insertion
// (doc 03). Value 0 is reserved for "no id" / invalid.
template <class Tag> struct Id
{
    uint64_t value = 0;

    friend bool operator==(Id a, Id b)
    {
        return a.value == b.value;
    }
    friend bool operator!=(Id a, Id b)
    {
        return a.value != b.value;
    }
    friend bool operator<(Id a, Id b)
    {
        return a.value < b.value;
    }

    bool isValid() const
    {
        return value != 0;
    }
};

struct ClipTag
{};
struct TrackTag
{};
struct AssetTag
{};
struct EffectTag
{};
struct MarkerTag
{};
struct TransitionTag
{};
struct AdjustmentBlockTag
{};
struct LookTag
{};
struct SequenceTag
{};

using ClipId = Id<ClipTag>;
using TrackId = Id<TrackTag>;
using AssetId = Id<AssetTag>;
using EffectId = Id<EffectTag>;
using MarkerId = Id<MarkerTag>;
using TransitionId = Id<TransitionTag>;
using SequenceId = Id<SequenceTag>;
using AdjustmentBlockId = Id<AdjustmentBlockTag>;
using LookId = Id<LookTag>;

} // namespace ustudio::core

template <class Tag> struct std::hash<ustudio::core::Id<Tag>>
{
    size_t operator()(const ustudio::core::Id<Tag> &id) const noexcept
    {
        return std::hash<uint64_t>{}(id.value);
    }
};
