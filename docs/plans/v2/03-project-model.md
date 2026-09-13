# 03 — Project model

Lives in `src/core/model/`. Pure C++23, no GTK, no MLT. This is the source of
truth for everything the user edits (ADR-003).

## Time and identity

```cpp
using FrameIndex = int64_t;          // position/length in SEQUENCE frames (profile fps)
struct Rational { int32_t num, den; }; // fps, sample aspect
struct FrameRange { FrameIndex start, length; FrameIndex end() const; };

template <class Tag> struct Id { uint64_t value = 0; /* ==, <, hash */ };
using ClipId = Id<struct ClipTag>;   using TrackId = Id<struct TrackTag>;
using AssetId = Id<struct AssetTag>; using EffectId = Id<struct EffectTag>;
using MarkerId = Id<struct MarkerTag>;
```

- Ids are allocated by a per-document monotonic counter and **persisted** in
  the project file. They are never reused within a document, so undo/redo can
  refer to objects by id across removal and re-insertion.
- All timeline positions are integer frames at the sequence profile's fps.
  Source in/out points are *also* in sequence frames, because MLT creates
  every producer against the sequence profile and expresses in/out in profile
  frames. Frame-rate conversion is MLT's job. The model records the asset's
  native fps for display only.
- Time is never a `double` in the model. Timecode formatting is a view concern
  (`core/time.h` provides NDF timecode; drop-frame is a later option).

## Entities

```cpp
struct MediaInfo {
    bool hasVideo, hasAudio;
    int width, height; Rational fps; Rational sar;
    int audioChannels, sampleRate;
    FrameIndex lengthInSequenceFrames;   // computed when the asset is bound to a sequence
    double nativeDurationSeconds;
    std::string videoCodec, audioCodec, container;
    bool isImageSequence, isStillImage;
};

struct Asset {
    AssetId id;
    std::string path;                 // absolute at runtime; stored relative to project dir when possible
    std::string displayName;
    std::string folder;               // bin folder path, "/" separated, "" = root
    MediaInfo info;
    enum class Status { Pending, Ready, Missing, Failed } status;
    std::string proxyPath;            // empty if none
    std::string fileFingerprint;      // size + mtime, for relink and cache keys
};

struct Keyframe { FrameIndex at; double value; enum class Interp { Linear, Smooth, Hold } interp; };

struct Param {
    std::string name;                 // MLT property name
    std::variant<double, int64_t, bool, std::string, Color, Rect> value;
    std::vector<Keyframe> keyframes;  // empty = constant; positions relative to clip start
};

struct Effect {
    EffectId id;
    std::string service;              // MLT service id, e.g. "affine", "volume", "avfilter.eq"
    std::string displayName;
    bool enabled = true;
    std::vector<Param> params;
};

struct Clip {
    ClipId id; AssetId asset; TrackId track;
    FrameIndex position;              // on the track
    FrameIndex in, out;               // source range, [in, out] inclusive like MLT; length = out-in+1
    double speed = 1.0;               // 1.0 only in v2.0; field reserved
    bool videoEnabled = true, audioEnabled = true;
    std::string name;                 // defaults to asset displayName
    std::vector<Effect> effects;
    std::optional<FadeSpec> fadeIn, fadeOut;   // sugar over volume/brightness keyframes
};

struct Track {
    TrackId id;
    enum class Kind { Video, Audio } kind;
    std::string name;
    bool muted = false, hidden = false, locked = false;
    std::vector<ClipId> clips;        // sorted by position, non-overlapping (invariant)
    std::vector<Effect> effects;      // track-level (e.g. volume)
    double volume = 1.0;              // audio tracks and the audio part of video tracks
};

struct Transition {                   // dissolve between two adjacent clips on one track
    TransitionId id; TrackId track; ClipId a, b; FrameIndex length;
    std::string service = "luma";     // "luma" (dissolve) in v2.0; "mix" for audio auto
};

struct Marker { MarkerId id; FrameIndex at; std::string text; uint8_t color; };

struct Profile {                      // mirrors Mlt::Profile
    int width, height; Rational fps; Rational sar; Rational dar;
    bool progressive = true; int colorspace = 709;
    std::string mltName;              // "atsc_1080p_25" if it maps to a stock profile, else ""
};

struct Sequence {
    SequenceId id; std::string name; Profile profile;
    std::vector<Track> tracks;        // index 0 = TOP video track visually; audio tracks after video
    std::unordered_map<ClipId, Clip> clips;
    std::vector<Transition> transitions;
    std::vector<Marker> markers;
    FrameIndex length() const;        // max clip end over all tracks
};

struct Project {
    std::vector<Asset> bin;
    std::vector<Sequence> sequences;  // exactly 1 in v2.0; the type allows more
    SequenceId activeSequence;
    uint64_t nextId;                  // id allocator state
    std::map<std::string, std::string> settings;   // free-form, persisted
};
```

