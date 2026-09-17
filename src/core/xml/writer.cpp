#include "writer.h"

#include "core/model/mlt_order.h"

#include <libxml/tree.h>

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <sstream>
#include <system_error>
#include <unordered_map>

// libxml2's BAD_CAST is a C-style cast (used throughout this file to
// satisfy its const xmlChar* API from our const char*/std::string data).
// Unlike the GTK macro-noise case (docs/plans/v2/11), a pragma push/pop
// actually works here: it wraps our own function bodies directly, not an
// #include block whose macros get invoked from call sites elsewhere.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"

namespace ustudio::core {

namespace {

namespace fs = std::filesystem;

constexpr int kFormatVersion = 2; // doc 09

xmlNodePtr addProperty(xmlNodePtr parent, const std::string &name, const std::string &value)
{
    xmlNodePtr prop = xmlNewTextChild(parent, nullptr, BAD_CAST "property", BAD_CAST value.c_str());
    xmlNewProp(prop, BAD_CAST "name", BAD_CAST name.c_str());
    return prop;
}

std::string jsonEscape(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
        }
    }
    return out;
}

std::string markersToJson(const std::vector<Marker> &markers)
{
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < markers.size(); ++i) {
        const Marker &m = markers[i];
        if (i > 0)
            out << ",";
        out << "{\"id\":" << m.id.value << ",\"at\":" << m.at << ",\"text\":\"" << jsonEscape(m.text)
            << "\",\"color\":" << static_cast<int>(m.color) << "}";
    }
    out << "]";
    return out.str();
}

std::string settingsToJson(const std::map<std::string, std::string> &settings)
{
    std::ostringstream out;
    out << "{";
    bool first = true;
    for (const auto &[key, value] : settings) {
        if (!first)
            out << ",";
        first = false;
        out << "\"" << jsonEscape(key) << "\":\"" << jsonEscape(value) << "\"";
    }
    out << "}";
    return out.str();
}

// Relative to `projectDir` when the asset lives under it (doc 09), else
// left absolute.
std::string relativizePath(const std::string &assetPath, const fs::path &projectDir)
{
    if (assetPath.empty() || assetPath.front() != '/')
        return assetPath; // not an absolute filesystem path (e.g. a color:/noise:/tone: generator) -- leave as-is

    std::error_code ec;
    fs::path relative = fs::relative(assetPath, projectDir, ec);
    if (ec || relative.empty() || relative.native().starts_with(".."))
        return assetPath;
    return relative.string();
}

void writeAssetProducer(xmlNodePtr mlt, const Asset &asset, const fs::path &projectDir, const std::string &nodeId)
{
    xmlNodePtr producer = xmlNewChild(mlt, nullptr, BAD_CAST "producer", nullptr);
    xmlNewProp(producer, BAD_CAST "id", BAD_CAST nodeId.c_str());
    xmlNewProp(producer, BAD_CAST "in", BAD_CAST "0");
    FrameIndex out = asset.info.lengthInSequenceFrames > 0 ? asset.info.lengthInSequenceFrames - 1 : 0;
    xmlNewProp(producer, BAD_CAST "out", BAD_CAST std::to_string(out).c_str());

    addProperty(producer, "resource", relativizePath(asset.path, projectDir));

    addProperty(producer, "ustudio:asset_id", std::to_string(asset.id.value));
    addProperty(producer, "ustudio:display_name", asset.displayName);
    addProperty(producer, "ustudio:folder", asset.folder);
    addProperty(producer, "ustudio:fingerprint", asset.fileFingerprint);
    addProperty(producer, "ustudio:proxy", asset.proxyPath);
    addProperty(producer, "ustudio:status", std::to_string(static_cast<int>(asset.status)));

    const MediaInfo &info = asset.info;
    addProperty(producer, "ustudio:has_video", info.hasVideo ? "1" : "0");
    addProperty(producer, "ustudio:has_audio", info.hasAudio ? "1" : "0");
    addProperty(producer, "ustudio:width", std::to_string(info.width));
    addProperty(producer, "ustudio:height", std::to_string(info.height));
    addProperty(producer, "ustudio:fps_num", std::to_string(info.fps.num));
    addProperty(producer, "ustudio:fps_den", std::to_string(info.fps.den));
    addProperty(producer, "ustudio:sar_num", std::to_string(info.sar.num));
    addProperty(producer, "ustudio:sar_den", std::to_string(info.sar.den));
    addProperty(producer, "ustudio:audio_channels", std::to_string(info.audioChannels));
    addProperty(producer, "ustudio:sample_rate", std::to_string(info.sampleRate));
    addProperty(producer, "ustudio:native_duration", std::to_string(info.nativeDurationSeconds));
    addProperty(producer, "ustudio:video_codec", info.videoCodec);
    addProperty(producer, "ustudio:audio_codec", info.audioCodec);
    addProperty(producer, "ustudio:container", info.container);
    addProperty(producer, "ustudio:is_image_sequence", info.isImageSequence ? "1" : "0");
    addProperty(producer, "ustudio:is_still_image", info.isStillImage ? "1" : "0");
}

