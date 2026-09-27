#pragma once

#include "core/model/model.h"
#include "engine/preview_scale.h"
#include "core/model/model_event.h"
#include "core/model/signal.h"
#include "core/model/track_segments.h"
#include "core/model/transform.h"
#include "core/render/render_profile.h"
#include "engine/engine_extension.h"
#include "engine/producer_open.h"

#include <mlt++/Mlt.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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
// v1's uniform chaining is proven working. M1 reused v1's pattern; since
// M4 F (ADR-018) every "composite" takes track 0 as its A track instead of
// the track below (a chain lost an upper clip's alpha), and "mix" stays
// chained.
class EngineSync
{
  public:
    // `previewScale` sets how large the playback tractor renders (see
    // setPreviewScale()). The default, Full, is what export and tests want:
    // renderProject() builds its own EngineSync and must render at the
    // sequence's real size.
    //
    // Built from an immutable project snapshot (doc 19 MT2), never the live
    // Model: the owner publishes each new state with setProject(), so a
    // batch of edits is one snapshot and one rebuild, and the graph can be
    // built on another thread. The Model overload takes model.snapshot()
    // once, for callers (render, tests) that build a graph of one state.
    //
    // `reads` says at what size the graph's frames are read. ProfileSize
    // (playback, a render at the sequence's size) lets the track compositor
    // centre a Fit picture of another aspect by itself, skipping its affine
    // filter (core::compositorFits()). composite's alignment is right only
    // for frames read at the profile's own size: at any other it shifts the
    // picture right by the difference, a frame-sized one too (MLT 7.40:
    // get_image aligns item.w, in profile pixels, against the B image's
    // width in requested pixels; standalone repro 2026-09-27). AnySize, the
    // default, keeps every compositor left-aligned and fits with affine.
    enum class FrameReads
    {
        AnySize,
        ProfileSize,
    };
    // An export's own frame size, which the graph is built at (as a scaled
    // preview is), so its frames are still read at the profile's size.
    // Square-pixel sequences only; 0x0 is the sequence's size.
    struct OutputSize
    {
        int width;
        int height;
    };
    explicit EngineSync(std::shared_ptr<const core::Project> project, PreviewScale previewScale = PreviewScale::Full,
                        FrameReads reads = FrameReads::AnySize, OutputSize outputSize = {0, 0});
    explicit EngineSync(const core::Model &model, PreviewScale previewScale = PreviewScale::Full,
                        FrameReads reads = FrameReads::AnySize, OutputSize outputSize = {0, 0});
    ~EngineSync();

    // A new state of the same project. Rebuilds unless nothing the graph is
    // built from changed (markers, the id allocator and settings never
    // reach MLT); a different sequence profile rebuilds on a new profile.
    // The same snapshot again is a no-op.
    void setProject(std::shared_ptr<const core::Project> project);
    // The project the current graph was built from.
    const std::shared_ptr<const core::Project> &project() const
    {
        return m_project;
    }

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
    // M4 C: play each asset's proxy (Asset::proxyPath) instead of its file
    // when there is one. View state, not the model's; renders build their
    // own EngineSync with it off, so export always uses the originals. A
    // proxy whose file is gone (the cache was cleared) falls back to the
    // original quietly. Rebuilds when it changes anything.
    void setUseProxies(bool use);
    bool useProxies() const
    {
        return m_useProxies;
    }
    // ADR-019: which pipeline the graph is built for. Gpu composites with
    // movit.overlay and places pictures with movit filters (a rotated one
    // keeps the CPU affine), and needs a live GpuSession: its glsl.manager
    // is what gives the masters movit's normalisers, so every master is
    // reopened here, with `hardwareDecodeApi` as setHardwareDecode() takes
    // it (one rebuild for both). Rebuilds when either changes.
    enum class Pipeline
    {
        Cpu,
        Gpu,
    };
    void setPipeline(Pipeline pipeline, const std::string &hardwareDecodeApi);
    Pipeline pipeline() const
    {
        return m_pipeline;
    }
    // ADR-019 G1: decode video files with this hardware API ("vaapi"; ""
    // is software), per master producer, so worker producers stay on
    // software decode. MLT falls back to software when the device fails.
    // Rebuilds when it changes anything.
    void setHardwareDecode(const std::string &api);
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

