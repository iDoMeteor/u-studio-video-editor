#include "model.h"
#include "transform.h"
#include "transition_native.h"

#include "core/log.h"

#include <algorithm>
#include <cstdio>
#include <tuple>
#include <unordered_map>
#include <cassert>

namespace ustudio::core {

std::string backgroundResource(uint32_t rgb)
{
    char text[16];
    std::snprintf(text, sizeof text, "0x%06xff", static_cast<unsigned>(rgb & 0xffffff));
    return text;
}

std::string backgroundHex(uint32_t rgb)
{
    char text[8];
    std::snprintf(text, sizeof text, "#%06x", static_cast<unsigned>(rgb & 0xffffff));
    return text;
}

std::optional<uint32_t> parseBackgroundHex(const std::string &text)
{
    if (text.size() != 7 || text[0] != '#')
        return std::nullopt;
    uint32_t rgb = 0;
    for (size_t i = 1; i < 7; ++i) {
        const char c = text[i];
        const int digit = c >= '0' && c <= '9'   ? c - '0'
                          : c >= 'a' && c <= 'f' ? c - 'a' + 10
                          : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                 : -1;
        if (digit < 0)
            return std::nullopt;
        rgb = rgb << 4 | static_cast<uint32_t>(digit);
    }
    return rgb;
}

namespace {
// A caller broke one of Model's preconditions (commands validate first, in
// their apply()). Debug builds stop here, after logging which; a release
// build (NDEBUG) logs it and the caller takes a safe path -- the plain
// assert()s these replace compiled out and left an end() iterator
// dereferenced or erased.
void preconditionFailed(const char *what)
{
    Log::error(std::string("[model] ") + what);
    assert(false && "Model precondition failed; the log says which");
}

void shiftKeyframes(std::vector<Keyframe> &keyframes, FrameIndex by)
{
    for (Keyframe &keyframe : keyframes)
        keyframe.at += by;
}

void shiftEffectKeyframes(Effect &effect, FrameIndex by)
{
    for (Param &param : effect.params)
        shiftKeyframes(param.keyframes, by);
    shiftKeyframes(effect.mix.keyframes, by);
    if (effect.mask) {
        for (Param &param : effect.mask->params)
            shiftKeyframes(param.keyframes, by);
        shiftKeyframes(effect.mask->feather.keyframes, by);
    }
}
} // namespace

Model Model::createEmpty(Profile profile)
{
    Project project;
    Sequence sequence;
    sequence.id = SequenceId{1};
    sequence.name = "Sequence 1";
    sequence.profile = std::move(profile);
    project.sequences.push_back(std::move(sequence));
    project.activeSequence = SequenceId{1};
    project.nextId = 2; // SequenceId{1} was consumed above
    return Model(std::move(project));
}

Model::Model(Project project) : m_project(std::move(project)) {}

Model::Model(const Model &other) : m_project(other.m_project) {}

Model &Model::operator=(const Model &other)
{
    m_project = other.m_project;
    m_snapshot.reset(); // a whole new project (Open Project)
    return *this;
}

Model::Model(Model &&other) noexcept : m_project(std::move(other.m_project))
{
    other.m_snapshot.reset(); // it described the moved-out project
}

Model &Model::operator=(Model &&other) noexcept
{
    m_project = std::move(other.m_project);
    m_snapshot.reset();
    other.m_snapshot.reset();
    return *this;
}

uint64_t Model::allocateId()
{
    return m_project.nextId++;
}

void Model::reserveId(uint64_t value)
{
    // Invariant 9 (doc 03): all ids < project.nextId. Reused ids (redo)
    // may already satisfy this; ids from a fresh apply might not yet.
    if (value >= m_project.nextId)
        m_project.nextId = value + 1;
}

Sequence &Model::activeSequence()
{
    for (auto &seq : m_project.sequences) {
        if (seq.id == m_project.activeSequence)
            return seq;
    }
    preconditionFailed("no active sequence -- Project invariant violated");
    if (m_project.sequences.empty()) {
        Sequence empty;
        empty.id = m_project.activeSequence;
        m_project.sequences.push_back(std::move(empty));
    }
    return m_project.sequences.front();
}

const Sequence &Model::activeSequence() const
{
    for (const auto &seq : m_project.sequences) {
        if (seq.id == m_project.activeSequence)
            return seq;
    }
    preconditionFailed("no active sequence -- Project invariant violated");
    static const Sequence kNone;
    return m_project.sequences.empty() ? kNone : m_project.sequences.front();
}

const Sequence &Model::sequence() const
{
    return activeSequence();
}

Sequence &Model::mutableSequence()
{
    return activeSequence();
}

bool Model::hasAsset(AssetId id) const
{
    const auto &bin = m_project.bin;
    return std::any_of(bin.begin(), bin.end(), [id](const Asset &entry) { return entry.id == id; });
}

const Asset &Model::asset(AssetId id) const
{
    const auto &bin = m_project.bin;
    auto it = std::find_if(bin.begin(), bin.end(), [id](const Asset &entry) { return entry.id == id; });
    if (it == bin.end()) {
        preconditionFailed("Model::asset: unknown AssetId");
        static const Asset kNone;
        return kNone;
    }
    return *it;
}

bool Model::hasClip(ClipId id) const
{
    return activeSequence().clips.contains(id);
}

const Clip &Model::clip(ClipId id) const
{
    const auto &clips = activeSequence().clips;
    auto it = clips.find(id);
    if (it == clips.end()) {
        preconditionFailed("Model::clip: unknown ClipId");
        static const Clip kNone;
        return kNone;
    }
    return it->second;
}

Clip &Model::mutableClip(ClipId id)
{
    auto &clips = activeSequence().clips;
    auto it = clips.find(id);
    if (it == clips.end()) {
        preconditionFailed("Model::mutableClip: unknown ClipId");
        static Clip scratch; // somewhere harmless for the caller's writes
        scratch = Clip{};
        return scratch;
    }
    return it->second;
}

bool Model::hasTrack(TrackId id) const
{
    const auto &tracks = activeSequence().tracks;
    return std::any_of(tracks.begin(), tracks.end(), [id](const Track &entry) { return entry.id == id; });
}

const Track &Model::track(TrackId id) const
{
    const auto &tracks = activeSequence().tracks;
    auto it = std::find_if(tracks.begin(), tracks.end(), [id](const Track &entry) { return entry.id == id; });
    if (it == tracks.end()) {
        preconditionFailed("Model::track: unknown TrackId");
        static const Track kNone;
        return kNone;
    }
    return *it;
}

bool Model::hasTransition(TransitionId id) const
{
    const auto &transitions = activeSequence().transitions;
    return std::any_of(transitions.begin(), transitions.end(),
                       [id](const Transition &entry) { return entry.id == id; });
}

const Transition &Model::transition(TransitionId id) const
{
    const auto &transitions = activeSequence().transitions;
    auto it =
        std::find_if(transitions.begin(), transitions.end(), [id](const Transition &entry) { return entry.id == id; });
    if (it == transitions.end()) {
        preconditionFailed("Model::transition: unknown TransitionId");
        static const Transition kNone;
        return kNone;
    }
    return *it;
}

bool Model::isRangeFree(TrackId trackId, FrameIndex start, FrameIndex end, const std::vector<ClipId> &ignoreClips) const
{
    // The track is sorted by position, and check()'s invariant 2 lets a clip
    // overlap only its predecessor, by a dissolve no longer than either clip,
    // so ends never decrease either. Only the clips from the last one ending
    // at or before `start` to the first starting at or after `end` can
    // overlap: found by binary search, not a scan of the whole track (which
    // made inserting n clips quadratic: 20,000 captions took minutes).
    const Track &target = track(trackId);
    const auto &clips = activeSequence().clips;
    auto startsAtOrAfterEnd = std::partition_point(target.clips.begin(), target.clips.end(),
                                                   [&](ClipId id) { return clips.at(id).position < end; });
    for (auto it = startsAtOrAfterEnd; it != target.clips.begin();) {
        --it;
        const Clip &existing = clips.at(*it);
        if (existing.end() <= start)
            break; // and every clip before it ends earlier still
        if (std::find(ignoreClips.begin(), ignoreClips.end(), *it) == ignoreClips.end())
            return false; // overlap
    }
    return true;
}

Track &Model::mutableTrack(TrackId id)
{
    auto &tracks = activeSequence().tracks;
    auto it = std::find_if(tracks.begin(), tracks.end(), [id](const Track &entry) { return entry.id == id; });
    if (it == tracks.end()) {
        preconditionFailed("Model::mutableTrack: unknown TrackId");
        static Track scratch; // somewhere harmless for the caller's writes
        scratch = Track{};
        return scratch;
    }
    return *it;
}

bool Model::clipBefore(ClipId a, ClipId b) const
{
    const auto &clips = activeSequence().clips;
    const Clip &ca = clips.at(a), &cb = clips.at(b);
    return ClipOrderKey{ca.position, ca.end(), a} < ClipOrderKey{cb.position, cb.end(), b};
}

void Model::placeClip(Track &trackRef, ClipId id, std::optional<ClipOrderKey> was)
{
    // One clip arrived, moved or changed length; the rest are still in
    // order. Re-sorting the whole track (a hash lookup per comparison) made
    // bulk inserts quadratic: 20,000 captions took minutes. Here it's binary
    // searches, and a move of the ids after it only when its place changes.
    auto &ids = trackRef.clips;
    const auto &clips = activeSequence().clips;
    auto key = [&clips](ClipId other) {
        const Clip &c = clips.at(other);
        return ClipOrderKey{c.position, c.end(), other};
    };
    const Clip &placed = clips.at(id);
    const ClipOrderKey now{placed.position, placed.end(), id};
    if (was) {
        // Found by its key before the change (every other clip is unchanged,
        // so the track is ordered by that). Where it still sits between its
        // neighbours (a ripple shifts every later clip alike), nothing moves.
        auto it = std::lower_bound(ids.begin(), ids.end(), *was, [&](ClipId other, const ClipOrderKey &target) {
            return (other == id ? *was : key(other)) < target;
        });
        if (it != ids.end() && *it == id) {
            const bool afterPrevious = it == ids.begin() || key(*(it - 1)) < now;
            const bool beforeNext = it + 1 == ids.end() || now < key(*(it + 1));
            if (afterPrevious && beforeNext)
                return;
            ids.erase(it);
        } else if (auto found = std::find(ids.begin(), ids.end(), id); found != ids.end()) {
            ids.erase(found); // the caller's key was stale: still correct, just slower
        }
    }
    ids.insert(std::upper_bound(ids.begin(), ids.end(), now,
                                [&](const ClipOrderKey &target, ClipId other) { return target < key(other); }),
               id);
}

void Model::sortTrackClips(Track &trackRef)
{
    // Ties broken by end, then id, so the order is the same however the
    // clips got there: two clips can share a start only inside a dissolve
    // (saved before AddTransition refused whole-clip overlaps), where the
    // outgoing one ends first.
    std::sort(trackRef.clips.begin(), trackRef.clips.end(), [this](ClipId a, ClipId b) { return clipBefore(a, b); });
}

// --- Asset mutators -------------------------------------------------------

AssetId Model::addAsset(Asset newAsset, std::optional<AssetId> reuseId)
{
    AssetId id = reuseId.value_or(AssetId{allocateId()});
    reserveId(id.value);
    newAsset.id = id;
    m_project.bin.push_back(std::move(newAsset));
    notify(AssetChanged{id});
    return id;
}

void Model::removeAsset(AssetId id)
{
    auto &bin = m_project.bin;
    auto it = std::find_if(bin.begin(), bin.end(), [id](const Asset &entry) { return entry.id == id; });
    if (it == bin.end()) {
        preconditionFailed("Model::removeAsset: unknown AssetId");
        return;
    }
    bin.erase(it);
    notify(AssetChanged{id});
}

void Model::extendAssetLength(AssetId id, FrameIndex minimumLength)
{
    auto &bin = m_project.bin;
    auto it = std::find_if(bin.begin(), bin.end(), [id](const Asset &entry) { return entry.id == id; });
    if (it == bin.end()) {
        preconditionFailed("Model::extendAssetLength: unknown AssetId");
        return;
    }
    if (minimumLength <= it->info.lengthInSequenceFrames)
        return;
    it->info.lengthInSequenceFrames = minimumLength;
    notify(AssetChanged{id});
}

void Model::setAssetLength(AssetId id, FrameIndex length)
{
    auto &bin = m_project.bin;
    auto it = std::find_if(bin.begin(), bin.end(), [id](const Asset &entry) { return entry.id == id; });
    if (it == bin.end()) {
        preconditionFailed("Model::setAssetLength: unknown AssetId");
        return;
    }
    if (length == it->info.lengthInSequenceFrames)
        return;
    it->info.lengthInSequenceFrames = length;
    notify(AssetChanged{id});
}

void Model::setAssetStatus(AssetId id, Asset::Status status)
{
    auto &bin = m_project.bin;
    auto it = std::find_if(bin.begin(), bin.end(), [id](const Asset &entry) { return entry.id == id; });
    if (it == bin.end()) {
        preconditionFailed("Model::setAssetStatus: unknown AssetId");
        return;
    }
    if (it->status == status)
        return;
    it->status = status;
    notify(AssetChanged{id});
}

void Model::setAssetSource(AssetId id, std::string path, std::string fingerprint, Asset::Status status)
{
    auto &bin = m_project.bin;
    auto it = std::find_if(bin.begin(), bin.end(), [id](const Asset &entry) { return entry.id == id; });
    if (it == bin.end()) {
        preconditionFailed("Model::setAssetSource: unknown AssetId");
        return;
    }
    it->path = std::move(path);
    it->fileFingerprint = std::move(fingerprint);
    it->status = status;
    notify(AssetChanged{id});
}

void Model::setAssetSequenceBegin(AssetId id, int begin)
{
    auto it = std::find_if(m_project.bin.begin(), m_project.bin.end(), [id](const Asset &a) { return a.id == id; });
    if (it == m_project.bin.end()) {
        preconditionFailed("Model::setAssetSequenceBegin: unknown AssetId");
        return;
    }
    if (it->info.sequenceBegin == begin)
        return;
    it->info.sequenceBegin = begin;
    notify(AssetChanged{id});
}

void Model::setAssetProxy(AssetId id, std::string proxyPath)
{
    auto &bin = m_project.bin;
    auto it = std::find_if(bin.begin(), bin.end(), [id](const Asset &entry) { return entry.id == id; });
    if (it == bin.end()) {
        preconditionFailed("Model::setAssetProxy: unknown AssetId");
        return;
    }
    if (it->proxyPath == proxyPath)
        return;
    it->proxyPath = std::move(proxyPath);
    notify(AssetChanged{id});
}

void Model::setProjectSetting(const std::string &key, const std::string &value)
{
    if (value.empty())
        m_project.settings.erase(key);
    else
        m_project.settings[key] = value;
}

// --- Track mutators --------------------------------------------------------

TrackId Model::addTrack(Track::Kind kind, size_t index, std::string name, std::optional<TrackId> reuseId)
{
    TrackId id = reuseId.value_or(TrackId{allocateId()});
    reserveId(id.value);

    Track newTrack;
    newTrack.id = id;
    newTrack.kind = kind;
    newTrack.name = std::move(name);

    auto &tracks = activeSequence().tracks;
    size_t clampedIndex = std::min(index, tracks.size());
    tracks.insert(tracks.begin() + static_cast<std::ptrdiff_t>(clampedIndex), std::move(newTrack));

    notify(TrackAdded{id});
    return id;
}

void Model::removeTrack(TrackId id)
{
    auto &tracks = activeSequence().tracks;
    auto it = std::find_if(tracks.begin(), tracks.end(), [id](const Track &entry) { return entry.id == id; });
    if (it == tracks.end()) {
        preconditionFailed("Model::removeTrack: unknown TrackId");
        return;
    }

    size_t index = static_cast<size_t>(std::distance(tracks.begin(), it));

    // Remove every clip that lived on this track (invariant 1: every
    // sequence.clips entry has a home in some track's clip list).
    auto &clips = activeSequence().clips;
    for (ClipId clipId : it->clips)
        clips.erase(clipId);

    tracks.erase(it);
    notify(TrackRemoved{id, index});
}

void Model::setTrackFlags(TrackId id, bool muted, bool hidden, bool locked)
{
    Track &target = mutableTrack(id);
    target.muted = muted;
    target.hidden = hidden;
    target.locked = locked;
    notify(TrackFlagsChanged{id});
}

void Model::setTrackVolume(TrackId id, double volume)
{
    Track &target = mutableTrack(id);
    target.volume = volume;
    notify(TrackVolumeChanged{id});
}

void Model::setSequenceProfile(const Profile &profile)
{
    activeSequence().profile = profile;
    notify(SequenceProfileChanged{});
}

void Model::setSequenceBackground(uint32_t rgb)
{
    activeSequence().background = rgb & 0xffffff;
    notify(SequenceBackgroundChanged{});
}

void Model::replaceSequenceAndBin(Sequence sequence, std::vector<Asset> bin)
{
    activeSequence() = std::move(sequence);
    m_project.bin = std::move(bin);
    notify(SequenceProfileChanged{});
}

MarkerId Model::addMarker(FrameIndex at, std::string text, std::optional<MarkerId> reuseId)
{
    MarkerId id = reuseId ? *reuseId : MarkerId{allocateId()};
    if (reuseId)
        reserveId(reuseId->value);
    auto &markers = activeSequence().markers;
    markers.push_back(Marker{id, at, std::move(text), 0});
    std::stable_sort(markers.begin(), markers.end(), [](const Marker &a, const Marker &b) { return a.at < b.at; });
    notify(MarkersChanged{});
    return id;
}

void Model::removeMarker(MarkerId id)
{
    auto &markers = activeSequence().markers;
    auto it = std::find_if(markers.begin(), markers.end(), [id](const Marker &m) { return m.id == id; });
    if (it == markers.end()) {
        preconditionFailed("Model::removeMarker: unknown MarkerId");
        return;
    }
    markers.erase(it);
    notify(MarkersChanged{});
}

void Model::setMarker(MarkerId id, FrameIndex at, std::string text)
{
    auto &markers = activeSequence().markers;
    auto it = std::find_if(markers.begin(), markers.end(), [id](const Marker &m) { return m.id == id; });
    if (it == markers.end()) {
        preconditionFailed("Model::setMarker: unknown MarkerId");
        return;
    }
    it->at = at;
    it->text = std::move(text);
    std::stable_sort(markers.begin(), markers.end(), [](const Marker &a, const Marker &b) { return a.at < b.at; });
    notify(MarkersChanged{});
}

bool Model::hasMarker(MarkerId id) const
{
    const auto &markers = activeSequence().markers;
    return std::any_of(markers.begin(), markers.end(), [id](const Marker &m) { return m.id == id; });
}

const Marker &Model::marker(MarkerId id) const
{
    const auto &markers = activeSequence().markers;
    auto it = std::find_if(markers.begin(), markers.end(), [id](const Marker &m) { return m.id == id; });
    if (it == markers.end()) {
        preconditionFailed("Model::marker: unknown MarkerId");
        static const Marker kNone;
        return kNone;
    }
    return *it;
}

void Model::setTrackName(TrackId id, std::string name)
{
    Track &target = mutableTrack(id);
    target.name = std::move(name);
    notify(TrackRenamed{id});
}

void Model::moveTrack(TrackId id, size_t newIndex)
{
    auto &tracks = activeSequence().tracks;
    auto it = std::find_if(tracks.begin(), tracks.end(), [id](const Track &entry) { return entry.id == id; });
    if (it == tracks.end()) {
        preconditionFailed("Model::moveTrack: unknown TrackId");
        return;
    }

    Track moved = std::move(*it);
    tracks.erase(it);
    size_t clampedIndex = std::min(newIndex, tracks.size());
    tracks.insert(tracks.begin() + static_cast<std::ptrdiff_t>(clampedIndex), std::move(moved));

    notify(TrackReordered{id});
}

// --- Clip mutators -----------------------------------------------------------

ClipId Model::insertClip(TrackId trackId, AssetId assetId, FrameIndex pos, FrameIndex in, FrameIndex out,
                         std::optional<ClipId> reuseId)
{
    ClipId id = reuseId.value_or(ClipId{allocateId()});
    reserveId(id.value);

    Clip newClip;
    newClip.id = id;
    newClip.asset = assetId;
    newClip.track = trackId;
    newClip.position = pos;
    newClip.in = in;
    newClip.out = out;
    newClip.name = hasAsset(assetId) ? asset(assetId).displayName : std::string{};

    activeSequence().clips.emplace(id, std::move(newClip));

    placeClip(mutableTrack(trackId), id);

    notify(ClipInserted{id});
    return id;
}

void Model::removeClip(ClipId id)
{
    Clip &target = mutableClip(id);
    TrackId trackId = target.track;

    // Found by its key (still in order), not a scan: undoing a bulk insert
    // removes every clip, which a scan per clip made quadratic.
    Track &owningTrack = mutableTrack(trackId);
    auto &ids = owningTrack.clips;
    const ClipOrderKey key{target.position, target.end(), id};
    auto it = std::lower_bound(ids.begin(), ids.end(), key, [this](ClipId other, const ClipOrderKey &k) {
        const Clip &c = activeSequence().clips.at(other);
        return ClipOrderKey{c.position, c.end(), other} < k;
    });
    if (it != ids.end() && *it == id)
        ids.erase(it);
    else
        ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end()); // out of order: still removed

