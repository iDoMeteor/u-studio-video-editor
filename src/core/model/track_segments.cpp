#include "track_segments.h"

namespace ustudio::core {

std::vector<TrackSegment> planTrackSegments(const Model &model, const Track &modelTrack)
{
    std::vector<TrackSegment> segments;
    const Sequence &seq = model.sequence();
    const auto &clips = modelTrack.clips; // sorted by position (Model invariant)

    for (size_t i = 0; i < clips.size(); ++i) {
        ClipId clipId = clips[i];
        const Clip &clip = model.clip(clipId);

        // Is this clip the `b` side of a transition with the PREVIOUS
        // clip? If so, its head is already spoken for by that
        // transition's sub-tractor segment (appended below when the
        // previous clip was processed) -- only its own exclusive tail
        // (if any) needs a segment here.
        const Transition *incoming = nullptr;
        if (i > 0) {
            ClipId previous = clips[i - 1];
            for (const auto &t : seq.transitions) {
                if (t.track == modelTrack.id && t.a == previous && t.b == clipId) {
                    incoming = &t;
                    break;
                }
            }
        }
        const Transition *outgoing = nullptr;
        if (i + 1 < clips.size()) {
            ClipId next = clips[i + 1];
            for (const auto &t : seq.transitions) {
                if (t.track == modelTrack.id && t.a == clipId && t.b == next) {
                    outgoing = &t;
                    break;
                }
            }
        }

        // Trimmed at the head if a transition already consumed it (that
        // clip was `incoming`'s `a`) and/or at the tail if this clip
        // starts one of its own (`outgoing`) -- either, both, or neither.
        FrameIndex segStart = incoming ? clip.position + incoming->length : clip.position;
        FrameIndex segIn = incoming ? clip.in + incoming->length : clip.in;
        FrameIndex segEnd = outgoing ? clip.end() - outgoing->length : clip.end();
        FrameIndex segOut = outgoing ? clip.out - outgoing->length : clip.out;
        if (segStart < segEnd) { // omitted entirely if transition(s) consumed the whole clip
            TrackSegment seg;
            seg.kind = TrackSegment::Kind::Clip;
            seg.start = segStart;
            seg.length = segEnd - segStart;
            seg.clip = clipId;
            seg.in = segIn;
            seg.out = segOut;
            segments.push_back(seg);
        }

        if (outgoing) {
            TrackSegment seg;
            seg.kind = TrackSegment::Kind::Transition;
            seg.start = segEnd; // == the next clip's (already-adjusted) position
            seg.length = outgoing->length;
            seg.transition = outgoing->id;
            seg.a = clipId;
            seg.b = clips[i + 1];
            segments.push_back(seg);
        }
    }

    return segments;
}

} // namespace ustudio::core