### Track ordering and MLT mapping

MLT composites tracks bottom-up: track 0 in the tractor is the bottom.
Visually, editors show the top track at the top. The model stores tracks in
**visual order** (index 0 = top). `EngineSync` reverses when building the
tractor. Audio tracks are stored after video tracks in the model, and in MLT
they get the lowest indices (they carry no video, so their order is
irrelevant for compositing and they must sit below all video for `mix`).

Kdenlive uses a hidden "black" track at index 0 so that gaps composite over
black rather than over nothing. We do the same: `EngineSync` inserts a
`color:black` producer of the sequence length at tractor index 0. It is not a
model track.

## Model class

```cpp
class Model {
public:
    const Project& project() const;
    const Sequence& sequence() const;              // active
    // Mutators — the ONLY way to change state. Each validates, mutates, emits.
    ClipId insertClip(TrackId, AssetId, FrameIndex pos, FrameIndex in, FrameIndex out, std::optional<ClipId> reuseId);
    void   removeClip(ClipId);
    void   moveClip(ClipId, TrackId, FrameIndex pos);
    void   resizeClip(ClipId, FrameIndex newIn, FrameIndex newOut, FrameIndex newPos);
    // … tracks, effects, params, keyframes, transitions, markers, assets …

    // Change events — synchronous, main thread, emitted AFTER state is consistent.
    Signal<const ModelEvent&> changed;

    // Invariants; cheap enough for debug builds after every command.
    std::vector<std::string> check() const;
};
```

`ModelEvent` is a `std::variant` of small structs
(`ClipInserted{ClipId}`, `ClipRemoved{ClipId, TrackId}`,
`ClipMoved{ClipId, TrackId from, TrackId to}`, `ClipResized{ClipId}`,
`TrackAdded{TrackId}`, `TrackRemoved{TrackId, size_t index}`,
`TrackFlagsChanged{TrackId}`, `EffectChanged{ClipId or TrackId, EffectId}`,
`TransitionChanged{…}`, `AssetChanged{AssetId}`, `SequenceProfileChanged{}`,
`BatchBegin{}`, `BatchEnd{}`).

`BatchBegin/End` wrap a transaction so the engine can defer rebuilding a
track until the batch closes, and the UI can defer relayout.

### Invariants (`Model::check()`)

1. Every `Track::clips` id exists in `sequence.clips` and has `clip.track ==
   track.id`.
2. Within a track, clips are sorted by `position` and `[position,
   position+length)` ranges are disjoint.
3. `0 <= in <= out < asset.info.lengthInSequenceFrames` unless the asset is
   a still image or a generator (then any non-negative range).
4. `position >= 0`.
5. Every `Clip::asset` exists in the bin (status may be Missing).
6. Transitions reference two clips that are adjacent on the same track and
   `length <= min(len(a), len(b))`.
7. Keyframe lists are sorted by `at`, unique, and `0 <= at < clip length`.
8. Audio tracks contain only clips with `videoEnabled == false`.
9. All ids `< project.nextId`.

Violations in debug builds are a hard assert with the message list. In release
they are logged and the offending command is reverted.

## Gaps and blanks

The model does not store gaps. A gap is the absence of a clip. `EngineSync`
derives MLT blank entries from the positions. Ripple operations are composite
commands over Move/Remove (doc 04).

## What is *not* in the model

- View state: zoom, scroll, selection, playhead, panel layout. Lives in the
  app layer; playhead position lives in `PlaybackController`.
- Caches: thumbnails, waveforms, proxies' existence. Derived from assets.
- The undo stack. It sits next to the model in `Document`.
