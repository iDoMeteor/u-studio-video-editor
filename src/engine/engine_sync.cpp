#include "engine_sync.h"

#include "core/log.h"
#include "core/model/mlt_order.h"

#include <algorithm>

namespace ustudio::engine {

namespace Log = ustudio::core::Log;

namespace {
constexpr const char *kBlackResource = "color:black";
}

EngineSync::EngineSync(core::Model &model) : m_model(model)
{
    applyProfile();
    rebuildAll();
}

void EngineSync::applyProfile()
{
    const core::Profile &p = m_model.sequence().profile;
    m_profile = std::make_unique<Mlt::Profile>();
    m_profile->set_width(p.width);
    m_profile->set_height(p.height);
    m_profile->set_frame_rate(p.fps.num, p.fps.den);
    m_profile->set_sample_aspect(p.sar.num, p.sar.den);
    m_profile->set_display_aspect(p.dar.num, p.dar.den);
    m_profile->set_progressive(p.progressive ? 1 : 0);
    m_profile->set_colorspace(p.colorspace);
}

Mlt::Producer &EngineSync::masterProducerFor(core::AssetId assetId)
{
    auto it = m_masterProducers.find(assetId.value);
    if (it != m_masterProducers.end())
        return *it->second;

    const core::Asset &asset = m_model.asset(assetId);
    auto producer = std::make_shared<Mlt::Producer>(*m_profile, asset.path.c_str());
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
        playlist.append(*cut);

        cursor = clip.end();
    }
}

void EngineSync::rebuildAll()
{
    const core::Sequence &seq = m_model.sequence();

    auto newTractor = std::make_unique<Mlt::Tractor>(*m_profile);
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

} // namespace ustudio::engine