    // Fires synchronously, on the thread that called in, at the end of every
    // rebuildAll() -- whether triggered by setProject(), reset(), a preview
    // scale change or an explicit call (a test). tractorPtr() has just
    // changed identity; listeners that hand the tractor to a consumer
    // (PlaybackController::setTractor) connect here once instead of
    // calling it after every edit themselves.
    core::Signal<> rebuilt;

    // IP3: a new snapshot's effect value changes were applied to the live
    // filters instead of rebuilding (setProject()); a paused consumer needs
    // to redraw its frame. Same thread as setProject().
    core::Signal<> appliedInPlace;

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

    // Full reset for "Open Project" (a different project, not a new state
    // of this one): setProject() isn't enough, since master producers are
    // cached per AssetId across edits (doc 07) -- after a project load,
    // those ids may now name entirely different assets. Re-derives the
    // profile too, in case the loaded project's differs.
    void reset(std::shared_ptr<const core::Project> project);

    // Debug/test safety net (doc 05): for each model track, compares its
    // playlist's clip start/length/resource/in/out against the model, and
    // compares tractor length to sequence.length(). Empty = OK.
    std::vector<std::string> verify() const;

    // A track with more playlist entries (cuts, gaps, dissolves) than this
    // is built from nested sub-playlists of this many (rebuildTrackPlaylist()
    // says why). Process-wide; tests and benchmarks change it to compare
    // flat and chunked graphs, and to sweep sizes.
    static constexpr size_t kDefaultPlaylistChunkSize = 64;
    static void setPlaylistChunkSize(size_t entries);
    static size_t playlistChunkSize();

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
        // The rate `length` is counted in: the sequence's when probed. An
        // import applied after the sequence's rate changed (its first video,
        // doc 13 R7) retimes it.
        core::Rational sequenceFps{0, 0};
        // Why it can't be imported ("it's empty"), for the import report;
        // "" when it can. Set by the importer's checks around the probe.
        std::string error;
        // core::fileFingerprint(), taken on the pool with the probe.
        std::string fingerprint;
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

    // IP3: the drop-ins' extensions this graph is built with (one of each
    // registered, made at construction; engine_extension.h).
    size_t extensionCount() const
    {
        return m_extensions.size();
    }

