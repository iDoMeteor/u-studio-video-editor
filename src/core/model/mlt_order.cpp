#include "mlt_order.h"

namespace ustudio::core {

std::vector<TrackId> mltTrackOrder(const Sequence &sequence)
{
    std::vector<TrackId> order;

    for (const Track &track : sequence.tracks) {
        if (track.kind == Track::Kind::Audio)
            order.push_back(track.id);
    }
    for (auto it = sequence.tracks.rbegin(); it != sequence.tracks.rend(); ++it) {
        if (it->kind == Track::Kind::Video)
            order.push_back(it->id);
    }

    return order;
}

} // namespace ustudio::core
