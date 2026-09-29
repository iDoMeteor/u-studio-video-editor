#include "mlt_order.h"

#include <algorithm>

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

AdjustmentLayers adjustmentLayers(const Sequence &sequence)
{
    AdjustmentLayers layers;
    for (const AdjustmentBlock &block : sequence.adjustmentBlocks) {
        if (block.lane <= 0 || std::find(layers.lanes.begin(), layers.lanes.end(), block.lane) != layers.lanes.end())
            continue;
        for (size_t row = static_cast<size_t>(block.lane); row < sequence.tracks.size(); ++row)
            if (sequence.tracks[row].kind == Track::Kind::Video) {
                layers.lanes.push_back(block.lane);
                break;
            }
    }
    std::sort(layers.lanes.begin(), layers.lanes.end());
    layers.layerTracks.resize(layers.lanes.size() + 1);
    for (TrackId id : mltTrackOrder(sequence)) {
        size_t layer = 0;
        for (size_t row = 0; row < sequence.tracks.size(); ++row) {
            if (sequence.tracks[row].id != id || sequence.tracks[row].kind != Track::Kind::Video)
                continue;
            for (size_t i = 0; i < layers.lanes.size(); ++i)
                if (static_cast<int>(row) >= layers.lanes[i])
                    layer = i + 1;
        }
        layers.layerTracks[layer].push_back(id);
    }
    return layers;
}

} // namespace ustudio::core