  private:
    std::vector<std::unique_ptr<EngineExtension>> m_extensions;
    // IP3: when `project` differs from the current one only in effect
    // parameter or mix values, offers each change to the extensions; true
    // if every one was applied in place (no rebuild needed).
    bool applyInPlace(const core::Project &project);
    // Clips whose producer an extension made (verify() skips their
    // resource check): per build.
    std::unordered_set<uint64_t> m_extensionProducers;
    std::unordered_map<uint64_t, std::shared_ptr<Mlt::Producer>> m_clipProducers; // per build
    Mlt::Producer &producerForClip(const core::Clip &clip);
    void decorateCut(Mlt::Producer &cut, const core::Clip &clip, core::FrameIndex in, core::FrameIndex out);
    // The snapshot the graph was built from, and a Model over a copy of it
    // for the lookups (clip(), track(), planTrackSegments()) the build uses.
    std::shared_ptr<const core::Project> m_project;
    core::Model m_model;
    std::unique_ptr<Mlt::Profile> m_profile;
    std::shared_ptr<Mlt::Tractor> m_tractor;
    PreviewScale m_previewScale = PreviewScale::Full;
    FrameReads m_reads = FrameReads::AnySize;
    uint32_t m_blackMasterColour = core::Sequence::kDefaultBackground; // the background m_blackMaster shows
    OutputSize m_outputSize{0, 0};
    bool compositorFits(const core::Clip &clip, bool inDissolve) const;
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
    bool m_useProxies = false;
    std::string m_hardwareDecodeApi;
    Pipeline m_pipeline = Pipeline::Cpu;
    // ADR-018: each clip's transform filters, per cut (the exclusive cut
    // and any dissolve tail or head), kept so a transform-only snapshot
    // updates them in place (applyTransformsInPlace()); per build.
    struct TransformFilters
    {
        std::string shape; // the services in order: a change of shape rebuilds
        std::vector<std::vector<std::shared_ptr<Mlt::Filter>>> cuts;
    };
    std::unordered_map<uint64_t, TransformFilters> m_transformFilters;
    // One transparent background for every transform filter (see
    // applyTransform()), built from the current profile.
    std::unique_ptr<Mlt::Producer> m_transformBackground;
    static constexpr const char *kMixedShape = "mixed"; // cuts of different shapes: never updated in place
    // True when it attached filters (the picture then has transparent edges).
    // The filters realising `t` on this graph's pipeline (core::transformFilters()
    // or core::gpuTransformFilters()).
    // How this graph opens its producers (producer_open.h).
    ProducerUse graphUse() const
    {
        return m_pipeline == Pipeline::Gpu ? ProducerUse::GpuGraph : ProducerUse::CpuGraph;
    }
    std::vector<core::NativeFilter> transformNatives(const core::Transform &t, const core::MediaInfo &info,
                                                     const core::Profile &profile, double sourceScale) const;
    bool applyTransform(Mlt::Producer &cut, const core::Clip &clip, bool inDissolve = false);
    // Whether a clip's picture may have partial alpha: stills, image
    // sequences, drop-in producers (titles), transformed cuts. Those get
    // attachAlphaPairing() (engine_sync.cpp) before the track compositor.
    bool carriesAlpha(const core::Clip &clip, bool transformed) const;
    bool applyTransformsInPlace(const core::Project &next);
    // Output pixels per project pixel, and the playing file's pixels per
    // source pixel (a proxy is smaller).
    double outputScale() const;
    double sourceScale(const core::Clip &clip);
    // Assets playing their proxy this build (verify() skips their resource).
    std::unordered_set<uint64_t> m_proxiedAssets;
    void dropProxiedMasters();
    // Forgets the masters of assets whose path or status changed (relink,
    // missing on load, found again), so the next build opens them afresh.
    void dropChangedMasters(const core::Project &before, const core::Project &after);
    // MLT tractor index -> model TrackId; std::nullopt at index 0 (the
    // black backing track, not a model track).
    std::vector<std::optional<core::TrackId>> m_mltTrackOrder;

    void applyProfile();
    // Swaps in a freshly derived profile and rebuilds everything built on
    // the old one (master producers, the black master, the tractor); shared
    // by reset() and setPreviewScale().
    void rebuildOnNewProfile();
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
    std::unique_ptr<Mlt::Tractor> buildTransitionSubTractor(const TrackSegment &segment, bool video);
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
//
// `profile` picks size and quality (core::encoderSettings()); the default is
// the pre-profile settings, which the engine tests rely on. `threadBudget`
// > 0 caps the render's threads (core::splitRenderThreads()); 0 leaves
// MLT's defaults (one render thread, the encoder's automatic count).
//
// `extra`: further avformat consumer properties, set last (a proxy's
// keyframe interval, "g").
bool renderProject(core::Model &model, const std::string &outputPath, std::string &error,
                   std::function<void(int currentFrame, int totalFrames)> onProgress = {},
                   const std::atomic<bool> *cancel = nullptr,
                   const core::RenderProfile &profile = core::legacyRenderProfile(), int threadBudget = 0,
                   const std::vector<std::pair<std::string, std::string>> &extra = {});

// The H.264 encoder renderProject uses: libx264 where ffmpeg has it,
// otherwise libopenh264 (stock Fedora's ffmpeg-free ships only that one;
// given an unknown vcodec, avformat writes an MP4 with no video stream at
// all). Empty if neither is present. Asked of MLT once per process, on the
// first call, which must not be on the main thread (it opens an avformat
// consumer); the app asks from a pool job at startup, and renders ask too.
const std::string &h264Encoder();

// Whether that encoder has a constant-quality (CRF) mode (libx264 does,
// OpenH264 doesn't), once h264Encoder() has answered; nullopt before. Reads
// only the cached answer, so any thread may call it.
std::optional<bool> h264HasQualityMode();

} // namespace ustudio::engine
