#include "primitives.h"

#include <algorithm>

namespace ustudio::core {

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

    m_capturedClips.clear();
    for (ClipId clipId : it->clips)
        m_capturedClips.push_back(model.clip(clipId));

    m_capturedTrack = *it;
    m_capturedTrack.clips.clear(); // restoreClip() repopulates this on revert

    model.removeTrack(m_track);
    return true;
}

void RemoveTrack::revert(Model &model)
{
    model.restoreTrack(m_capturedTrack, m_capturedIndex);
    for (const Clip &clip : m_capturedClips)
        model.restoreClip(clip);
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
    if (boundless)
        model.extendAssetLength(m_asset, m_out + 1);
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
}

RemoveClip::RemoveClip(ClipId clip) : m_clip(clip) {}

bool RemoveClip::apply(Model &model)
{
    if (!model.hasClip(m_clip))
        return false;
    m_captured = model.clip(m_clip);
    if (model.track(m_captured.track).locked)
        return false;
    model.removeClip(m_clip);
    return true;
}

void RemoveClip::revert(Model &model)
{
    model.restoreClip(m_captured);
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
    FrameIndex length = current.length();
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

    m_oldTrack = current.track;
    m_oldPos = current.position;
    m_oldVideoEnabled = current.videoEnabled;
    model.moveClip(m_clip, m_newTrack, m_newPos);
    if (destIsAudio && current.videoEnabled)
        model.setClipEnabled(m_clip, /*videoEnabled=*/false, current.audioEnabled);
    return true;
}

void MoveClip::revert(Model &model)
{
    model.moveClip(m_clip, m_oldTrack, m_oldPos);
    const Clip &restored = model.clip(m_clip);
    if (restored.videoEnabled != m_oldVideoEnabled)
        model.setClipEnabled(m_clip, m_oldVideoEnabled, restored.audioEnabled);
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

    m_oldIn = current.in;
    m_oldOut = current.out;
    m_oldPos = current.position;
    // See InsertClip::apply for why this is batched and why boundless assets
    // get their recorded length extended alongside the resize.
    model.notify(BatchBegin{});
    model.resizeClip(m_clip, m_newIn, m_newOut, m_newPos);
    if (boundless)
        model.extendAssetLength(current.asset, m_newOut + 1);
    model.notify(BatchEnd{});
    return true;
}

void ResizeClip::revert(Model &model)
{
    model.resizeClip(m_clip, m_oldIn, m_oldOut, m_oldPos);
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

    m_oldOut = current.out;
    m_oldFadeOut = current.fadeOut;
    m_rightId = model.splitClip(m_clip, m_at, m_appliedBefore ? std::optional<ClipId>(m_rightId) : std::nullopt);
    m_appliedBefore = true;
    return true;
}

void SplitClip::revert(Model &model)
{
    model.removeClip(m_rightId);
    const Clip &left = model.clip(m_clip);
    model.resizeClip(m_clip, left.in, m_oldOut, left.position);
    // Model::splitClip() clears the left clip's fadeOut (the new right edge
    // becomes a hard cut) -- resizeClip() above restores the geometry but
    // doesn't know about fades, so put back whatever was captured at apply()
    // time explicitly (audit C5).
    model.setClipFadeOut(m_clip, m_oldFadeOut);
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

} // namespace ustudio::core