void writeClipEntry(xmlNodePtr playlist, const Clip &clip, const std::string &producerId)
{
    xmlNodePtr entry = xmlNewChild(playlist, nullptr, BAD_CAST "entry", nullptr);
    xmlNewProp(entry, BAD_CAST "producer", BAD_CAST producerId.c_str());
    xmlNewProp(entry, BAD_CAST "in", BAD_CAST std::to_string(clip.in).c_str());
    xmlNewProp(entry, BAD_CAST "out", BAD_CAST std::to_string(clip.out).c_str());

    addProperty(entry, "ustudio:clip_id", std::to_string(clip.id.value));
    addProperty(entry, "ustudio:name", clip.name);
    addProperty(entry, "ustudio:speed", std::to_string(clip.speed));
    addProperty(entry, "ustudio:video_enabled", clip.videoEnabled ? "1" : "0");
    addProperty(entry, "ustudio:audio_enabled", clip.audioEnabled ? "1" : "0");
    if (clip.fadeIn)
        addProperty(entry, "ustudio:fade_in", std::to_string(clip.fadeIn->length));
    if (clip.fadeOut)
        addProperty(entry, "ustudio:fade_out", std::to_string(clip.fadeOut->length));
}

void writeTrackPlaylist(xmlNodePtr mlt, const Model &model, const Track &track, const std::string &playlistId,
                        size_t visualIndex, const std::unordered_map<uint64_t, std::string> &producerIdByAsset)
{
    xmlNodePtr playlist = xmlNewChild(mlt, nullptr, BAD_CAST "playlist", nullptr);
    xmlNewProp(playlist, BAD_CAST "id", BAD_CAST playlistId.c_str());

    addProperty(playlist, "ustudio:track_id", std::to_string(track.id.value));
    // The model's Sequence::tracks order (visual, top-to-bottom) is not
    // recoverable from the MLT tractor's own bottom-to-top / audio-first
    // <track> ordering alone, and Model::operator== is order-sensitive on
    // it -- this is what the reader sorts by to reconstruct it exactly.
    addProperty(playlist, "ustudio:visual_index", std::to_string(visualIndex));
    addProperty(playlist, "ustudio:kind", track.kind == Track::Kind::Audio ? "audio" : "video");
    addProperty(playlist, "ustudio:name", track.name);
    addProperty(playlist, "ustudio:muted", track.muted ? "1" : "0");
    addProperty(playlist, "ustudio:hidden", track.hidden ? "1" : "0");
    addProperty(playlist, "ustudio:locked", track.locked ? "1" : "0");
    addProperty(playlist, "ustudio:volume", std::to_string(track.volume));

    FrameIndex cursor = 0;
    for (ClipId clipId : track.clips) {
        const Clip &clip = model.clip(clipId);
        if (clip.position > cursor) {
            xmlNodePtr blank = xmlNewChild(playlist, nullptr, BAD_CAST "blank", nullptr);
            xmlNewProp(blank, BAD_CAST "length", BAD_CAST std::to_string(clip.position - cursor).c_str());
        }
        writeClipEntry(playlist, clip, producerIdByAsset.at(clip.asset.value));
        cursor = clip.end();
    }
}

} // namespace

