#pragma once

#include "core/commands/command.h"
#include "core/model/frame_time.h"
#include "core/model/ids.h"
#include "row_layout.h"
#include "selection.h"
#include "viewport.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ustudio::core {
class Model;
}

namespace ustudio::app::timeline {

// Everything the controller reads to interpret a gesture. Passed per call,
// never stored: the model and the viewport change between events.
struct TimelineContext
{
    const core::Model &model;
    const Viewport &viewport;
    RowLayout layout;
    double handleWidth = 22.0;    // the track-handle strip left of frame 0
    double edgeGrabPx = 8.0;      // how close to an edge counts as the edge
    double dragThresholdPx = 3.0; // below this a press-release is a click
    core::FrameIndex playhead = 0;
    core::FrameIndex sequenceLength = 0; // frames; 0 = nothing to edit or seek
};

enum class Modifiers : unsigned
{
    None = 0,
    Shift = 1,
    Ctrl = 2,
    Alt = 4,
};
inline Modifiers operator|(Modifiers a, Modifiers b)
{
    return static_cast<Modifiers>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
inline bool has(Modifiers set, Modifiers flag)
{
    return (static_cast<unsigned>(set) & static_cast<unsigned>(flag)) != 0;
}

// What a gesture asks the window to do. `attempts` run in order through the
// undo stack until one succeeds (a trim into a touching neighbour first tries
// the trim, then a dissolve); if none does, `failureStatus` is shown.
struct TimelineOutcome
{
    struct Attempt
    {
        std::unique_ptr<core::Command> command;
        std::string successStatus; // empty = say nothing
    };
    std::vector<Attempt> attempts;
    std::string failureStatus;
    std::optional<core::FrameIndex> seek;
    std::optional<int> activeRow;          // applied immediately
    std::optional<int> activeRowOnSuccess; // applied if an attempt succeeds
    enum class Rename
    {
        None,
        Track,
        Clip
    } rename = Rename::None;
    int renameRow = -1;
    core::ClipId renameClip;
};

// What sits under a right-click, for the context menu.
struct ContextTarget
{
    int row = -1;
    core::FrameIndex frame = -1;                 // -1 = on the handle strip
    core::ClipId clip;                           // the clip under the pointer
    std::optional<core::FrameIndex> gapStart;    // inside a gap: where it starts
    core::TransitionId transition;               // inside a dissolve
    core::ClipId addTransitionA, addTransitionB; // at a touching, unlinked cut
};

// doc 06's TimelineController: the timeline's interaction state machine,
// fed by the window's GTK gesture callbacks and turning them into
// core::Commands. No GTK here, so tests/app/test_timeline_controller.cpp
// drives it against a real Model.
class TimelineController
{
  public:
    enum class Mode
    {
        None,
        TrackReorder,
        Scrub,
        MoveClip,
        TrimClipStart,
        TrimClipEnd,
        // Dragging one edge of an existing dissolve's hatch; the other
        // edge stays put. Resizing re-adds the transition with new
        // extendA/extendB (both clips keep their combined span).
        TransitionResizeLeft,
        TransitionResizeRight,
        // Shift+drag on empty space: a marquee that adds every clip it
        // touches to the selection. (doc 06 puts the marquee on a plain drag
        // and scrubbing on Alt; the owner's plain drag already scrubs.)
        RubberBand,
        // doc 06's modifier gestures on a single clip: Alt+drag an edge to
        // ripple trim (later clips follow), Shift+drag an edge to slip
        // (same place and length, different source frames), Ctrl+drag the
        // body to copy. A Ctrl press that never moves toggles the clip's
        // selection instead.
        RippleTrimStart,
        RippleTrimEnd,
        Slip,
        CopyClip,
    };

    // The live drag, for drawing: a ghost at the candidate position, a
    // dissolve at its candidate size, the row a track would drop on.
    struct Preview
    {
        core::ClipId clip;
        int row = -1;
        core::FrameIndex start = 0;
        core::FrameIndex length = 0;
        core::TransitionId transition;
        core::FrameIndex transitionLeft = 0; // exclusive range [left, right)
        core::FrameIndex transitionRight = 0;
        int reorderFromRow = -1;
        int reorderHoverRow = -1;
        std::optional<core::FrameIndex> snappedTo; // the frame an edge snapped to
        // False while a move or copy would be refused where it is (drawn
        // red, doc 06): overlap, a locked track, no audio for an audio track.
        bool valid = true;
        // MoveClip with several clips selected: every selected clip is
        // drawn shifted by these.
        bool group = false;
        core::FrameIndex groupDelta = 0;
        int groupRowDelta = 0;
        // Slip: how far the source window moves (source frames, +later).
        core::FrameIndex slipDelta = 0;
        // RubberBand, in widget coordinates.
        double bandX0 = 0, bandY0 = 0, bandX1 = 0, bandY1 = 0;
    };

    Mode mode() const
    {
        return m_mode;
    }
    const Preview &preview() const
    {
        return m_preview;
    }
    Selection &selection()
    {
        return m_selection;
    }
    const Selection &selection() const
    {
        return m_selection;
    }

    // A single or double click that didn't become a drag.
    TimelineOutcome click(const TimelineContext &ctx, int nPress, double x, double y, Modifiers mods);
    // The drag gesture: press, motion by offset from the press point, release.
    TimelineOutcome press(const TimelineContext &ctx, double x, double y, Modifiers mods);
    TimelineOutcome motion(const TimelineContext &ctx, double offsetX, double offsetY);
    TimelineOutcome release(const TimelineContext &ctx, double offsetX, double offsetY);
    // Escape, or the model changed under the drag: drop the drag, no edit.
    void cancel();
    void selectAll(const core::Model &model);

    ContextTarget contextTargetAt(const TimelineContext &ctx, double x, double y) const;

  private:
    // Edges the dragged edge snaps to: every clip edge on every track
    // (except the dragged clip's own), the playhead, markers and frame 0.
    std::vector<core::FrameIndex> snapTargets(const TimelineContext &ctx, core::ClipId exclude) const;
    std::optional<core::FrameIndex> snapDelta(const TimelineContext &ctx, core::FrameIndex position,
                                              const std::vector<core::FrameIndex> &targets) const;
    // Whether `clips`, shifted by `delta` frames and `rowDelta` rows, would
    // land somewhere free (a copy leaves the originals in place).
    bool placementValid(const TimelineContext &ctx, const std::vector<core::ClipId> &clips, core::FrameIndex delta,
                        int rowDelta, bool copy) const;
    void releaseMove(const TimelineContext &ctx, TimelineOutcome &out);
    void releaseTrim(const TimelineContext &ctx, TimelineOutcome &out);
    void releaseTransitionResize(const TimelineContext &ctx, TimelineOutcome &out);

    Mode m_mode = Mode::None;
    Preview m_preview;
    Selection m_selection;
    double m_pressX = 0.0;
    double m_pressY = 0.0;
    Modifiers m_pressMods = Modifiers::None;
    // The dragged clip as it was at press time.
    int m_originRow = -1;
    core::FrameIndex m_originStart = 0;
    core::FrameIndex m_originLength = 0;
};

} // namespace ustudio::app::timeline