    activeSequence().clips.erase(id);
    notify(ClipRemoved{id, trackId});
}

void Model::moveClip(ClipId id, TrackId newTrackId, FrameIndex pos)
{
    Clip &target = mutableClip(id);
    TrackId oldTrackId = target.track;
    const ClipOrderKey was{target.position, target.end(), id};

    if (oldTrackId != newTrackId) {
        Track &oldTrack = mutableTrack(oldTrackId);
        oldTrack.clips.erase(std::remove(oldTrack.clips.begin(), oldTrack.clips.end(), id), oldTrack.clips.end());

        target.track = newTrackId;
        target.position = pos;

        placeClip(mutableTrack(newTrackId), id);
    } else {
        target.position = pos;
        placeClip(mutableTrack(oldTrackId), id, was);
    }

    notify(ClipMoved{id, oldTrackId, newTrackId});
}

void Model::resizeClip(ClipId id, FrameIndex newIn, FrameIndex newOut, FrameIndex newPos)
{
    Clip &target = mutableClip(id);
    const ClipOrderKey was{target.position, target.end(), id};
    target.in = newIn;
    target.out = newOut;
    target.position = newPos;
    placeClip(mutableTrack(target.track), id, was);
    notify(ClipResized{id});
}

ClipId Model::splitClip(ClipId id, FrameIndex at, std::optional<ClipId> reuseRightId,
                        std::vector<EffectId> *rightEffectIds)
{
    if (!hasClip(id)) {
        preconditionFailed("Model::splitClip: unknown ClipId");
        return ClipId{};
    }
    Clip &left = mutableClip(id);
    if (at <= left.position || at >= left.end()) {
        preconditionFailed("Model::splitClip: split point must be strictly inside the clip");
        return ClipId{};
    }

    FrameIndex offsetIntoClip = at - left.position;

    Clip right = left; // copies asset, track, effects, name, fade-out
    right.position = at;
    right.in = left.in + offsetIntoClip;
    right.fadeIn.reset(); // the new left edge is a hard cut, not a fade

    left.out = left.in + offsetIntoClip - 1;
    left.fadeOut.reset(); // the new right edge is a hard cut

    // Batched (audit C5): insertClip() below fires its own ClipInserted
    // before the effects/flags/fade copies just below it run, so a
    // listener that resyncs immediately on every event (EngineSync) would
    // otherwise rebuild once against the right clip's momentary default
    // flags/effects, then again once ClipResized fires with the real
    // ones -- functionally harmless (both notifies are synchronous within
    // this call, so nothing outside ever observes the intermediate state),
    // but two rebuilds for what is, from the caller's side, one edit.
    notify(BatchBegin{});
    ClipId rightId = insertClip(left.track, right.asset, right.position, right.in, right.out, reuseRightId);
    Clip &insertedRight = mutableClip(rightId);
    // The right half's effects are its own (ids are unique project-wide),
    // animated from the same point in time: keyframes are relative to the
    // clip's start, so they shift by the split offset (those now before the
    // start stay, so interpolation into it is unchanged).
    const bool reuse = rightEffectIds && rightEffectIds->size() == right.effects.size();
    if (rightEffectIds && !reuse)
        rightEffectIds->clear();
    for (size_t i = 0; i < right.effects.size(); ++i) {
        Effect &copy = right.effects[i];
        copy.id = reuse ? (*rightEffectIds)[i] : EffectId{allocateId()};
        reserveId(copy.id.value);
        if (rightEffectIds && !reuse)
            rightEffectIds->push_back(copy.id);
        shiftEffectKeyframes(copy, -offsetIntoClip);
    }
    insertedRight.effects = right.effects;
    insertedRight.audioEnabled = right.audioEnabled;
    insertedRight.videoEnabled = right.videoEnabled;
    insertedRight.speed = right.speed;
    insertedRight.name = right.name;
    insertedRight.fadeOut = right.fadeOut;
    // A generated clip's source (titles) and its placement (ADR-018) are the
    // same on both halves; transform keyframes shift like effects' do.
    insertedRight.sourceParams = right.sourceParams;
    Transform placed = right.transform.get();
    for (KeyframedValue *value : {&placed.x, &placed.y, &placed.width, &placed.height, &placed.rotation,
                                  &placed.cropLeft, &placed.cropTop, &placed.cropRight, &placed.cropBottom})
        for (Keyframe &keyframe : value->keyframes)
            keyframe.at -= offsetIntoClip;
    insertedRight.transform.set(std::move(placed));

    notify(ClipResized{id});
    notify(BatchEnd{});
    return rightId;
}

