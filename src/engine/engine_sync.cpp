#include "engine_sync.h"

#include "core/log.h"
#include "core/model/mlt_order.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <system_error>
#include <variant>

namespace ustudio::engine {

namespace Log = ustudio::core::Log;

namespace {
constexpr const char *kBlackResource = "color:black";

// MLT's "volume" filter takes its "level" property in dB (verified against
// the module's own YAML metadata: "level"/"The animated value of the gain
// adjustment in dB" -- "gain" is the deprecated linear/string form).
// Track::volume is a plain linear scale (1.0 = unity) for a simpler slider
// UI, so this is the conversion point. Standard 20*log10 amplitude-ratio
// formula; empirically confirmed with a standalone repro (a 440Hz tone
// through a playlist with this filter attached at -20dB measured 0.1x peak
// amplitude, exactly the expected ratio). Treats anything at or below a
// small linear threshold as a fixed silence floor rather than computing
// log10(0) = -inf.
double linearToDecibels(double linear)
{
    constexpr double kSilenceFloorDb = -60.0;
    constexpr double kMinLinear = 0.0001;
    if (linear <= kMinLinear)
        return kSilenceFloorDb;
    return 20.0 * std::log10(linear);
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

EngineSync::EngineSync(core::Model &model) : m_model(model)
{
    applyProfile();
    rebuildAll();
    connectToModel();
}

EngineSync::~EngineSync()
{
    m_model.changed.disconnect(m_modelConnection);
}

void EngineSync::reset()
{
    Log::ScopedTimer timer("[engine] reset");
    Log::debug("[engine] reset: dropping " + std::to_string(m_masterProducers.size()) + " cached master producer(s)");
    m_masterProducers.clear();
    m_unavailableAssets.clear(); // a reopened project's media may have come back since -- give it a fresh try

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
    connectToModel();
}

void EngineSync::connectToModel()
{
    // Idempotent (audit C4): disconnects whatever this EngineSync was
    // previously subscribed with before adding a new one, so calling this
    // twice (construction, then reset()) never leaves two live
    // subscriptions both firing onModelEvent() for the same edit.
    m_model.changed.disconnect(m_modelConnection);
    m_modelConnection = m_model.changed.connect([this](const core::ModelEvent &event) { onModelEvent(event); });
}

void EngineSync::onModelEvent(const core::ModelEvent &event)
{
    if (std::holds_alternative<core::BatchBegin>(event)) {
        ++m_batchDepth;
        return;
    }
    if (std::holds_alternative<core::BatchEnd>(event)) {
        if (m_batchDepth > 0)
            --m_batchDepth;
        if (m_batchDepth == 0 && m_dirty)
            rebuildAll();
        return;
    }

    m_dirty = true;
    if (m_batchDepth == 0)
        rebuildAll();
}

void EngineSync::applyProfile()
{
    m_profile = makeProfileFrom(m_model.sequence().profile);
}

EngineSync::ProbedMedia EngineSync::probeMedia(const std::string &path)
{
    // Its own throwaway profile, not the live *m_profile: that one backs
    // the tractor PlaybackController's Mlt::Consumer may be pulling a
    // frame from right now, on its own thread (CLAUDE.md: things that must
    // not contend with the live playback state open their own
    // Profile/Producer, same as renderProject()).
    std::unique_ptr<Mlt::Profile> probeProfile = makeProfileFrom(m_model.sequence().profile);
    Mlt::Producer producer(*probeProfile, path.c_str());
    if (!producer.is_valid())
        return {};

    ProbedMedia result;
    result.length = producer.get_length();
    const char *service = producer.get("mlt_service");
    result.isStillImage = service && (std::string(service) == "pixbuf" || std::string(service) == "qimage");

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

Mlt::Producer &EngineSync::masterProducerFor(core::AssetId assetId)
{
    const core::Asset &asset = m_model.asset(assetId);

    auto it = m_masterProducers.find(assetId.value);
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
            Log::error("[engine] could not open asset " + std::to_string(assetId.value) + " (" + asset.path +
                      ") -- showing black in its place");
            mediaUnavailable.emit(asset.path);
            m_unavailableAssets.insert(assetId.value);
            producer = std::make_shared<Mlt::Producer>(*m_profile, kBlackResource);
            core::FrameIndex placeholderLength = std::max<core::FrameIndex>(asset.info.lengthInSequenceFrames, 1);
            producer->set("length", static_cast<int>(placeholderLength));
            producer->set_in_and_out(0, static_cast<int>(placeholderLength - 1));
        }
        it = m_masterProducers.emplace(assetId.value, std::move(producer)).first;
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

std::vector<EngineSync::TrackSegment> EngineSync::planTrackSegments(const core::Track &modelTrack) const
{
    std::vector<TrackSegment> segments;
    const core::Sequence &seq = m_model.sequence();
    const auto &clips = modelTrack.clips; // sorted by position (Model invariant)

    for (size_t i = 0; i < clips.size(); ++i) {
        core::ClipId clipId = clips[i];
        const core::Clip &clip = m_model.clip(clipId);

        // Is this clip the `b` side of a transition with the PREVIOUS
        // clip? If so, its head is already spoken for by that
        // transition's sub-tractor segment (appended below when the
        // previous clip was processed) -- only its own exclusive tail
        // (if any) needs a segment here.
        const core::Transition *incoming = nullptr;
        if (i > 0) {
            core::ClipId previous = clips[i - 1];
            for (const auto &t : seq.transitions) {
                if (t.track == modelTrack.id && t.a == previous && t.b == clipId) {
                    incoming = &t;
                    break;
                }
            }
        }
        const core::Transition *outgoing = nullptr;
        if (i + 1 < clips.size()) {
            core::ClipId next = clips[i + 1];
            for (const auto &t : seq.transitions) {
                if (t.track == modelTrack.id && t.a == clipId && t.b == next) {
                    outgoing = &t;
                    break;
                }
            }
        }

        // Trimmed at the head if a transition already consumed it (that
        // clip was `incoming`'s `a`) and/or at the tail if this clip
        // starts one of its own (`outgoing`) -- either, both, or neither.
        core::FrameIndex segStart = incoming ? clip.position + incoming->length : clip.position;
        core::FrameIndex segIn = incoming ? clip.in + incoming->length : clip.in;
        core::FrameIndex segEnd = outgoing ? clip.end() - outgoing->length : clip.end();
        core::FrameIndex segOut = outgoing ? clip.out - outgoing->length : clip.out;
        if (segStart < segEnd) { // omitted entirely if transition(s) consumed the whole clip
            TrackSegment seg;
            seg.kind = TrackSegment::Kind::Clip;
            seg.start = segStart;
            seg.length = segEnd - segStart;
            seg.clip = clipId;
            seg.in = segIn;
            seg.out = segOut;
            segments.push_back(seg);
        }

        if (outgoing) {
            TrackSegment seg;
            seg.kind = TrackSegment::Kind::Transition;
            seg.start = segEnd; // == the next clip's (already-adjusted) position
            seg.length = outgoing->length;
            seg.transition = outgoing->id;
            seg.a = clipId;
            seg.b = clips[i + 1];
            segments.push_back(seg);
        }
    }

    return segments;
}

std::unique_ptr<Mlt::Tractor> EngineSync::buildTransitionSubTractor(const TrackSegment &segment)
{
    const core::Transition &t = m_model.transition(segment.transition);
    const core::Clip &clipA = m_model.clip(segment.a);
    const core::Clip &clipB = m_model.clip(segment.b);

    auto sub = std::make_unique<Mlt::Tractor>(*m_profile);

    Mlt::Producer &masterA = masterProducerFor(clipA.asset);
    std::unique_ptr<Mlt::Producer> tailA(
        masterA.cut(static_cast<int>(clipA.out - t.length + 1), static_cast<int>(clipA.out)));
    if (!clipA.videoEnabled)
        tailA->set("video_index", -1);
    if (!clipA.audioEnabled)
        tailA->set("audio_index", -1);

    Mlt::Producer &masterB = masterProducerFor(clipB.asset);
    std::unique_ptr<Mlt::Producer> headB(
        masterB.cut(static_cast<int>(clipB.in), static_cast<int>(clipB.in + t.length - 1)));
    if (!clipB.videoEnabled)
        headB->set("video_index", -1);
    if (!clipB.audioEnabled)
        headB->set("audio_index", -1);

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
    Mlt::Transition luma(*m_profile, t.service.c_str());
    luma.set_in_and_out(0, static_cast<int>(t.length - 1));
    sub->field()->plant_transition(luma, 0, 1);

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
    sub->field()->plant_transition(mix, 0, 1);

    sub->refresh();
    return sub;
}

void EngineSync::rebuildTrackPlaylist(const core::Track &modelTrack, Mlt::Playlist &playlist)
{
    playlist.clear();
    core::FrameIndex cursor = 0;

    for (const TrackSegment &seg : planTrackSegments(modelTrack)) {
        if (seg.start > cursor) {
            // Playlist::blank(out) appends out+1 frames.
            playlist.blank(static_cast<int>(seg.start - cursor - 1));
        }

        if (seg.kind == TrackSegment::Kind::Clip) {
            const core::Clip &clip = m_model.clip(seg.clip);
            Mlt::Producer &master = masterProducerFor(clip.asset);
            std::unique_ptr<Mlt::Producer> cut(master.cut(static_cast<int>(seg.in), static_cast<int>(seg.out)));
            // video_index/audio_index=-1 ("off"), verified against avformat's
            // own YAML metadata. Set on the cut, not the master: cuts carry
            // their own property overrides in MLT (the mechanism that
            // already lets different cuts of the same file have different
            // speed/effects), so this only silences this one clip, not
            // every other cut of the same asset elsewhere on the timeline.
            // This is what makes SplitAudio's two resulting clips (a
            // video-only original, a new audio-only one) actually play as
            // split, not just look split in the model.
            if (!clip.videoEnabled)
                cut->set("video_index", -1);
            if (!clip.audioEnabled)
                cut->set("audio_index", -1);
            playlist.append(*cut);
        } else {
            playlist.append(*buildTransitionSubTractor(seg));
        }

        cursor = seg.start + seg.length;
    }
}

void EngineSync::rebuildAll()
{
    Log::ScopedTimer timer("[engine] rebuildAll");
    const core::Sequence &seq = m_model.sequence();
    size_t clipCount = seq.clips.size();
    size_t trackCount = seq.tracks.size();
    Log::debug("[engine] rebuildAll: " + std::to_string(trackCount) + " tracks, " + std::to_string(clipCount) +
               " clips, sequence length " + std::to_string(seq.length()));

    auto newTractor = std::make_shared<Mlt::Tractor>(*m_profile);
    std::vector<std::optional<core::TrackId>> order;

    // Index 0: black backing track (doc 03), not a model track -- gaps on
    // every real track composite over black instead of over nothing.
    Mlt::Producer black(*m_profile, kBlackResource);
    core::FrameIndex sequenceLength = std::max<core::FrameIndex>(seq.length(), 1);
    black.set_in_and_out(0, static_cast<int>(sequenceLength - 1));
    newTractor->set_track(black, 0);
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
            volumeFilter.set("level", linearToDecibels(modelTrack.volume));
            playlist->attach(volumeFilter);
        }

        newTractor->set_track(*playlist, static_cast<int>(order.size()));
        order.emplace_back(trackId);
        playlists.push_back(std::move(playlist));
    }

    // Chain composite (video) + mix (audio) across every adjacent index --
    // matches v1's proven-working MltEngine::plantTrackTransitions exactly
    // (see the class comment for why this is simpler than doc 05's graph).
    for (int index = 1; index < newTractor->count(); ++index) {
        Mlt::Transition composite(*m_profile, "composite");
        newTractor->field()->plant_transition(composite, index - 1, index);

        Mlt::Transition mix(*m_profile, "mix");
        mix.set("start", 1.0);
        mix.set("sum", 1);
        mix.set("always_active", 1);
        newTractor->field()->plant_transition(mix, index - 1, index);
    }

    newTractor->refresh();
    m_tractor = std::move(newTractor);
    m_mltTrackOrder = std::move(order);
    m_dirty = false;
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

        Mlt::Producer *raw = m_tractor->track(static_cast<int>(mltIndex));
        if (!raw) {
            problems.push_back("track " + std::to_string(trackId.value) + ": no MLT producer at index");
            continue;
        }
        Mlt::Playlist playlist(*raw);
        std::vector<TrackSegment> segments = planTrackSegments(modelTrack);

        int nonBlankCount = 0;
        for (int i = 0; i < playlist.count(); ++i) {
            if (!playlist.is_blank(i))
                ++nonBlankCount;
        }
        if (nonBlankCount != static_cast<int>(segments.size())) {
            problems.push_back("track " + std::to_string(trackId.value) + ": playlist has " +
                               std::to_string(nonBlankCount) + " non-blank entries, expected " +
                               std::to_string(segments.size()));
            continue;
        }

        int entryIndex = 0;
        for (int i = 0; i < playlist.count() && entryIndex < static_cast<int>(segments.size()); ++i) {
            if (playlist.is_blank(i))
                continue;

            const TrackSegment &seg = segments[static_cast<size_t>(entryIndex)];
            std::unique_ptr<Mlt::ClipInfo> info(playlist.clip_info(i));
            if (!info) {
                problems.push_back("track " + std::to_string(trackId.value) + ": clip_info(" + std::to_string(i) +
                                   ") failed");
                ++entryIndex;
                continue;
            }

            if (info->start != seg.start) {
                problems.push_back("track " + std::to_string(trackId.value) + " segment " +
                                   std::to_string(entryIndex) + ": playlist start " + std::to_string(info->start) +
                                   " != expected " + std::to_string(seg.start));
            }
            if (info->frame_count != seg.length) {
                problems.push_back("track " + std::to_string(trackId.value) + " segment " +
                                   std::to_string(entryIndex) + ": playlist frame_count " +
                                   std::to_string(info->frame_count) + " != expected length " +
                                   std::to_string(seg.length));
            }

            if (seg.kind == TrackSegment::Kind::Clip) {
                if (info->frame_in != seg.in || info->frame_out != seg.out) {
                    problems.push_back("clip " + std::to_string(seg.clip.value) + ": playlist in/out " +
                                       std::to_string(info->frame_in) + "/" + std::to_string(info->frame_out) +
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
                    std::string actualResource = info->resource ? info->resource : "";
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

bool renderProject(core::Model &model, const std::string &outputPath, std::string &error,
                   std::function<void(int, int)> onProgress)
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
    consumer.set("vcodec", "libx264");
    consumer.set("acodec", "aac");
    consumer.set("f", "mp4");
    consumer.set("vb", "922698");
    consumer.set("ab", "126422");
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
    int result = consumer.run();
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
