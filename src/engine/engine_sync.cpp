#include "engine_sync.h"

#include "factory_policy.h"

#include "core/log.h"
#include "core/trace.h"
#include "core/model/audio_level.h"
#include "core/model/mlt_order.h"
#include "core/model/retime.h"
#include "core/model/track_segments.h"
#include "core/model/transform.h"
#include "platform/console.h"

#include <algorithm>
#include <cstring>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <system_error>
#include <thread>
#include <variant>

#include <cstdio>

namespace ustudio::engine {

// core::Easing is MLT's mlt_keyframe_type value for value (IP1, doc 15): the
// engine maps it by casting, so pin the ends and the smooth variants here,
// against the installed MLT headers.
static_assert(static_cast<int>(core::Easing::Discrete) == mlt_keyframe_discrete);
static_assert(static_cast<int>(core::Easing::Linear) == mlt_keyframe_linear);
static_assert(static_cast<int>(core::Easing::SmoothLoose) == mlt_keyframe_smooth_loose);
static_assert(static_cast<int>(core::Easing::SmoothNatural) == mlt_keyframe_smooth_natural);
static_assert(static_cast<int>(core::Easing::SmoothTight) == mlt_keyframe_smooth_tight);
static_assert(static_cast<int>(core::Easing::SinusoidalIn) == mlt_keyframe_sinusoidal_in);
static_assert(static_cast<int>(core::Easing::ExponentialInOut) == mlt_keyframe_exponential_in_out);
static_assert(static_cast<int>(core::Easing::BounceInOut) == mlt_keyframe_bounce_in_out);

namespace Log = ustudio::core::Log;

namespace {
constexpr const char *kBlackResource = "color:black";
// What a missing file's clips play (doc 07, M4 B): style.css's
// semantic_danger (#ff4d6d) darkened, so it reads as "something's wrong"
// without glaring for the length of a clip. The engine can't include the
// app's generated tokens.h; keep the two in step. "#rrggbb" verified with
// MLT 7.40's color producer (standalone repro, 2026-09-25).
constexpr const char *kMissingResource = "color:#7a2232";
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

namespace {

// Track 0's black is the A frame every track composites onto, and the
// composited frame keeps its properties. producer_colour tags any YUV
// image it makes BT.601 (producer_colour.c, MLT 7.40) and composite never
// changes the tag, so the final YUV->RGB step (the preview's rgba, a
// render's conversion) decoded every BT.709 source with the 601 matrix:
// pure red 255 -> 233, cyan's red 0 -> 22, in preview and export alike.
// This filter re-tags the black's YUV frames with the profile's colour
// space (black is the same YUV in both). The alternatives measured worse:
// the black as RGBA cost a full-frame conversion (+7 ms at 1080p); an
// empty track 0 draws gaps white; a lavfi black took 600+ ms to seek.
// Standalone repro, docs/developer/notes/engine-sync.md (2026-09-27).
int retagImage(mlt_frame frame, uint8_t **image, mlt_image_format *format, int *width, int *height, int writable)
{
    mlt_properties properties = MLT_FRAME_PROPERTIES(frame);
    const int colorspace = mlt_properties_get_int(properties, "ustudio.colorspace");
    const int error = mlt_frame_get_image(frame, image, format, width, height, writable);
    if (!error && *format != mlt_image_rgb && *format != mlt_image_rgba && *format != mlt_image_rgba64)
        mlt_properties_set_int(properties, "colorspace", colorspace);
    return error;
}

mlt_frame retagProcess(mlt_filter filter, mlt_frame frame)
{
    mlt_properties_set_int(MLT_FRAME_PROPERTIES(frame), "ustudio.colorspace",
                           mlt_service_profile(MLT_FILTER_SERVICE(filter))->colorspace);
    mlt_frame_push_get_image(frame, retagImage);
    return frame;
}

void attachProfileColorspace(Mlt::Producer &producer, Mlt::Profile &profile)
{
    mlt_filter raw = mlt_filter_new();
    if (!raw)
        return;
    raw->process = retagProcess;
    mlt_service_set_profile(MLT_FILTER_SERVICE(raw), profile.get_profile());
    Mlt::Filter filter(raw); // its own reference
    mlt_filter_close(raw);
    producer.attach(filter);
}

} // namespace

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

EngineSync::EngineSync(std::shared_ptr<const core::Project> project, PreviewScale previewScale, FrameReads reads,
                       OutputSize outputSize)
    : m_extensions(createEngineExtensions()), m_project(std::move(project)), m_model(*m_project),
      m_previewScale(previewScale), m_reads(reads), m_outputSize(outputSize)
{
    applyProfile();
    rebuildAll();
}

EngineSync::EngineSync(const core::Model &model, PreviewScale previewScale, FrameReads reads, OutputSize outputSize)
    : EngineSync(model.snapshot(), previewScale, reads, outputSize)
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
            x.transitions != y.transitions || x.effects != y.effects || x.adjustmentBlocks != y.adjustmentBlocks)
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
    bool rebuild = !sameGraphInput(*m_project, *project);
    // IP3: a change that's only effect values may be applied to the live
    // filters instead (applyInPlace()). Never considered without extensions,
    // so without drop-ins this is exactly the old path.
    bool inPlace = false;
    // ADR-018: a drag changes only clip transforms; they're set on the live
    // filters, so the consumer never restarts.
    if (rebuild && applyTransformsInPlace(*project)) {
        rebuild = false;
        inPlace = true;
    }
    if (rebuild && !m_extensions.empty() && applyInPlace(*project)) {
        rebuild = false;
        inPlace = true;
    }
    const core::Profile profileBefore = m_model.sequence().profile;
    if (rebuild)
        dropChangedMasters(*m_project, *project);
    m_project = std::move(project);
    m_model = core::Model(*m_project);
    // Auto preview scale follows whether any clip is transformed.
    const bool newProfile = m_model.sequence().profile != profileBefore ||
                            previewScaleFactor(m_previewScale, m_model.sequence().profile.height,
                                               core::hasTransformedClip(*m_project)) != m_previewFactor;
    if (newProfile)
        rebuildOnNewProfile();
    else if (rebuild)
        rebuildAll();
    if (inPlace && !newProfile)
        appliedInPlace.emit();
}

