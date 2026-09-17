#pragma once

#include "core/model/model.h"

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
// projection of it into MLT objects. Rebuild policy (ADR-005):
// rebuildAll() clears and repopulates every track playlist under the
// tractor -- O(clips on all tracks), always consistent. Cuts share a
// per-asset master producer so no file is reopened per clip (doc 07).
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
    // shared_ptr, not the plain reference above: MltEngine's playback loop
    // holds this too, so a rebuildAll() that replaces the tractor doesn't
    // free the one MltEngine might still be mid-get_frame() on -- it stays
    // alive until MltEngine's own setTractor() call drops its reference,
    // under MltEngine's mutex.
    std::shared_ptr<Mlt::Tractor> tractorPtr() const
    {
        return m_tractor;
    }

    // Rebuilds the black backing track, every model track's playlist, and
    // the transition graph from scratch.
    void rebuildAll();

    // Full reset for "Open Project": rebuildAll() alone isn't enough,
    // since master producers are cached per AssetId across incremental
    // edits (doc 07) -- after a project load, those ids may now name
    // entirely different assets. Re-derives the profile from the model
    // too, in case the loaded project's differs.
    void reset();

    // Debug/test safety net (doc 05): for each model track, compares its
    // playlist's clip start/length/resource/in/out against the model, and
    // compares tractor length to sequence.length(). Empty = OK.
    std::vector<std::string> verify() const;

    // Opens `path` against this EngineSync's profile just long enough to
    // read its length in sequence frames -- 0 if it can't be opened. Used
    // by the app layer to fill in InsertClip's out point on import, since
    // app/ never touches MLT directly (build-enforced boundary).
    core::FrameIndex probeLength(const std::string &path);

  private:
    core::Model &m_model;
    std::unique_ptr<Mlt::Profile> m_profile;
    std::shared_ptr<Mlt::Tractor> m_tractor;
    std::unordered_map<uint64_t, std::shared_ptr<Mlt::Producer>> m_masterProducers; // keyed by AssetId::value
    // MLT tractor index -> model TrackId; std::nullopt at index 0 (the
    // black backing track, not a model track).
    std::vector<std::optional<core::TrackId>> m_mltTrackOrder;

    void applyProfile();
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
