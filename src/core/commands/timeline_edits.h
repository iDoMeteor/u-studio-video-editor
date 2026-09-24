#pragma once

#include "command.h"
#include "core/model/model.h"
#include "primitives.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ustudio::core {

// M3's timeline edits (doc 06's gesture table): ripple, slip, copy, and
// markers. Each is one undo step. Validation happens before any mutation,
// and apply() leaves the model untouched when it returns false (doc 04).

// Moves every clip on `track` that starts at or after `from` by `delta`
// frames, keeping them in step so the dissolves between them survive (the
// reason this exists: chaining MoveClip strips a dissolve on every clip it
// moves, see primitives.h's T1 note). Refuses when there is nothing to
// move, the track is locked, a dissolve straddles `from` (half its pair
// would move), anything would land before frame 0, or the shifted group
// would overlap a clip that stays put. Close Gap, ripple delete and ripple
// trim are built on it.
class ShiftClips : public Command
{
  public:
    ShiftClips(TrackId track, FrameIndex from, FrameIndex delta);
    std::string label() const override
    {
        return "Shift clips";
    }
    bool apply(Model &) override;
    void revert(Model &) override;

  private:
    TrackId m_track;
    FrameIndex m_from, m_delta;
    std::vector<ClipId> m_moved; // in apply order
};

// Moves a set of clips together, `delta` frames along the timeline and
// `rowDelta` tracks down (negative: up), keeping their relative places
// (doc 06: dragging a multi-selection). A dissolve between two moved clips
// on the same track survives a sideways move; any other dissolve on a moved
// clip is removed, as MoveClip does. Refused if a track is locked, a clip
// would leave the track list or land before frame 0, anything would
// overlap, or a clip with no audio would land on an audio track. Validated
// by a dry run on a copy of the model, so it can't half-apply.
class MoveClips : public Command
{
  public:
    MoveClips(std::vector<ClipId> clips, FrameIndex delta, int rowDelta);
    std::string label() const override
    {
        return m_clips.size() == 1 ? "Move clip" : "Move clips";
    }
    bool apply(Model &) override;
    void revert(Model &) override;

  private:
    struct Moved
    {
        ClipId clip;
        TrackId oldTrack;
        FrameIndex oldPosition = 0;
        bool oldVideoEnabled = true;
    };
    // Performs the move on `model` (the real one or a scratch copy),
    // recording what revert() needs. False if a clip or track is missing.
    bool perform(Model &model, std::vector<Moved> &moved, std::vector<Transition> &stripped) const;

    std::vector<ClipId> m_clips;
    FrameIndex m_delta;
    int m_rowDelta;
    std::vector<Moved> m_moved;
    std::vector<Transition> m_stripped;
};

// Moves one clip in ripple mode (doc 06): the gap it leaves closes (later
// clips on its old track move left), and clips at or after the drop point
// on the new track move right to make room. `pos` is where the clip's start
// goes, counted on the timeline as it was before the move. Refused where
// the drop point is inside another clip or would split a dissolve, a track
// is locked, or a clip with no audio would land on an audio track. The
// clip's own dissolves are removed, as MoveClip does. Validated by a dry run
// on a copy of the model.
class RippleMove : public Command
{
  public:
    RippleMove(ClipId clip, TrackId track, FrameIndex pos);
    std::string label() const override
    {
        return "Ripple move";
    }
    bool apply(Model &) override;
    void revert(Model &) override;

  private:
    struct Record
    {
        TrackId oldTrack;
        FrameIndex oldPosition = 0;
        bool oldVideoEnabled = true;
        std::vector<ClipId> closedUp; // moved left by the length on the old track
        std::vector<ClipId> pushedOn; // moved right by the length on the new track
        FrameIndex length = 0;
        std::vector<Transition> stripped;
    };
    bool perform(Model &model, Record &record) const;

    ClipId m_clip;
    TrackId m_track;
    FrameIndex m_pos;
    Record m_record;
};