void Model::setClipEnabled(ClipId id, bool videoEnabled, bool audioEnabled)
{
    Clip &target = mutableClip(id);
    target.videoEnabled = videoEnabled;
    target.audioEnabled = audioEnabled;
    notify(ClipFlagsChanged{id});
}

void Model::setClipFadeOut(ClipId id, std::optional<FadeSpec> fadeOut)
{
    Clip &target = mutableClip(id);
    target.fadeOut = fadeOut;
    notify(ClipFlagsChanged{id});
}

void Model::setClipName(ClipId id, std::string name)
{
    Clip &target = mutableClip(id);
    target.name = std::move(name);
    notify(ClipRenamed{id});
}

void Model::restoreClip(Clip clipToRestore)
{
    reserveId(clipToRestore.id.value);
    ClipId id = clipToRestore.id;
    TrackId trackId = clipToRestore.track;

    activeSequence().clips.emplace(id, std::move(clipToRestore));

    placeClip(mutableTrack(trackId), id);

    notify(ClipInserted{id});
}

void Model::restoreTrack(Track trackToRestore, size_t index)
{
    reserveId(trackToRestore.id.value);
    TrackId id = trackToRestore.id;

    auto &tracks = activeSequence().tracks;
    size_t clampedIndex = std::min(index, tracks.size());
    tracks.insert(tracks.begin() + static_cast<std::ptrdiff_t>(clampedIndex), std::move(trackToRestore));

    notify(TrackAdded{id});
}

