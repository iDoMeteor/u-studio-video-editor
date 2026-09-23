#include "primitives.h"

#include <algorithm>

namespace ustudio::core {

namespace {

// Non-mutating: how much a transition (if any) is currently adding to
// `clip`'s on-model length, without actually touching anything -- used
// to validate a move/resize against the clip's post-strip length before
// committing to stripping it (T1: a command must not partially mutate
// the model and then refuse).
FrameIndex transitionExtensionOf(const Model &model, ClipId clip)
{
    FrameIndex extension = 0;
    for (const Transition &t : model.sequence().transitions) {
        if (t.a == clip)
            extension += t.extendA;
        if (t.b == clip)
            extension += t.extendB;
    }
    return extension;
}

// Removes (via Model::removeTransition, which un-extends both linked
// clips first) every transition where `clip` is either `a` or `b`,
// returning the removed records so the caller's revert() can restore
// them via restoreTransitions() below -- the same apply/revert pairing
// RemoveTransition itself already relies on for a single pair. A clip
// can be linked on both sides at once (the middle clip of an A-dissolve-
// B-dissolve-C chain), so this can return up to two entries.
std::vector<Transition> stripTransitionsInvolvingClip(Model &model, ClipId clip)
{
    std::vector<Transition> removed;
    for (const Transition &t : model.sequence().transitions) {
        if (t.a == clip || t.b == clip)
            removed.push_back(t);
    }
    for (const Transition &t : removed)
        model.removeTransition(t.id);
    return removed;
}

void restoreTransitions(Model &model, const std::vector<Transition> &transitions)
{
    for (const Transition &t : transitions)
        model.addTransition(t.track, t.a, t.b, t.extendA, t.extendB, t.id);
}

} // namespace

// --- AddAsset ---------------------------------------------------------------

AddAsset::AddAsset(Asset asset) : m_asset(std::move(asset)) {}

bool AddAsset::apply(Model &model)
{
    m_assetId = model.addAsset(m_asset, m_appliedBefore ? std::optional<AssetId>(m_assetId) : std::nullopt);
    m_appliedBefore = true;
    return true;
}

void AddAsset::revert(Model &model)
{
    model.removeAsset(m_assetId);
}

RemoveAsset::RemoveAsset(AssetId asset) : m_asset(asset) {}

bool RemoveAsset::apply(Model &model)
{
    if (!model.hasAsset(m_asset))
        return false;

    // Collect first, mutate second: removing a clip while iterating
    // Sequence::clips (an unordered_map) would invalidate the iterator
    // RemoveAsset itself is walking.
    m_capturedClips.clear();
    for (const auto &[clipId, clip] : model.sequence().clips) {
        if (clip.asset != m_asset)
            continue;
        if (model.track(clip.track).locked)
            return false; // refuse the whole removal, not a partial one
        m_capturedClips.push_back(clip);
    }

    m_capturedAsset = model.asset(m_asset);
    // Batched: each removeClip below and the final removeAsset would
    // otherwise each trigger their own EngineSync rebuild/consumer
    // restart -- an asset used by several clips could restart the real
    // audio device many times in a row, which a T1 investigation
    // (2026-09-23) confirmed crashes deep in PipeWire/SDL3's own
    // teardown when done rapidly and unbatched.
    model.notify(BatchBegin{});
    for (const Clip &clip : m_capturedClips)
        model.removeClip(clip.id);
    model.removeAsset(m_asset);
    model.notify(BatchEnd{});
    return true;
}

void RemoveAsset::revert(Model &model)
{
    model.notify(BatchBegin{});
    model.addAsset(m_capturedAsset, m_asset);
    for (const Clip &clip : m_capturedClips)
        model.restoreClip(clip);
    model.notify(BatchEnd{});
}

// --- AddTrack / RemoveTrack / SetTrackFlags ---------------------------------

AddTrack::AddTrack(Track::Kind kind, size_t index, std::string name)
    : m_kind(kind), m_index(index), m_name(std::move(name))
{}

bool AddTrack::apply(Model &model)
{
    m_trackId =
        model.addTrack(m_kind, m_index, m_name, m_appliedBefore ? std::optional<TrackId>(m_trackId) : std::nullopt);
    m_appliedBefore = true;
    return true;
}

void AddTrack::revert(Model &model)
{
    model.removeTrack(m_trackId);
}

RemoveTrack::RemoveTrack(TrackId track) : m_track(track) {}

bool RemoveTrack::apply(Model &model)
{
    if (!model.hasTrack(m_track))
        return false;
    // A sequence with zero tracks has nowhere for a future import/insert to
    // land, and v1 refused this for the same reason; the model itself
    // doesn't enforce a minimum (doc 03: it asserts on programmer error and
    // otherwise trusts the caller), so it's the command's job.
    if (model.sequence().tracks.size() <= 1)
        return false;
    if (model.track(m_track).locked)
        return false;

    const Sequence &seq = model.sequence();
    const auto &tracks = seq.tracks;
    auto it = std::find_if(tracks.begin(), tracks.end(), [this](const Track &t) { return t.id == m_track; });
    m_capturedIndex = static_cast<size_t>(std::distance(tracks.begin(), it));

    // T1: strip this track's own transitions BEFORE capturing its clips,
    // so the captured geometry is the un-extended base (restoreClip +
    // addTransition on revert, not restoreClip-already-extended +
    // addTransition-extends-again -- see primitives.h's own comment). A
    // transition's clips are always on the transition's own track
    // (AddTransition::apply enforces it), so this can never touch a clip
    // on a different, still-live track. Batched (every removeTransition
    // here, plus the removeTrack below) so a track carrying several
    // dissolves doesn't restart the real audio consumer once per
    // transition -- confirmed via coredumpctl that rapid, unbatched
    // restarts crash deep in PipeWire/SDL3, 2026-09-23.
    model.notify(BatchBegin{});
    m_capturedTransitions.clear();
    for (const Transition &t : seq.transitions) {
        if (t.track == m_track)
            m_capturedTransitions.push_back(t);
    }
    for (const Transition &t : m_capturedTransitions)
        model.removeTransition(t.id);

    m_capturedClips.clear();
    for (ClipId clipId : it->clips)
        m_capturedClips.push_back(model.clip(clipId));

    m_capturedTrack = *it;
    m_capturedTrack.clips.clear(); // restoreClip() repopulates this on revert

    model.removeTrack(m_track);
    model.notify(BatchEnd{});
    return true;
}

void RemoveTrack::revert(Model &model)
{
    model.notify(BatchBegin{});
    model.restoreTrack(m_capturedTrack, m_capturedIndex);
    for (const Clip &clip : m_capturedClips)
        model.restoreClip(clip);
    restoreTransitions(model, m_capturedTransitions);
    model.notify(BatchEnd{});
}

MoveTrack::MoveTrack(TrackId track, size_t newIndex) : m_track(track), m_newIndex(newIndex) {}

bool MoveTrack::apply(Model &model)
{
    if (!model.hasTrack(m_track))
        return false;

    const auto &tracks = model.sequence().tracks;
    auto it = std::find_if(tracks.begin(), tracks.end(), [this](const Track &t) { return t.id == m_track; });
    m_oldIndex = static_cast<size_t>(std::distance(tracks.begin(), it));

    model.moveTrack(m_track, m_newIndex);
    return true;
}

void MoveTrack::revert(Model &model)
{
    model.moveTrack(m_track, m_oldIndex);
}

SetTrackFlags::SetTrackFlags(TrackId track, bool muted, bool hidden, bool locked)
    : m_track(track), m_muted(muted), m_hidden(hidden), m_locked(locked)
{}

bool SetTrackFlags::apply(Model &model)
{
    if (!model.hasTrack(m_track))
        return false;
    const Track &current = model.track(m_track);
    m_oldMuted = current.muted;
    m_oldHidden = current.hidden;
    m_oldLocked = current.locked;
    model.setTrackFlags(m_track, m_muted, m_hidden, m_locked);
    return true;
}

void SetTrackFlags::revert(Model &model)
{
    model.setTrackFlags(m_track, m_oldMuted, m_oldHidden, m_oldLocked);
}

SetTrackVolume::SetTrackVolume(TrackId track, double volume) : m_track(track), m_volume(volume) {}

bool SetTrackVolume::apply(Model &model)
{
    if (!model.hasTrack(m_track))
        return false;
    m_oldVolume = model.track(m_track).volume;
    model.setTrackVolume(m_track, m_volume);
    return true;
}

void SetTrackVolume::revert(Model &model)
{
    model.setTrackVolume(m_track, m_oldVolume);
}

bool SetTrackVolume::mergeWith(const Command &next)
{
    const auto *nextVolume = dynamic_cast<const SetTrackVolume *>(&next);
    if (!nextVolume || nextVolume->m_track != m_track)
        return false;
    m_volume = nextVolume->m_volume; // keep this command's m_oldVolume: the drag's true start
    return true;
}

RenameTrack::RenameTrack(TrackId track, std::string name) : m_track(track), m_name(std::move(name)) {}

bool RenameTrack::apply(Model &model)
{
    if (!model.hasTrack(m_track))
        return false;
    m_oldName = model.track(m_track).name;
    model.setTrackName(m_track, m_name);
    return true;
}

void RenameTrack::revert(Model &model)
{
    model.setTrackName(m_track, m_oldName);
}

// --- InsertClip / RemoveClip / MoveClip / ResizeClip / SplitClip -----------

InsertClip::InsertClip(TrackId track, AssetId asset, FrameIndex pos, FrameIndex in, FrameIndex out)
    : m_track(track), m_asset(asset), m_pos(pos), m_in(in), m_out(out)
{}

bool InsertClip::apply(Model &model)
{
    if (!model.hasTrack(m_track) || !model.hasAsset(m_asset))
        return false;
    if (model.track(m_track).locked)
        return false;
    // m_in < 0 is rejected unconditionally, not just for bounded assets: MLT
    // clamps a negative cut `in` to 0 rather than rejecting it, so letting one
    // through here would silently shift where playback starts inside the
    // source and desync every clip laid out after it on the track.
    if (m_in < 0 || m_in > m_out || m_pos < 0)
        return false;
    if (!model.isRangeFree(m_track, m_pos, m_pos + (m_out - m_in + 1)))
        return false;

    const Asset &sourceAsset = model.asset(m_asset);
    bool boundless = sourceAsset.info.isBoundless();
    if (!boundless && m_out >= sourceAsset.info.lengthInSequenceFrames)
        return false;

    bool destIsAudio = model.track(m_track).kind == Track::Kind::Audio;

    // Two-to-three model mutations here (the clip, maybe the asset's
    // recorded length, maybe the video-disable below) -- wrap in a batch so
    // EngineSync coalesces them into one rebuild instead of several, same
    // as CompositeCommand.
    model.notify(BatchBegin{});
    m_clipId = model.insertClip(m_track, m_asset, m_pos, m_in, m_out,
                                m_appliedBefore ? std::optional<ClipId>(m_clipId) : std::nullopt);
    m_appliedBefore = true;
    // A still image or generator has no real fixed duration (doc 13's E3):
    // keep the asset's recorded length truthful for whatever clip has cut
    // furthest into it, so EngineSync sizes the underlying MLT producer
    // long enough and the saved project file matches what's on the timeline.
    // Audit C1: extendAssetLength() only grows, so capture whether (and
    // from what) it actually did BEFORE calling it, using sourceAsset's
    // pre-mutation value captured above -- revert() needs the exact old
    // length to restore, and whether to bother at all (another clip may
    // already have cut further into this asset, in which case this
    // apply()'s own extend was a no-op and reverting must not shrink it).
    if (boundless) {
        m_oldAssetLength = sourceAsset.info.lengthInSequenceFrames;
        m_setAssetLength = m_out + 1;
        m_extendedAsset = m_setAssetLength > m_oldAssetLength;
        model.extendAssetLength(m_asset, m_setAssetLength);
    }
    // Audit A4: a clip lands with videoEnabled=true by default (Clip's own
    // field default); an audio track showing that video through wherever
    // the video tracks above it have a gap violates Model::check()'s
    // invariant 8. Import onto an audio track isn't refused outright (see
    // MoveClip::apply() for the one case that is) -- it just never shows
    // its picture there.
    if (destIsAudio)
        model.setClipEnabled(m_clipId, /*videoEnabled=*/false, /*audioEnabled=*/true);
    model.notify(BatchEnd{});
    return true;
}

void InsertClip::revert(Model &model)
{
    model.removeClip(m_clipId);
    // Only undo the extension if nothing else has since cut even
    // further into the same asset (primitives.h's own comment).
    if (m_extendedAsset && model.hasAsset(m_asset) &&
        model.asset(m_asset).info.lengthInSequenceFrames == m_setAssetLength)
        model.setAssetLength(m_asset, m_oldAssetLength);
}

RemoveClip::RemoveClip(ClipId clip) : m_clip(clip) {}

bool RemoveClip::apply(Model &model)
{
    if (!model.hasClip(m_clip))
        return false;
    if (model.track(model.clip(m_clip).track).locked)
        return false;
    // T1: strip before capturing (see primitives.h's own comment) --
    // m_captured must hold the un-extended base geometry, not whatever a
    // transition had extended this clip to. Batched (like InsertClip/
    // ResizeClip) so EngineSync coalesces the strip's own removeTransition
    // notify and this removeClip's into one rebuild/consumer-restart
    // instead of two back to back -- two rapid restarts of the real audio
    // device were confirmed (coredumpctl + gdb, 2026-09-23) to crash deep
    // in PipeWire/SDL3's own teardown, unrelated to anything this
    // command controls, so avoiding the double restart isn't optional
    // polish here.
    model.notify(BatchBegin{});
    m_capturedTransitions = stripTransitionsInvolvingClip(model, m_clip);
    m_captured = model.clip(m_clip);
    model.removeClip(m_clip);
    model.notify(BatchEnd{});
    return true;
}

void RemoveClip::revert(Model &model)
{
    model.notify(BatchBegin{});
    model.restoreClip(m_captured);
    restoreTransitions(model, m_capturedTransitions);
    model.notify(BatchEnd{});
}

MoveClip::MoveClip(ClipId clip, TrackId newTrack, FrameIndex newPos)
    : m_clip(clip), m_newTrack(newTrack), m_newPos(newPos)
{}

bool MoveClip::apply(Model &model)
{
    if (!model.hasClip(m_clip) || !model.hasTrack(m_newTrack))
        return false;
    if (m_newPos < 0)
        return false;

    const Clip &current = model.clip(m_clip);
    // Both ends of the move must be unlocked: leaving a locked track's
    // content untouched is the whole point, and dropping something new
    // onto a locked track is just as much an edit to it as moving one of
    // its own clips would be.
    if (model.track(current.track).locked || model.track(m_newTrack).locked)
        return false;
    // T1: the move is about to break whatever adjacency a transition on
    // this clip depends on, so it will be stripped below -- validate
    // against the length it will actually have afterward (its on-model
    // length minus whatever a transition is currently extending it by),
    // not a possibly-larger extended one. transitionExtensionOf() is
    // non-mutating specifically so every check below can run, and this
    // whole apply() can still cleanly refuse, before anything is
    // stripped (a command must not partially mutate the model and then
    // return false).
    FrameIndex length = current.length() - transitionExtensionOf(model, m_clip);
    if (!model.isRangeFree(m_newTrack, m_newPos, m_newPos + length, m_clip))
        return false;

    // Audit A4: landing on an audio track with videoEnabled still true
    // violates Model::check()'s invariant 8 (its picture would show
    // through wherever the video tracks above it have a gap). Unlike
    // InsertClip, a move onto one is refused outright when the clip would
    // become entirely inaudible too (no asset audio, or its own audio
    // already off) -- there'd be nothing left for that track to
    // contribute, so "snap back to where it was" is the more useful
    // outcome than silently parking a dead clip there.
    bool destIsAudio = model.track(m_newTrack).kind == Track::Kind::Audio;
    if (destIsAudio) {
        bool hasAudioContent = current.audioEnabled && model.hasAsset(current.asset) &&
                               model.asset(current.asset).info.hasAudio;
        if (!hasAudioContent)
            return false;
    }

    // All checks passed -- now safe to actually strip (T1: capture the
    // un-extended base geometry AFTER stripping, same reasoning as
    // RemoveClip::apply). Batched for the same reason RemoveClip's own
    // apply() is -- see its comment.
    model.notify(BatchBegin{});
    m_capturedTransitions = stripTransitionsInvolvingClip(model, m_clip);
    const Clip &shrunk = model.clip(m_clip);
    m_oldTrack = shrunk.track;
    m_oldPos = shrunk.position;
    m_oldVideoEnabled = shrunk.videoEnabled;
    model.moveClip(m_clip, m_newTrack, m_newPos);
    if (destIsAudio && shrunk.videoEnabled)
        model.setClipEnabled(m_clip, /*videoEnabled=*/false, shrunk.audioEnabled);
    model.notify(BatchEnd{});
    return true;
}

void MoveClip::revert(Model &model)
{
    model.notify(BatchBegin{});
    model.moveClip(m_clip, m_oldTrack, m_oldPos);
    const Clip &restored = model.clip(m_clip);
    if (restored.videoEnabled != m_oldVideoEnabled)
        model.setClipEnabled(m_clip, m_oldVideoEnabled, restored.audioEnabled);
    restoreTransitions(model, m_capturedTransitions);
    model.notify(BatchEnd{});
}

ResizeClip::ResizeClip(ClipId clip, FrameIndex newIn, FrameIndex newOut, FrameIndex newPos)
    : m_clip(clip), m_newIn(newIn), m_newOut(newOut), m_newPos(newPos)
{}

bool ResizeClip::apply(Model &model)
{
    if (!model.hasClip(m_clip))
        return false;
    // m_newIn < 0 is rejected unconditionally -- see InsertClip::apply.
    if (m_newIn < 0 || m_newIn > m_newOut || m_newPos < 0)
        return false;

    const Clip &current = model.clip(m_clip);
    if (model.track(current.track).locked)
        return false;
    FrameIndex newLength = m_newOut - m_newIn + 1;
    if (!model.isRangeFree(current.track, m_newPos, m_newPos + newLength, m_clip))
        return false;

    bool boundless = false;
    if (model.hasAsset(current.asset)) {
        const Asset &sourceAsset = model.asset(current.asset);
        boundless = sourceAsset.info.isBoundless();
        if (!boundless && m_newOut >= sourceAsset.info.lengthInSequenceFrames)
            return false;
    }

    // T1: a resize on a transition-linked clip invalidates the dissolve
    // (its overlap assumed specific handles on both sides -- doc 08), so
    // strip it first. m_old{In,Out,Pos} are captured AFTER stripping (see
    // primitives.h's own comment) so they hold the un-extended base
    // geometry, not whatever a transition had extended this clip to --
    // isRangeFree/the boundless check above use m_newOut/m_newPos (the
    // caller's requested values, not current.length()), so they don't
    // need the strip to have already happened to be correct.
    model.notify(BatchBegin{});
    m_capturedTransitions = stripTransitionsInvolvingClip(model, m_clip);
    const Clip &shrunk = model.clip(m_clip);
    m_oldIn = shrunk.in;
    m_oldOut = shrunk.out;
    m_oldPos = shrunk.position;
    // See InsertClip::apply for why this is batched and why boundless assets
    // get their recorded length extended alongside the resize. Audit C1
    // -- see InsertClip::apply's own comment on capturing the old length
    // and whether this call actually grew it, for revert() to undo.
    model.resizeClip(m_clip, m_newIn, m_newOut, m_newPos);
    if (boundless) {
        m_oldAssetLength = model.asset(current.asset).info.lengthInSequenceFrames;
        m_setAssetLength = m_newOut + 1;
        m_extendedAsset = m_setAssetLength > m_oldAssetLength;
        model.extendAssetLength(current.asset, m_setAssetLength);
    }
    model.notify(BatchEnd{});
    return true;
}

void ResizeClip::revert(Model &model)
{
    model.notify(BatchBegin{});
    model.resizeClip(m_clip, m_oldIn, m_oldOut, m_oldPos);
    // Only undo the extension if nothing else has since cut even
    // further into the same asset (primitives.h's own comment).
    AssetId resizedAsset = model.clip(m_clip).asset;
    if (m_extendedAsset && model.hasAsset(resizedAsset) &&
        model.asset(resizedAsset).info.lengthInSequenceFrames == m_setAssetLength)
        model.setAssetLength(resizedAsset, m_oldAssetLength);
    restoreTransitions(model, m_capturedTransitions);
    model.notify(BatchEnd{});
}

SplitClip::SplitClip(ClipId clip, FrameIndex at) : m_clip(clip), m_at(at) {}

bool SplitClip::apply(Model &model)
{
    if (!model.hasClip(m_clip))
        return false;
    const Clip &current = model.clip(m_clip);
    if (model.track(current.track).locked)
        return false;
    if (!(m_at > current.position && m_at < current.end()))
        return false;

    // T1: splitting a transition-linked clip leaves an ambiguous link (is
    // the dissolve now on the left half, the right half, both, or
    // neither? -- see primitives.h's own comment for the failure modes
    // this used to produce), so strip it first, same as RemoveClip/
    // MoveClip/ResizeClip. Stripping can move the clip's position/end()
    // enough that a split point valid against the transition-extended
    // span above is no longer strictly inside the un-extended one, so
    // re-check and cleanly unwind (restore the transition, refuse) rather
    // than let Model::splitClip assert on an out-of-range `at`. Batched
    // (both the strip-and-split success path and the strip-then-restore
    // refusal path) for the same reason RemoveClip::apply's own comment
    // gives -- two rapid, unbatched consumer restarts crash deep in
    // PipeWire/SDL3, confirmed via coredumpctl, 2026-09-23.
    model.notify(BatchBegin{});
    m_capturedTransitions = stripTransitionsInvolvingClip(model, m_clip);
    const Clip &shrunk = model.clip(m_clip);
    if (!(m_at > shrunk.position && m_at < shrunk.end())) {
        restoreTransitions(model, m_capturedTransitions);
        m_capturedTransitions.clear();
        model.notify(BatchEnd{});
        return false;
    }

    m_oldOut = shrunk.out;
    m_oldFadeOut = shrunk.fadeOut;
    m_rightId = model.splitClip(m_clip, m_at, m_appliedBefore ? std::optional<ClipId>(m_rightId) : std::nullopt);
    m_appliedBefore = true;
    model.notify(BatchEnd{});
    return true;
}

void SplitClip::revert(Model &model)
{
    model.notify(BatchBegin{});
    model.removeClip(m_rightId);
    const Clip &left = model.clip(m_clip);
    model.resizeClip(m_clip, left.in, m_oldOut, left.position);
    // Model::splitClip() clears the left clip's fadeOut (the new right edge
    // becomes a hard cut) -- resizeClip() above restores the geometry but
    // doesn't know about fades, so put back whatever was captured at apply()
    // time explicitly (audit C5).
    model.setClipFadeOut(m_clip, m_oldFadeOut);
    restoreTransitions(model, m_capturedTransitions);
    model.notify(BatchEnd{});
}

// --- SplitAudio --------------------------------------------------------

SplitAudio::SplitAudio(ClipId clip) : m_clip(clip) {}

bool SplitAudio::apply(Model &model)
{
    if (!model.hasClip(m_clip))
        return false;
    const Clip &original = model.clip(m_clip);
    if (model.track(original.track).locked)
        return false;
    if (!original.audioEnabled || !original.videoEnabled)
        return false; // nothing to split, or already audio-only

    TrackId audioTrackId;
    bool found = false;
    for (const Track &track : model.sequence().tracks) {
        // A locked audio track is not a valid destination either, same as
        // one that's occupied -- keep searching past it.
        if (track.kind == Track::Kind::Audio && !track.locked &&
            model.isRangeFree(track.id, original.position, original.end())) {
            audioTrackId = track.id;
            found = true;
            break;
        }
    }
    if (!found) {
        size_t audioTrackCount = 0;
        for (const Track &track : model.sequence().tracks) {
            if (track.kind == Track::Kind::Audio)
                ++audioTrackCount;
        }
        audioTrackId = model.addTrack(Track::Kind::Audio, model.sequence().tracks.size(),
                                      "A" + std::to_string(audioTrackCount + 1));
    }

    m_audioClipId = model.insertClip(audioTrackId, original.asset, original.position, original.in, original.out,
                                     m_appliedBefore ? std::optional<ClipId>(m_audioClipId) : std::nullopt);
    model.setClipEnabled(m_audioClipId, /*videoEnabled=*/false, /*audioEnabled=*/true);
    model.setClipEnabled(m_clip, /*videoEnabled=*/true, /*audioEnabled=*/false);

    m_appliedBefore = true;
    return true;
}

void SplitAudio::revert(Model &model)
{
    model.removeClip(m_audioClipId);
    model.setClipEnabled(m_clip, /*videoEnabled=*/true, /*audioEnabled=*/true);
}

RenameClip::RenameClip(ClipId clip, std::string name) : m_clip(clip), m_name(std::move(name)) {}

bool RenameClip::apply(Model &model)
{
    if (!model.hasClip(m_clip))
        return false;
    m_oldName = model.clip(m_clip).name;
    model.setClipName(m_clip, m_name);
    return true;
}

void RenameClip::revert(Model &model)
{
    model.setClipName(m_clip, m_oldName);
}

AddTransition::AddTransition(TrackId track, ClipId a, ClipId b, FrameIndex extendA, FrameIndex extendB)
    : m_track(track), m_a(a), m_b(b), m_extendA(extendA), m_extendB(extendB)
{}

bool AddTransition::apply(Model &model)
{
    if (!model.hasClip(m_a) || !model.hasClip(m_b))
        return false;
    if (!model.hasTrack(m_track) || model.track(m_track).locked)
        return false;
    if (m_extendA < 0 || m_extendB < 0 || (m_extendA == 0 && m_extendB == 0))
        return false;

    const Clip &clipA = model.clip(m_a);
    const Clip &clipB = model.clip(m_b);
    if (clipA.track != m_track || clipB.track != m_track)
        return false;
    // a must be immediately before b -- exactly touching, no gap and no
    // existing overlap (doc 08: "a ends where b starts").
    if (clipA.end() != clipB.position)
        return false;

    if (m_extendA > 0) {
        if (!model.hasAsset(clipA.asset))
            return false;
        const Asset &assetA = model.asset(clipA.asset);
        if (!assetA.info.isBoundless() && clipA.out + m_extendA >= assetA.info.lengthInSequenceFrames)
            return false;
    }
    if (m_extendB > 0 && clipB.in - m_extendB < 0)
        return false;

    // Same bound Model::check() enforces on the result; checking it here
    // too, before mutating, also keeps b's new position from crossing
    // before a's start (and vice versa) -- see the class comment.
    FrameIndex newLengthA = clipA.length() + m_extendA;
    FrameIndex newLengthB = clipB.length() + m_extendB;
    if (m_extendA + m_extendB > std::min(newLengthA, newLengthB))
        return false;

    // T2 (2026-09-22 audit): the check above only looks at THIS
    // transition against each clip's own length -- it says nothing about
    // a clip that's already linked on its OTHER side (the middle of an
    // A-dissolve-B-dissolve-C chain). Two dissolves that each pass in
    // isolation can still combine to exceed the shared middle clip's own
    // length, which pushes EngineSync::planTrackSegments's segStart past
    // segEnd for that clip and lays the rest of the track out wrong
    // (Model::check()'s own mirror of this rule, added alongside this).
    // `a` matters here for its INCOMING side (some other transition where
    // `a` is `b`); `b` matters for its OUTGOING side (some other
    // transition where `b` is `a`) -- the new transition itself is
    // `a`'s outgoing / `b`'s incoming, so it can't be the same record.
    FrameIndex newLength = m_extendA + m_extendB;
    for (const Transition &existing : model.sequence().transitions) {
        if (existing.b == m_a && existing.length + newLength > newLengthA)
            return false;
        if (existing.a == m_b && existing.length + newLength > newLengthB)
            return false;
    }

    m_transitionId = model.addTransition(m_track, m_a, m_b, m_extendA, m_extendB,
                                         m_appliedBefore ? std::optional<TransitionId>(m_transitionId) : std::nullopt);
    m_appliedBefore = true;
    return true;
}

void AddTransition::revert(Model &model)
{
    model.removeTransition(m_transitionId);
}

RemoveTransition::RemoveTransition(TransitionId transition) : m_transition(transition) {}

bool RemoveTransition::apply(Model &model)
{
    if (!model.hasTransition(m_transition))
        return false;
    m_captured = model.transition(m_transition);
    if (model.track(m_captured.track).locked)
        return false;
    model.removeTransition(m_transition);
    return true;
}

void RemoveTransition::revert(Model &model)
{
    model.addTransition(m_captured.track, m_captured.a, m_captured.b, m_captured.extendA, m_captured.extendB,
                        m_captured.id);
}

} // namespace ustudio::core
