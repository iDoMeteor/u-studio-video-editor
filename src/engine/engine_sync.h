#pragma once

#include "core/model/model.h"
#include "core/model/model_event.h"
#include "core/model/signal.h"

#include <mlt++/Mlt.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ustudio::engine {

namespace core = ustudio::core;

// Owns both the Mlt::Profile (built from Sequence::profile, doc 05 -- not
// a stock name, so custom profiles work) and the Mlt::Tractor built FROM
// the Model: the model is the source of truth, EngineSync is a one-way
// projection of it into MLT objects (ADR-003). It subscribes to
// Model::changed itself (connectToModel()) so a caller never has to
// remember to resync after an edit -- the only thing that used to make
// that true was every AppWindow call site calling rebuildAll() manually,
// which nothing enforced.
//
// Rebuild policy (ADR-005 simplified): every model event that isn't part
// of an open batch (BatchBegin/BatchEnd, doc 04's Transaction/
// CompositeCommand) triggers one rebuildAll() -- clears and repopulates
// every track playlist under the tractor, O(clips on all tracks), always
// consistent. Events inside a batch just mark "dirty" and are coalesced
// into a single rebuildAll() at BatchEnd, so e.g. "Close gap" (N MoveClips)
// costs one resync, not N. True per-track incremental rebuild (rebuilding
// only the touched playlist in place, not the whole tractor) is ADR-005's
// further-out optimisation; doc 13 (risk R5) calls it acceptable to defer
// until profiling on real timelines asks for it. Cuts share a per-asset
// master producer so no file is reopened per clip (doc 07).
//
// Simplified vs. doc 05's full graph: v1's MltEngine plants BOTH
// "composite" and "mix" between every adjacent tractor track index
// (including the black backing track), rather than doc 05's more precise
// video-only-composite / audio-only-mix split. That split hasn't been
// empirically verified (project rule: reproduce before relying), while
// v1's uniform chaining is proven working. M1 reuses v1's pattern
// unchanged; refining it to doc 05's graph is PlaybackController/M2
// territory, where it can be verified against real rendered frames.
class EngineSync
{
  public:
    explicit EngineSync(core::Model &model);
    // Disconnects from Model::changed (audit C4): Model outlives EngineSync
    // in every current caller, but nothing enforces that, and a dangling
    // subscription firing into a destroyed `this` is exactly the kind of
    // bug that only shows up once something changes that assumption.
    ~EngineSync();

    Mlt::Profile &profile()
    {
        return *m_profile;
    }
    Mlt::Tractor &tractor()
    {
        return *m_tractor;
    }
    // shared_ptr, not the plain reference above: PlaybackController's
    // Mlt::Consumer holds this too (via its own setTractor()), so a
    // rebuildAll() that replaces the tractor doesn't free the one the
    // consumer might still be mid-frame on -- it stays alive until
    // PlaybackController's own setTractor() call drops its reference.
    std::shared_ptr<Mlt::Tractor> tractorPtr() const
    {
        return m_tractor;
    }

    // Fires synchronously, main thread only, at the end of every
    // rebuildAll() -- whether triggered by an automatic model-event resync
    // or by an explicit call (reset(), a test). tractorPtr() has just
    // changed identity; listeners that hand the tractor to a consumer
    // (PlaybackController::setTractor) connect here once instead of
    // calling it after every edit themselves.
    core::Signal<> rebuilt;

    // Rebuilds the black backing track, every model track's playlist, and
    // the transition graph from scratch.
    void rebuildAll();

    // Full reset for "Open Project": rebuildAll() alone isn't enough,
    // since master producers are cached per AssetId across incremental
    // edits (doc 07) -- after a project load, those ids may now name
    // entirely different assets. Re-derives the profile from the model
    // too, in case the loaded project's differs.
    //
    // Also re-subscribes to Model::changed via connectToModel(), which is
    // idempotent (disconnects its own previous connection first, audit
    // C4) -- safe to call again here even though the subscription from
    // construction is still perfectly valid: Model::operator=, the
    // "Open Project" reassignment this runs after (`m_model =
    // std::move(*loaded)`), deliberately leaves the target's existing
    // `changed` and its subscribers untouched (Model's copy/move are
    // hand-written for exactly this, see model.h), so nothing here is
    // actually replacing a dropped connection any more -- it's just
    // cheap insurance against ever assuming that in the future.
    void reset();

    // Debug/test safety net (doc 05): for each model track, compares its
    // playlist's clip start/length/resource/in/out against the model, and
    // compares tractor length to sequence.length(). Empty = OK.
    std::vector<std::string> verify() const;

    struct ProbedMedia
    {
        core::FrameIndex length = 0; // 0 if the path couldn't be opened
        bool isStillImage = false;
        bool hasAudio = false;
        // 0/0 if unavailable (still images/generators never set these;
        // real media does, but only after a frame has actually been
        // decoded -- see probeMedia()'s comment).
        core::Rational fps{0, 0};
        int width = 0;
        int height = 0;
    };

