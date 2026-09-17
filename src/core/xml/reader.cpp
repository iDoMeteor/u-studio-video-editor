#include "reader.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <unordered_map>

// See writer.cpp's identical comment: BAD_CAST is libxml2's C-style cast,
// used throughout this file; the pragma push/pop works here because it
// wraps our own function bodies, not an #include block.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"

namespace ustudio::core {

namespace {

namespace fs = std::filesystem;

constexpr int kFormatVersion = 2; // must match writer.cpp

std::string attr(xmlNodePtr node, const char *name)
{
    xmlChar *value = xmlGetProp(node, BAD_CAST name);
    std::string result = value ? reinterpret_cast<const char *>(value) : "";
    if (value)
        xmlFree(value);
    return result;
}

std::optional<std::string> getProperty(xmlNodePtr node, const std::string &name)
{
    for (xmlNodePtr child = node->children; child; child = child->next) {
        if (child->type != XML_ELEMENT_NODE || xmlStrcmp(child->name, BAD_CAST "property") != 0)
            continue;
        xmlChar *propName = xmlGetProp(child, BAD_CAST "name");
        if (!propName)
            continue;
        bool matches = name == reinterpret_cast<const char *>(propName);
        xmlFree(propName);
        if (!matches)
            continue;

        xmlChar *content = xmlNodeGetContent(child);
        std::string value = content ? reinterpret_cast<const char *>(content) : "";
        if (content)
            xmlFree(content);
        return value;
    }
    return std::nullopt;
}

std::string prop(xmlNodePtr node, const std::string &name, const std::string &fallback = "")
{
    return getProperty(node, name).value_or(fallback);
}

int64_t toI64(const std::string &s)
{
    return s.empty() ? 0 : std::strtoll(s.c_str(), nullptr, 10);
}

uint64_t toU64(const std::string &s)
{
    return s.empty() ? 0 : std::strtoull(s.c_str(), nullptr, 10);
}

double toDouble(const std::string &s)
{
    return s.empty() ? 0.0 : std::strtod(s.c_str(), nullptr);
}

bool toBool(const std::string &s)
{
    return s == "1";
}

xmlNodePtr firstChildNamed(xmlNodePtr parent, const char *name)
{
    for (xmlNodePtr child = parent->children; child; child = child->next) {
        if (child->type == XML_ELEMENT_NODE && xmlStrcmp(child->name, BAD_CAST name) == 0)
            return child;
    }
    return nullptr;
}

// Inverse of writer.cpp's relativizePath(): a stored resource that starts
// with '/' is already absolute; one shaped like "service:arg" (a colon
// before any slash -- color:/noise:/tone: generators) is left as-is,
// since it was never a filesystem path; anything else is a path relative
// to the project file's directory.
std::string resolveResource(const std::string &stored, const fs::path &projectDir)
{
    if (stored.empty() || stored.front() == '/')
        return stored;

    size_t colon = stored.find(':');
    size_t slash = stored.find('/');
    bool looksLikeShorthand = colon != std::string::npos && (slash == std::string::npos || colon < slash);
    if (looksLikeShorthand)
        return stored;

    std::error_code ec;
    fs::path resolved = (projectDir / stored).lexically_normal();
    return ec ? stored : resolved.string();
}

// Matches writer.cpp's markersToJson()/settingsToJson() exactly -- not a
// general JSON parser, just the inverse of that specific hand-rolled
// encoding (a flat array of {id,at,text,color} objects; a flat
// string-to-string object).
std::string jsonUnescape(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char next = s[i + 1];
            switch (next) {
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            default:
                out += next;
            }
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Extracts the content of a JSON string literal starting at `pos`
// (pointing at the opening quote); returns the unescaped value and
// advances `pos` past the closing quote.
std::string readJsonString(const std::string &json, size_t &pos)
{
    ++pos; // skip opening quote
    std::string raw;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            raw += json[pos];
            raw += json[pos + 1];
            pos += 2;
        } else {
            raw += json[pos];
            ++pos;
        }
    }
    ++pos; // skip closing quote
    return jsonUnescape(raw);
}

std::vector<Marker> parseMarkersJson(const std::string &json)
{
    std::vector<Marker> markers;
    size_t pos = 0;
    while (pos < json.size() && json[pos] != '[')
        ++pos;
    ++pos;
    while (pos < json.size()) {
        while (pos < json.size() && (json[pos] == ',' || json[pos] == ' '))
            ++pos;
        if (pos >= json.size() || json[pos] == ']')
            break;
        if (json[pos] != '{') {
            ++pos;
            continue;
        }
        ++pos; // skip '{'
        Marker marker;
        while (pos < json.size() && json[pos] != '}') {
            while (pos < json.size() && (json[pos] == ',' || json[pos] == ' '))
                ++pos;
            if (pos >= json.size() || json[pos] == '}')
                break;
            std::string key = readJsonString(json, pos);
            while (pos < json.size() && json[pos] != ':')
                ++pos;
            ++pos; // skip ':'
            while (pos < json.size() && json[pos] == ' ')
                ++pos;

            std::string value;
            if (json[pos] == '"') {
                value = readJsonString(json, pos);
            } else {
                size_t start = pos;
                while (pos < json.size() && json[pos] != ',' && json[pos] != '}')
                    ++pos;
                value = json.substr(start, pos - start);
            }

            if (key == "id")
                marker.id = MarkerId{toU64(value)};
            else if (key == "at")
                marker.at = toI64(value);
            else if (key == "text")
                marker.text = value;
            else if (key == "color")
                marker.color = static_cast<uint8_t>(toI64(value));
        }
        ++pos; // skip '}'
        markers.push_back(std::move(marker));
    }
    return markers;
}

std::map<std::string, std::string> parseSettingsJson(const std::string &json)
{
    std::map<std::string, std::string> settings;
    size_t pos = 0;
    while (pos < json.size() && json[pos] != '{')
        ++pos;
    ++pos;
    while (pos < json.size()) {
        while (pos < json.size() && (json[pos] == ',' || json[pos] == ' '))
            ++pos;
        if (pos >= json.size() || json[pos] == '}')
            break;
        std::string key = readJsonString(json, pos);
        while (pos < json.size() && json[pos] != ':')
            ++pos;
        ++pos;
        while (pos < json.size() && json[pos] == ' ')
            ++pos;
        std::string value = readJsonString(json, pos);
        settings.emplace(std::move(key), std::move(value));
    }
    return settings;
}

} // namespace