// --- Transition mutators -----------------------------------------------------

TransitionId Model::addTransition(TrackId trackId, ClipId a, ClipId b, FrameIndex extendA, FrameIndex extendB,
                                  std::optional<TransitionId> reuseId)
{
    TransitionId id = reuseId.value_or(TransitionId{allocateId()});
    reserveId(id.value);

    notify(BatchBegin{});
    // `a`'s position never moves (only its tail extends); `b`'s in/position
    // both pull back by extendB, using its own existing head handle, so it
    // stays exactly where its now-overlapping content already put it --
    // see the Transition comment in types.h for why nothing after `b`
    // needs to move. AddTransition (core/commands) has already validated
    // handle availability, adjacency, and the resulting overlap width
    // against std::min(each clip's post-extension length) before calling
    // this; it asserts, it doesn't refuse.
    if (extendA > 0) {
        mutableClip(a).out += extendA;
        notify(ClipResized{a});
    }
    if (extendB > 0) {
        Clip &clipB = mutableClip(b);
        clipB.in -= extendB;
        clipB.position -= extendB;
        notify(ClipResized{b});
    }
    sortTrackClips(mutableTrack(trackId));

    Transition newTransition;
    newTransition.id = id;
    newTransition.track = trackId;
    newTransition.a = a;
    newTransition.b = b;
    newTransition.extendA = extendA;
    newTransition.extendB = extendB;
    newTransition.length = extendA + extendB;
    // Kept sorted by id (creation order), so undoing a removal puts the
    // transition back where it was: Sequence compares this vector in order,
    // and "revert restores the model exactly" (doc 04) must hold for it too.
    auto &transitions = activeSequence().transitions;
    transitions.insert(std::upper_bound(transitions.begin(), transitions.end(), newTransition,
                                        [](const Transition &x, const Transition &y) { return x.id < y.id; }),
                       newTransition);

    notify(TransitionAdded{id});
    notify(BatchEnd{});
    return id;
}

