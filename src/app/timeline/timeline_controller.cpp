#include "timeline_controller.h"

#include "core/commands/composite_command.h"
#include "core/commands/primitives.h"
#include "core/commands/timeline_edits.h"
#include "core/model/model.h"

#include <algorithm>
#include <cmath>

namespace ustudio::app::timeline {

namespace {

int trackCount(const TimelineContext &ctx)
{
    return static_cast<int>(ctx.model.sequence().tracks.size());
}

bool rowInRange(const TimelineContext &ctx, int row)
{
    return row >= 0 && row < trackCount(ctx);
}

core::TrackId trackAtRow(const TimelineContext &ctx, int row)
{
    return ctx.model.sequence().tracks[static_cast<size_t>(row)].id;
}

// The clip on `row` covering `frame`, if any.
core::ClipId clipAt(const TimelineContext &ctx, int row, core::FrameIndex frame)
{
    if (!rowInRange(ctx, row))
        return {};
    for (core::ClipId id : ctx.model.track(trackAtRow(ctx, row)).clips) {
        const core::Clip &clip = ctx.model.clip(id);
        if (frame >= clip.position && frame < clip.end())
            return id;
    }
    return {};
}

bool near(double a, double b, double within)
{
    return std::abs(a - b) < within;
}

} // namespace

TimelineOutcome TimelineController::click(const TimelineContext &ctx, int nPress, double x, double y, Modifiers mods)
{
    TimelineOutcome out;
    // The top lane (a drop-in's, RowLayout::topLane) is no row: no active
    // track change, no selection change.
    if (trackCount(ctx) <= 0 || ctx.layout.inTopLane(y))
        return out;

    int row = ctx.layout.clampedRowAt(y, trackCount(ctx));
    out.activeRow = row;
    // The handle strip is for dragging tracks; a click there only makes
    // the row active.
    if (x < ctx.handleWidth || ctx.sequenceLength <= 0)
        return out;

    core::FrameIndex frame = ctx.viewport.frameForX(x);
    core::ClipId clip = clipAt(ctx, row, frame);

    // Double-click renames: the name strip at the top of a row edits the
    // track's name, a clip edits the clip's. The strip check comes first:
    // a clip under the strip still matches by frame.
    if (nPress >= 2) {
        if (ctx.layout.inNameStrip(y)) {
            out.rename = TimelineOutcome::Rename::Track;
            out.renameRow = row;
            return out;
        }
        if (clip.isValid()) {
            m_selection.selectOnly(clip);
            out.rename = TimelineOutcome::Rename::Clip;
            out.renameClip = clip;
            return out;
        }
    }

    // Shift and Ctrl clicks only change the selection, which press()
    // already did; they don't move the playhead.
    if (has(mods, Modifiers::Shift) || has(mods, Modifiers::Ctrl))
        return out;
    if (clip.isValid())
        m_selection.selectOnly(clip);
    else
        m_selection.clear();
    out.seek = std::clamp<core::FrameIndex>(frame, 0, ctx.sequenceLength - 1);
    return out;
}

void TimelineController::selectAll(const core::Model &model)
{
    m_selection.clear();
    for (const core::Track &track : model.sequence().tracks) {
        for (core::ClipId id : track.clips)
            m_selection.add(id);
    }
}

TimelineOutcome TimelineController::press(const TimelineContext &ctx, double x, double y, Modifiers mods)
{
    TimelineOutcome out;
    cancel();
    m_pressX = x;
    m_pressY = y;
    m_pressMods = mods;
    int count = trackCount(ctx);
    if (count <= 0)
        return out;

    if (x < ctx.handleWidth) {
        m_mode = Mode::TrackReorder;
        m_preview.reorderFromRow = ctx.layout.clampedRowAt(y, count);
        m_preview.reorderHoverRow = m_preview.reorderFromRow;
        return out;
    }
    if (ctx.sequenceLength <= 0)
        return out;

    int row = ctx.layout.rowAt(y);
    core::FrameIndex pressFrame = ctx.viewport.frameForX(x);

    if (rowInRange(ctx, row)) {
        core::TrackId track = trackAtRow(ctx, row);

        // A dissolve's edges sit exactly on the edges of the two clips it
        // links, so they are checked before clip edges: dragging there
        // resizes the dissolve rather than trying (and failing) a trim.
        for (const core::Transition &t : ctx.model.sequence().transitions) {
            if (t.track != track || !ctx.model.hasClip(t.a) || !ctx.model.hasClip(t.b))
                continue;
            const core::Clip &a = ctx.model.clip(t.a);
            const core::Clip &b = ctx.model.clip(t.b);
            bool nearLeft = near(x, ctx.viewport.xForFrame(static_cast<double>(b.position)), ctx.edgeGrabPx);
            bool nearRight = near(x, ctx.viewport.xForFrame(static_cast<double>(a.end())), ctx.edgeGrabPx);
            if (!nearLeft && !nearRight)
                continue;
            m_mode = nearLeft ? Mode::TransitionResizeLeft : Mode::TransitionResizeRight;
            m_preview.transition = t.id;
            m_preview.row = row;
            m_preview.transitionLeft = b.position;
            m_preview.transitionRight = a.end();
            out.activeRow = row;
            return out;
        }

        if (core::ClipId id = clipAt(ctx, row, pressFrame); id.isValid()) {
            const core::Clip &clip = ctx.model.clip(id);
            m_originRow = row;
            m_originStart = clip.position;
            m_originLength = clip.length();
            m_preview.clip = id;
            m_preview.row = row;
            m_preview.start = clip.position;
            m_preview.length = clip.length();
            out.activeRow = row;
            double left = ctx.viewport.xForFrame(static_cast<double>(clip.position));
            double right = ctx.viewport.xForFrame(static_cast<double>(clip.end()));
            bool onHead = x - left < ctx.edgeGrabPx;
            bool onTail = !onHead && right - x < ctx.edgeGrabPx;

            // Ctrl: a drag copies, a click toggles the selection (decided
            // at release, so a copy never changes the selection).
            if (has(mods, Modifiers::Ctrl)) {
                m_mode = Mode::CopyClip;
                return out;
            }
            if ((onHead || onTail) && has(mods, Modifiers::Alt)) {
                m_selection.selectOnly(id);
                m_mode = onHead ? Mode::RippleTrimStart : Mode::RippleTrimEnd;
                return out;
            }
            if ((onHead || onTail) && has(mods, Modifiers::Shift)) {
                m_selection.selectOnly(id);
                m_mode = Mode::Slip;
                return out;
            }

            // Shift adds; a plain press on an unselected clip selects just
            // it, and on a selected one keeps the selection so the whole
            // group can be dragged.
            if (has(mods, Modifiers::Shift))
                m_selection.add(id);
            else if (!m_selection.contains(id))
                m_selection.selectOnly(id);

            if (onHead)
                m_mode = Mode::TrimClipStart;
            else if (onTail)
                m_mode = Mode::TrimClipEnd;
            else
                m_mode = Mode::MoveClip;
            return out;
        }
    }

    if (has(mods, Modifiers::Shift)) {
        m_mode = Mode::RubberBand;
        m_preview.bandX0 = m_preview.bandX1 = x;
        m_preview.bandY0 = m_preview.bandY1 = y;
        return out;
    }

    // Empty space scrubs: seek now, so a press-release that never moves
    // still lands where a click would.
    m_mode = Mode::Scrub;
    out.seek = std::clamp<core::FrameIndex>(pressFrame, 0, ctx.sequenceLength - 1);
    out.activeRow = std::clamp(row, 0, count - 1);
    return out;
}

TimelineOutcome TimelineController::motion(const TimelineContext &ctx, double offsetX, double offsetY)
{
    TimelineOutcome out;
    int count = trackCount(ctx);
    double x = m_pressX + offsetX;
    double y = m_pressY + offsetY;
    core::FrameIndex delta = ctx.viewport.framesForPixels(offsetX);
    m_preview.snappedTo.reset();
    m_preview.valid = true;

    switch (m_mode) {
    case Mode::None:
        break;
    case Mode::TrackReorder:
        m_preview.reorderHoverRow = ctx.layout.clampedRowAt(y, count);
        break;
    case Mode::Scrub:
        if (ctx.sequenceLength > 0)
            out.seek = std::clamp<core::FrameIndex>(ctx.viewport.frameForX(x), 0, ctx.sequenceLength - 1);
        break;
    case Mode::RubberBand:
        m_preview.bandX1 = x;
        m_preview.bandY1 = y;
        break;
    case Mode::CopyClip: {
        m_preview.row = ctx.layout.clampedRowAt(y, count);
        core::FrameIndex start = std::max<core::FrameIndex>(0, m_originStart + delta);
        // The original stays, so its own edges are fair snap targets too
        // (drop the copy right after it).
        std::vector<core::FrameIndex> targets = snapTargets(ctx, core::ClipId{});
        std::optional<core::FrameIndex> head = snapDelta(ctx, start, targets);
        std::optional<core::FrameIndex> tail = snapDelta(ctx, start + m_originLength, targets);
        std::optional<core::FrameIndex> chosen = head;
        if (tail && (!head || std::abs(*tail) < std::abs(*head)))
            chosen = tail;
        if (chosen) {
            start += *chosen;
            m_preview.snappedTo = chosen == head ? start : start + m_originLength;
        }
        m_preview.start = std::max<core::FrameIndex>(0, start);
        m_preview.length = m_originLength;
        m_preview.valid =
            placementValid(ctx, {m_preview.clip}, m_preview.start - m_originStart, m_preview.row - m_originRow, true);
        break;
    }
    case Mode::RippleTrimStart: {
        // The clip's start stays put; its head is cut (or restored) and
        // everything after it follows, so the preview only shortens.
        core::FrameIndex cut = std::min(delta, m_originLength - 1);
        m_preview.row = m_originRow;
        m_preview.start = m_originStart;
        m_preview.length = m_originLength - cut;
        break;
    }
    case Mode::RippleTrimEnd: {
        core::FrameIndex end = std::max(m_originStart + 1, m_originStart + m_originLength + delta);
        if (auto d = snapDelta(ctx, end, snapTargets(ctx, m_preview.clip))) {
            end = std::max(m_originStart + 1, end + *d);
            m_preview.snappedTo = end;
        }
        m_preview.row = m_originRow;
        m_preview.start = m_originStart;
        m_preview.length = end - m_originStart;
        break;
    }
    case Mode::Slip:
        // Dragging right reveals earlier source frames, the way the
        // footage would move if pulled along under the clip.
        m_preview.row = m_originRow;
        m_preview.start = m_originStart;
        m_preview.length = m_originLength;
        m_preview.slipDelta = -delta;
        // Stop at the ends of the source instead of refusing at release.
        if (ctx.model.hasClip(m_preview.clip)) {
            const core::Clip &clip = ctx.model.clip(m_preview.clip);
            core::FrameIndex earliest = -clip.in;
            core::FrameIndex latest = 0;
            if (ctx.model.hasAsset(clip.asset))
                latest = std::max<core::FrameIndex>(0, ctx.model.asset(clip.asset).info.lengthInSequenceFrames - 1 -
                                                           clip.out);
            m_preview.slipDelta = std::clamp(m_preview.slipDelta, earliest, latest);
        }
        break;
    case Mode::MoveClip: {
        m_preview.row = ctx.layout.clampedRowAt(y, count);
        m_preview.group = m_selection.clips().size() > 1;
        if (m_preview.group) {
            // Keep every selected clip on a real track, and none before 0.
            int lowest = count, highest = -1;
            core::FrameIndex earliest = m_originStart;
            for (core::ClipId id : m_selection.clips()) {
                if (!ctx.model.hasClip(id))
                    continue;
                const core::Clip &clip = ctx.model.clip(id);
                for (int r = 0; r < count; ++r) {
                    if (trackAtRow(ctx, r) == clip.track) {
                        lowest = std::min(lowest, r);
                        highest = std::max(highest, r);
                    }
                }
                earliest = std::min(earliest, clip.position);
            }
            int rowDelta = std::clamp(m_preview.row - m_originRow, -lowest, count - 1 - highest);
            m_preview.row = m_originRow + rowDelta;
            m_preview.groupRowDelta = rowDelta;
            delta = std::max(delta, -earliest);
        }
        core::FrameIndex start = std::max<core::FrameIndex>(0, m_originStart + delta);
        // Whichever edge of the clip is nearer a target snaps, and the
        // whole clip moves by that one delta.
        std::vector<core::FrameIndex> targets = snapTargets(ctx, m_preview.clip);
        std::optional<core::FrameIndex> head = snapDelta(ctx, start, targets);
        std::optional<core::FrameIndex> tail = snapDelta(ctx, start + m_originLength, targets);
        std::optional<core::FrameIndex> chosen = head;
        if (tail && (!head || std::abs(*tail) < std::abs(*head)))
            chosen = tail;
        if (chosen) {
            start += *chosen;
            m_preview.snappedTo = chosen == head ? start : start + m_originLength;
        }
        m_preview.start = std::max<core::FrameIndex>(0, start);
        m_preview.length = m_originLength;
        // Ripple mode on the clip's own track: a drop inside its own old
        // span leaves it where it is. RippleMove judges the drop against the
        // closed-up timeline, where the next clip now fills that span, so
        // anything else there would preview valid and then be refused
        // (post-M3 audit P6).
        if (ctx.rippleMode && !m_preview.group && m_preview.row == m_originRow && m_originStart < m_preview.start &&
            m_preview.start < m_originStart + m_originLength)
            m_preview.start = m_originStart;
        // Ripple mode inserts at a cut: a drop inside another clip moves to
        // whichever of its edges is nearer.
        if (ctx.rippleMode && !m_preview.group && rowInRange(ctx, m_preview.row)) {
            for (core::ClipId id : ctx.model.track(trackAtRow(ctx, m_preview.row)).clips) {
                const core::Clip &other = ctx.model.clip(id);
                if (id != m_preview.clip && other.position < m_preview.start && m_preview.start < other.end()) {
                    m_preview.start = m_preview.start - other.position <= other.end() - m_preview.start ? other.position
                                                                                                        : other.end();
                    m_preview.snappedTo = m_preview.start;
                    break;
                }
            }
        }
        m_preview.groupDelta = m_preview.start - m_originStart;
        if (m_preview.group) {
            std::vector<core::ClipId> clips(m_selection.clips().begin(), m_selection.clips().end());
            m_preview.valid = placementValid(ctx, clips, m_preview.groupDelta, m_preview.groupRowDelta, false);
        } else if (!(m_preview.row == m_originRow && m_preview.start == m_originStart)) {
            m_preview.valid = placementValid(ctx, {m_preview.clip}, m_preview.start - m_originStart,
                                             m_preview.row - m_originRow, false, ctx.rippleMode);
        }
        break;
    }
    case Mode::TrimClipStart: {
        core::FrameIndex end = m_originStart + m_originLength;
        core::FrameIndex start = std::clamp<core::FrameIndex>(m_originStart + delta, 0, end - 1);
        if (auto d = snapDelta(ctx, start, snapTargets(ctx, m_preview.clip))) {
            start = std::clamp<core::FrameIndex>(start + *d, 0, end - 1);
            m_preview.snappedTo = start;
        }
        m_preview.row = m_originRow;
        m_preview.start = start;
        m_preview.length = end - start;
        break;
    }
    case Mode::TrimClipEnd: {
        core::FrameIndex end = std::max(m_originStart + 1, m_originStart + m_originLength + delta);
        if (auto d = snapDelta(ctx, end, snapTargets(ctx, m_preview.clip))) {
            end = std::max(m_originStart + 1, end + *d);
            m_preview.snappedTo = end;
        }
        m_preview.row = m_originRow;
        m_preview.start = m_originStart;
        m_preview.length = end - m_originStart;
        break;
    }
    case Mode::TransitionResizeLeft:
    case Mode::TransitionResizeRight: {
        // Preview only; whether the clips have the source frames is
        // AddTransition's call at release.
        if (!ctx.model.hasTransition(m_preview.transition))
            break;
        const core::Transition &t = ctx.model.transition(m_preview.transition);
        core::FrameIndex left = ctx.model.clip(t.b).position;
        core::FrameIndex right = ctx.model.clip(t.a).end();
        if (m_mode == Mode::TransitionResizeLeft)
            m_preview.transitionLeft = std::clamp<core::FrameIndex>(left + delta, 0, right - 1);
        else
            m_preview.transitionRight = std::max(left + 1, right + delta);
        break;
    }
    }
    return out;
}

TimelineOutcome TimelineController::release(const TimelineContext &ctx, double offsetX, double offsetY)
{
    TimelineOutcome out;
    bool trivial = std::abs(offsetX) < ctx.dragThresholdPx && std::abs(offsetY) < ctx.dragThresholdPx;
    Mode mode = m_mode;

    if (trivial && mode == Mode::CopyClip) {
        core::ClipId clip = m_preview.clip;
        cancel();
        m_selection.toggle(clip);
        return click(ctx, 1, m_pressX, m_pressY, m_pressMods);
    }
    if (trivial && mode != Mode::TrackReorder) {
        cancel();
        return click(ctx, 1, m_pressX, m_pressY, m_pressMods);
    }

    switch (mode) {
    case Mode::None:
    case Mode::Scrub: // the playhead already followed the pointer
        break;
    case Mode::TrackReorder: {
        int target = m_preview.reorderHoverRow;
        int from = m_preview.reorderFromRow;
        if (target != from && rowInRange(ctx, from)) {
            out.attempts.push_back(
                {std::make_unique<core::MoveTrack>(trackAtRow(ctx, from), static_cast<size_t>(target)),
                 "Moved track " + std::to_string(from) + " to " + std::to_string(target) + "."});
            out.activeRowOnSuccess = target;
        }
        break;
    }
    case Mode::MoveClip:
        releaseMove(ctx, out);
        break;
    case Mode::RubberBand: {
        double left = std::min(m_preview.bandX0, m_preview.bandX1);
        double right = std::max(m_preview.bandX0, m_preview.bandX1);
        double top = std::min(m_preview.bandY0, m_preview.bandY1);
        double bottom = std::max(m_preview.bandY0, m_preview.bandY1);
        for (int r = 0; r < trackCount(ctx); ++r) {
            double rowTop = ctx.layout.clipTop(r);
            if (rowTop + ctx.layout.clipHeight() < top || rowTop > bottom)
                continue;
            for (core::ClipId id : ctx.model.track(trackAtRow(ctx, r)).clips) {
                const core::Clip &clip = ctx.model.clip(id);
                double clipLeft = ctx.viewport.xForFrame(static_cast<double>(clip.position));
                double clipRight = ctx.viewport.xForFrame(static_cast<double>(clip.end()));
                if (clipRight >= left && clipLeft <= right)
                    m_selection.add(id);
            }
        }
        break;
    }
    case Mode::TrimClipStart:
    case Mode::TrimClipEnd:
        releaseTrim(ctx, out);
        break;
    case Mode::RippleTrimStart:
    case Mode::RippleTrimEnd: {
        using Edge = core::RippleTrim::Edge;
        bool head = mode == Mode::RippleTrimStart;
        // Head: frames cut from the front. Tail: frames added at the end.
        core::FrameIndex d = head ? m_originLength - m_preview.length : m_preview.length - m_originLength;
        if (d != 0) {
            out.attempts.push_back(
                {std::make_unique<core::RippleTrim>(m_preview.clip, head ? Edge::Head : Edge::Tail, d), ""});
            out.failureStatus = "Can't ripple trim that far — the source has no more frames, or a track is locked.";
        }
        break;
    }
    case Mode::Slip:
        if (m_preview.slipDelta != 0) {
            out.attempts.push_back({std::make_unique<core::SlipClip>(m_preview.clip, m_preview.slipDelta),
                                    "Slipped the clip " + std::to_string(m_preview.slipDelta) + " frames."});
            out.failureStatus = "Can't slip that far — the source has no more frames there.";
        }
        break;
    case Mode::CopyClip:
        if (rowInRange(ctx, m_preview.row)) {
            out.attempts.push_back(
                {std::make_unique<core::CopyClip>(m_preview.clip, trackAtRow(ctx, m_preview.row), m_preview.start),
                 "Copied the clip."});
            out.activeRowOnSuccess = m_preview.row;
            out.failureStatus = "Can't copy the clip there — that space is occupied.";
        }
        break;
    case Mode::TransitionResizeLeft:
    case Mode::TransitionResizeRight:
        releaseTransitionResize(ctx, out);
        break;
    }
    cancel();
    return out;
}

void TimelineController::releaseMove(const TimelineContext &ctx, TimelineOutcome &out)
{
    if (!ctx.model.hasClip(m_preview.clip) || !rowInRange(ctx, m_preview.row))
        return;
    const core::Clip &clip = ctx.model.clip(m_preview.clip);
    core::TrackId dest = trackAtRow(ctx, m_preview.row);
    // Back where it started is not a move: MoveClip would strip the clip's
    // dissolves for nothing (audit C3).
    if (dest == clip.track && m_preview.start == clip.position)
        return;
    if (m_preview.group) {
        std::vector<core::ClipId> clips(m_selection.clips().begin(), m_selection.clips().end());
        out.attempts.push_back(
            {std::make_unique<core::MoveClips>(std::move(clips), m_preview.groupDelta, m_preview.groupRowDelta), ""});
        out.activeRowOnSuccess = m_preview.row;
        out.failureStatus = "Can't move those clips there — something is in the way, or a track is locked.";
        return;
    }
    if (ctx.rippleMode) {
        // Landing back where it is once the timeline closes up (the drop on
        // the cut right after the clip) isn't a move either (P4).
        if (dest == clip.track && m_preview.start == clip.end())
            return;
        out.attempts.push_back({std::make_unique<core::RippleMove>(m_preview.clip, dest, m_preview.start), ""});
        out.activeRowOnSuccess = m_preview.row;
        out.failureStatus = "Can't ripple the clip there — that's inside another clip or a dissolve, or a track "
                            "is locked.";
        return;
    }
    out.attempts.push_back({std::make_unique<core::MoveClip>(m_preview.clip, dest, m_preview.start), ""});
    out.activeRowOnSuccess = m_preview.row;
    out.failureStatus = "Can't move the clip there — that space is occupied.";
}

void TimelineController::releaseTrim(const TimelineContext &ctx, TimelineOutcome &out)
{
    if (!ctx.model.hasClip(m_preview.clip))
        return;
    const core::Clip &clip = ctx.model.clip(m_preview.clip);
    core::TrackId track = clip.track;
    const std::vector<core::ClipId> &onTrack = ctx.model.track(track).clips;

    if (m_mode == Mode::TrimClipStart) {
        core::FrameIndex delta = m_preview.start - m_originStart;
        out.attempts.push_back(
            {std::make_unique<core::ResizeClip>(m_preview.clip, clip.in + delta, clip.out, m_preview.start), ""});
        // Dragging the head left into the clip that touches it makes a
        // dissolve instead, as long as the drag distance, all of it from
        // this clip's own head handle.
        if (delta < 0) {
            for (core::ClipId other : onTrack) {
                if (ctx.model.clip(other).end() == m_originStart) {
                    out.attempts.push_back(
                        {std::make_unique<core::AddTransition>(track, other, m_preview.clip, 0, -delta),
                         "Created a " + std::to_string(-delta) + "-frame dissolve."});
                    break;
                }
            }
        }
        out.failureStatus = "Can't trim the clip that far — space is occupied or the source has no more frames.";
    } else {
        // A source frame: the clip's own in-point plus its new length. (The
        // window used to pass the timeline end here, which was only right
        // for a clip whose in-point equals its position.)
        core::FrameIndex newOut = clip.in + m_preview.length - 1;
        out.attempts.push_back(
            {std::make_unique<core::ResizeClip>(m_preview.clip, clip.in, newOut, clip.position), ""});
        if (newOut > clip.out) {
            for (core::ClipId other : onTrack) {
                if (ctx.model.clip(other).position == m_originStart + m_originLength) {
                    out.attempts.push_back(
                        {std::make_unique<core::AddTransition>(track, m_preview.clip, other, newOut - clip.out, 0),
                         "Created a " + std::to_string(newOut - clip.out) + "-frame dissolve."});
                    break;
                }
            }
        }
        out.failureStatus = "Can't trim the clip that far — move the next clip out of the way first, or the "
                            "source has no more frames.";
    }
}

void TimelineController::releaseTransitionResize(const TimelineContext &ctx, TimelineOutcome &out)
{
    if (!ctx.model.hasTransition(m_preview.transition))
        return;
    const core::Transition &current = ctx.model.transition(m_preview.transition);
    core::FrameIndex extendA = current.extendA;
    core::FrameIndex extendB = current.extendB;
    // Only the dragged edge moves. The touch point (where the clips would
    // meet with no dissolve) comes from the side that stays put.
    if (m_mode == Mode::TransitionResizeLeft) {
        core::FrameIndex touch = ctx.model.clip(current.b).position + extendB;
        extendB = std::max<core::FrameIndex>(0, touch - m_preview.transitionLeft);
    } else {
        core::FrameIndex touch = ctx.model.clip(current.a).end() - extendA;
        extendA = std::max<core::FrameIndex>(0, m_preview.transitionRight - touch);
    }

    if (extendA + extendB > 0) {
        std::vector<std::unique_ptr<core::Command>> steps;
        steps.push_back(std::make_unique<core::RemoveTransition>(current.id));
        steps.push_back(std::make_unique<core::AddTransition>(current.track, current.a, current.b, extendA, extendB));
        out.attempts.push_back({std::make_unique<core::CompositeCommand>("Resize transition", std::move(steps)),
                                "Resized dissolve to " + std::to_string(extendA + extendB) + " frames."});
    } else {
        out.attempts.push_back({std::make_unique<core::RemoveTransition>(current.id), "Removed dissolve."});
    }
    out.failureStatus = "Couldn't resize that transition — not enough source frames.";
}

void TimelineController::cancel()
{
    m_mode = Mode::None;
    m_preview = Preview{};
    m_originRow = -1;
    m_originStart = 0;
    m_originLength = 0;
}

bool TimelineController::placementValid(const TimelineContext &ctx, const std::vector<core::ClipId> &clips,
                                        core::FrameIndex delta, int rowDelta, bool copy, bool rippled) const
{
    // What the move leaves behind doesn't block it: every moving clip,
    // and (C2) any dissolve partner whose extended span overlaps it.
    std::vector<core::ClipId> ignore;
    if (!copy) {
        ignore = clips;
        for (const core::Transition &t : ctx.model.sequence().transitions) {
            for (core::ClipId id : clips) {
                if (t.a == id)
                    ignore.push_back(t.b);
                if (t.b == id)
                    ignore.push_back(t.a);
            }
        }
    }
    for (core::ClipId id : clips) {
        if (!ctx.model.hasClip(id))
            return false;
        const core::Clip &clip = ctx.model.clip(id);
        int row = -1;
        for (int r = 0; r < trackCount(ctx); ++r) {
            if (trackAtRow(ctx, r) == clip.track)
                row = r;
        }
        int to = row + rowDelta;
        if (!rowInRange(ctx, to))
            return false;
        const core::Track &dest = ctx.model.track(trackAtRow(ctx, to));
        if (dest.locked || (!copy && ctx.model.track(clip.track).locked))
            return false;
        if (dest.kind == core::Track::Kind::Audio &&
            !(clip.audioEnabled && ctx.model.hasAsset(clip.asset) && ctx.model.asset(clip.asset).info.hasAudio))
            return false;
        core::FrameIndex start = clip.position + delta;
        if (start < 0 || (!rippled && !ctx.model.isRangeFree(dest.id, start, start + clip.length(), ignore)))
            return false;
    }
    return true;
}

std::vector<core::FrameIndex> TimelineController::snapTargets(const TimelineContext &ctx, core::ClipId exclude) const
{
    std::vector<core::FrameIndex> targets{0, ctx.playhead};
    for (const core::Track &track : ctx.model.sequence().tracks) {
        for (core::ClipId id : track.clips) {
            if (id == exclude)
                continue;
            const core::Clip &clip = ctx.model.clip(id);
            targets.push_back(clip.position);
            targets.push_back(clip.end());
        }
    }
    for (const core::Marker &marker : ctx.model.sequence().markers)
        targets.push_back(marker.at);
    return targets;
}

std::optional<core::FrameIndex> TimelineController::snapDelta(const TimelineContext &ctx, core::FrameIndex position,
                                                              const std::vector<core::FrameIndex> &targets) const
{
    if (!ctx.snapping)
        return std::nullopt;
    std::optional<core::FrameIndex> best;
    double bestPx = ctx.edgeGrabPx; // strictly closer than this snaps
    for (core::FrameIndex target : targets) {
        double px = static_cast<double>(std::abs(target - position)) * ctx.viewport.pxPerFrame();
        if (px < bestPx) {
            bestPx = px;
            best = target - position;
        }
    }
    return best;
}

ContextTarget TimelineController::contextTargetAt(const TimelineContext &ctx, double x, double y) const
{
    ContextTarget target;
    int count = trackCount(ctx);
    if (count <= 0 || ctx.layout.inTopLane(y))
        return target; // row -1: no track there
    target.row = ctx.layout.clampedRowAt(y, count);
    if (ctx.sequenceLength <= 0 || x < ctx.handleWidth)
        return target;

    core::FrameIndex frame = ctx.viewport.frameForX(x);
    target.frame = frame;
    core::TrackId trackId = trackAtRow(ctx, target.row);
    const core::Track &track = ctx.model.track(trackId);

    target.clip = clipAt(ctx, target.row, frame);
    if (!target.clip.isValid()) {
        // Inside a gap: somewhere a later clip exists. The gap starts where
        // the clip before it ends (or at 0), not at the click (audit A3),
        // so Close Gap closes all of it. Track clips are sorted.
        core::FrameIndex gapStart = 0;
        for (core::ClipId id : track.clips) {
            const core::Clip &candidate = ctx.model.clip(id);
            if (candidate.position > frame) {
                target.gapStart = gapStart;
                break;
            }
            gapStart = candidate.end();
        }
    }

    // A dissolve's overlap sits inside both clips, so it's offered as well
    // as the clip, not instead of it.
    for (const core::Transition &t : ctx.model.sequence().transitions) {
        if (t.track != trackId || !ctx.model.hasClip(t.a) || !ctx.model.hasClip(t.b))
            continue;
        if (frame >= ctx.model.clip(t.b).position && frame < ctx.model.clip(t.a).end()) {
            target.transition = t.id;
            break;
        }
    }

    // Near the cut between two touching clips with no dissolve yet.
    for (size_t i = 0; i + 1 < track.clips.size(); ++i) {
        const core::Clip &a = ctx.model.clip(track.clips[i]);
        const core::Clip &b = ctx.model.clip(track.clips[i + 1]);
        if (a.end() != b.position)
            continue;
        bool linked = std::any_of(ctx.model.sequence().transitions.begin(), ctx.model.sequence().transitions.end(),
                                  [&](const core::Transition &t) { return t.a == a.id && t.b == b.id; });
        if (!linked && std::abs(x - ctx.viewport.xForFrame(static_cast<double>(a.end()))) <= ctx.edgeGrabPx) {
            target.addTransitionA = a.id;
            target.addTransitionB = b.id;
            break;
        }
    }
    return target;
}

} // namespace ustudio::app::timeline