std::string saveProject(const Model &model, const std::string &path)
{
    const Project &project = model.project();
    const Sequence &seq = model.sequence();
    fs::path targetPath(path);
    fs::path projectDir = targetPath.parent_path();

    xmlDocPtr doc = xmlNewDoc(BAD_CAST "1.0");
    xmlNodePtr mlt = xmlNewNode(nullptr, BAD_CAST "mlt");
    xmlDocSetRootElement(doc, mlt);
    xmlNewProp(mlt, BAD_CAST "LC_NUMERIC", BAD_CAST "C");
    xmlNewProp(mlt, BAD_CAST "producer", BAD_CAST "main_bin");
    xmlNewProp(mlt, BAD_CAST "root", BAD_CAST projectDir.string().c_str());

    xmlNodePtr profileNode = xmlNewChild(mlt, nullptr, BAD_CAST "profile", nullptr);
    xmlNewProp(profileNode, BAD_CAST "description", BAD_CAST "ustudio");
    xmlNewProp(profileNode, BAD_CAST "width", BAD_CAST std::to_string(seq.profile.width).c_str());
    xmlNewProp(profileNode, BAD_CAST "height", BAD_CAST std::to_string(seq.profile.height).c_str());
    xmlNewProp(profileNode, BAD_CAST "frame_rate_num", BAD_CAST std::to_string(seq.profile.fps.num).c_str());
    xmlNewProp(profileNode, BAD_CAST "frame_rate_den", BAD_CAST std::to_string(seq.profile.fps.den).c_str());
    xmlNewProp(profileNode, BAD_CAST "sample_aspect_num", BAD_CAST std::to_string(seq.profile.sar.num).c_str());
    xmlNewProp(profileNode, BAD_CAST "sample_aspect_den", BAD_CAST std::to_string(seq.profile.sar.den).c_str());
    xmlNewProp(profileNode, BAD_CAST "display_aspect_num", BAD_CAST std::to_string(seq.profile.dar.num).c_str());
    xmlNewProp(profileNode, BAD_CAST "display_aspect_den", BAD_CAST std::to_string(seq.profile.dar.den).c_str());
    xmlNewProp(profileNode, BAD_CAST "progressive", BAD_CAST(seq.profile.progressive ? "1" : "0"));
    xmlNewProp(profileNode, BAD_CAST "colorspace", BAD_CAST std::to_string(seq.profile.colorspace).c_str());

    std::unordered_map<uint64_t, std::string> producerIdByAsset;
    xmlNodePtr mainBin = xmlNewChild(mlt, nullptr, BAD_CAST "playlist", nullptr);
    xmlNewProp(mainBin, BAD_CAST "id", BAD_CAST "main_bin");

    for (const Asset &asset : project.bin) {
        std::string nodeId = "asset" + std::to_string(asset.id.value);
        producerIdByAsset.emplace(asset.id.value, nodeId);
        writeAssetProducer(mlt, asset, projectDir, nodeId);

        FrameIndex out = asset.info.lengthInSequenceFrames > 0 ? asset.info.lengthInSequenceFrames - 1 : 0;
        xmlNodePtr entry = xmlNewChild(mainBin, nullptr, BAD_CAST "entry", nullptr);
        xmlNewProp(entry, BAD_CAST "producer", BAD_CAST nodeId.c_str());
        xmlNewProp(entry, BAD_CAST "in", BAD_CAST "0");
        xmlNewProp(entry, BAD_CAST "out", BAD_CAST std::to_string(out).c_str());
    }

    std::unordered_map<uint64_t, size_t> visualIndexByTrack;
    for (size_t i = 0; i < seq.tracks.size(); ++i)
        visualIndexByTrack.emplace(seq.tracks[i].id.value, i);

    std::vector<TrackId> order = mltTrackOrder(seq);
    std::unordered_map<uint64_t, std::string> playlistIdByTrack;
    for (TrackId trackId : order) {
        const Track &track = model.track(trackId);
        std::string playlistId = "track_" + std::to_string(trackId.value);
        playlistIdByTrack.emplace(trackId.value, playlistId);
        writeTrackPlaylist(mlt, model, track, playlistId, visualIndexByTrack.at(trackId.value), producerIdByAsset);
    }

    xmlNodePtr blackProducer = xmlNewChild(mlt, nullptr, BAD_CAST "producer", nullptr);
    xmlNewProp(blackProducer, BAD_CAST "id", BAD_CAST "black");
    FrameIndex sequenceLength = std::max<FrameIndex>(seq.length(), 1);
    xmlNewProp(blackProducer, BAD_CAST "in", BAD_CAST "0");
    xmlNewProp(blackProducer, BAD_CAST "out", BAD_CAST std::to_string(sequenceLength - 1).c_str());
    addProperty(blackProducer, "resource", "color:black");

    xmlNodePtr tractor = xmlNewChild(mlt, nullptr, BAD_CAST "tractor", nullptr);
    xmlNewProp(tractor, BAD_CAST "id", BAD_CAST("seq" + std::to_string(seq.id.value)).c_str());
    xmlNewProp(tractor, BAD_CAST "in", BAD_CAST "0");
    xmlNewProp(tractor, BAD_CAST "out", BAD_CAST std::to_string(sequenceLength - 1).c_str());

    addProperty(tractor, "ustudio:format_version", std::to_string(kFormatVersion));
    addProperty(tractor, "ustudio:sequence_id", std::to_string(seq.id.value));
    addProperty(tractor, "ustudio:sequence_name", seq.name);
    addProperty(tractor, "ustudio:active_sequence", std::to_string(project.activeSequence.value));
    addProperty(tractor, "ustudio:next_id", std::to_string(project.nextId));
    addProperty(tractor, "ustudio:mlt_profile_name", seq.profile.mltName);
    addProperty(tractor, "ustudio:markers", markersToJson(seq.markers));
    addProperty(tractor, "ustudio:settings", settingsToJson(project.settings));

    xmlNodePtr blackTrack = xmlNewChild(tractor, nullptr, BAD_CAST "track", nullptr);
    xmlNewProp(blackTrack, BAD_CAST "producer", BAD_CAST "black");
    for (TrackId trackId : order) {
        xmlNodePtr trackNode = xmlNewChild(tractor, nullptr, BAD_CAST "track", nullptr);
        xmlNewProp(trackNode, BAD_CAST "producer", BAD_CAST playlistIdByTrack.at(trackId.value).c_str());
    }

    // Matches EngineSync::rebuildAll() exactly, so `melt`/u-studio-render
    // (no editor) sees the identical compositing/mix graph our own
    // playback does. See EngineSync's class comment for why this is a
    // uniform chain rather than doc 05's video/audio-split graph.
    for (size_t index = 1; index <= order.size(); ++index) {
        xmlNodePtr composite = xmlNewChild(tractor, nullptr, BAD_CAST "transition", nullptr);
        addProperty(composite, "mlt_service", "composite");
        addProperty(composite, "a_track", std::to_string(index - 1));
        addProperty(composite, "b_track", std::to_string(index));

        xmlNodePtr mix = xmlNewChild(tractor, nullptr, BAD_CAST "transition", nullptr);
        addProperty(mix, "mlt_service", "mix");
        addProperty(mix, "a_track", std::to_string(index - 1));
        addProperty(mix, "b_track", std::to_string(index));
        addProperty(mix, "start", "1");
        addProperty(mix, "sum", "1");
        addProperty(mix, "always_active", "1");
    }

    std::string tmpPath = path + ".tmp";
    int written = xmlSaveFormatFileEnc(tmpPath.c_str(), doc, "UTF-8", 1);
    xmlFreeDoc(doc);
    if (written < 0)
        return "failed to write " + tmpPath;

    int fd = ::open(tmpPath.c_str(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }

    std::error_code ec;
    fs::rename(tmpPath, targetPath, ec);
    if (ec) {
        fs::remove(tmpPath, ec);
        return "failed to replace " + path + " (" + ec.message() + ")";
    }

    return {};
}

} // namespace ustudio::core

#pragma GCC diagnostic pop