void Model::removeTransition(TransitionId id)
{
    // Copy out (not a reference): mutableClip() below touches
    // activeSequence().clips, never .transitions, so a reference into the
    // transitions vector would stay valid too, but copying the handful of
    // scalars up front makes that not something a future edit here has to
    // reason about.
    const Transition captured = transition(id);

    notify(BatchBegin{});
    // Exact inverse of addTransition's clip mutation above.
    if (captured.extendA > 0) {
        mutableClip(captured.a).out -= captured.extendA;
        notify(ClipResized{captured.a});
    }
    if (captured.extendB > 0) {
        Clip &clipB = mutableClip(captured.b);
        clipB.in += captured.extendB;
        clipB.position += captured.extendB;
        notify(ClipResized{captured.b});
    }
    sortTrackClips(mutableTrack(captured.track));

    auto &transitions = activeSequence().transitions;
    transitions.erase(std::remove_if(transitions.begin(), transitions.end(),
                                     [id](const Transition &entry) { return entry.id == id; }),
                      transitions.end());

    notify(TransitionRemoved{id, captured.track});
    notify(BatchEnd{});
}

void Model::retargetTransitionClip(TransitionId id, ClipId oldClip, ClipId newClip)
{
    auto &transitions = activeSequence().transitions;
    auto it = std::find_if(transitions.begin(), transitions.end(), [id](const Transition &t) { return t.id == id; });
    if (it == transitions.end()) {
        preconditionFailed("Model::retargetTransitionClip: unknown TransitionId");
        return;
    }
    if (it->a == oldClip)
        it->a = newClip;
    else if (it->b == oldClip)
        it->b = newClip;
    else {
        preconditionFailed("Model::retargetTransitionClip: oldClip is not referenced by this transition");
        return;
    }
    notify(TransitionChanged{id});
}

// --- Effects, adjustment blocks, looks (IP1) --------------------------------

std::vector<Effect> *Model::mutableEffects(EffectTarget target)
{
    Sequence &seq = activeSequence();
    switch (target.kind) {
    case EffectTarget::Kind::Clip:
        if (auto it = seq.clips.find(ClipId{target.id}); it != seq.clips.end())
            return &it->second.effects;
        return nullptr;
    case EffectTarget::Kind::Track:
        for (Track &track : seq.tracks)
            if (track.id.value == target.id)
                return &track.effects;
        return nullptr;
    case EffectTarget::Kind::Sequence:
        return &seq.effects;
    case EffectTarget::Kind::AdjustmentBlock:
        if (AdjustmentBlock *block = mutableAdjustmentBlock(AdjustmentBlockId{target.id}))
            return &block->effects;
        return nullptr;
    }
    return nullptr;
}

bool Model::hasEffectTarget(EffectTarget target) const
{
    return const_cast<Model *>(this)->mutableEffects(target) != nullptr;
}

const std::vector<Effect> &Model::effects(EffectTarget target) const
{
    if (const std::vector<Effect> *list = const_cast<Model *>(this)->mutableEffects(target))
        return *list;
    preconditionFailed("Model::effects: unknown effect target");
    static const std::vector<Effect> kNone;
    return kNone;
}

std::optional<std::pair<Model::EffectTarget, size_t>> Model::findEffect(EffectId id) const
{
    const Sequence &seq = activeSequence();
    auto search = [id](const std::vector<Effect> &list) -> std::optional<size_t> {
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].id == id)
                return i;
        return std::nullopt;
    };
    for (const auto &[clipId, clip] : seq.clips)
        if (auto index = search(clip.effects))
            return std::make_pair(EffectTarget::clip(clipId), *index);
    for (const Track &track : seq.tracks)
        if (auto index = search(track.effects))
            return std::make_pair(EffectTarget::track(track.id), *index);
    if (auto index = search(seq.effects))
        return std::make_pair(EffectTarget::sequence(), *index);
    for (const AdjustmentBlock &block : seq.adjustmentBlocks)
        if (auto index = search(block.effects))
            return std::make_pair(EffectTarget::adjustmentBlock(block.id), *index);
    return std::nullopt;
}

Effect *Model::mutableEffect(EffectId id)
{
    auto found = findEffect(id);
    if (!found)
        return nullptr;
    return &(*mutableEffects(found->first))[found->second];
}

const Effect &Model::effect(EffectId id) const
{
    if (const Effect *found = const_cast<Model *>(this)->mutableEffect(id))
        return *found;
    preconditionFailed("Model::effect: unknown EffectId");
    static const Effect kNone;
    return kNone;
}