void EngineSync::dropChangedMasters(const core::Project &before, const core::Project &after)
{
    // A relinked, missing or found-again asset needs its file (or the
    // placeholder) opened afresh; the cache is keyed by asset id alone.
    for (const core::Asset &now : after.bin) {
        auto was =
            std::find_if(before.bin.begin(), before.bin.end(), [&](const core::Asset &a) { return a.id == now.id; });
        if (was == before.bin.end() ||
            (was->path == now.path && was->status == now.status && was->proxyPath == now.proxyPath))
            continue;
        for (uint64_t variant = 0; variant < 4; ++variant)
            m_masterProducers.erase((now.id.value << 2) | variant);
        m_unavailableAssets.erase(now.id.value);
        m_proxiedAssets.erase(now.id.value);
    }
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
    double factor = previewScaleFactor(scale, m_model.sequence().profile.height, core::hasTransformedClip(*m_project));
    if (factor == m_previewFactor)
        return;
    Log::debug("[engine] preview scale factor " + std::to_string(m_previewFactor) + " -> " + std::to_string(factor));
    rebuildOnNewProfile();
}

void EngineSync::setUseProxies(bool use)
{
    if (use == m_useProxies)
        return;
    m_useProxies = use;
    const auto &bin = m_model.project().bin;
    if (std::none_of(bin.begin(), bin.end(), [](const core::Asset &a) { return !a.proxyPath.empty(); }))
        return; // nothing plays differently
    dropProxiedMasters();
    rebuildAll();
}

void EngineSync::dropProxiedMasters()
{
    for (const core::Asset &asset : m_model.project().bin) {
        if (asset.proxyPath.empty())
            continue;
        for (uint64_t variant = 0; variant < 4; ++variant)
            m_masterProducers.erase((asset.id.value << 2) | variant);
        m_proxiedAssets.erase(asset.id.value);
    }
}

