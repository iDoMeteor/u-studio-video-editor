#include "engine_sync.h"

#include "factory_policy.h"

#include "core/log.h"
#include "core/trace.h"
#include "core/model/audio_level.h"
#include "core/model/mlt_order.h"
#include "core/model/track_segments.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <system_error>
#include <thread>
#include <variant>

namespace ustudio::engine {

namespace Log = ustudio::core::Log;

namespace {
constexpr const char *kBlackResource = "color:black";
// ~3.8 days at 30 fps: longer than any sequence this app will hold, so the
// black master (rebuildAll()) never needs resizing after creation.
constexpr int kBlackMasterLength = 10'000'000;

// See rebuildTrackPlaylist(). Sweep: doc 19 MT2.
constexpr const char *kChunkProperty = "ustudio.chunk";
std::atomic<size_t> g_playlistChunkSize{EngineSync::kDefaultPlaylistChunkSize};

// verify()'s view of a track playlist: chunks (rebuildTrackPlaylist())
// flattened back into one list, their entries' starts offset by the
// chunk's own position, so it compares against the model exactly as it
// does for a flat track. ClipInfo::producer is the cut's parent
// (mlt_playlist_get_clip_info()), i.e. the chunk itself for a chunk entry.
struct PlaylistEntry
{
    bool blank = false;
    bool valid = true; // clip_info() succeeded
    int start = 0;
    int frameCount = 0;
    int frameIn = 0;
    int frameOut = 0;
    std::string resource;
};

void flattenPlaylist(Mlt::Playlist &playlist, int offset, std::vector<PlaylistEntry> &out)
{
    for (int i = 0; i < playlist.count(); ++i) {
        PlaylistEntry entry;
        if (playlist.is_blank(i)) {
            entry.blank = true;
            out.push_back(entry);
            continue;
        }
        std::unique_ptr<Mlt::ClipInfo> info(playlist.clip_info(i));
        if (!info) {
            entry.valid = false;
            out.push_back(entry);
            continue;
        }
        if (info->producer && info->producer->get_int(kChunkProperty)) {
            Mlt::Playlist chunk(*info->producer);
            flattenPlaylist(chunk, offset + info->start, out);
            continue;
        }
        entry.start = offset + info->start;
        entry.frameCount = info->frame_count;
        entry.frameIn = info->frame_in;
        entry.frameOut = info->frame_out;
        entry.resource = info->resource ? info->resource : "";
        out.push_back(entry);
    }
}

std::unique_ptr<Mlt::Profile> makeProfileFrom(const core::Profile &p)
{
    auto profile = std::make_unique<Mlt::Profile>();
    profile->set_width(p.width);
    profile->set_height(p.height);
    profile->set_frame_rate(p.fps.num, p.fps.den);
    profile->set_sample_aspect(p.sar.num, p.sar.den);
    profile->set_display_aspect(p.dar.num, p.dar.den);
    profile->set_progressive(p.progressive ? 1 : 0);
    profile->set_colorspace(p.colorspace);
    return profile;
}
} // namespace

EngineSync::EngineSync(std::shared_ptr<const core::Project> project, PreviewScale previewScale)
    : m_project(std::move(project)), m_model(*m_project), m_previewScale(previewScale)
{
    applyProfile();
    rebuildAll();
}

EngineSync::EngineSync(const core::Model &model, PreviewScale previewScale) : EngineSync(model.snapshot(), previewScale)
{}

EngineSync::~EngineSync() = default;

namespace {
// Everything rebuildAll() reads: the bin and the active sequence's
// profile, tracks, clips and transitions. Not markers (they never reach
// MLT), the id allocator (adding a marker advances it) or settings.
bool sameGraphInput(const core::Project &a, const core::Project &b)
{
    if (a.bin != b.bin || a.activeSequence != b.activeSequence || a.sequences.size() != b.sequences.size())
        return false;
    for (size_t i = 0; i < a.sequences.size(); ++i) {
        const core::Sequence &x = a.sequences[i];
        const core::Sequence &y = b.sequences[i];
        if (x.id != y.id || x.profile != y.profile || x.tracks != y.tracks || x.clips != y.clips ||
            x.transitions != y.transitions)
            return false;
    }
    return true;
}
} // namespace

void EngineSync::setProject(std::shared_ptr<const core::Project> project)
{
    if (!project || project == m_project)
        return;
    core::trace::Scope trace("EngineSync::setProject");
    const bool rebuild = !sameGraphInput(*m_project, *project);
    const core::Profile profileBefore = m_model.sequence().profile;
    m_project = std::move(project);
    m_model = core::Model(*m_project);
    const bool newProfile = m_model.sequence().profile != profileBefore;
    if (newProfile)
        rebuildOnNewProfile();
    else if (rebuild)
        rebuildAll();
}

void EngineSync::reset(std::shared_ptr<const core::Project> project)
{
    Log::ScopedTimer timer("[engine] reset");
    m_project = std::move(project);
    m_model = core::Model(*m_project);
    m_unavailableAssets.clear(); // a reopened project's media may have come back since -- give it a fresh try
    rebuildOnNewProfile();
}

void EngineSync::setPreviewScale(PreviewScale scale)
{
    m_previewScale = scale;
    double factor = previewScaleFactor(scale, m_model.sequence().profile.height);
    if (factor == m_previewFactor)
        return;
    Log::debug("[engine] preview scale factor " + std::to_string(m_previewFactor) + " -> " + std::to_string(factor));
    rebuildOnNewProfile();
}

void EngineSync::rebuildOnNewProfile()
{
    Log::debug("[engine] new profile: dropping " + std::to_string(m_masterProducers.size()) +
               " cached master producer(s)");
    m_masterProducers.clear();
    // Built from the old profile, which applyProfile() below replaces.
    m_blackMaster.reset();

    // Keep the OLD profile alive across applyProfile()+rebuildAll(), not
    // just swap it in place: applyProfile() would otherwise destroy it
    // immediately (Mlt::Profile's destructor frees the underlying
    // mlt_profile struct), while the tractor/producers the *running*
    // consumer is still actively pulling frames from (via its read-ahead
    // thread, at any speed including 0) were built from it and hold raw
    // pointers to it -- reading fps/width/height out of freed memory the
    // whole time rebuildAll() runs (which opens a producer per asset, so
    // this window is long for a real project). rebuildAll() ends by
    // firing `rebuilt`, which PlaybackController::setTractor() handles
    // synchronously by stopping the consumer *before* touching the new
    // tractor -- so by the time rebuildAll() returns below, nothing is
    // reading the old tractor (or its profile) anymore, and it's safe for
    // `oldProfile` to go out of scope and free it.
    std::unique_ptr<Mlt::Profile> oldProfile = std::move(m_profile);
    applyProfile();
    rebuildAll();
}

void EngineSync::applyProfile()
{
    const core::Profile &sequenceProfile = m_model.sequence().profile;
    m_previewFactor = previewScaleFactor(m_previewScale, sequenceProfile.height);
    m_profile = makeProfileFrom(sequenceProfile);
    if (m_previewFactor != 1.0) {
        // Even dimensions: yuv420p sources and most scalers need them, and
        // an odd width would skew the display aspect by a pixel. Fps, SAR
        // and DAR are unchanged, so positions and aspect are identical to
        // the full-size profile.
        auto scaled = [this](int size) {
            int value = static_cast<int>(size * m_previewFactor + 0.5);
            return std::max(2, value + value % 2);
        };
        m_profile->set_width(scaled(sequenceProfile.width));
        m_profile->set_height(scaled(sequenceProfile.height));
    }
    Log::debug("[engine] playback profile " + std::to_string(m_profile->width()) + "x" +
               std::to_string(m_profile->height()) + " (preview factor " + std::to_string(m_previewFactor) + ")");
}

EngineSync::ProbedMedia EngineSync::probeMedia(const std::string &path)
{
    return probeMediaFile(m_model.sequence().profile, path);
}

EngineSync::ProbedMedia EngineSync::probeMediaFile(const core::Profile &sequenceProfile, const std::string &path)
{
    core::trace::Scope trace("probeMedia");
    // Its own throwaway profile, not the live *m_profile: that one backs
    // the tractor PlaybackController's Mlt::Consumer may be pulling a
    // frame from right now, on its own thread (CLAUDE.md: things that must
    // not contend with the live playback state open their own
    // Profile/Producer, same as renderProject()).
    std::unique_ptr<Mlt::Profile> probeProfile = makeProfileFrom(sequenceProfile);
    Mlt::Producer producer(*probeProfile, path.c_str());
    if (!producer.is_valid())
        return {};

    ProbedMedia result;
    result.length = producer.get_length();
    const char *service = producer.get("mlt_service");
    result.isStillImage = service && (std::string(service) == "pixbuf" || std::string(service) == "qimage");
    // Where gdk-pixbuf can't load the image (Fedora 44's pixbuf loaders go
    // through glycin, whose sandbox fails to start in a container), MLT
    // falls back to avformat. Standalone repro, MLT 7.40 (2026-09-24):
    // avformat opens a single PNG or JPEG with length INT_MAX and
    // seekable=0, whereas a one-second PNG .mov or MJPEG .avi reports a
    // finite length and seekable=1.
    if (!result.isStillImage && service && std::string(service) == "avformat")
        result.isStillImage = result.length == std::numeric_limits<int>::max() && producer.get_int("seekable") == 0;

    if (!result.isStillImage) {
        // audit E3: previously guessed as "not a still image" -- verified
        // against the avformat producer's own YAML metadata
        // (producer_avformat.yml: "audio_index ... Choose the absolute
        // stream index of audio stream to use (-1 is off)") and a
        // standalone repro (two rendered MP4s, one muxed with an AAC
        // track and one without): the avformat producer auto-detects and
        // sets audio_index to the real stream index at OPEN time, -1 if
        // the container has no audio stream at all -- no frame decode
        // needed first, unlike meta.media.width/height below. The
        // property_exists() guard matters: a non-avformat producer (a
        // generator like color:, reachable here since this whole branch
        // is keyed on "not a still image", not "is avformat") has no
        // audio_index property at all, and get_int() on a missing
        // property returns 0 -- indistinguishable from "stream 0" -- not
        // -1, also confirmed with a standalone repro.
        result.hasAudio = producer.property_exists("audio_index") && producer.get_int("audio_index") >= 0;
        // meta.media.* is populated lazily by the avformat producer on its
        // first decoded frame, not at open time (empirically confirmed) --
        // this throwaway producer is discarded right after, so the decode
        // cost here is one frame, once, per import.
        std::unique_ptr<Mlt::Frame> frame(producer.get_frame());
        int fpsNum = producer.get_int("meta.media.frame_rate_num");
        int fpsDen = producer.get_int("meta.media.frame_rate_den");
        if (fpsNum > 0 && fpsDen > 0)
            result.fps = core::Rational{fpsNum, fpsDen};
        result.width = producer.get_int("meta.media.width");
        result.height = producer.get_int("meta.media.height");
    }

    return result;
}

Mlt::Producer &EngineSync::masterProducerFor(core::AssetId assetId, bool videoEnabled, bool audioEnabled)
{
    const core::Asset &asset = m_model.asset(assetId);

    // One master per (asset, stream switches). MLT ignores video_index/
    // audio_index set on a *cut*: they only take effect on the producer
    // itself, and every cut of it inherits them. Confirmed with a
    // standalone repro against MLT 7.40 (2026-09-24): a cut with
    // audio_index=-1 still played at full level, and one with
    // video_index=-1 still showed its picture, while the same property on
    // the master silenced / blanked it and every cut taken from it. Until
    // this was fixed, a Split Audio clip's video half kept playing its
    // sound under the new audio clip.
    const uint64_t key = (assetId.value << 2) | (videoEnabled ? 1u : 0u) | (audioEnabled ? 2u : 0u);
    auto it = m_masterProducers.find(key);
    if (it == m_masterProducers.end()) {
        auto producer = std::make_shared<Mlt::Producer>(*m_profile, asset.path.c_str());
        if (!producer->is_valid()) {
            // Untrusted input (CLAUDE.md): a project can reference a file
            // that's since been moved, deleted, or lives on an unmounted
            // drive. Confirmed empirically (standalone repro) that an
            // invalid Mlt::Producer still lets .cut() "succeed" -- the
            // resulting cut reports is_valid()==true, appends to a
            // playlist fine, and the tractor built from it reports a
            // normal length -- it only segfaults once a real frame is
            // pulled through the live consumer, deep inside MLT's own
            // mlt_producer_seek/transition_get_frame. That means this is
            // the ONLY place that can catch it: by the time a bad
            // producer would otherwise reach rebuildTrackPlaylist(), it's
            // already too late to tell it apart from a real one. Falls
            // back to a black placeholder sized to the asset's own
            // recorded length, so the clip's span keeps its correct
            // duration and every other clip's timing is unaffected --
            // only its own frames show black instead of crashing the app.
            // Once per asset, not once per stream-switch variant.
            if (m_unavailableAssets.insert(assetId.value).second) {
                Log::error("[engine] could not open asset " + std::to_string(assetId.value) + " (" + asset.path +
                           ") -- showing black in its place");
                mediaUnavailable.emit(asset.path);
            }
            producer = std::make_shared<Mlt::Producer>(*m_profile, kBlackResource);
            core::FrameIndex placeholderLength = std::max<core::FrameIndex>(asset.info.lengthInSequenceFrames, 1);
            producer->set("length", static_cast<int>(placeholderLength));
            producer->set_in_and_out(0, static_cast<int>(placeholderLength - 1));
        }
        // "Off" per avformat's own YAML (-1); harmless on producers that
        // have no such streams (generators, stills).
        if (!videoEnabled)
            producer->set("video_index", -1);
        if (!audioEnabled)
            producer->set("audio_index", -1);
        it = m_masterProducers.emplace(key, std::move(producer)).first;
    }
    Mlt::Producer &producer = *it->second;

    // Still images default to a fixed length in MLT (pixbuf: 15000 frames,
    // verified empirically) regardless of how long a clip the model wants
    // to cut from them -- a still is boundless (MediaInfo::isBoundless()),
    // so the model may legitimately ask for an out point past that, and
    // that recorded length (core::Model::extendAssetLength) can keep
    // growing as later edits cut further in. Re-check and bump on EVERY
    // call, not just when the producer is first created above: a cached
    // producer from an earlier, shorter cut would otherwise keep
    // master.cut(in, out) silently clipping to the old length after a
    // later InsertClip/ResizeClip extends the asset past it (doc 13's E3).
    // Harmless for a real (non-still) asset: its own natural length is
    // left alone.
    if (asset.info.isStillImage && asset.info.lengthInSequenceFrames > producer.get_length()) {
        producer.set("length", static_cast<int>(asset.info.lengthInSequenceFrames));
        producer.set_in_and_out(0, static_cast<int>(asset.info.lengthInSequenceFrames - 1));
    }
    return producer;
}

std::unique_ptr<Mlt::Tractor> EngineSync::buildTransitionSubTractor(const TrackSegment &segment)
{
    const core::Transition &t = m_model.transition(segment.transition);
    const core::Clip &clipA = m_model.clip(segment.a);
    const core::Clip &clipB = m_model.clip(segment.b);

    auto sub = std::make_unique<Mlt::Tractor>(*m_profile);

    Mlt::Producer &masterA = masterProducerFor(clipA.asset, clipA.videoEnabled, clipA.audioEnabled);
    std::unique_ptr<Mlt::Producer> tailA(
        masterA.cut(static_cast<int>(clipA.out - t.length + 1), static_cast<int>(clipA.out)));

    Mlt::Producer &masterB = masterProducerFor(clipB.asset, clipB.videoEnabled, clipB.audioEnabled);
    std::unique_ptr<Mlt::Producer> headB(
        masterB.cut(static_cast<int>(clipB.in), static_cast<int>(clipB.in + t.length - 1)));

    sub->set_track(*tailA, 0);
    sub->set_track(*headB, 1);

    // Both transitions MUST have their own in/out set explicitly to the
    // sub-tractor's local [0, t.length-1) range -- empirically confirmed
    // (2026-09-22, scratchpad/dissolve_repro*.cpp) that leaving them unset
    // corrupts the video dissolve into flat garbage colour (not a
    // documented failure mode; nothing in transition_luma.yml mentions
    // in/out at all) for exactly the frames near the end of the overlap,
    // and ONLY when track 0's cut producer (tailA here) has a non-zero
    // absolute `in` -- i.e. exactly the real case (a clip's tail is never
    // at source frame 0), never the toy zero-based repro that first
    // seemed to prove the plain no-in/out approach worked. Root cause is
    // presumably the transition deriving its own progress ratio from the
    // frame's absolute position when its own in/out aren't set, which is
    // wrong once track 0's producer isn't itself zero-based.
    // Tractor::field() allocates a NEW Mlt::Field wrapper holding its own
    // reference on the field on every call, despite mlt++'s header saying
    // "caller does not own the result" -- leaking it leaks every transition
    // planted in this tractor (sanitizer report S1, 2026-09-23, confirmed
    // with docs/audit/2026-09-23-sanitizer-run/rebuildrepro.cpp: RSS grows
    // ~10 MB per 100 rebuilds leaked, flat when deleted). Own it, once.
    std::unique_ptr<Mlt::Field> field(sub->field());

    Mlt::Transition luma(*m_profile, t.service.c_str());
    luma.set_in_and_out(0, static_cast<int>(t.length - 1));
    field->plant_transition(luma, 0, 1);

    // start=-1 ("automatic linear crossfade from 0 to 1", per the mix
    // module's own YAML) is the crossfade mode, NOT the sum=1/
    // always_active=1 config rebuildAll() uses just below to permanently
    // blend two separate simultaneous tracks -- the YAML says sum is
    // "incompatible with start < 0", confirming those two uses need
    // different settings. Verified against the documented semantics and
    // the same explicit-in/out fix as luma above; unlike luma's visual
    // dissolve, an audio crossfade's correctness wasn't independently
    // confirmed sample-by-sample (an RMS probe on two tone: generators
    // wasn't discriminating enough to prove it either way) -- flagged
    // here rather than claimed as verified.
    Mlt::Transition mix(*m_profile, "mix");
    mix.set("start", -1);
    mix.set_in_and_out(0, static_cast<int>(t.length - 1));
    field->plant_transition(mix, 0, 1);

    sub->refresh();
    return sub;
}

void EngineSync::setPlaylistChunkSize(size_t entries)
{
    g_playlistChunkSize = std::max<size_t>(1, entries);
}

size_t EngineSync::playlistChunkSize()
{
    return g_playlistChunkSize;
}

void EngineSync::rebuildTrackPlaylist(const core::Track &modelTrack, Mlt::Playlist &playlist)
{
    playlist.clear();
    const std::vector<TrackSegment> segments = core::planTrackSegments(m_model, modelTrack);

    // MLT refreshes a whole playlist after every append or blank
    // (mlt_playlist_virtual_append() ends in mlt_playlist_virtual_refresh(),
    // which walks every entry: mlt_playlist.c, v7.40.0), so building a track
    // of k entries flat is O(k^2) -- 5,000 clips on one track took 21.6 s.
    // A track with more than kPlaylistChunk entries is built from nested
    // sub-playlists of that many, each marked "ustudio.chunk" for verify();
    // measured linear, same frames (doc 19 MT2). A track that fits in one
    // chunk stays flat, exactly as before.
    size_t entryCount = 0;
    core::FrameIndex cursor = 0;
    for (const TrackSegment &seg : segments) {
        entryCount += (seg.start > cursor ? 2 : 1);
        cursor = seg.start + seg.length;
    }
    const size_t chunkSize = playlistChunkSize();
    const bool chunked = entryCount > chunkSize;

    std::unique_ptr<Mlt::Playlist> chunk;
    size_t inChunk = 0;
    auto target = [&]() -> Mlt::Playlist & {
        if (!chunked)
            return playlist;
        if (!chunk) {
            chunk = std::make_unique<Mlt::Playlist>(*m_profile);
            chunk->set(kChunkProperty, 1);
        }
        return *chunk;
    };
    auto flush = [&] {
        if (chunk) {
            playlist.append(*chunk);
            chunk.reset();
            inChunk = 0;
        }
    };
    auto added = [&] {
        if (chunked && ++inChunk == chunkSize)
            flush();
    };

    cursor = 0;
    for (const TrackSegment &seg : segments) {
        if (seg.start > cursor) {
            target().blank(static_cast<int>(seg.start - cursor - 1));
            added();
        }

        if (seg.kind == TrackSegment::Kind::Clip) {
            const core::Clip &clip = m_model.clip(seg.clip);
            Mlt::Producer &master = masterProducerFor(clip.asset, clip.videoEnabled, clip.audioEnabled);
            std::unique_ptr<Mlt::Producer> cut(master.cut(static_cast<int>(seg.in), static_cast<int>(seg.out)));
            target().append(*cut);
        } else {
            target().append(*buildTransitionSubTractor(seg));
        }
        added();

        cursor = seg.start + seg.length;
    }
    flush();
}

void EngineSync::rebuildAll()
{
    Log::ScopedTimer timer("[engine] rebuildAll");
    core::trace::Scope trace("EngineSync::rebuildAll");
    const core::Sequence &seq = m_model.sequence();
    size_t clipCount = seq.clips.size();
    size_t trackCount = seq.tracks.size();
    Log::debug("[engine] rebuildAll: " + std::to_string(trackCount) + " tracks, " + std::to_string(clipCount) +
               " clips, sequence length " + std::to_string(seq.length()));

    FactoryPolicy::raiseAvformatDecoderLimit(trackCount);
    auto newTractor = std::make_shared<Mlt::Tractor>(*m_profile);
    std::vector<std::optional<core::TrackId>> order;

    // Index 0: black backing track (doc 03), not a model track -- gaps on
    // every real track composite over black instead of over nothing.
    // Cut from one cached master rather than a fresh "color:black" per
    // rebuild: a producer created through MLT's loader leaks the normaliser
    // filters the loader attaches, ~4.6 KB each, and this one used to be
    // recreated on every edit (sanitizer report S3, 2026-09-23; LSan's
    // largest remaining per-rebuild leak once S1 was fixed). The report's
    // other suggestion, naming the service directly (Mlt::Producer(profile,
    // "colour", "black")) to skip the loader, is NOT safe: without the
    // loader's normalisers, engine-render hits a deterministic
    // heap-buffer-overflow in avformat's sample_fifo_append under ASan (3/3
    // runs; 3/3 clean through the loader) -- so the loader stays, once.
    // Created with a fixed, very long length and never mutated afterwards:
    // a running consumer may still be pulling from an older tractor's cut
    // of it while this rebuild runs, so resizing it per rebuild would race.
    if (!m_blackMaster) {
        m_blackMaster = std::make_unique<Mlt::Producer>(*m_profile, kBlackResource);
        m_blackMaster->set("length", kBlackMasterLength);
        m_blackMaster->set_in_and_out(0, kBlackMasterLength - 1);
    }
    core::FrameIndex sequenceLength = std::max<core::FrameIndex>(seq.length(), 1);
    // Mlt::Producer::cut() returns a new, caller-owned wrapper (README's
    // "mlt++ accessors" note); set_track() takes its own reference.
    std::unique_ptr<Mlt::Producer> black(
        m_blackMaster->cut(0, static_cast<int>(std::min<core::FrameIndex>(sequenceLength, kBlackMasterLength) - 1)));
    newTractor->set_track(*black, 0);
    order.emplace_back(std::nullopt);

    // core::mltTrackOrder: audio tracks first (model order), then video
    // tracks bottom-to-top (doc 03) -- shared with core/xml's writer so
    // the saved file's MLT-facing structure matches this exactly.
    // Mlt::Playlist has no move constructor (only Playlist(Playlist&), a
    // non-const-ref copy), so std::vector<Playlist> can't grow -- each is
    // heap-allocated instead. set_track() bumps the underlying mlt_service's
    // refcount, so these can be dropped once rebuildAll() returns; MLT
    // itself keeps the object alive (same pattern v1's MltEngine relies on
    // for locals passed to Playlist::insert()).
    std::vector<std::unique_ptr<Mlt::Playlist>> playlists;

    for (core::TrackId trackId : core::mltTrackOrder(seq)) {
        const core::Track &modelTrack = m_model.track(trackId);
        auto playlist = std::make_unique<Mlt::Playlist>(*m_profile);
        rebuildTrackPlaylist(modelTrack, *playlist);

        // Track-wide level (doc 03: "audio tracks and the audio part of
        // video tracks"). A local, like `black` above -- attach() bumps
        // the filter's own refcount on the service, so it's fine for this
        // wrapper to go out of scope once the loop body ends.
        if (modelTrack.volume != 1.0) {
            Mlt::Filter volumeFilter(*m_profile, "volume");
            volumeFilter.set("level", core::linearToDecibels(modelTrack.volume));
            playlist->attach(volumeFilter);
        }

        // Track mute/hide: MLT's own per-track "hide" (1 = video, 2 = audio,
        // 3 = both) on the track's producer, confirmed with a standalone
        // repro (2026-09-24): a hidden top track showed what was under it,
        // a muted one mixed to silence.
        int hide = (modelTrack.hidden ? 1 : 0) | (modelTrack.muted ? 2 : 0);
        if (hide != 0)
            playlist->set("hide", hide);

        newTractor->set_track(*playlist, static_cast<int>(order.size()));
        order.emplace_back(trackId);
        playlists.push_back(std::move(playlist));
    }

    // Chain composite (video) + mix (audio) across every adjacent index --
    // matches v1's proven-working MltEngine::plantTrackTransitions exactly
    // (see the class comment for why this is simpler than doc 05's graph).
    // One owned Field wrapper for the whole tractor -- see
    // buildTransitionSubTractor()'s comment (sanitizer report S1).
    std::unique_ptr<Mlt::Field> field(newTractor->field());
    for (int index = 1; index < newTractor->count(); ++index) {
        Mlt::Transition composite(*m_profile, "composite");
        field->plant_transition(composite, index - 1, index);

        Mlt::Transition mix(*m_profile, "mix");
        mix.set("start", 1.0);
        mix.set("sum", 1);
        mix.set("always_active", 1);
        field->plant_transition(mix, index - 1, index);
    }

    newTractor->refresh();
    m_tractor = std::move(newTractor);
    m_mltTrackOrder = std::move(order);
    rebuilt.emit();
}

std::vector<std::string> EngineSync::verify() const
{
    std::vector<std::string> problems;
    const core::Sequence &seq = m_model.sequence();

    if (m_tractor->get_length() != static_cast<int>(std::max<core::FrameIndex>(seq.length(), 1))) {
        problems.push_back("tractor length " + std::to_string(m_tractor->get_length()) + " != sequence length " +
                           std::to_string(seq.length()));
    }

    for (size_t mltIndex = 0; mltIndex < m_mltTrackOrder.size(); ++mltIndex) {
        if (!m_mltTrackOrder[mltIndex])
            continue; // black backing track

        core::TrackId trackId = *m_mltTrackOrder[mltIndex];
        const core::Track &modelTrack = m_model.track(trackId);

        // Same new-wrapper-per-call behaviour as Tractor::field() (report
        // S1, and the 2026-09-20 audit's E7): owned, not borrowed.
        std::unique_ptr<Mlt::Producer> raw(m_tractor->track(static_cast<int>(mltIndex)));
        if (!raw) {
            problems.push_back("track " + std::to_string(trackId.value) + ": no MLT producer at index");
            continue;
        }
        Mlt::Playlist playlist(*raw);
        std::vector<TrackSegment> segments = core::planTrackSegments(m_model, modelTrack);
        std::vector<PlaylistEntry> entries;
        flattenPlaylist(playlist, 0, entries);

        int nonBlankCount = 0;
        for (const PlaylistEntry &entry : entries) {
            if (!entry.blank)
                ++nonBlankCount;
        }
        if (nonBlankCount != static_cast<int>(segments.size())) {
            problems.push_back("track " + std::to_string(trackId.value) + ": playlist has " +
                               std::to_string(nonBlankCount) + " non-blank entries, expected " +
                               std::to_string(segments.size()));
            continue;
        }

        int entryIndex = 0;
        for (size_t i = 0; i < entries.size() && entryIndex < static_cast<int>(segments.size()); ++i) {
            const PlaylistEntry &entry = entries[i];
            if (entry.blank)
                continue;

            const TrackSegment &seg = segments[static_cast<size_t>(entryIndex)];
            if (!entry.valid) {
                problems.push_back("track " + std::to_string(trackId.value) + ": clip_info(" + std::to_string(i) +
                                   ") failed");
                ++entryIndex;
                continue;
            }

            if (entry.start != seg.start) {
                problems.push_back("track " + std::to_string(trackId.value) + " segment " + std::to_string(entryIndex) +
                                   ": playlist start " + std::to_string(entry.start) + " != expected " +
                                   std::to_string(seg.start));
            }
            if (entry.frameCount != seg.length) {
                problems.push_back("track " + std::to_string(trackId.value) + " segment " + std::to_string(entryIndex) +
                                   ": playlist frame_count " + std::to_string(entry.frameCount) +
                                   " != expected length " + std::to_string(seg.length));
            }

            if (seg.kind == TrackSegment::Kind::Clip) {
                if (entry.frameIn != seg.in || entry.frameOut != seg.out) {
                    problems.push_back("clip " + std::to_string(seg.clip.value) + ": playlist in/out " +
                                       std::to_string(entry.frameIn) + "/" + std::to_string(entry.frameOut) +
                                       " != expected " + std::to_string(seg.in) + "/" + std::to_string(seg.out));
                }
                const core::Clip &clip = m_model.clip(seg.clip);
                // A known-unavailable asset's cut deliberately comes from
                // the black placeholder masterProducerFor() substituted,
                // not the asset's own path -- that mismatch is the
                // intended fallback (see its own comment), not a sync bug.
                if (m_model.hasAsset(clip.asset) && !m_unavailableAssets.contains(clip.asset.value)) {
                    std::string expectedResource = m_model.asset(clip.asset).path;
                    // MLT's "resource" property for a "service:arg" shorthand
                    // producer (color:/noise:/tone: generators, used by
                    // tests/engine/test_engine_sync.cpp) is just the argument
                    // -- "service" is split off into mlt_service separately,
                    // confirmed empirically. Real absolute file paths (the
                    // production case) never match this shape, since they
                    // start with '/' before any colon, so this only strips
                    // the prefix for the shorthand form.
                    if (size_t colon = expectedResource.find(':');
                        colon != std::string::npos && expectedResource.find('/') > colon) {
                        expectedResource = expectedResource.substr(colon + 1);
                    }
                    std::string actualResource = entry.resource;
                    // A generator with no argument ("tone:") has an empty
                    // resource, and a cut of it reports MLT's "<producer>"
                    // placeholder instead (seen 2026-09-24 in
                    // test_xml_playback) -- the same producer, not a mismatch.
                    if (expectedResource.empty() && actualResource == "<producer>")
                        actualResource.clear();
                    if (actualResource != expectedResource) {
                        problems.push_back("clip " + std::to_string(seg.clip.value) + ": playlist resource '" +
                                           actualResource + "' != asset path '" + expectedResource + "'");
                    }
                }
            }
            // Kind::Transition: start/length already checked above; its
            // internals (the nested tail/head cuts + luma/mix) were
            // verified once via the standalone repro this class comments
            // reference -- clip_info() on a nested-tractor entry doesn't
            // expose a single resource/in/out to compare against a model
            // clip the way an ordinary cut does.

            ++entryIndex;
        }
    }

    return problems;
}

namespace {
// Bridges MLT's C-style "consumer-frame-render" event (a plain function
// pointer, no captures) to the caller's std::function. NOT the
// "consumer-frame-show" PlaybackController::handleFrameShow listens for
// (live playback's sdl2_audio consumer) -- verified via
// mlt_consumer.h's own doc comment that "consumer-frame-show" is fired
// by SUBCLASS implementations only (sdl2_audio fires it when it
// actually shows/plays a frame), while "consumer-frame-render" is fired
// by the BASE CLASS before rendering every frame, for every consumer
// type. Confirmed empirically: a real render with "consumer-frame-show"
// wired up here got zero callbacks (avformat never shows anything, so
// never fires it); switching to "consumer-frame-render" fixed it. Same
// event-data shape either way (a frame, per the same header). Throttled
// to roughly once every half a second of wall-clock render time so a
// long render doesn't call back on every single frame (108,000 of them
// for a 30fps hour-long project) for what's ultimately a status label.
//
// lastCall is an optional, not a bare time_point defaulted to min(): the
// obvious "now - min() is huge, so the first call always passes" reads
// right but isn't -- found live that steady_clock's duration overflows
// computing that difference (min() is near the representation's most
// negative value; subtracting it from a normal "now" overflows the
// underlying signed rep), wrapping around to a garbage, usually-negative
// result. That silently failed the throttle check on *every* call, not
// just skipped some -- the render completed correctly and the listener
// fired every time (confirmed via temporary logging), but onProgress()
// itself was never reached. An empty optional has no such arithmetic to
// go wrong.
struct RenderProgressContext
{
    const std::function<void(int, int)> &onProgress;
    int totalFrames;
    std::optional<std::chrono::steady_clock::time_point> lastCall;
};

void renderProgressTrampoline(mlt_properties /*owner*/, void *self, mlt_event_data data)
{
    auto *context = static_cast<RenderProgressContext *>(self);
    Mlt::Frame frame(Mlt::EventData(data).to_frame());
    if (!frame.is_valid())
        return;

    auto now = std::chrono::steady_clock::now();
    if (context->lastCall && now - *context->lastCall < std::chrono::milliseconds(500))
        return;
    context->lastCall = now;

    context->onProgress(frame.get_position(), context->totalFrames);
}
} // namespace

const std::string &h264Encoder()
{
    static const std::string encoder = [] {
        // avformat's documented "list" value (consumer_avformat.yml):
        // start() fills the consumer's "vcodec" data with every encoder
        // name, and also prints them to stdout -- once per process.
        Mlt::Profile profile;
        Mlt::Consumer consumer(profile, "avformat");
        consumer.set("vcodec", "list");
        consumer.start();
        consumer.stop();
        auto *list = static_cast<mlt_properties>(consumer.get_data("vcodec"));
        std::string found;
        if (!list) {
            Log::warn("[engine] avformat returned no encoder list");
            return found;
        }
        Mlt::Properties codecs(list);
        for (const char *preferred : {"libx264", "libopenh264"}) {
            for (int i = 0; found.empty() && i < codecs.count(); ++i) {
                if (const char *name = codecs.get(i); name && std::string(name) == preferred)
                    found = preferred;
            }
        }
        Log::info("[engine] H.264 encoder: " + (found.empty() ? std::string("none") : found));
        return found;
    }();
    return encoder;
}

bool h264HasQualityMode()
{
    return h264Encoder() == "libx264";
}

bool renderProject(core::Model &model, const std::string &outputPath, std::string &error,
                   std::function<void(int, int)> onProgress, const std::atomic<bool> *cancel,
                   const core::RenderProfile &profile)
{
    Log::ScopedTimer timer("[engine] renderProject total");
    EngineSync renderSync(model); // its own Profile/Tractor, independent of any live one

    // Audit A6: render to a "<path>.part" sibling and rename into place
    // only on success, matching CLAUDE.md's "renders... written to an
    // explicitly chosen output path, atomically" -- a render that dies
    // partway through (crash, disk full, killed process) must never leave
    // a half-written file sitting at the name the user actually asked for,
    // which a later render or another program could mistake for a
    // complete one.
    std::string partPath = outputPath + ".part";
    Mlt::Consumer consumer(renderSync.profile(), "avformat", partPath.c_str());
    if (!consumer.is_valid()) {
        error = "Could not create renderer for: " + outputPath;
        Log::error("[engine] " + error);
        std::error_code ec;
        std::filesystem::remove(partPath, ec);
        return false;
    }
    // Matches this project's fixed working format (see mlt_engine.cpp's
    // former profile comment, now on Model::createEmpty()'s default
    // Profile): h264 High/yuv420p, AAC 48kHz stereo, MP4 — verified
    // against a real reference file's ffprobe output and confirmed via a
    // standalone render+reprobe round-trip before wiring this in (v1).
    if (h264Encoder().empty()) {
        error = "No H.264 encoder available (ffmpeg has neither libx264 nor libopenh264)";
        Log::error("[engine] " + error);
        return false;
    }
    consumer.set("vcodec", h264Encoder().c_str());
    consumer.set("acodec", "aac");
    consumer.set("f", "mp4");
    // Verified with a standalone repro (2026-09-25, MLT 7.40, libx264):
    // "crf" and "preset" pass through to x264 as AVOptions (its SEI read
    // back "rc=crf ... crf=18.0", and preset=slow gave subme=8), and
    // "width"/"height" -- documented in consumer_avformat.yml as overriding
    // the profile -- scale the output. "frame_rate_num/den" only relabel
    // the stream's rate (60 frames at 60 fps played 1 s, not 2), so the
    // output always keeps the project's frame rate.
    const core::EncoderSettings settings =
        core::encoderSettings(profile, model.sequence().profile, h264HasQualityMode());
    if (settings.width > 0 && settings.height > 0) {
        consumer.set("width", settings.width);
        consumer.set("height", settings.height);
        consumer.set("sample_aspect_num", 1);
        consumer.set("sample_aspect_den", 1);
    }
    if (settings.crf >= 0) {
        consumer.set("crf", settings.crf);
        consumer.set("preset", settings.preset.c_str());
    } else {
        consumer.set("vb", std::to_string(settings.videoBitrate).c_str());
    }
    consumer.set("ab", std::to_string(settings.audioBitrate).c_str());
    Log::info("[engine] Render profile \"" + profile.name + "\": " +
              (settings.crf >= 0 ? "crf " + std::to_string(settings.crf) + " " + settings.preset
                                 : "vb " + std::to_string(settings.videoBitrate)) +
              (settings.width > 0 ? ", " + std::to_string(settings.width) + "x" + std::to_string(settings.height)
                                  : std::string()));
    consumer.set("ar", "48000");
    consumer.set("channels", 2);
    consumer.set("pix_fmt", "yuv420p");
    consumer.set("real_time", -1); // render every frame; don't drop frames to keep up with a clock
    consumer.connect(renderSync.tractor());

    // Registered before run() (which blocks until the render finishes),
    // so every "consumer-frame-render" the encode fires reaches
    // renderProgressTrampoline for however long that call runs; the
    // returned Event must stay alive for the same span, hence the local
    // (not immediately discarded) unique_ptr.
    RenderProgressContext progressContext{onProgress, renderSync.tractor().get_length(), std::nullopt};
    std::unique_ptr<Mlt::Event> progressEvent;
    if (onProgress)
        progressEvent.reset(consumer.listen("consumer-frame-render", &progressContext, renderProgressTrampoline));

    Log::info("[engine] Rendering project to " + outputPath + " (via " + partPath + ") ...");
    // Consumer::run() is start() plus a wait for "consumer-stopped" (mlt++'s
    // MltConsumer.cpp). Doing the wait here instead lets a cancel stop the
    // consumer from this thread, which is not the consumer's own.
    int result = consumer.start();
    bool cancelled = false;
    while (result == 0 && !consumer.is_stopped()) {
        if (cancel && cancel->load()) {
            consumer.stop();
            cancelled = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    // A finished render reads stopped only after avformat's thread has
    // written the trailer and closed the file (consumer_avformat.c ends with
    // mlt_consumer_stopped(), which clears "running"; MLT 7.40 source).
    // stop() then just joins that thread, before the .part is renamed.
    consumer.stop();
    if (cancelled) {
        error = "Render cancelled";
        Log::info("[engine] Render to " + outputPath + " cancelled");
        std::error_code ec;
        std::filesystem::remove(partPath, ec);
        return false;
    }
    if (result != 0) {
        error = "Render failed (consumer returned " + std::to_string(result) + ")";
        Log::error("[engine] " + error);
        std::error_code ec;
        std::filesystem::remove(partPath, ec);
        return false;
    }

    // consumer.run()'s return code is one more MLT value CLAUDE.md's
    // empirical-knowledge rule says not to trust: reproduced by pointing
    // outputPath at a directory that doesn't exist -- avformat logs
    // "Could not open '<part path>'" and consumer.run() still returns 0,
    // even though the .part file was never created. The rename below is
    // the actual safety net for that case, not just for a genuine disk-
    // full/permission failure partway through encoding.
    std::error_code ec;
    std::filesystem::rename(partPath, outputPath, ec);
    if (ec) {
        error = "Rendered successfully but couldn't move it to " + outputPath + " (" + ec.message() + ")";
        Log::error("[engine] " + error);
        return false;
    }

    Log::info("[engine] Rendered project to " + outputPath);
    return true;
}

} // namespace ustudio::engine