EffectId Model::addEffect(EffectTarget target, Effect effect, size_t index, std::optional<EffectId> reuseId)
{
    std::vector<Effect> *list = mutableEffects(target);
    if (!list) {
        preconditionFailed("Model::addEffect: unknown effect target");
        return EffectId{};
    }
    effect.id = reuseId.value_or(EffectId{allocateId()});
    reserveId(effect.id.value);
    const EffectId id = effect.id;
    list->insert(list->begin() + static_cast<std::ptrdiff_t>(std::min(index, list->size())), std::move(effect));
    notify(EffectChanged{id});
    return id;
}

Effect Model::removeEffect(EffectId id)
{
    auto found = findEffect(id);
    if (!found) {
        preconditionFailed("Model::removeEffect: unknown EffectId");
        return {};
    }
    std::vector<Effect> &list = *mutableEffects(found->first);
    Effect removed = std::move(list[found->second]);
    list.erase(list.begin() + static_cast<std::ptrdiff_t>(found->second));
    notify(EffectChanged{id});
    return removed;
}

void Model::moveEffect(EffectId id, size_t newIndex)
{
    auto found = findEffect(id);
    if (!found) {
        preconditionFailed("Model::moveEffect: unknown EffectId");
        return;
    }
    std::vector<Effect> &list = *mutableEffects(found->first);
    Effect moved = std::move(list[found->second]);
    list.erase(list.begin() + static_cast<std::ptrdiff_t>(found->second));
    list.insert(list.begin() + static_cast<std::ptrdiff_t>(std::min(newIndex, list.size())), std::move(moved));
    notify(EffectChanged{id});
}

void Model::setEffectEnabled(EffectId id, bool enabled)
{
    Effect *target = mutableEffect(id);
    if (!target) {
        preconditionFailed("Model::setEffectEnabled: unknown EffectId");
        return;
    }
    target->enabled = enabled;
    notify(EffectChanged{id});
}

void Model::setEffectParam(EffectId id, Param param)
{
    Effect *target = mutableEffect(id);
    if (!target) {
        preconditionFailed("Model::setEffectParam: unknown EffectId");
        return;
    }
    const std::string name = param.name;
    auto it = std::find_if(target->params.begin(), target->params.end(),
                           [&](const Param &existing) { return existing.name == name; });
    if (it != target->params.end())
        *it = std::move(param);
    else
        target->params.push_back(std::move(param));
    notify(EffectParamChanged{id, name});
}

void Model::setEffectMix(EffectId id, KeyframedValue mix)
{
    Effect *target = mutableEffect(id);
    if (!target) {
        preconditionFailed("Model::setEffectMix: unknown EffectId");
        return;
    }
    target->mix = std::move(mix);
    notify(EffectParamChanged{id, "mix"});
}

void Model::setEffectMask(EffectId id, std::optional<EffectMask> mask)
{
    Effect *target = mutableEffect(id);
    if (!target) {
        preconditionFailed("Model::setEffectMask: unknown EffectId");
        return;
    }
    target->mask = std::move(mask);
    notify(EffectChanged{id});
}

AdjustmentBlock *Model::mutableAdjustmentBlock(AdjustmentBlockId id)
{
    for (AdjustmentBlock &block : activeSequence().adjustmentBlocks)
        if (block.id == id)
            return &block;
    return nullptr;
}

bool Model::hasAdjustmentBlock(AdjustmentBlockId id) const
{
    return const_cast<Model *>(this)->mutableAdjustmentBlock(id) != nullptr;
}

const AdjustmentBlock &Model::adjustmentBlock(AdjustmentBlockId id) const
{
    if (const AdjustmentBlock *block = const_cast<Model *>(this)->mutableAdjustmentBlock(id))
        return *block;
    preconditionFailed("Model::adjustmentBlock: unknown AdjustmentBlockId");
    static const AdjustmentBlock kNone;
    return kNone;
}

namespace {
// Blocks kept sorted by lane, then start, then id: a stable order for the
// writer and for equality after undo.
void sortAdjustmentBlocks(std::vector<AdjustmentBlock> &blocks)
{
    std::sort(blocks.begin(), blocks.end(), [](const AdjustmentBlock &a, const AdjustmentBlock &b) {
        return std::tie(a.lane, a.start, a.id.value) < std::tie(b.lane, b.start, b.id.value);
    });
}
} // namespace

AdjustmentBlockId Model::addAdjustmentBlock(AdjustmentBlock block, std::optional<AdjustmentBlockId> reuseId)
{
    block.id = reuseId.value_or(AdjustmentBlockId{allocateId()});
    reserveId(block.id.value);
    // Effects arriving without ids get them here; with ids (redo, a
    // restored block) they're kept.
    for (Effect &effect : block.effects) {
        if (!effect.id.isValid())
            effect.id = EffectId{allocateId()};
        reserveId(effect.id.value);
    }
    const AdjustmentBlockId id = block.id;
    auto &blocks = activeSequence().adjustmentBlocks;
    blocks.push_back(std::move(block));
    sortAdjustmentBlocks(blocks);
    notify(AdjustmentBlockChanged{id});
    return id;
}

AdjustmentBlock Model::removeAdjustmentBlock(AdjustmentBlockId id)
{
    auto &blocks = activeSequence().adjustmentBlocks;
    auto it = std::find_if(blocks.begin(), blocks.end(), [id](const AdjustmentBlock &b) { return b.id == id; });
    if (it == blocks.end()) {
        preconditionFailed("Model::removeAdjustmentBlock: unknown AdjustmentBlockId");
        return {};
    }
    AdjustmentBlock removed = std::move(*it);
    blocks.erase(it);
    notify(AdjustmentBlockChanged{id});
    return removed;
}

void Model::setAdjustmentBlockRange(AdjustmentBlockId id, int lane, FrameIndex start, FrameIndex length)
{
    AdjustmentBlock *block = mutableAdjustmentBlock(id);
    if (!block) {
        preconditionFailed("Model::setAdjustmentBlockRange: unknown AdjustmentBlockId");
        return;
    }
    block->lane = lane;
    block->start = start;
    block->length = length;
    sortAdjustmentBlocks(activeSequence().adjustmentBlocks);
    notify(AdjustmentBlockChanged{id});
}

void Model::setAdjustmentBlockFades(AdjustmentBlockId id, std::optional<FadeSpec> fadeIn,
                                    std::optional<FadeSpec> fadeOut)
{
    AdjustmentBlock *block = mutableAdjustmentBlock(id);
    if (!block) {
        preconditionFailed("Model::setAdjustmentBlockFades: unknown AdjustmentBlockId");
        return;
    }
    block->fadeIn = fadeIn;
    block->fadeOut = fadeOut;
    notify(AdjustmentBlockChanged{id});
}

LookId Model::addLook(Look look, std::optional<LookId> reuseId)
{
    look.id = reuseId.value_or(LookId{allocateId()});
    reserveId(look.id.value);
    // Effects arriving without ids get them here; with ids (redo, a
    // restored block) they're kept.
    for (Effect &effect : look.effects) {
        if (!effect.id.isValid())
            effect.id = EffectId{allocateId()};
        reserveId(effect.id.value);
    }
    const LookId id = look.id;
    m_project.looks.push_back(std::move(look));
    notify(LooksChanged{});
    return id;
}

