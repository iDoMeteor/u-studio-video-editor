#pragma once

#include "frame_time.h"
#include "ids.h"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace ustudio::core {

struct MediaInfo
{
    bool hasVideo = false;
    bool hasAudio = false;
    int width = 0;
    int height = 0;
    Rational fps;
    Rational sar;
    int audioChannels = 0;
    int sampleRate = 0;
    FrameIndex lengthInSequenceFrames = 0; // computed when the asset is bound to a sequence
    double nativeDurationSeconds = 0.0;
    std::string videoCodec, audioCodec, container;
    bool isImageSequence = false;
    bool isStillImage = false;
};

struct Asset
{
    AssetId id;
    std::string path; // absolute at runtime; stored relative to project dir when possible
    std::string displayName;
    std::string folder; // bin folder path, "/" separated, "" = root
    MediaInfo info;
    enum class Status
    {
        Pending,
        Ready,
        Missing,
        Failed
    } status = Status::Pending;
    std::string proxyPath;       // empty if none
    std::string fileFingerprint; // size + mtime, for relink and cache keys
};

struct Keyframe
{
    FrameIndex at = 0;
    double value = 0.0;
    enum class Interp
    {
        Linear,
        Smooth,
        Hold
    } interp = Interp::Linear;
};

struct Color
{
    uint8_t r = 0, g = 0, b = 0, a = 255;
};

struct Rect
{
    double x = 0, y = 0, w = 0, h = 0;
};

struct Param
{
    std::string name; // MLT property name
    std::variant<double, int64_t, bool, std::string, Color, Rect> value;
    std::vector<Keyframe> keyframes; // empty = constant; positions relative to clip start
};

struct Effect
{
    EffectId id;
    std::string service; // MLT service id, e.g. "affine", "volume", "avfilter.eq"
    std::string displayName;
    bool enabled = true;
    std::vector<Param> params;
};

struct FadeSpec
{
    FrameIndex length = 0;
};

struct Clip
{
    ClipId id;
    AssetId asset;
    TrackId track;
    FrameIndex position = 0;    // on the track
    FrameIndex in = 0, out = 0; // source range, [in, out] inclusive like MLT; length = out-in+1
    double speed = 1.0;         // 1.0 only in v2.0; field reserved
    bool videoEnabled = true, audioEnabled = true;
    std::string name; // defaults to asset displayName
    std::vector<Effect> effects;
    std::optional<FadeSpec> fadeIn, fadeOut;

    FrameIndex length() const
    {
        return out - in + 1;
    }
    FrameIndex end() const
    {
        return position + length();
    } // exclusive
};

struct Track
{
    TrackId id;
    enum class Kind
    {
        Video,
        Audio
    } kind = Kind::Video;
    std::string name;
    bool muted = false, hidden = false, locked = false;
    std::vector<ClipId> clips;   // sorted by position, non-overlapping (invariant)
    std::vector<Effect> effects; // track-level (e.g. volume)
    double volume = 1.0;         // audio tracks and the audio part of video tracks
};

struct Transition
{
    TransitionId id;
    TrackId track;
    ClipId a, b;
    FrameIndex length = 0;
    std::string service = "luma"; // "luma" (dissolve) in v2.0; "mix" for audio auto
};

struct Marker
{
    MarkerId id;
    FrameIndex at = 0;
    std::string text;
    uint8_t color = 0;
};

struct Profile
{
    int width = 1920, height = 1080;
    Rational fps{30, 1};
    Rational sar{1, 1};
    Rational dar{16, 9};
    bool progressive = true;
    int colorspace = 709;
    std::string mltName; // "atsc_1080p_25" if it maps to a stock profile, else ""
};

struct Sequence
{
    SequenceId id;
    std::string name;
    Profile profile;
    std::vector<Track> tracks; // index 0 = TOP video track visually; audio tracks after video
    std::unordered_map<ClipId, Clip> clips;
    std::vector<Transition> transitions;
    std::vector<Marker> markers;

    FrameIndex length() const
    {
        FrameIndex max = 0;
        for (const auto &entry : clips)
            max = std::max(max, entry.second.end());
        return max;
    }
};

struct Project
{
    std::vector<Asset> bin;
    std::vector<Sequence> sequences; // exactly 1 in v2.0; the type allows more
    SequenceId activeSequence;
    uint64_t nextId = 1;                         // id allocator state; 0 is reserved for "invalid"
    std::map<std::string, std::string> settings; // free-form, persisted
};

} // namespace ustudio::core
