#include "model.h"

#include <algorithm>
#include <cassert>

namespace ustudio::core {

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
    return *this;
}

Model::Model(Model &&other) noexcept : m_project(std::move(other.m_project)) {}

Model &Model::operator=(Model &&other) noexcept
{
    m_project = std::move(other.m_project);
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
    assert(false && "Model: no active sequence -- Project invariant violated");
    return m_project.sequences.front();
}

const Sequence &Model::activeSequence() const
{
    for (const auto &seq : m_project.sequences) {
        if (seq.id == m_project.activeSequence)
            return seq;
    }
    assert(false && "Model: no active sequence -- Project invariant violated");
    return m_project.sequences.front();
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
    assert(it != bin.end() && "Model::asset: unknown AssetId");
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
    assert(it != clips.end() && "Model::clip: unknown ClipId");
    return it->second;
}

Clip &Model::mutableClip(ClipId id)
{
    auto &clips = activeSequence().clips;
    auto it = clips.find(id);
    assert(it != clips.end() && "Model::mutableClip: unknown ClipId");
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
    assert(it != tracks.end() && "Model::track: unknown TrackId");
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
    auto it = std::find_if(transitions.begin(), transitions.end(),
                           [id](const Transition &entry) { return entry.id == id; });
    assert(it != transitions.end() && "Model::transition: unknown TransitionId");
    return *it;
}

bool Model::isRangeFree(TrackId trackId, FrameIndex start, FrameIndex end,
                        const std::vector<ClipId> &ignoreClips) const
{
    const Track &target = track(trackId);
    for (ClipId clipId : target.clips) {
        if (std::find(ignoreClips.begin(), ignoreClips.end(), clipId) != ignoreClips.end())
            continue;
        const Clip &existing = clip(clipId);
        if (start < existing.end() && end > existing.position)
            return false; // overlap
    }
    return true;
}

Track &Model::mutableTrack(TrackId id)
{
    auto &tracks = activeSequence().tracks;
    auto it = std::find_if(tracks.begin(), tracks.end(), [id](const Track &entry) { return entry.id == id; });
    assert(it != tracks.end() && "Model::mutableTrack: unknown TrackId");
    return *it;
}


void Model::sortTrackClips(Track &trackRef)
{
    const auto &clips = activeSequence().clips;
    std::sort(trackRef.clips.begin(), trackRef.clips.end(),
              [&clips](ClipId a, ClipId b) { return clips.at(a).position < clips.at(b).position; });
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
    assert(it != bin.end() && "Model::removeAsset: unknown AssetId");
    bin.erase(it);
    notify(AssetChanged{id});
}

void Model::extendAssetLength(AssetId id, FrameIndex minimumLength)
{
    auto &bin = m_project.bin;
    auto it = std::find_if(bin.begin(), bin.end(), [id](const Asset &entry) { return entry.id == id; });
    assert(it != bin.end() && "Model::extendAssetLength: unknown AssetId");
    if (minimumLength <= it->info.lengthInSequenceFrames)
        return;
    it->info.lengthInSequenceFrames = minimumLength;
    notify(AssetChanged{id});
}

void Model::setAssetLength(AssetId id, FrameIndex length)
{
    auto &bin = m_project.bin;
    auto it = std::find_if(bin.begin(), bin.end(), [id](const Asset &entry) { return entry.id == id; });
    assert(it != bin.end() && "Model::setAssetLength: unknown AssetId");
    if (length == it->info.lengthInSequenceFrames)
        return;
    it->info.lengthInSequenceFrames = length;
    notify(AssetChanged{id});
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
    assert(it != tracks.end() && "Model::removeTrack: unknown TrackId");

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
    assert(it != tracks.end() && "Model::moveTrack: unknown TrackId");

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

    Track &target = mutableTrack(trackId);
    target.clips.push_back(id);
    sortTrackClips(target);

    notify(ClipInserted{id});
    return id;
}

void Model::removeClip(ClipId id)
{
    Clip &target = mutableClip(id);
    TrackId trackId = target.track;

    Track &owningTrack = mutableTrack(trackId);
    owningTrack.clips.erase(std::remove(owningTrack.clips.begin(), owningTrack.clips.end(), id),
                            owningTrack.clips.end());

    activeSequence().clips.erase(id);
    notify(ClipRemoved{id, trackId});
}

void Model::moveClip(ClipId id, TrackId newTrackId, FrameIndex pos)
{
    Clip &target = mutableClip(id);
    TrackId oldTrackId = target.track;

    if (oldTrackId != newTrackId) {
        Track &oldTrack = mutableTrack(oldTrackId);
        oldTrack.clips.erase(std::remove(oldTrack.clips.begin(), oldTrack.clips.end(), id), oldTrack.clips.end());

        target.track = newTrackId;
        target.position = pos;

        Track &newTrack = mutableTrack(newTrackId);
        newTrack.clips.push_back(id);
        sortTrackClips(newTrack);
    } else {
        target.position = pos;
        sortTrackClips(mutableTrack(oldTrackId));
    }

    notify(ClipMoved{id, oldTrackId, newTrackId});
}

void Model::resizeClip(ClipId id, FrameIndex newIn, FrameIndex newOut, FrameIndex newPos)
{
    Clip &target = mutableClip(id);
    target.in = newIn;
    target.out = newOut;
    target.position = newPos;
    sortTrackClips(mutableTrack(target.track));
    notify(ClipResized{id});
}

ClipId Model::splitClip(ClipId id, FrameIndex at, std::optional<ClipId> reuseRightId)
{
    Clip &left = mutableClip(id);
    assert(at > left.position && at < left.end() && "Model::splitClip: split point must be strictly inside the clip");

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
    insertedRight.effects = right.effects;
    insertedRight.audioEnabled = right.audioEnabled;
    insertedRight.videoEnabled = right.videoEnabled;
    insertedRight.speed = right.speed;
    insertedRight.name = right.name;
    insertedRight.fadeOut = right.fadeOut;

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

    Track &target = mutableTrack(trackId);
    target.clips.push_back(id);
    sortTrackClips(target);

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
    activeSequence().transitions.push_back(newTransition);

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
    assert(it != transitions.end() && "Model::retargetTransitionClip: unknown TransitionId");
    if (it->a == oldClip)
        it->a = newClip;
    else if (it->b == oldClip)
        it->b = newClip;
    else
        assert(false && "Model::retargetTransitionClip: oldClip is not referenced by this transition");
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