Look Model::removeLook(LookId id)
{
    auto &looks = m_project.looks;
    auto it = std::find_if(looks.begin(), looks.end(), [id](const Look &l) { return l.id == id; });
    if (it == looks.end()) {
        preconditionFailed("Model::removeLook: unknown LookId");
        return {};
    }
    Look removed = std::move(*it);
    looks.erase(it);
    notify(LooksChanged{});
    return removed;
}

bool Model::hasLook(LookId id) const
{
    return std::any_of(m_project.looks.begin(), m_project.looks.end(), [id](const Look &l) { return l.id == id; });
}

void Model::setClipSourceParams(ClipId id, std::vector<Param> params)
{
    if (!hasClip(id)) {
        preconditionFailed("Model::setClipSourceParams: unknown ClipId");
        return;
    }
    mutableClip(id).sourceParams = std::move(params);
    notify(ClipSourceChanged{id});
}

void Model::setClipSource(ClipId id, AssetId asset, std::vector<Param> params)
{
    if (!hasClip(id) || !hasAsset(asset)) {
        preconditionFailed("Model::setClipSource: unknown ClipId or AssetId");
        return;
    }
    Clip &clip = mutableClip(id);
    clip.asset = asset;
    clip.sourceParams = std::move(params);
    notify(ClipSourceChanged{id});
}

void Model::setClipTransform(ClipId id, Transform transform)
{
    if (!hasClip(id)) {
        preconditionFailed("Model::setClipTransform: unknown ClipId");
        return;
    }
    mutableClip(id).transform.set(std::move(transform));
    notify(ClipTransformChanged{id});
}

void Model::setTransitionRecipe(TransitionId id, std::string recipe, std::vector<Param> params)
{
    auto &transitions = activeSequence().transitions;
    auto it = std::find_if(transitions.begin(), transitions.end(), [id](const Transition &t) { return t.id == id; });
    if (it == transitions.end()) {
        preconditionFailed("Model::setTransitionRecipe: unknown TransitionId");
        return;
    }
    it->recipe = std::move(recipe);
    it->params = std::move(params);
    notify(TransitionChanged{id});
}

// --- Invariants (doc 03) -----------------------------------------------------

