#pragma once

#include "frame_time.h"
#include "ids.h"

#include <algorithm>
#include <map>
#include <memory>
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
    int sequenceBegin = 0; // an image sequence's first file number (the path is its %0Nd pattern)
    bool isStillImage = false;
    // The video has an alpha channel (ProRes 4444, VP9 alpha, QuickTime
    // Animation, a PNG with alpha...): its edges need the engine's alpha
    // pairing over other tracks. Set by the probe.
    bool hasAlpha = false;

    // Still images/sequences and assets with no known length (0 = length
    // not yet probed, or a generator with no fixed duration) have no
    // meaningful source-range bound; a clip's in/out against them is never
    // rejected for being "out of range" (Model::check(), ResizeClip,
    // InsertClip all share this predicate so the definition can't drift).
    bool isBoundless() const
    {
        // An image sequence has a real length: one frame per file (M4 E).
        return isStillImage || lengthInSequenceFrames <= 0;
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

// How a keyframe eases into the next one: MLT's mlt_keyframe_type one for
// one, same order and values (verified against ~/Repos/mlt at v7.40.0,
// src/framework/mlt_types.h), so the engine maps it by value (doc 15,
// "Animation strings").
enum class Easing
{
    Discrete = 0, // holds until the next keyframe
    Linear,
    SmoothLoose,   // Catmull-Rom; may overshoot (MLT's "smooth")
    SmoothNatural, // centripetal, natural slope, no overshoot
    SmoothTight,   // centripetal, flat at each keyframe
    SinusoidalIn,
    SinusoidalOut,
    SinusoidalInOut,
    QuadraticIn,
    QuadraticOut,
    QuadraticInOut,
    CubicIn,
    CubicOut,
    CubicInOut,
    QuarticIn,
    QuarticOut,
    QuarticInOut,
    QuinticIn,
    QuinticOut,
    QuinticInOut,
    ExponentialIn,
    ExponentialOut,
    ExponentialInOut,
    CircularIn,
    CircularOut,
    CircularInOut,
    BackIn,
    BackOut,
    BackInOut,
    ElasticIn,
    ElasticOut,
    ElasticInOut,
    BounceIn,
    BounceOut,
    BounceInOut,
};

struct Keyframe
{
    FrameIndex at = 0; // relative to the owner's start (clip, block); may lie past its end after a trim
    double value = 0.0;
    Easing easing = Easing::Linear;

    bool operator==(const Keyframe &) const = default;
};

// A number that may be animated: `value` when `keyframes` is empty.
struct KeyframedValue
{
    double value = 0.0;
    std::vector<Keyframe> keyframes;

    bool operator==(const KeyframedValue &) const = default;
};

// Where a clip's picture sits in the frame, OBS-style (ADR-018). Project
// pixels, so proxies and preview scale don't matter. Every number is a
// KeyframedValue (single-valued for now; keyframing is additive later).
struct Transform
{
    enum class Bounds
    {
        Fit,     // the (cropped) picture fitted into the frame, centred: the default
        Stretch, // the frame filled, aspect ignored
        None,    // placed explicitly: centre (x, y), size (width, height)
    };
    Bounds bounds = Bounds::Fit;
    KeyframedValue x, y;          // the picture's centre (None)
    KeyframedValue width, height; // its size on screen (None)
    KeyframedValue rotation;      // degrees clockwise, about the centre
    // Source pixels cut from each edge before placing.
    KeyframedValue cropLeft, cropTop, cropRight, cropBottom;
    bool flipH = false, flipV = false;

    bool operator==(const Transform &) const = default;
};

// A clip's Transform, copy-on-write. Every snapshot copies every clip
// (doc 19's publish budget: under 1 ms at 2,000 clips), and nine
// KeyframedValues per clip measured +0.45 ms there; most clips keep the
// default, which is a null pointer here, and a copy is a pointer copy.
class TransformSlot
{
  public:
    const Transform &get() const
    {
        static const Transform kDefault;
        return m_transform ? *m_transform : kDefault;
    }
    void set(Transform transform)
    {
        if (transform == Transform{})
            m_transform.reset();
        else
            m_transform = std::make_shared<const Transform>(std::move(transform));
    }
    bool operator==(const TransformSlot &other) const
    {
        return get() == other.get();
    }

  private:
    std::shared_ptr<const Transform> m_transform;
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
    using Value = std::variant<double, int64_t, bool, std::string, Color, Rect>;
    Value value;
    std::vector<Keyframe> keyframes; // empty = constant; positions relative to clip start

    bool operator==(const Param &) const = default;
};

// Limits an effect to a region (doc 15, "Mix and masks").
struct EffectMask
{
    std::string shape;         // "rectangle", "ellipse", or a luma map's name
    std::vector<Param> params; // the shape's geometry, keyframable
    KeyframedValue feather;
    bool invert = false;

    bool operator==(const EffectMask &) const = default;
};

struct Effect
{
    EffectId id;
    std::string service; // MLT service id, e.g. "affine", "volume", "avfilter.eq"
    std::string displayName;
    bool enabled = true;
    std::vector<Param> params;
    KeyframedValue mix{1.0, {}}; // wet/dry, 0-1
    std::optional<EffectMask> mask;
    // The drop-in that applies it ("effects", "audio-polish"; doc 17): an
    // effect whose owner isn't loaded is kept, saved and shown, not played.
    std::string owner;

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
    // Parameters of the clip's own producer, for clips a drop-in generates
    // (titles, doc 16). Empty for media.
    std::vector<Param> sourceParams;
    TransformSlot transform; // ADR-018; Fit by default (transform.get())

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
    // A drop-in's transition recipe (wipes, motion, blends; doc 15,
    // "Transitions") and its parameters. "" is the plain dissolve, played
    // with `service`; an unknown recipe also plays as that.
    std::string recipe;
    std::vector<Param> params;

    bool operator==(const Transition &) const = default;
};

// Effects on everything beneath it for a time range, on the FX lane
// `lane` above the tracks (doc 15, "FX lane").
struct AdjustmentBlock
{
    AdjustmentBlockId id;
    int lane = 0;
    FrameIndex start = 0;
    FrameIndex length = 0;
    std::vector<Effect> effects;
    std::optional<FadeSpec> fadeIn, fadeOut;

    FrameIndex end() const
    {
        return start + length;
    }
    bool operator==(const AdjustmentBlock &) const = default;
};

// A saved effect stack in the project bin (doc 15, "Looks").
struct Look
{
    LookId id;
    std::string name;
    std::vector<Effect> effects;

    bool operator==(const Look &) const = default;
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
    std::vector<Effect> effects; // master effects on the output
    std::vector<AdjustmentBlock> adjustmentBlocks;
    // The colour under every track, where nothing covers the frame:
    // 0xRRGGBB, opaque (a transparent export is the titles side's).
    uint32_t background = kDefaultBackground;
    static constexpr uint32_t kDefaultBackground = 0x000000;

    FrameIndex length() const
    {
        FrameIndex max = 0;
        for (const auto &entry : clips)
            max = std::max(max, entry.second.end());
        return max;
    }

    bool operator==(const Sequence &) const = default;
};

// A background as MLT's colour producer reads it ("0xRRGGBBAA", opaque),
// and as saved and shown ("#rrggbb").
std::string backgroundResource(uint32_t rgb);
std::string backgroundHex(uint32_t rgb);
// "#rrggbb" back to 0xRRGGBB; nullopt for anything else (untrusted input).
std::optional<uint32_t> parseBackgroundHex(const std::string &text);

struct Project
{
    std::vector<Asset> bin;
    std::vector<Look> looks;
    std::vector<Sequence> sequences; // exactly 1 in v2.0; the type allows more
    SequenceId activeSequence;
    uint64_t nextId = 1;                         // id allocator state; 0 is reserved for "invalid"
    std::map<std::string, std::string> settings; // free-form, persisted

    bool operator==(const Project &) const = default;
};

} // namespace ustudio::core