void EngineSync::rebuildOnNewProfile()
{
    Log::debug("[engine] new profile: dropping " + std::to_string(m_masterProducers.size()) +
               " cached master producer(s)");
    m_masterProducers.clear();
    m_proxiedAssets.clear();
    // Built from the old profile, which applyProfile() below replaces.
    m_blackMaster.reset();
    m_transformBackground.reset(); // the old filters keep their references

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
    m_previewFactor = previewScaleFactor(m_previewScale, sequenceProfile.height, core::hasTransformedClip(*m_project));
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
    if (m_outputSize.width > 0 && m_outputSize.height > 0) {
        m_profile->set_width(m_outputSize.width);
        m_profile->set_height(m_outputSize.height);
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
    result.sequenceFps = sequenceProfile.fps;
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
    } else {
        // A still's size too (M4 E: an image sequence is probed by its first
        // image, and fitting a picture needs its aspect). pixbuf sets
        // meta.media.width/height when it loads the image, on the first
        // get_image; avformat's single-image path on its first frame.
        std::unique_ptr<Mlt::Frame> frame(producer.get_frame());
        mlt_image_format format = mlt_image_rgba;
        int w = 0, h = 0;
        frame->get_image(format, w, h);
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
        // A file already known to be missing (checked on load) isn't
        // opened at all: straight to the placeholder, no warning.
        const bool knownMissing = asset.status == core::Asset::Status::Missing;
        if (knownMissing)
            m_unavailableAssets.insert(assetId.value);
        // The proxy when asked for and its file is there; the cache may have
        // been cleared, which isn't missing media: the original plays.
        std::string resource = asset.path;
        m_proxiedAssets.erase(assetId.value);
        if (m_useProxies && !knownMissing && !asset.proxyPath.empty()) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(asset.proxyPath, ec)) {
                resource = asset.proxyPath;
                m_proxiedAssets.insert(assetId.value);
            } else {
                Log::debug("[engine] proxy of asset " + std::to_string(assetId.value) +
                           " is gone; playing the original");
            }
        }
        // An image sequence (M4 E): pixbuf with its first number (`begin`
        // needs the explicit "pixbuf:" prefix; the default loader refuses
        // the query), one picture per frame (ttl 1; the default is 25), and
        // the counted length (pixbuf reports 15000 and loops). Standalone
        // repro, MLT 7.40, docs/developer/notes/engine-sync.md.
        const bool sequence = asset.info.isImageSequence && resource == asset.path;
        if (sequence)
            resource = "pixbuf:" + asset.path + "?begin=" + std::to_string(asset.info.sequenceBegin);
        auto producer = std::make_shared<Mlt::Producer>(*m_profile, knownMissing ? kMissingResource : resource.c_str());
        if (sequence && !knownMissing && producer->is_valid()) {
            const int frames = static_cast<int>(std::max<core::FrameIndex>(asset.info.lengthInSequenceFrames, 1));
            producer->set("ttl", 1);
            producer->set("length", frames);
            producer->set_in_and_out(0, frames - 1);
        }
        if (!knownMissing && !producer->is_valid()) {
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
            // back to the missing-media placeholder sized to the asset's
            // own recorded length, so the clip's span keeps its correct
            // duration and every other clip's timing is unaffected --
            // only its own frames show the placeholder instead of crashing.
            // Once per asset, not once per stream-switch variant.
            if (m_unavailableAssets.insert(assetId.value).second) {
                Log::error("[engine] could not open asset " + std::to_string(assetId.value) + " (" + asset.path +
                           ") -- showing the missing-media placeholder in its place");
                mediaUnavailable.emit(asset.path);
            }
            producer = std::make_shared<Mlt::Producer>(*m_profile, kMissingResource);
        }
        if (knownMissing || m_unavailableAssets.contains(assetId.value)) {
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
        // A colour's YUV is BT.601 values tagged 601, and MLT composites YUV
        // without converting between colour spaces: as RGBA, the conversion
        // to YUV uses the profile's matrix (the re-tag note above).
        if (const char *service = producer->get("mlt_service");
            service && (std::strcmp(service, "color") == 0 || std::strcmp(service, "colour") == 0))
            producer->set("mlt_image_format", "rgba");
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

namespace {
template <class Visit> void forEachEffect(core::Project &project, const Visit &visit)
{
    for (core::Sequence &seq : project.sequences) {
        for (auto &[id, clip] : seq.clips)
            for (core::Effect &effect : clip.effects)
                visit(effect);
        for (core::Track &track : seq.tracks)
            for (core::Effect &effect : track.effects)
                visit(effect);
        for (core::Effect &effect : seq.effects)
            visit(effect);
        for (core::AdjustmentBlock &block : seq.adjustmentBlocks)
            for (core::Effect &effect : block.effects)
                visit(effect);
    }
}
} // namespace

bool EngineSync::applyInPlace(const core::Project &next)
{
    // Only effect values may differ: the same graph input with every
    // parameter value and mix blanked out on both sides.
    core::Project before = *m_project;
    core::Project after = next;
    std::unordered_map<uint64_t, core::Effect> oldEffects;
    forEachEffect(before, [&](core::Effect &effect) { oldEffects.emplace(effect.id.value, effect); });
    std::vector<core::Effect> changed;
    forEachEffect(after, [&](core::Effect &effect) {
        auto it = oldEffects.find(effect.id.value);
        if (it != oldEffects.end() && (it->second.params != effect.params || it->second.mix != effect.mix))
            changed.push_back(effect);
    });
    auto blank = [](core::Effect &effect) {
        for (core::Param &param : effect.params) {
            param.value = 0.0;
            param.keyframes.clear();
        }
        effect.mix = {};
    };
    forEachEffect(before, blank);
    forEachEffect(after, blank);
    if (!sameGraphInput(before, after) || changed.empty())
        return false;

    for (const core::Effect &effect : changed) {
        const core::Effect &old = oldEffects.at(effect.id.value);
        ParamChange change{effect, {}};
        for (size_t i = 0; i < effect.params.size() && i < old.params.size(); ++i)
            if (effect.params[i] != old.params[i])
                change.params.push_back(effect.params[i].name);
        if (effect.mix != old.mix)
            change.params.push_back("mix");
        bool applied = false;
        for (const std::unique_ptr<EngineExtension> &extension : m_extensions)
            applied = extension->applyInPlace(change) || applied;
        if (!applied)
            return false; // a rebuild follows, which supersedes anything applied so far
    }
    Log::debug("[engine] " + std::to_string(changed.size()) + " effect value change(s) applied in place");
    return true;
}

Mlt::Producer &EngineSync::producerForClip(const core::Clip &clip)
{
    // IP3: a drop-in may make a clip's producer (titles); one per clip per
    // build, shared by its cuts.
    if (!m_extensions.empty()) {
        if (auto it = m_clipProducers.find(clip.id.value); it != m_clipProducers.end())
            return *it->second;
        for (const std::unique_ptr<EngineExtension> &extension : m_extensions) {
            if (std::unique_ptr<Mlt::Producer> made = extension->makeProducer(m_model, clip, *m_profile)) {
                m_extensionProducers.insert(clip.id.value);
                return *m_clipProducers.emplace(clip.id.value, std::shared_ptr<Mlt::Producer>(std::move(made)))
                            .first->second;
            }
        }
    }
    return masterProducerFor(clip.asset, clip.videoEnabled, clip.audioEnabled);
}

double EngineSync::outputScale() const
{
    const int projectWidth = m_model.sequence().profile.width;
    return projectWidth > 0 ? static_cast<double>(m_profile->width()) / projectWidth : 1.0;
}

double EngineSync::sourceScale(const core::Clip &clip)
{
    // Crop is in source pixels; a proxy's frames are smaller.
    if (!m_proxiedAssets.contains(clip.asset.value) || !m_model.hasAsset(clip.asset))
        return 1.0;
    const int sourceWidth = m_model.asset(clip.asset).info.width;
    const int playing = masterProducerFor(clip.asset, clip.videoEnabled, clip.audioEnabled).get_int("meta.media.width");
    return sourceWidth > 0 && playing > 0 ? static_cast<double>(playing) / sourceWidth : 1.0;
}

bool EngineSync::compositorFits(const core::Clip &clip, bool inDissolve) const
{
    return m_reads == FrameReads::ProfileSize && !inDissolve && core::compositorFits(clip.transform.get());
}

void EngineSync::applyTransform(Mlt::Producer &cut, const core::Clip &clip, bool inDissolve)
{
    if (!m_model.hasAsset(clip.asset))
        return;
    const core::MediaInfo &info = m_model.asset(clip.asset).info;
    // Empty when the track compositor places the picture by itself.
    const std::vector<core::NativeFilter> natives =
        compositorFits(clip, inDissolve)
            ? std::vector<core::NativeFilter>{}
            : core::transformFilters(clip.transform.get(), info.width, info.height, m_model.sequence().profile,
                                     outputScale(), sourceScale(clip));
    // Every cut is recorded, filtered or not: a clip's own cuts and its
    // dissolve cuts can differ (compositorFits()), and applyTransformsInPlace()
    // may only update a clip whose cuts all share one shape.
    std::string shape;
    for (const core::NativeFilter &native : natives)
        shape += native.service + ";";
    TransformFilters &kept = m_transformFilters[clip.id.value];
    kept.shape = kept.cuts.empty() || kept.shape == shape ? shape : std::string(kMixedShape);
    std::vector<std::shared_ptr<Mlt::Filter>> filters;
    for (const core::NativeFilter &native : natives) {
        auto filter = std::make_shared<Mlt::Filter>(*m_profile, native.service.c_str());
        for (const auto &[name, value] : native.properties)
            filter->set(name.c_str(), value.c_str());
        if (native.service == "affine") {
            // The filter draws onto a background it makes itself ("colour:0"),
            // and producer_colour caches its last image in its properties:
            // one frame-sized RGBA image per filter, kept for the filter's
            // life (8 MB at 1080p; 2.4 GB for 300 played transformed cuts).
            // Hand every filter the same one, a reference each (unset
            // _background keeps the filter from replacing it). Safe across
            // render threads: the filter holds its own lock for its whole
            // get_image and producer_colour locks itself around the cache
            // (filter_affine.c, producer_colour.c, MLT 7.40; TSan-checked by
            // tests/engine/test_transform). Standalone repro 2026-09-25.
            if (!m_transformBackground)
                m_transformBackground = std::make_unique<Mlt::Producer>(*m_profile, "colour:0");
            m_transformBackground->inc_ref();
            filter->set(
                "producer", m_transformBackground->get_producer(), 0,
                +[](void *producer) { mlt_producer_close(static_cast<mlt_producer>(producer)); });
        }
        attachToCut(cut, *filter); // animated from the cut's own start (engine_extension.h)
        filters.push_back(std::move(filter));
    }
    kept.cuts.push_back(std::move(filters));
}

bool EngineSync::applyTransformsInPlace(const core::Project &next)
{
    // Only transforms may differ: the same graph input with them blanked.
    auto blank = [](core::Project project) {
        for (core::Sequence &seq : project.sequences)
            for (auto &[id, clip] : seq.clips)
                clip.transform.set(core::Transform{});
        return project;
    };
    if (!sameGraphInput(blank(*m_project), blank(next)))
        return false;
    const core::Sequence *seq = nullptr;
    for (const core::Sequence &candidate : next.sequences)
        if (candidate.id == next.activeSequence)
            seq = &candidate;
    if (!seq)
        return false;
    const core::Profile &profile = seq->profile;
    bool changed = false;
    for (const auto &[id, clip] : seq->clips) {
        const core::Clip &before = m_model.clip(id);
        if (before.transform == clip.transform)
            continue;
        const core::MediaInfo &info = m_model.asset(clip.asset).info;
        // As applyTransform() builds a clip's own cuts; a clip with dissolve
        // cuts of another shape is kMixedShape and rebuilds.
        const std::vector<core::NativeFilter> natives =
            compositorFits(clip, false) ? std::vector<core::NativeFilter>{}
                                        : core::transformFilters(clip.transform.get(), info.width, info.height, profile,
                                                                 outputScale(), sourceScale(clip));
        std::string shape;
        for (const core::NativeFilter &native : natives)
            shape += native.service + ";";
        auto kept = m_transformFilters.find(id.value);
        const std::string keptShape = kept == m_transformFilters.end() ? std::string() : kept->second.shape;
        if (shape != keptShape)
            return false; // filters to add or remove (identity <-> placed, a flip): rebuild
        changed = true;
        if (kept == m_transformFilters.end())
            continue; // no cut in the graph, nothing to update
        for (const auto &filters : kept->second.cuts)
            for (size_t i = 0; i < natives.size() && i < filters.size(); ++i)
                for (const auto &[name, value] : natives[i].properties)
                    filters[i]->set(name.c_str(), value.c_str());
    }
    if (changed)
        Log::debug("[engine] clip transforms applied in place");
    return changed;
}

void EngineSync::decorateCut(Mlt::Producer &cut, const core::Clip &clip, core::FrameIndex in, core::FrameIndex out)
{
    for (const std::unique_ptr<EngineExtension> &extension : m_extensions)
        extension->decorateCut(cut, CutContext{m_model, clip, in - clip.in, out - in + 1, *m_profile});
}

std::unique_ptr<Mlt::Tractor> EngineSync::buildTransitionSubTractor(const TrackSegment &segment)
{
    const core::Transition &t = m_model.transition(segment.transition);
    const core::Clip &clipA = m_model.clip(segment.a);
    const core::Clip &clipB = m_model.clip(segment.b);

    Mlt::Producer &masterA = producerForClip(clipA);
    std::unique_ptr<Mlt::Producer> tailA(
        masterA.cut(static_cast<int>(clipA.out - t.length + 1), static_cast<int>(clipA.out)));
    decorateCut(*tailA, clipA, clipA.out - t.length + 1, clipA.out);
    applyTransform(*tailA, clipA, true);

    Mlt::Producer &masterB = producerForClip(clipB);
    std::unique_ptr<Mlt::Producer> headB(
        masterB.cut(static_cast<int>(clipB.in), static_cast<int>(clipB.in + t.length - 1)));
    decorateCut(*headB, clipB, clipB.in, clipB.in + t.length - 1);
    applyTransform(*headB, clipB, true);

    // IP3: a drop-in's recipe (wipes, motion; FX3) builds the whole segment.
    for (const std::unique_ptr<EngineExtension> &extension : m_extensions)
        if (std::unique_ptr<Mlt::Tractor> made =
                extension->makeTransitionSegment(m_model, t, *tailA, *headB, *m_profile))
            return made;

    auto sub = std::make_unique<Mlt::Tractor>(*m_profile);

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
            Mlt::Producer &master = producerForClip(clip);
            std::unique_ptr<Mlt::Producer> cut(master.cut(static_cast<int>(seg.in), static_cast<int>(seg.out)));
            decorateCut(*cut, clip, seg.in, seg.out);
            applyTransform(*cut, clip);
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
    m_clipProducers.clear();
    m_extensionProducers.clear();
    m_transformFilters.clear();
    for (const std::unique_ptr<EngineExtension> &extension : m_extensions)
        extension->beginBuild();
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
        attachProfileColorspace(*m_blackMaster, *m_profile);
    }
    core::FrameIndex sequenceLength = std::max<core::FrameIndex>(seq.length(), 1);
    // Mlt::Producer::cut() returns a new, caller-owned wrapper (the "mlt++
    // accessors" note, docs/developer/notes/engine-sync.md); set_track() takes its own reference.
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

        for (const std::unique_ptr<EngineExtension> &extension : m_extensions)
            extension->decoratePlaylist(*playlist, m_model, modelTrack, *m_profile);

        newTractor->set_track(*playlist, static_cast<int>(order.size()));
        order.emplace_back(trackId);
        playlists.push_back(std::move(playlist));
    }

    // Video: every track composited onto track 0 (the background), bottom
    // to top. Audio: mix chained across adjacent indexes, as v1's
    // MltEngine::plantTrackTransitions did. Chaining the video compositor
    // too (i-1 -> i) lost an upper clip's alpha: a transformed picture
    // showed black instead of the tracks under it (the F spike, ADR-018,
    // standalone repro 2026-09-25); onto track 0 (kdenlive's arrangement)
    // it shows them.
    // One owned Field wrapper for the whole tractor -- see
    // buildTransitionSubTractor()'s comment (sanitizer report S1).
    std::unique_ptr<Mlt::Field> field(newTractor->field());
    for (int index = 1; index < newTractor->count(); ++index) {
        // IP3: a drop-in may replace the compositor (effects: frei0r.cairoblend).
        std::unique_ptr<Mlt::Transition> compositor;
        for (const std::unique_ptr<EngineExtension> &extension : m_extensions)
            if (!compositor)
                compositor = extension->compositor(*m_profile, 0, index);
        if (compositor) {
            field->plant_transition(*compositor, 0, index);
        } else {
            // composite, with fill=1 (its YAML says that's the default; the
            // code's is 0): it scales a same-aspect picture to the frame, and
            // a transformed or other-aspect cut arrives frame-sized from its
            // affine filter (ADR-018). An affine compositor fits any aspect
            // itself but cost 21 ms a frame for one untransformed 1080p track
            // against composite's 5 (39 against 7 for four). Standalone
            // repros, MLT 7.40, 2026-09-25; docs/developer/notes/engine-sync.md.
            // Centred (the YAML's values; defaults left/top) when frames are
            // read at the profile's size (FrameReads), so a plain Fit of
            // another aspect needs no affine filter: 20 ms a frame against
            // 63 for a 1344x768 source in 1080p, the edges within a pixel
            // (standalone repro, 2026-09-27).
            Mlt::Transition composite(*m_profile, "composite");
            composite.set("fill", 1);
            if (m_reads == FrameReads::ProfileSize) {
                composite.set("halign", "centre");
                composite.set("valign", "middle");
            }
            field->plant_transition(composite, 0, index);
        }

        Mlt::Transition mix(*m_profile, "mix");
        mix.set("start", 1.0);
        mix.set("sum", 1);
        mix.set("always_active", 1);
        field->plant_transition(mix, index - 1, index);
    }

    for (const std::unique_ptr<EngineExtension> &extension : m_extensions)
        extension->decorateTractor(*newTractor, m_model, *m_profile);

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
                if (m_model.hasAsset(clip.asset) && !m_unavailableAssets.contains(clip.asset.value) &&
                    !m_proxiedAssets.contains(clip.asset.value) && !m_extensionProducers.contains(clip.id.value)) {
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
                    // An image sequence opens as "<pattern>?begin=N" (masterProducerFor()).
                    if (m_model.asset(clip.asset).info.isImageSequence &&
                        actualResource.starts_with(expectedResource + "?begin="))
                        actualResource = expectedResource;
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

namespace {
// -1 unknown, 0 no, 1 yes: h264HasQualityMode()'s answer, set once
// h264Encoder() has one.
std::atomic<int> s_h264QualityMode{-1};
} // namespace

const std::string &h264Encoder()
{
    static const std::string encoder = [] {
        Log::ScopedTimer timer("[engine] H.264 encoder query");
        // avformat's documented "list" value (consumer_avformat.yml):
        // start() fills the consumer's "vcodec" data with every encoder
        // name, and also printf()s them all to stdout, which is silenced
        // for the call (platform::ScopedStdoutSilence).
        Mlt::Profile profile;
        Mlt::Consumer consumer(profile, "avformat");
        consumer.set("vcodec", "list");
        {
            platform::ScopedStdoutSilence silence;
            consumer.start();
            consumer.stop();
        }
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
        s_h264QualityMode = found == "libx264" ? 1 : 0;
        return found;
    }();
    return encoder;
}

std::optional<bool> h264HasQualityMode()
{
    const int known = s_h264QualityMode.load();
    return known < 0 ? std::nullopt : std::optional<bool>(known == 1);
}

bool renderProject(core::Model &model, const std::string &outputPath, std::string &error,
                   std::function<void(int, int)> onProgress, const std::atomic<bool> *cancel,
                   const core::RenderProfile &profile, int threadBudget,
                   const std::vector<std::pair<std::string, std::string>> &extra)
{
    Log::ScopedTimer timer("[engine] renderProject total");
    // Another output rate: render a retimed copy, built on a profile at that
    // rate (MLT maps each source by time; doc 12, "Frame rate": verified
    // 30 <-> 60 by frame count, duration, picture and beep sync).
    std::optional<core::Model> retimed;
    if (profile.frameRate.num > 0) {
        core::Project project = core::retime(model.project(), profile.frameRate);
        if (project != model.project()) {
            retimed.emplace(std::move(project));
            Log::info("[engine] Rendering at " + std::to_string(profile.frameRate.num) + "/" +
                      std::to_string(profile.frameRate.den) + " fps (the project is " +
                      std::to_string(model.sequence().profile.fps.num) + "/" +
                      std::to_string(model.sequence().profile.fps.den) + ")");
        }
    }
    core::Model &renderModel = retimed ? *retimed : model;
    // Its own Profile/Tractor, independent of any live one. An export at
    // another size is built at that size, so frames are read at the
    // profile's (FrameReads): read smaller, composite's alignment and the
    // affine filter's rect, both in profile pixels, came out wrong (a Fit
    // or placed picture ran to the right edge of a 720p export of a 1080p
    // project; tests/engine/test_transform, 2026-09-27). A non-square-pixel
    // sequence keeps the consumer's scaling: its export changes the pixel
    // shape, which one outputScale() can't express.
    const core::EncoderSettings settings =
        core::encoderSettings(profile, renderModel.sequence().profile, h264Encoder() == "libx264");
    const core::Profile &sequenceProfile = renderModel.sequence().profile;
    const bool resized = settings.width > 0 && settings.height > 0 &&
                         (settings.width != sequenceProfile.width || settings.height != sequenceProfile.height);
    const bool squarePixels = sequenceProfile.sar.num == sequenceProfile.sar.den;
    EngineSync renderSync(renderModel, PreviewScale::Full,
                          !resized || squarePixels ? EngineSync::FrameReads::ProfileSize
                                                   : EngineSync::FrameReads::AnySize,
                          resized && squarePixels ? EngineSync::OutputSize{settings.width, settings.height}
                                                  : EngineSync::OutputSize{0, 0});

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
    // Negative real_time: render every frame, never drop to keep up with a
    // clock, on that many threads (consumer_avformat.yml). "threads" is the
    // encoder's (consumer_avformat.c: 0 = the codec's automatic count). Both
    // measured on a generated 1080p project (doc 19, MT5): -8 gave output
    // bit-identical to -1 (PSNR inf on all 600 frames, same frame count and
    // audio), 20% faster on a composited graph; real_time > 1 being broken
    // is sdl2 playback's problem (doc 05), not the avformat consumer's.
    if (threadBudget > 0) {
        const core::RenderThreads threads = core::splitRenderThreads(threadBudget);
        consumer.set("real_time", -threads.frames);
        consumer.set("threads", threads.encoder);
        Log::info("[engine] Render threads: " + std::to_string(threads.frames) + " frame, " +
                  std::to_string(threads.encoder) + " encoder (budget " + std::to_string(threadBudget) + ")");
    } else {
        consumer.set("real_time", -1);
    }
    for (const auto &[name, value] : extra)
        consumer.set(name.c_str(), value.c_str());
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