// Removes a clip and closes the hole it leaves: every later clip on its
// track moves left by the clip's length (doc 06: Shift+Delete). Any
// dissolve on the removed clip goes with it, as with a plain delete.
class RippleDelete : public Command
{
  public:
    explicit RippleDelete(ClipId clip);
    std::string label() const override
    {
        return "Ripple delete";
    }
    bool apply(Model &) override;
    void revert(Model &) override;

  private:
    ClipId m_clip;
    std::vector<std::unique_ptr<Command>> m_steps; // applied, in order
};

// Trims one edge of a clip and moves everything after it on the track by
// the same amount, so no gap opens and nothing is overwritten (doc 06:
// Alt+drag on an edge). `delta` is in timeline frames: for the tail, a
// positive delta lengthens the clip; for the head, a positive delta
// shortens it from the front (its start stays put and later clips move
// left). The clip's source range must have room.
class RippleTrim : public Command
{
  public:
    enum class Edge
    {
        Head,
        Tail,
    };
    RippleTrim(ClipId clip, Edge edge, FrameIndex delta);
    std::string label() const override
    {
        return "Ripple trim";
    }
    bool apply(Model &) override;
    void revert(Model &) override;

  private:
    ClipId m_clip;
    Edge m_edge;
    FrameIndex m_delta;
    std::vector<std::unique_ptr<Command>> m_steps; // applied, in order
};

// Moves a clip's source window by `delta` frames while the clip itself
// stays where it is, the same length (doc 06: Shift+drag on an edge).
class SlipClip : public Command
{
  public:
    SlipClip(ClipId clip, FrameIndex delta);
    std::string label() const override
    {
        return "Slip clip";
    }
    bool apply(Model &) override;
    void revert(Model &) override;

  private:
    ClipId m_clip;
    FrameIndex m_delta;
    std::unique_ptr<ResizeClip> m_resize;
};

// Copies a clip to `track` at `pos` (doc 06: Ctrl+drag): same asset,
// source range, name and stream switches. Refused where it would overlap,
// on a locked track, or where an audio track would get a clip with no
// audio (same rules as InsertClip).
class CopyClip : public Command
{
  public:
    CopyClip(ClipId source, TrackId track, FrameIndex pos);
    std::string label() const override
    {
        return "Copy clip";
    }
    bool apply(Model &) override;
    void revert(Model &) override;
    ClipId copyId() const
    {
        return m_copy;
    }

  private:
    ClipId m_source;
    TrackId m_track;
    FrameIndex m_pos;
    std::unique_ptr<InsertClip> m_insert;
    ClipId m_copy;
};

class AddMarker : public Command
{
  public:
    AddMarker(FrameIndex at, std::string text);
    std::string label() const override
    {
        return "Add marker";
    }
    bool apply(Model &) override;
    void revert(Model &) override;
    MarkerId markerId() const
    {
        return m_id;
    }

  private:
    FrameIndex m_at;
    std::string m_text;
    MarkerId m_id;
    bool m_appliedBefore = false;
};

class RemoveMarker : public Command
{
  public:
    explicit RemoveMarker(MarkerId marker);
    std::string label() const override
    {
        return "Remove marker";
    }
    bool apply(Model &) override;
    void revert(Model &) override;

  private:
    MarkerId m_id;
    Marker m_captured;
};

// Moves and/or renames a marker. Consecutive moves of the same marker merge
// into one undo step (a drag).
class EditMarker : public Command
{
  public:
    EditMarker(MarkerId marker, FrameIndex at, std::string text);
    std::string label() const override
    {
        return "Edit marker";
    }
    bool apply(Model &) override;
    void revert(Model &) override;
    bool mergeWith(const Command &next) override;

  private:
    MarkerId m_id;
    FrameIndex m_at;
    std::string m_text;
    FrameIndex m_oldAt = 0;
    std::string m_oldText;
};

} // namespace ustudio::core
