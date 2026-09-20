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
    // Also re-subscribes to Model::changed: the caller reassigns *this
    // EngineSync's* m_model's contents wholesale for "Open Project"
    // (`m_model = std::move(*loaded)`), and Model's implicit assignment
    // operator overwrites every member including `changed` itself -- which
    // silently drops whatever was connected to it. reset() is the one
    // operation defined to run right after that kind of replacement, so
    // it's where the subscription gets re-established.
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

    void applyProfile();
    void connectToModel();
    void onModelEvent(const core::ModelEvent &event);
    Mlt::Producer &masterProducerFor(core::AssetId);
    void rebuildTrackPlaylist(const core::Track &modelTrack, Mlt::Playlist &playlist);
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
