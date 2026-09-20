#include "engine_sync.h"

#include "core/log.h"
#include "core/model/mlt_order.h"

#include <algorithm>
#include <cmath>
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

void EngineSync::reset()
{
    m_masterProducers.clear();
    applyProfile();
    rebuildAll();
    connectToModel();
}

void EngineSync::connectToModel()
{
    m_model.changed.connect([this](const core::ModelEvent &event) { onModelEvent(event); });
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
    result.hasAudio = !result.isStillImage;
    return result;
}

Mlt::Producer &EngineSync::masterProducerFor(core::AssetId assetId)
{
    auto it = m_masterProducers.find(assetId.value);
    if (it != m_masterProducers.end())
        return *it->second;

    const core::Asset &asset = m_model.asset(assetId);
    auto producer = std::make_shared<Mlt::Producer>(*m_profile, asset.path.c_str());
    // Still images default to a fixed length in MLT (pixbuf: 15000 frames,
    // verified empirically) regardless of how long a clip the model wants
    // to cut from them -- a still is boundless (MediaInfo::isBoundless()),
    // so the model may legitimately ask for an out point past that. Bump
    // the producer's own "length" property to match before any cut is
    // taken, so master.cut(in, out) never silently clips to 15000 frames
    // for a watermark spanning a longer timeline. Harmless for a real
    // (non-still) asset: its own natural length is left alone.
    if (asset.info.isStillImage && asset.info.lengthInSequenceFrames > producer->get_length()) {
        producer->set("length", static_cast<int>(asset.info.lengthInSequenceFrames));
        producer->set_in_and_out(0, static_cast<int>(asset.info.lengthInSequenceFrames - 1));
    }
    Mlt::Producer &ref = *producer;
    m_masterProducers.emplace(assetId.value, std::move(producer));
    return ref;
}

void EngineSync::rebuildTrackPlaylist(const core::Track &modelTrack, Mlt::Playlist &playlist)
{
    playlist.clear();
    core::FrameIndex cursor = 0;

    for (core::ClipId clipId : modelTrack.clips) {
        const core::Clip &clip = m_model.clip(clipId);
        if (clip.position > cursor) {
            // Playlist::blank(out) appends out+1 frames.
            playlist.blank(static_cast<int>(clip.position - cursor - 1));
        }

        Mlt::Producer &master = masterProducerFor(clip.asset);
        std::unique_ptr<Mlt::Producer> cut(master.cut(static_cast<int>(clip.in), static_cast<int>(clip.out)));
        // video_index/audio_index=-1 ("off"), verified against avformat's
        // own YAML metadata. Set on the cut, not the master: cuts carry
        // their own property overrides in MLT (the mechanism that already
        // lets different cuts of the same file have different speed/
        // effects), so this only silences this one clip, not every other
        // cut of the same asset elsewhere on the timeline. This is what
        // makes SplitAudio's two resulting clips (a video-only original, a
        // new audio-only one) actually play as split, not just look split
        // in the model.
        if (!clip.videoEnabled)
            cut->set("video_index", -1);
        if (!clip.audioEnabled)
            cut->set("audio_index", -1);
        playlist.append(*cut);

        cursor = clip.end();
    }
}

void EngineSync::rebuildAll()
{
    const core::Sequence &seq = m_model.sequence();

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

        int nonBlankCount = 0;
        for (int i = 0; i < playlist.count(); ++i) {
            if (!playlist.is_blank(i))
                ++nonBlankCount;
        }
        if (nonBlankCount != static_cast<int>(modelTrack.clips.size())) {
            problems.push_back("track " + std::to_string(trackId.value) + ": playlist has " +
                               std::to_string(nonBlankCount) + " non-blank entries, model has " +
                               std::to_string(modelTrack.clips.size()) + " clips");
            continue;
        }

        int entryIndex = 0;
        for (int i = 0; i < playlist.count() && entryIndex < static_cast<int>(modelTrack.clips.size()); ++i) {
            if (playlist.is_blank(i))
                continue;

            const core::Clip &clip = m_model.clip(modelTrack.clips[static_cast<size_t>(entryIndex)]);
            std::unique_ptr<Mlt::ClipInfo> info(playlist.clip_info(i));
            if (!info) {
                problems.push_back("track " + std::to_string(trackId.value) + ": clip_info(" + std::to_string(i) +
                                   ") failed");
                ++entryIndex;
                continue;
            }

            if (info->start != clip.position) {
                problems.push_back("clip " + std::to_string(clip.id.value) + ": playlist start " +
                                   std::to_string(info->start) + " != model position " + std::to_string(clip.position));
            }
            if (info->frame_in != clip.in || info->frame_out != clip.out) {
                problems.push_back("clip " + std::to_string(clip.id.value) + ": playlist in/out " +
                                   std::to_string(info->frame_in) + "/" + std::to_string(info->frame_out) +
                                   " != model " + std::to_string(clip.in) + "/" + std::to_string(clip.out));
            }
            if (m_model.hasAsset(clip.asset)) {
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
                    problems.push_back("clip " + std::to_string(clip.id.value) + ": playlist resource '" +
                                       actualResource + "' != asset path '" + expectedResource + "'");
                }
            }

            ++entryIndex;
        }
    }

    return problems;
}

bool renderProject(core::Model &model, const std::string &outputPath, std::string &error)
{
    EngineSync renderSync(model); // its own Profile/Tractor, independent of any live one

    Mlt::Consumer consumer(renderSync.profile(), "avformat", outputPath.c_str());
    if (!consumer.is_valid()) {
        error = "Could not create renderer for: " + outputPath;
        Log::error("[engine] " + error);
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

    Log::info("[engine] Rendering project to " + outputPath + " ...");
    int result = consumer.run();
    if (result != 0) {
        error = "Render failed (consumer returned " + std::to_string(result) + ")";
        Log::error("[engine] " + error);
        return false;
    }

    Log::info("[engine] Rendered project to " + outputPath);
    return true;
}

} // namespace ustudio::engine
