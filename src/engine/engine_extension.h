#pragma once

// IP3 (doc 15): how a drop-in takes part in building the MLT graph.
//
// A drop-in registers a factory (dropins::DropInHost::addEngineExtension);
// every EngineSync -- the engine thread's live one, and each render's own --
// creates its own instances, so an extension keeps per-graph state (the
// filters it attached, by EffectId) without locking, and is only ever called
// on the thread that owns that EngineSync. The methods are called during
// EngineSync's build, in graph order; every default is "change nothing", and
// with no extension registered EngineSync builds exactly as it always did.

#include "core/model/model.h"

#include <mlt++/Mlt.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ustudio::engine {

// One cut of a clip: its exclusive segment, or its tail or head inside a
// dissolve sub-tractor. `offset` is how many frames into the clip the cut
// starts (for shifting the clip's keyframes, core::keyframesForCut()).
// Attach filters with attachToCut() below, not Service::attach().
struct CutContext
{
    const core::Model &model;
    const core::Clip &clip;
    core::FrameIndex offset;
    core::FrameIndex length;
    Mlt::Profile &profile;
};

// Effect parameter or mix values that changed between two snapshots with
// nothing else different (EngineSync::setProject()).
struct ParamChange
{
    const core::Effect &effect;      // as it is now
    std::vector<std::string> params; // the parameter names that changed; "mix" for the mix
};

class EngineExtension
{
  public:
    virtual ~EngineExtension() = default;
    virtual std::string name() const = 0;

    // A new build is starting: forget the previous graph's objects.
    virtual void beginBuild() {}
    // Every cut that plays a clip (attach the clip's filters here).
    virtual void decorateCut(Mlt::Producer &, const CutContext &) {}
    // A track's playlist, after its cuts and the track volume filter.
    virtual void decoratePlaylist(Mlt::Playlist &, const core::Model &, const core::Track &, Mlt::Profile &) {}
    // The whole tractor, after its tracks and transitions (master effects).
    virtual void decorateTractor(Mlt::Tractor &, const core::Model &, Mlt::Profile &) {}
    // The adjustment blocks on one lane (AdjustmentBlock::lane, in start
    // order): attach their filters to `target`, with each block's in/out
    // (frames count from the sequence's 0). Lane 0's target is the whole
    // tractor; a lane k > 0's is the sub-tractor of the video rows >= k,
    // which EngineSync builds (FX4; until it does, only lane 0 is called).
    // Called once per lane that has blocks, after decorateTractor().
    virtual void decorateLane(Mlt::Service &, const core::Model &, int /*lane*/,
                              const std::vector<const core::AdjustmentBlock *> & /*blocks*/, Mlt::Profile &)
    {}
    // The producer a clip is cut from, for clips the drop-in generates
    // (titles: Clip::sourceParams). Null: the asset's media, as usual.
    // Open it with engine::openProducer(..., ProducerUse::Graph)
    // (producer_open.h): the GPU pipeline's graphs need the default loader
    // and the CPU pipeline's must not use it while a GPU session lives.
    virtual std::unique_ptr<Mlt::Producer> makeProducer(const core::Model &, const core::Clip &, Mlt::Profile &)
    {
        return nullptr;
    }
    // A dissolve segment built from its recipe (Transition::recipe). The
    // cuts are already decorated. Null: the plain luma + mix sub-tractor.
    virtual std::unique_ptr<Mlt::Tractor> makeTransitionSegment(const core::Model &, const core::Transition &,
                                                                Mlt::Producer & /*tailA*/, Mlt::Producer & /*headB*/,
                                                                Mlt::Profile &)
    {
        return nullptr;
    }
    // The video compositor between tractor tracks `aTrack` and `bTrack`.
    // Null: "composite".
    virtual std::unique_ptr<Mlt::Transition> compositor(Mlt::Profile &, int aTrack, int bTrack)
    {
        (void)aTrack;
        (void)bTrack;
        return nullptr;
    }
    // Apply a value change to the live filters without a rebuild. True if it
    // did; a change no extension claims rebuilds the graph.
    virtual bool applyInPlace(const ParamChange &)
    {
        return false;
    }
};

// Attaches `filter` to a clip's `cut` so its animation counts from the cut's
// first frame. A cut's frames carry their position in the *source*
// (mlt_filter_process() records the frame's own position), and
// mlt_filter_get_position() subtracts the filter's "in" (MLT 7.40,
// mlt_filter.c:241 and :285) -- so the filter's in/out must be the cut's.
// Verified: an animation starting at 0 on a cut of source frames 100-194
// began at the cut's first frame only with this (tests/dropins).
void attachToCut(Mlt::Producer &cut, Mlt::Filter &filter);

using EngineExtensionFactory = std::function<std::unique_ptr<EngineExtension>()>;

// Registered once, at startup, before any EngineSync exists (main thread).
void registerEngineExtension(EngineExtensionFactory factory);
// What an EngineSync builds with: one of each.
std::vector<std::unique_ptr<EngineExtension>> createEngineExtensions();
// Tests only: back to none.
void clearEngineExtensions();

} // namespace ustudio::engine
