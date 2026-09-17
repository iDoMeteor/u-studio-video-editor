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

// --- InsertClip / RemoveClip / MoveClip / ResizeClip / SplitClip -----------

InsertClip::InsertClip(TrackId track, AssetId asset, FrameIndex pos, FrameIndex in, FrameIndex out)
    : m_track(track), m_asset(asset), m_pos(pos), m_in(in), m_out(out)
{}

bool InsertClip::apply(Model &model)
{
    if (!model.hasTrack(m_track) || !model.hasAsset(m_asset))
        return false;
    if (m_in > m_out || m_pos < 0)
        return false;
    if (!model.isRangeFree(m_track, m_pos, m_pos + (m_out - m_in + 1)))
        return false;

    m_clipId = model.insertClip(m_track, m_asset, m_pos, m_in, m_out,
                                m_appliedBefore ? std::optional<ClipId>(m_clipId) : std::nullopt);
    m_appliedBefore = true;
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
    FrameIndex length = current.length();
    if (!model.isRangeFree(m_newTrack, m_newPos, m_newPos + length, m_clip))
        return false;

    m_oldTrack = current.track;
    m_oldPos = current.position;
    model.moveClip(m_clip, m_newTrack, m_newPos);
    return true;
}

void MoveClip::revert(Model &model)
{
    model.moveClip(m_clip, m_oldTrack, m_oldPos);
}

ResizeClip::ResizeClip(ClipId clip, FrameIndex newIn, FrameIndex newOut, FrameIndex newPos)
    : m_clip(clip), m_newIn(newIn), m_newOut(newOut), m_newPos(newPos)
{}

bool ResizeClip::apply(Model &model)
{
    if (!model.hasClip(m_clip))
        return false;
    if (m_newIn > m_newOut || m_newPos < 0)
        return false;

    const Clip &current = model.clip(m_clip);
    FrameIndex newLength = m_newOut - m_newIn + 1;
    if (!model.isRangeFree(current.track, m_newPos, m_newPos + newLength, m_clip))
        return false;

    if (model.hasAsset(current.asset)) {
        const Asset &sourceAsset = model.asset(current.asset);
        bool boundless = sourceAsset.info.isStillImage || sourceAsset.info.isImageSequence ||
                         sourceAsset.info.lengthInSequenceFrames <= 0;
        if (!boundless && (m_newIn < 0 || m_newOut >= sourceAsset.info.lengthInSequenceFrames))
            return false;
    }

    m_oldIn = current.in;
    m_oldOut = current.out;
    m_oldPos = current.position;
    model.resizeClip(m_clip, m_newIn, m_newOut, m_newPos);
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
    if (!(m_at > current.position && m_at < current.end()))
        return false;

    m_oldOut = current.out;
    m_rightId = model.splitClip(m_clip, m_at, m_appliedBefore ? std::optional<ClipId>(m_rightId) : std::nullopt);
    m_appliedBefore = true;
    return true;
}

void SplitClip::revert(Model &model)
{
    model.removeClip(m_rightId);
    const Clip &left = model.clip(m_clip);
    model.resizeClip(m_clip, left.in, m_oldOut, left.position);
}

} // namespace ustudio::core
