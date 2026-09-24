#pragma once

#include "core/model/model.h"
#include "engine/preview_scale.h"
#include "core/model/model_event.h"
#include "core/model/signal.h"
#include "core/model/track_segments.h"

#include <mlt++/Mlt.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    // `previewScale` sets how large the playback tractor renders (see
    // setPreviewScale()). The default, Full, is what export and tests want:
    // renderProject() builds its own EngineSync and must render at the
    // sequence's real size.
    explicit EngineSync(core::Model &model, PreviewScale previewScale = PreviewScale::Full);
    // Disconnects from Model::changed (audit C4): Model outlives EngineSync
    // in every current caller, but nothing enforces that, and a dangling
    // subscription firing into a destroyed `this` is exactly the kind of
    // bug that only shows up once something changes that assumption.
    ~EngineSync();

    // The profile the tractor is built on: the sequence profile scaled by
    // previewFactor(). At Full it *is* the sequence profile.
    Mlt::Profile &profile()
    {
        return *m_profile;
    }

    // Renders the playback tractor at a fraction of the sequence size by
    // building it on a scaled profile (doc 05). MLT has no cheaper way:
    // producers and transitions render at their profile's size, and the
    // consumer's "scale" property does not shrink that (measured,
    // 2026-09-24: 4K60 playback at "Half" still delivered 3840x2160
    // frames), while overriding the consumer's width/height only adds a
    // final downscale after full-size rendering (measured slower). kdenlive
    // likewise resizes its monitor profile. Changing the resolved factor
    // rebuilds the tractor on a new profile, which restarts playback via
    // `rebuilt`; setting the same factor again does nothing. Positions,
    // lengths and fps are unaffected; only pixel dimensions change.
    void setPreviewScale(PreviewScale scale);
    PreviewScale previewScale() const
    {
        return m_previewScale;
    }
    double previewFactor() const
    {
        return m_previewFactor;
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

    // Fires synchronously, main thread only, from inside rebuildAll() (via
    // masterProducerFor()) whenever an asset's file can't actually be
    // opened -- a project referencing media that's since been moved,
    // deleted, or lives on an unmounted drive (CLAUDE.md: project files
    // are untrusted input). The carried string is the asset's own path.
    // That clip plays as black rather than crashing (see
    // masterProducerFor()'s comment for why letting an invalid producer
    // through is not survivable); listeners should tell the user which
    // file is missing.
    core::Signal<const std::string &> mediaUnavailable;

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
    // The same probe, against a sequence profile passed by value: touches
    // nothing of any EngineSync, so a worker-pool job can call it (doc 19
    // MT1, parallel import); each call opens its own Profile and Producer.
    static ProbedMedia probeMediaFile(const core::Profile &sequenceProfile, const std::string &path);

  private:
    core::Model &m_model;
    std::unique_ptr<Mlt::Profile> m_profile;
    std::shared_ptr<Mlt::Tractor> m_tractor;
    PreviewScale m_previewScale = PreviewScale::Full;
    double m_previewFactor = 1.0;
    // Keyed by AssetId::value plus the clip's two stream switches (see
    // masterProducerFor()).
    std::unordered_map<uint64_t, std::shared_ptr<Mlt::Producer>> m_masterProducers;
    // The black backing track's master (rebuildAll()), created once through
    // MLT's loader and cut per rebuild -- see rebuildAll()'s comment for why
    // neither "a new loader producer per rebuild" nor "the explicit colour
    // service" works. Dropped with the other masters in reset().
    std::unique_ptr<Mlt::Producer> m_blackMaster;
    // AssetId::value of every asset masterProducerFor() had to substitute
    // a black placeholder for (its real file couldn't be opened) --
    // verify() skips its resource check for these, since the mismatch
    // there is the intended fallback, not a sync bug.
    std::unordered_set<uint64_t> m_unavailableAssets;
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
    // Swaps in a freshly derived profile and rebuilds everything built on
    // the old one (master producers, the black master, the tractor); shared
    // by reset() and setPreviewScale().
    void rebuildOnNewProfile();
    void connectToModel();
    void onModelEvent(const core::ModelEvent &event);
    Mlt::Producer &masterProducerFor(core::AssetId, bool videoEnabled, bool audioEnabled);
    void rebuildTrackPlaylist(const core::Track &modelTrack, Mlt::Playlist &playlist);

    // What each playlist entry is -- core/model/track_segments.h, shared
    // with core/xml's writer so a saved project's render structure and
    // live playback come from one planner.
    using TrackSegment = core::TrackSegment;
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
//
// `onProgress`, if set, is called with (currentFrame, totalFrames) from
// an MLT "consumer-frame-render" event listener (fired by the base
// consumer class before rendering every frame, for every consumer type)
// -- i.e. on the SAME thread blocked inside this call, not the
// subclass-fired "consumer-frame-show" PlaybackController::handleFrameShow
// listens for (only fired on actually showing a frame; avformat never
// shows anything, confirmed empirically to never fire it -- see the
// .cpp's own comment). It is the caller's job to marshal that onto the
// GTK main thread if it touches any widget -- nothing here does.
// Throttled to roughly once every half-second of render time (not every
// frame -- a 30fps hour-long render is 108,000
// frames, and every one of those hitting the caller's own dispatch would
// be pointless churn for a status label).
//
// `cancel`, if set, is polled while the render runs; once it reads true the
// consumer is stopped, the .part file removed, and this returns false with
// `error` "Render cancelled". The app sets it to quit mid-render (post-M3
// audit P2: MLT must not be torn down under a running render).
bool renderProject(core::Model &model, const std::string &outputPath, std::string &error,
                   std::function<void(int currentFrame, int totalFrames)> onProgress = {},
                   const std::atomic<bool> *cancel = nullptr);

// The H.264 encoder renderProject uses: libx264 where ffmpeg has it,
// otherwise libopenh264 (stock Fedora's ffmpeg-free ships only that one;
// given an unknown vcodec, avformat writes an MP4 with no video stream at
// all). Empty if neither is present. Asked of MLT once per process; needs
// the Factory initialised.
const std::string &h264Encoder();

} // namespace ustudio::engine