std::vector<std::string> Model::check() const
{
    std::vector<std::string> problems;
    const Sequence &seq = activeSequence();

    // A zero or negative fps reaches Mlt::Profile::set_frame_rate() and
    // every fps-based FrameIndex<->time conversion in the app (audit C3);
    // the loader already refuses this up front for a freshly-opened file,
    // but check() is meant to be the one place every caller (including a
    // future non-file source of a Project) can trust to catch it.
    if (seq.profile.fps.num <= 0 || seq.profile.fps.den <= 0)
        problems.push_back("sequence profile fps is not a valid positive ratio");

    for (const auto &trackEntry : seq.tracks) {
        FrameIndex previousEnd = -1;
        ClipId previousClipId; // invalid (id 0) until the first clip is seen
        for (ClipId clipId : trackEntry.clips) {
            auto it = seq.clips.find(clipId);
            if (it == seq.clips.end()) {
                problems.push_back("track " + std::to_string(trackEntry.id.value) + " references missing clip " +
                                   std::to_string(clipId.value));
                continue;
            }
            const Clip &clipEntry = it->second;
            if (clipEntry.track != trackEntry.id) {
                problems.push_back("clip " + std::to_string(clipEntry.id.value) +
                                   " has track=" + std::to_string(clipEntry.track.value) +
                                   " but is listed under track " + std::to_string(trackEntry.id.value)); // invariant 1
            }
            if (clipEntry.position < previousEnd) {
                // Overlap is allowed exactly where a Transition covers it
                // (AddTransition/types.h's Transition comment): the two
                // clips involved must be this exact adjacent pair, and the
                // overlap width must match the transition's recorded
                // length precisely, not just fit within it.
                FrameIndex overlap = previousEnd - clipEntry.position;
                bool coveredByTransition =
                    std::any_of(seq.transitions.begin(), seq.transitions.end(), [&](const Transition &t) {
                        return t.track == trackEntry.id && t.a == previousClipId && t.b == clipEntry.id &&
                               t.length == overlap;
                    });
                if (!coveredByTransition) {
                    problems.push_back("clip " + std::to_string(clipEntry.id.value) +
                                       " overlaps the previous clip on track " +
                                       std::to_string(trackEntry.id.value)); // invariant 2
                }
            }
            previousEnd = clipEntry.end();
            previousClipId = clipEntry.id;

            if (clipEntry.position < 0) {
                problems.push_back("clip " + std::to_string(clipEntry.id.value) +
                                   " has negative position"); // invariant 4
            }

            if (!hasAsset(clipEntry.asset)) {
                problems.push_back("clip " + std::to_string(clipEntry.id.value) + " references missing asset " +
                                   std::to_string(clipEntry.asset.value)); // invariant 5
            } else if (clipEntry.in < 0 || clipEntry.in > clipEntry.out) {
                // Checked unconditionally, not just for bounded assets
                // (audit C1): MLT clamps a negative cut `in` to 0 rather
                // than rejecting it, so a boundless (still image/generator)
                // asset is just as able to carry an invalid span here as a
                // bounded one.
                problems.push_back("clip " + std::to_string(clipEntry.id.value) +
                                   " has an invalid in/out range"); // invariant 3
            } else {
                const Asset &sourceAsset = asset(clipEntry.asset);
                if (!sourceAsset.info.isBoundless() && clipEntry.out >= sourceAsset.info.lengthInSequenceFrames) {
                    problems.push_back("clip " + std::to_string(clipEntry.id.value) +
                                       " has an out-of-range source span"); // invariant 3
                }
            }

            if (trackEntry.kind == Track::Kind::Audio && clipEntry.videoEnabled) {
                problems.push_back("clip " + std::to_string(clipEntry.id.value) + " on audio track " +
                                   std::to_string(trackEntry.id.value) + " has videoEnabled=true"); // invariant 8
            }
        }
    }

    for (const auto &[clipId, clipEntry] : seq.clips) {
        if (!hasTrack(clipEntry.track)) {
            problems.push_back("clip " + std::to_string(clipId.value) + " has an unknown track " +
                               std::to_string(clipEntry.track.value));
            continue;
        }
        const Track &owningTrack = track(clipEntry.track);
        if (std::find(owningTrack.clips.begin(), owningTrack.clips.end(), clipId) == owningTrack.clips.end()) {
            problems.push_back("clip " + std::to_string(clipId.value) + " is not listed under its own track " +
                               std::to_string(clipEntry.track.value)); // invariant 1
        }
    }

    for (const auto &transition : seq.transitions) {
        if (!hasClip(transition.a) || !hasClip(transition.b)) {
            problems.push_back("transition " + std::to_string(transition.id.value) +
                               " references a missing clip"); // invariant 6
            continue;
        }
        const Clip &clipA = clip(transition.a);
        const Clip &clipB = clip(transition.b);
        if (clipA.track != clipB.track || clipA.track != transition.track) {
            problems.push_back("transition " + std::to_string(transition.id.value) +
                               " spans clips on different tracks");
        }
        if (transition.length > std::min(clipA.length(), clipB.length())) {
            problems.push_back("transition " + std::to_string(transition.id.value) +
                               " is longer than the shorter clip");
        }
        if (transition.length != transition.extendA + transition.extendB) {
            problems.push_back("transition " + std::to_string(transition.id.value) +
                               " has length != extendA + extendB");
        }
        // A recipe's services come from the project file: only the allowed
        // ones (core/model/transition_native.h).
        if (std::string problem = transitionProblem(transition); !problem.empty())
            problems.push_back("transition " + std::to_string(transition.id.value) + ": " + problem);
    }

    // T2 (2026-09-22 audit): a clip can be linked on both sides at once
    // (the middle of an A-dissolve-B-dissolve-C chain) -- their combined
    // length must still fit within the clip's own resulting length, or
    // EngineSync::planTrackSegments's segStart (position + incoming
    // transition's length) can land past segEnd (end() - outgoing
    // transition's length), and the track lays out wrong (a later entry
    // silently shifts) instead of just losing a dissolve.
    for (const auto &[clipId, clipEntry] : seq.clips) {
        FrameIndex linkedLength = 0;
        for (const auto &transition : seq.transitions) {
            if (transition.a == clipId || transition.b == clipId)
                linkedLength += transition.length;
        }
        if (linkedLength > clipEntry.length()) {
            problems.push_back("clip " + std::to_string(clipId.value) +
                               " has combined transition overlap longer than its own length");
        }
    }

    // IP1 (doc 15): effect ids unique project-wide and allocated; keyframes
    // sorted, strictly, by position (they may lie past the owner's end
    // after a trim, or before its start after a split, so trimming back
    // restores the animation); mix within 0-1; adjustment blocks valid and
    // not overlapping on their lane; look ids allocated.
    {
        std::unordered_map<uint64_t, int> effectIds;
        auto keyframesSorted = [](const std::vector<Keyframe> &keyframes) {
            for (size_t i = 1; i < keyframes.size(); ++i)
                if (keyframes[i].at <= keyframes[i - 1].at)
                    return false;
            return true;
        };
        auto checkEffects = [&](const std::vector<Effect> &list, const std::string &where) {
            for (const Effect &effect : list) {
                const std::string name = "effect " + std::to_string(effect.id.value) + " on " + where;
                if (!effect.id.isValid() || effect.id.value >= m_project.nextId)
                    problems.push_back(name + " has an id not below nextId");
                if (++effectIds[effect.id.value] == 2)
                    problems.push_back(name + " shares its id with another effect");
                bool sorted = keyframesSorted(effect.mix.keyframes);
                for (const Param &param : effect.params)
                    sorted = sorted && keyframesSorted(param.keyframes);
                if (effect.mask) {
                    sorted = sorted && keyframesSorted(effect.mask->feather.keyframes);
                    for (const Param &param : effect.mask->params)
                        sorted = sorted && keyframesSorted(param.keyframes);
                }
                if (!sorted)
                    problems.push_back(name + " has keyframes out of order");
                bool mixInRange = effect.mix.value >= 0.0 && effect.mix.value <= 1.0;
                for (const Keyframe &keyframe : effect.mix.keyframes)
                    mixInRange = mixInRange && keyframe.value >= 0.0 && keyframe.value <= 1.0;
                if (!mixInRange)
                    problems.push_back(name + " has a mix outside 0-1");
            }
        };
        for (const auto &[clipId, clipEntry] : seq.clips) {
            checkEffects(clipEntry.effects, "clip " + std::to_string(clipId.value));
            if (std::string problem = transformProblem(clipEntry.transform.get()); !problem.empty())
                problems.push_back("clip " + std::to_string(clipId.value) + ": " + problem);
            for (const Param &param : clipEntry.sourceParams)
                if (!keyframesSorted(param.keyframes))
                    problems.push_back("clip " + std::to_string(clipId.value) + "'s source parameter " + param.name +
                                       " has keyframes out of order");
        }
        for (const Track &trackEntry : seq.tracks)
            checkEffects(trackEntry.effects, "track " + std::to_string(trackEntry.id.value));
        checkEffects(seq.effects, "the sequence");
        std::unordered_map<uint64_t, int> blockIds;
        for (size_t i = 0; i < seq.adjustmentBlocks.size(); ++i) {
            const AdjustmentBlock &block = seq.adjustmentBlocks[i];
            const std::string name = "adjustment block " + std::to_string(block.id.value);
            checkEffects(block.effects, name);
            if (!block.id.isValid() || block.id.value >= m_project.nextId)
                problems.push_back(name + " has an id not below nextId");
            if (++blockIds[block.id.value] == 2)
                problems.push_back(name + " shares its id");
            if (block.lane < 0 || block.start < 0 || block.length <= 0)
                problems.push_back(name + " has a negative lane or start, or no length");
            for (size_t j = i + 1; j < seq.adjustmentBlocks.size(); ++j) {
                const AdjustmentBlock &other = seq.adjustmentBlocks[j];
                if (other.lane == block.lane && other.start < block.end() && block.start < other.end())
                    problems.push_back(name + " overlaps adjustment block " + std::to_string(other.id.value) +
                                       " on lane " + std::to_string(block.lane));
            }
        }
        for (const Look &look : m_project.looks) {
            checkEffects(look.effects, "look " + std::to_string(look.id.value));
            if (!look.id.isValid() || look.id.value >= m_project.nextId)
                problems.push_back("look " + std::to_string(look.id.value) + " has an id not below nextId");
        }
    }

    if (seq.id.value >= m_project.nextId)
        problems.push_back("sequence id " + std::to_string(seq.id.value) + " is not less than nextId"); // invariant 9
    for (const auto &trackEntry : seq.tracks) {
        if (trackEntry.id.value >= m_project.nextId)
            problems.push_back("track id " + std::to_string(trackEntry.id.value) + " is not less than nextId");
    }
    for (const auto &[clipId, clipEntry] : seq.clips) {
        (void)clipEntry;
        if (clipId.value >= m_project.nextId)
            problems.push_back("clip id " + std::to_string(clipId.value) + " is not less than nextId");
    }
    for (const auto &binAsset : m_project.bin) {
        if (binAsset.id.value >= m_project.nextId)
            problems.push_back("asset id " + std::to_string(binAsset.id.value) + " is not less than nextId");
    }
    for (const auto &transitionEntry : seq.transitions) {
        if (transitionEntry.id.value >= m_project.nextId)
            problems.push_back("transition id " + std::to_string(transitionEntry.id.value) +
                               " is not less than nextId");
    }

    return problems;
}

} // namespace ustudio::core