    // Opens `path` against this EngineSync's profile just long enough to
    // read a few basic facts -- used by the app layer to fill in
    // AddAsset/InsertClip on import, since app/ never touches MLT directly
    // (build-enforced boundary). `isStillImage` is read from the opened
    // producer's own `mlt_service` property (verified empirically: PNG/JPEG
    // etc. load via the `pixbuf` service on this machine -- `qimage` is
    // Qt-based and excluded by FactoryPolicy's denylist, per ADR-007 --
    // never guessed from the file extension). `length` for a still image is
    // whatever MLT's pixbuf producer defaults to (15000 frames, verified
    // empirically) -- callers that need a specific duration should extend
    // it themselves; see the comment on masterProducerFor's length bump.
    // `fps`/`width`/`height` come from the avformat producer's own
    // `meta.media.frame_rate_num`/`_den`/`width`/`height` properties --
    // verified empirically (standalone repro, a rendered test MP4) that
    // these are populated lazily, only after the producer has actually
    // decoded at least one frame, hence the explicit get_frame() below
    // before reading them (absent on a still image, which never sets
    // meta.media.* at all -- fps/width/height stay at their zero default).
    ProbedMedia probeMedia(const std::string &path);

  private:
    core::Model &m_model;
    std::unique_ptr<Mlt::Profile> m_profile;
    std::shared_ptr<Mlt::Tractor> m_tractor;
    std::unordered_map<uint64_t, std::shared_ptr<Mlt::Producer>> m_masterProducers; // keyed by AssetId::value
    // MLT tractor index -> model TrackId; std::nullopt at index 0 (the
    // black backing track, not a model track).
    std::vector<std::optional<core::TrackId>> m_mltTrackOrder;

    // >0 while inside a Transaction/CompositeCommand's BatchBegin..BatchEnd
    // (doc 04); events during that window set m_dirty instead of resyncing
    // immediately, so the whole batch costs one rebuildAll(), not one per
    // sub-command.
    int m_batchDepth = 0;
    bool m_dirty = false;
    // 0 = not connected. Signal ids start at 1 (signal.h), so 0 is a safe
    // sentinel; disconnect(0) is a harmless no-op (nothing to remove).
    int m_modelConnection = 0;

    void applyProfile();
    void connectToModel();
    void onModelEvent(const core::ModelEvent &event);
    Mlt::Producer &masterProducerFor(core::AssetId);
    void rebuildTrackPlaylist(const core::Track &modelTrack, Mlt::Playlist &playlist);

    // One playlist entry rebuildTrackPlaylist() builds for a track, in
    // order -- and the same description verify() checks the resulting
    // playlist against, so the two can't independently drift out of sync
    // on what a track's MLT playlist is supposed to look like. A
    // transition between clips `a` and `b` replaces what would otherwise
    // be two whole-clip Clip segments with up to three: `a`'s own
    // exclusive (pre-overlap) span, the Transition sub-tractor, and `b`'s
    // exclusive (post-overlap) span -- either exclusive span is omitted
    // entirely if a clip's whole length is consumed by the transition(s)
    // touching it.
    struct TrackSegment
    {
        enum class Kind
        {
            Clip,
            Transition,
        } kind;

        core::FrameIndex start;  // position on the track
        core::FrameIndex length; // frame count, always > 0

        // Kind::Clip: `in`/`out` are this segment's own source range --
        // narrower than the model clip's full in/out when a transition
        // has trimmed the head and/or tail off it.
        core::ClipId clip;
        core::FrameIndex in = 0, out = 0;

        // Kind::Transition
        core::TransitionId transition;
        core::ClipId a, b;
    };
    std::vector<TrackSegment> planTrackSegments(const core::Track &modelTrack) const;
    // Builds the 2-track sub-tractor for one Transition segment: `a`'s
    // tail on track 0, `b`'s head on track 1, connected by a plain `luma`
    // dissolve (no `resource` -> dissolve per the module's own YAML) and a
    // `mix` for audio crossfade (start=-1). Both transitions have their
    // own in/out explicitly set to the sub-tractor's local [0, length)
    // range in the .cpp -- REQUIRED, not optional: a standalone repro
    // (2026-09-22, scratchpad/dissolve_repro*.cpp) first seemed to show a
    // plain unconfigured luma dissolving correctly when nested in an
    // outer playlist, but that repro's cuts both happened to start at
    // source frame 0; once track 0's cut has the non-zero absolute `in` a
    // real clip's tail always has, the same construction corrupted the
    // last couple of overlap frames into flat garbage colour until in/out
    // were set explicitly. See the .cpp for the fuller finding.
    std::unique_ptr<Mlt::Tractor> buildTransitionSubTractor(const TrackSegment &segment);
};

// Renders `model` to outputPath as H.264 (High, yuv420p, matching the
// model's own profile dimensions/fps) + AAC (48kHz stereo) in an MP4
// container -- the same encoder settings v1's MltEngine::renderProject
// empirically matched to this project's target format. Blocking and
// potentially slow (real encode time); the caller is expected to run this
// on its own thread, not the GTK main thread. Builds a completely
// separate, throwaway EngineSync (its own Profile/Tractor/master
// producers) from the model rather than rendering through a live,
// currently-playing EngineSync, so editing/playback aren't blocked or
// disturbed for however long the render takes.
bool renderProject(core::Model &model, const std::string &outputPath, std::string &error);

} // namespace ustudio::engine