std::expected<Model, std::string> loadProject(const std::string &path)
{
    xmlDocPtr doc = xmlReadFile(path.c_str(), nullptr, XML_PARSE_NOBLANKS);
    if (!doc)
        return std::unexpected("failed to parse " + path);

    xmlNodePtr mlt = xmlDocGetRootElement(doc);
    if (!mlt || xmlStrcmp(mlt->name, BAD_CAST "mlt") != 0) {
        xmlFreeDoc(doc);
        return std::unexpected(path + ": not an MLT XML file");
    }

    xmlNodePtr tractor = firstChildNamed(mlt, "tractor");
    if (!tractor) {
        xmlFreeDoc(doc);
        return std::unexpected(path + ": no <tractor> element");
    }

    std::optional<std::string> formatVersionStr = getProperty(tractor, "ustudio:format_version");
    if (!formatVersionStr) {
        xmlFreeDoc(doc);
        return std::unexpected(
            path + ": not a ustudio project (no ustudio:format_version) -- kdenlive import is a separate path (M7)");
    }
    int formatVersion = static_cast<int>(toI64(*formatVersionStr));
    if (formatVersion != kFormatVersion) {
        xmlFreeDoc(doc);
        return std::unexpected(path + ": unsupported ustudio:format_version " + std::to_string(formatVersion) +
                               " (this build writes " + std::to_string(kFormatVersion) + ")");
    }

    fs::path projectDir = fs::path(path).parent_path();

    Sequence seq;
    seq.id = SequenceId{toU64(prop(tractor, "ustudio:sequence_id", "1"))};
    seq.name = prop(tractor, "ustudio:sequence_name");
    seq.markers = parseMarkersJson(prop(tractor, "ustudio:markers", "[]"));

    if (xmlNodePtr profileNode = firstChildNamed(mlt, "profile")) {
        seq.profile.width = static_cast<int>(toI64(attr(profileNode, "width")));
        seq.profile.height = static_cast<int>(toI64(attr(profileNode, "height")));
        seq.profile.fps.num = static_cast<int32_t>(toI64(attr(profileNode, "frame_rate_num")));
        seq.profile.fps.den = static_cast<int32_t>(toI64(attr(profileNode, "frame_rate_den")));
        seq.profile.sar.num = static_cast<int32_t>(toI64(attr(profileNode, "sample_aspect_num")));
        seq.profile.sar.den = static_cast<int32_t>(toI64(attr(profileNode, "sample_aspect_den")));
        seq.profile.dar.num = static_cast<int32_t>(toI64(attr(profileNode, "display_aspect_num")));
        seq.profile.dar.den = static_cast<int32_t>(toI64(attr(profileNode, "display_aspect_den")));
        seq.profile.progressive = attr(profileNode, "progressive") == "1";
        seq.profile.colorspace = static_cast<int>(toI64(attr(profileNode, "colorspace")));
    }
    seq.profile.mltName = prop(tractor, "ustudio:mlt_profile_name");

    Project project;
    project.activeSequence = SequenceId{toU64(prop(tractor, "ustudio:active_sequence", "1"))};
    project.nextId = toU64(prop(tractor, "ustudio:next_id", "1"));
    project.settings = parseSettingsJson(prop(tractor, "ustudio:settings", "{}"));

    // Asset producers: any <producer> with an ustudio:asset_id property
    // (the "black" backing producer and any future non-asset producer
    // have none, so this filters them out naturally).
    std::unordered_map<std::string, AssetId> assetIdByNodeId;
    for (xmlNodePtr node = mlt->children; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE || xmlStrcmp(node->name, BAD_CAST "producer") != 0)
            continue;
        std::optional<std::string> assetIdStr = getProperty(node, "ustudio:asset_id");
        if (!assetIdStr)
            continue;

        Asset asset;
        asset.id = AssetId{toU64(*assetIdStr)};
        asset.path = resolveResource(prop(node, "resource"), projectDir);
        asset.displayName = prop(node, "ustudio:display_name");
        asset.folder = prop(node, "ustudio:folder");
        asset.fileFingerprint = prop(node, "ustudio:fingerprint");
        asset.proxyPath = prop(node, "ustudio:proxy");
        asset.status = static_cast<Asset::Status>(toI64(prop(node, "ustudio:status", "0")));

        MediaInfo &info = asset.info;
        info.hasVideo = toBool(prop(node, "ustudio:has_video"));
        info.hasAudio = toBool(prop(node, "ustudio:has_audio"));
        info.width = static_cast<int>(toI64(prop(node, "ustudio:width")));
        info.height = static_cast<int>(toI64(prop(node, "ustudio:height")));
        info.fps.num = static_cast<int32_t>(toI64(prop(node, "ustudio:fps_num", "1")));
        info.fps.den = static_cast<int32_t>(toI64(prop(node, "ustudio:fps_den", "1")));
        info.sar.num = static_cast<int32_t>(toI64(prop(node, "ustudio:sar_num", "1")));
        info.sar.den = static_cast<int32_t>(toI64(prop(node, "ustudio:sar_den", "1")));
        info.audioChannels = static_cast<int>(toI64(prop(node, "ustudio:audio_channels")));
        info.sampleRate = static_cast<int>(toI64(prop(node, "ustudio:sample_rate")));
        info.nativeDurationSeconds = toDouble(prop(node, "ustudio:native_duration"));
        info.videoCodec = prop(node, "ustudio:video_codec");
        info.audioCodec = prop(node, "ustudio:audio_codec");
        info.container = prop(node, "ustudio:container");
        info.isImageSequence = toBool(prop(node, "ustudio:is_image_sequence"));
        info.isStillImage = toBool(prop(node, "ustudio:is_still_image"));
        FrameIndex out = toI64(attr(node, "out"));
        info.lengthInSequenceFrames = out > 0 ? out + 1 : 0;

        assetIdByNodeId.emplace(attr(node, "id"), asset.id);
        project.bin.push_back(std::move(asset));
    }

    // Track playlists: any <playlist> with an ustudio:track_id property
    // (main_bin, the bin-keeper playlist, has none).
    struct ParsedTrack
    {
        size_t visualIndex;
        Track track;
    };
    std::vector<ParsedTrack> parsedTracks;

    for (xmlNodePtr node = mlt->children; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE || xmlStrcmp(node->name, BAD_CAST "playlist") != 0)
            continue;
        std::optional<std::string> trackIdStr = getProperty(node, "ustudio:track_id");
        if (!trackIdStr)
            continue;

        Track track;
        track.id = TrackId{toU64(*trackIdStr)};
        track.kind = prop(node, "ustudio:kind") == "audio" ? Track::Kind::Audio : Track::Kind::Video;
        track.name = prop(node, "ustudio:name");
        track.muted = toBool(prop(node, "ustudio:muted"));
        track.hidden = toBool(prop(node, "ustudio:hidden"));
        track.locked = toBool(prop(node, "ustudio:locked"));
        track.volume = toDouble(prop(node, "ustudio:volume", "1"));
        size_t visualIndex = static_cast<size_t>(toI64(prop(node, "ustudio:visual_index")));

        FrameIndex cursor = 0;
        for (xmlNodePtr entryNode = node->children; entryNode; entryNode = entryNode->next) {
            if (entryNode->type != XML_ELEMENT_NODE)
                continue;
            if (xmlStrcmp(entryNode->name, BAD_CAST "blank") == 0) {
                cursor += toI64(attr(entryNode, "length"));
                continue;
            }
            if (xmlStrcmp(entryNode->name, BAD_CAST "entry") != 0)
                continue;

            Clip clip;
            clip.id = ClipId{toU64(prop(entryNode, "ustudio:clip_id"))};
            clip.track = track.id;
            clip.asset = assetIdByNodeId.at(attr(entryNode, "producer"));
            clip.position = cursor;
            clip.in = toI64(attr(entryNode, "in"));
            clip.out = toI64(attr(entryNode, "out"));
            clip.name = prop(entryNode, "ustudio:name");
            clip.speed = toDouble(prop(entryNode, "ustudio:speed", "1"));
            clip.videoEnabled = toBool(prop(entryNode, "ustudio:video_enabled", "1"));
            clip.audioEnabled = toBool(prop(entryNode, "ustudio:audio_enabled", "1"));
            if (std::optional<std::string> fadeIn = getProperty(entryNode, "ustudio:fade_in"))
                clip.fadeIn = FadeSpec{toI64(*fadeIn)};
            if (std::optional<std::string> fadeOut = getProperty(entryNode, "ustudio:fade_out"))
                clip.fadeOut = FadeSpec{toI64(*fadeOut)};

            cursor = clip.position + (clip.out - clip.in + 1);
            track.clips.push_back(clip.id);
            seq.clips.emplace(clip.id, std::move(clip));
        }

        parsedTracks.push_back({visualIndex, std::move(track)});
    }

    std::sort(parsedTracks.begin(), parsedTracks.end(),
              [](const ParsedTrack &a, const ParsedTrack &b) { return a.visualIndex < b.visualIndex; });
    for (ParsedTrack &parsed : parsedTracks)
        seq.tracks.push_back(std::move(parsed.track));

    project.sequences.push_back(std::move(seq));

    xmlFreeDoc(doc);
    return Model(std::move(project));
}

} // namespace ustudio::core

#pragma GCC diagnostic pop
