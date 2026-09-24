#include "timeline_edits.h"

#include <algorithm>

namespace ustudio::core {

namespace {

// The transition that ends at `clip`'s head (clip is its `b`) or starts at
// its tail (clip is its `a`), if any.
std::optional<TransitionId> transitionAtEdge(const Model &model, ClipId clip, bool head)
{
    for (const Transition &t : model.sequence().transitions) {
        if ((head && t.b == clip) || (!head && t.a == clip))
            return t.id;
    }
    return std::nullopt;
}

} // namespace

// --- ShiftClips ------------------------------------------------------------

ShiftClips::ShiftClips(TrackId track, FrameIndex from, FrameIndex delta) : m_track(track), m_from(from), m_delta(delta)
{}

bool ShiftClips::apply(Model &model)
{
    if (m_delta == 0 || !model.hasTrack(m_track))
        return false;
    const Track &track = model.track(m_track);
    if (track.locked)
        return false;

    std::vector<ClipId> shifted;
    FrameIndex stayingEnd = 0;
    for (ClipId id : track.clips) { // sorted by position (Model invariant)
        const Clip &clip = model.clip(id);
        if (clip.position >= m_from)
            shifted.push_back(id);
        else
            stayingEnd = std::max(stayingEnd, clip.end());
    }
    if (shifted.empty())
        return false;

    // A dissolve with one clip on each side of `from` would be torn apart.
    for (const Transition &t : model.sequence().transitions) {
        if (t.track != m_track)
            continue;
        bool aMoves = model.clip(t.a).position >= m_from;
        bool bMoves = model.clip(t.b).position >= m_from;
        if (aMoves != bMoves)
            return false;
    }

    const FrameIndex firstNewPosition = model.clip(shifted.front()).position + m_delta;
    if (firstNewPosition < 0)
        return false;
    // Moving right can't collide (the group is the tail of the track);
    // moving left must stop at the end of whatever stays put.
    if (m_delta < 0 && firstNewPosition < stayingEnd)
        return false;

    if (m_delta > 0)
        std::reverse(shifted.begin(), shifted.end()); // make room from the far end first
    model.notify(BatchBegin{});
    for (ClipId id : shifted)
        model.moveClip(id, m_track, model.clip(id).position + m_delta);
    model.notify(BatchEnd{});
    m_moved = std::move(shifted);
    return true;
}

void ShiftClips::revert(Model &model)
{
    model.notify(BatchBegin{});
    for (auto it = m_moved.rbegin(); it != m_moved.rend(); ++it)
        model.moveClip(*it, m_track, model.clip(*it).position - m_delta);
    model.notify(BatchEnd{});
}

// --- Step lists ------------------------------------------------------------

namespace {

// Applies `step` and records it; false (nothing recorded) if refused.
bool runStep(Model &model, std::vector<std::unique_ptr<Command>> &steps, std::unique_ptr<Command> step)
{
    if (!step->apply(model))
        return false;
    steps.push_back(std::move(step));
    return true;
}

void revertSteps(Model &model, std::vector<std::unique_ptr<Command>> &steps)
{
    for (auto it = steps.rbegin(); it != steps.rend(); ++it)
        (*it)->revert(model);
}

// A clip's geometry without the extensions its dissolves give it: what it
// has once those are removed.
struct BaseGeometry
{
    FrameIndex position, in, out;
    FrameIndex end() const
    {
        return position + (out - in + 1);
    }
    FrameIndex length() const
    {
        return out - in + 1;
    }
};

BaseGeometry baseGeometry(const Model &model, ClipId id)
{
    const Clip &clip = model.clip(id);
    BaseGeometry base{clip.position, clip.in, clip.out};
    for (const Transition &t : model.sequence().transitions) {
        if (t.b == id) {
            base.position += t.extendB;
            base.in += t.extendB;
        }
        if (t.a == id)
            base.out -= t.extendA;
    }
    return base;
}

bool anyClipStartsAtOrAfter(const Model &model, TrackId track, FrameIndex from, ClipId except)
{
    for (ClipId id : model.track(track).clips) {
        if (id != except && model.clip(id).position >= from)
            return true;
    }
    return false;
}

} // namespace

// --- RippleDelete ----------------------------------------------------------

RippleDelete::RippleDelete(ClipId clip) : m_clip(clip) {}

bool RippleDelete::apply(Model &model)
{
    if (!model.hasClip(m_clip))
        return false;
    const TrackId track = model.clip(m_clip).track;
    // RemoveClip strips the clip's dissolves first, which returns its
    // neighbours to base geometry too: the hole to close is the clip's
    // base span.
    const BaseGeometry base = baseGeometry(model, m_clip);

    m_steps.clear();
    model.notify(BatchBegin{});
    bool ok = runStep(model, m_steps, std::make_unique<RemoveClip>(m_clip));
    // Decided after the removal: until then, a clip dissolving out of this
    // one still starts inside it (its extended position), not at base end.
    if (ok && anyClipStartsAtOrAfter(model, track, base.end(), m_clip))
        ok = runStep(model, m_steps, std::make_unique<ShiftClips>(track, base.end(), -base.length()));
    if (!ok) {
        revertSteps(model, m_steps);
        m_steps.clear();
    }
    model.notify(BatchEnd{});
    return ok;
}

void RippleDelete::revert(Model &model)
{
    model.notify(BatchBegin{});
    revertSteps(model, m_steps);
    model.notify(BatchEnd{});
}

// --- RippleTrim ------------------------------------------------------------

RippleTrim::RippleTrim(ClipId clip, Edge edge, FrameIndex delta) : m_clip(clip), m_edge(edge), m_delta(delta) {}

bool RippleTrim::apply(Model &model)
{
    if (m_delta == 0 || !model.hasClip(m_clip))
        return false;
    const TrackId track = model.clip(m_clip).track;
    if (model.track(track).locked)
        return false;
    const bool head = m_edge == Edge::Head;

    m_steps.clear();
    model.notify(BatchBegin{});
    auto fail = [&]() {
        revertSteps(model, m_steps);
        m_steps.clear();
        model.notify(BatchEnd{});
        return false;
    };

    // The dissolve on the trimmed edge can't survive the trim: remove it as
    // the first step so the rest works on base geometry at that edge.
    if (std::optional<TransitionId> t = transitionAtEdge(model, m_clip, head)) {
        if (!runStep(model, m_steps, std::make_unique<RemoveTransition>(*t)))
            return fail();
    }

    const Clip &clip = model.clip(m_clip);
    // Head: +delta trims the front (start stays put, content starts later);
    // tail: +delta extends the end. Either way the clip's end moves by
    // `lengthChange` and everything after it follows.
    const FrameIndex newIn = head ? clip.in + m_delta : clip.in;
    const FrameIndex newOut = head ? clip.out : clip.out + m_delta;
    const FrameIndex lengthChange = head ? -m_delta : m_delta;
    const FrameIndex oldEnd = clip.end();
    const bool anythingFollows = anyClipStartsAtOrAfter(model, track, oldEnd, m_clip);

    auto resize = std::make_unique<ResizeClip>(m_clip, newIn, newOut, clip.position);
    std::unique_ptr<Command> shift;
    if (anythingFollows)
        shift = std::make_unique<ShiftClips>(track, oldEnd, lengthChange);
    // Growing: make room first. Shrinking: shrink first, then close up.
    if (lengthChange > 0) {
        if (shift && !runStep(model, m_steps, std::move(shift)))
            return fail();
        if (!runStep(model, m_steps, std::move(resize)))
            return fail();
    } else {
        if (!runStep(model, m_steps, std::move(resize)))
            return fail();
        if (shift && !runStep(model, m_steps, std::move(shift)))
            return fail();
    }
    model.notify(BatchEnd{});
    return true;
}

void RippleTrim::revert(Model &model)
{
    model.notify(BatchBegin{});
    revertSteps(model, m_steps);
    model.notify(BatchEnd{});
}

// --- SlipClip --------------------------------------------------------------

SlipClip::SlipClip(ClipId clip, FrameIndex delta) : m_clip(clip), m_delta(delta) {}

bool SlipClip::apply(Model &model)
{
    if (m_delta == 0 || !model.hasClip(m_clip))
        return false;
    // Slipping changes the content under any dissolve on either edge, so
    // ResizeClip strips both (head and tail both change). Compute the new
    // window from the clip's base geometry, which is what it will have
    // once they're gone.
    const BaseGeometry base = baseGeometry(model, m_clip);
    const FrameIndex basePos = base.position, baseIn = base.in, baseOut = base.out;
    m_resize = std::make_unique<ResizeClip>(m_clip, baseIn + m_delta, baseOut + m_delta, basePos);
    return m_resize->apply(model);
}

void SlipClip::revert(Model &model)
{
    m_resize->revert(model);
}

// --- CopyClip --------------------------------------------------------------

CopyClip::CopyClip(ClipId source, TrackId track, FrameIndex pos) : m_source(source), m_track(track), m_pos(pos) {}

bool CopyClip::apply(Model &model)
{
    if (!model.hasClip(m_source) || !model.hasTrack(m_track))
        return false;
    const Clip source = model.clip(m_source);
    const bool toAudioTrack = model.track(m_track).kind == Track::Kind::Audio;
    if (toAudioTrack && !source.audioEnabled)
        return false; // a clip whose audio is off would be dead weight on an audio track

    // Base (un-extended) source range: a copy doesn't carry the dissolve.
    const BaseGeometry base = baseGeometry(model, m_source);
    const FrameIndex in = base.in, out = base.out;

    model.notify(BatchBegin{});
    m_insert = std::make_unique<InsertClip>(m_track, source.asset, m_pos, in, out);
    if (!m_insert->apply(model)) {
        model.notify(BatchEnd{});
        return false;
    }
    m_copy = m_insert->clipId();
    model.setClipName(m_copy, source.name);
    const Clip &copy = model.clip(m_copy);
    model.setClipEnabled(m_copy, copy.videoEnabled && source.videoEnabled, copy.audioEnabled && source.audioEnabled);
    model.notify(BatchEnd{});
    return true;
}

void CopyClip::revert(Model &model)
{
    m_insert->revert(model);
}

// --- Markers ---------------------------------------------------------------

AddMarker::AddMarker(FrameIndex at, std::string text) : m_at(at), m_text(std::move(text)) {}

bool AddMarker::apply(Model &model)
{
    if (m_at < 0)
        return false;
    m_id = model.addMarker(m_at, m_text, m_appliedBefore ? std::optional<MarkerId>(m_id) : std::nullopt);
    m_appliedBefore = true;
    return true;
}

void AddMarker::revert(Model &model)
{
    model.removeMarker(m_id);
}

RemoveMarker::RemoveMarker(MarkerId marker) : m_id(marker) {}

bool RemoveMarker::apply(Model &model)
{
    if (!model.hasMarker(m_id))
        return false;
    m_captured = model.marker(m_id);
    model.removeMarker(m_id);
    return true;
}

void RemoveMarker::revert(Model &model)
{
    model.addMarker(m_captured.at, m_captured.text, m_captured.id);
}

EditMarker::EditMarker(MarkerId marker, FrameIndex at, std::string text)
    : m_id(marker), m_at(at), m_text(std::move(text))
{}

bool EditMarker::apply(Model &model)
{
    if (!model.hasMarker(m_id) || m_at < 0)
        return false;
    const Marker &current = model.marker(m_id);
    if (current.at == m_at && current.text == m_text)
        return false;
    m_oldAt = current.at;
    m_oldText = current.text;
    model.setMarker(m_id, m_at, m_text);
    return true;
}

void EditMarker::revert(Model &model)
{
    model.setMarker(m_id, m_oldAt, m_oldText);
}

bool EditMarker::mergeWith(const Command &next)
{
    const auto *other = dynamic_cast<const EditMarker *>(&next);
    if (!other || other->m_id != m_id)
        return false;
    m_at = other->m_at;
    m_text = other->m_text;
    return true;
}

} // namespace ustudio::core
