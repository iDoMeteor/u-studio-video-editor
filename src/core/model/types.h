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

    // Still images/sequences and assets with no known length (0 = length
    // not yet probed, or a generator with no fixed duration) have no
    // meaningful source-range bound; a clip's in/out against them is never
    // rejected for being "out of range" (Model::check(), ResizeClip,
    // InsertClip all share this predicate so the definition can't drift).
    bool isBoundless() const
    {
        return isStillImage || isImageSequence || lengthInSequenceFrames <= 0;
    }

    bool operator==(const MediaInfo &) const = default;
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

    bool operator==(const Asset &) const = default;
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

    bool operator==(const Keyframe &) const = default;
};

struct Color
{
    uint8_t r = 0, g = 0, b = 0, a = 255;

    bool operator==(const Color &) const = default;
};

struct Rect
{
    double x = 0, y = 0, w = 0, h = 0;

    bool operator==(const Rect &) const = default;
};

struct Param
{
    std::string name; // MLT property name
    std::variant<double, int64_t, bool, std::string, Color, Rect> value;
    std::vector<Keyframe> keyframes; // empty = constant; positions relative to clip start

    bool operator==(const Param &) const = default;
};

struct Effect
{
    EffectId id;
    std::string service; // MLT service id, e.g. "affine", "volume", "avfilter.eq"
    std::string displayName;
    bool enabled = true;
    std::vector<Param> params;

    bool operator==(const Effect &) const = default;
};

struct FadeSpec
{
    FrameIndex length = 0;

    bool operator==(const FadeSpec &) const = default;
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

    bool operator==(const Clip &) const = default;
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

    bool operator==(const Track &) const = default;
};

// A dissolve between two adjacent clips on one track: `a` is the earlier
// clip, `b` the later one. Creating it grows `a`/`b` using their own
// existing source-media handle room -- `a.out` extends forward by
// `extendA`, `b.in`/`b.position` pull back by `extendB` -- so the pair's
// combined end-to-end span on the track is unchanged and nothing after
// `b` ever needs to move. `length` (the overlap width AddTransition/
// RemoveTransition operate on and Model::check() validates against) is
// always `extendA + extendB`; the split is stored explicitly (not just
// the total) so RemoveTransition can shrink exactly the side(s) that
// were actually grown, regardless of which edge was dragged to create it.
struct Transition
{
    TransitionId id;
    TrackId track;
    ClipId a, b;
    FrameIndex extendA = 0, extendB = 0;
    FrameIndex length = 0;        // == extendA + extendB
    std::string service = "luma"; // "luma" (dissolve) in v2.0; "mix" for audio auto

    bool operator==(const Transition &) const = default;
};

struct Marker
{
    MarkerId id;
    FrameIndex at = 0;
    std::string text;
    uint8_t color = 0;

    bool operator==(const Marker &) const = default;
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

    bool operator==(const Profile &) const = default;
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

    bool operator==(const Sequence &) const = default;
};

struct Project
{
    std::vector<Asset> bin;
    std::vector<Sequence> sequences; // exactly 1 in v2.0; the type allows more
    SequenceId activeSequence;
    uint64_t nextId = 1;                         // id allocator state; 0 is reserved for "invalid"
    std::map<std::string, std::string> settings; // free-form, persisted

    bool operator==(const Project &) const = default;
};

} // namespace ustudio::core
