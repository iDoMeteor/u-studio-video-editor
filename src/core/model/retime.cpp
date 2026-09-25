#include "core/model/retime.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>

namespace ustudio::core {

FrameIndex retimeFrame(FrameIndex frame, Rational from, Rational to)
{
    // frame * (to.num / to.den) / (from.num / from.den), rounded half up,
    // in integers. Fits 64 bits for any real timeline: a month at 60 fps
    // (1.6e8 frames) times 60000 * 1001 is about 1e16.
    const int64_t numerator = frame * to.num * from.den;
    const int64_t denominator = static_cast<int64_t>(to.den) * from.num;
    if (denominator <= 0)
        return frame;
    return (2 * numerator + denominator) / (2 * denominator);
}

namespace {

template <class At> void retimeKeyframes(std::vector<Keyframe> &keyframes, const At &at)
{
    for (Keyframe &keyframe : keyframes)
        keyframe.at = at(keyframe.at);
}

// Keyframes can lie before an owner's start after a split (negative):
// scaled as magnitudes so they stay mirrored around it.
template <class At> void retimeEffects(std::vector<Effect> &effects, const At &at)
{
    auto signedAt = [&at](FrameIndex frame) { return frame < 0 ? -at(-frame) : at(frame); };
    for (Effect &effect : effects) {
        for (Param &param : effect.params)
            retimeKeyframes(param.keyframes, signedAt);
        retimeKeyframes(effect.mix.keyframes, signedAt);
        if (effect.mask) {
            for (Param &param : effect.mask->params)
                retimeKeyframes(param.keyframes, signedAt);
            retimeKeyframes(effect.mask->feather.keyframes, signedAt);
        }
    }
}

} // namespace

Project retime(const Project &project, Rational fps)
{
    Project result = project;
    auto seqIt = std::find_if(result.sequences.begin(), result.sequences.end(),
                              [&](const Sequence &s) { return s.id == project.activeSequence; });
    if (seqIt == result.sequences.end() || fps.num <= 0 || fps.den <= 0)
        return result;
    Sequence &seq = *seqIt;
    const Rational from = seq.profile.fps;
    // The same rate however it's written (60/2 is 30/1): nothing to do.
    if (static_cast<int64_t>(from.num) * fps.den == static_cast<int64_t>(fps.num) * from.den)
        return result;
    auto at = [&](FrameIndex frame) { return retimeFrame(frame, from, fps); };

    // Clips: both ends and the in point rounded from their own times.
    const Sequence &old = *std::find_if(project.sequences.begin(), project.sequences.end(),
                                        [&](const Sequence &s) { return s.id == project.activeSequence; });
    std::unordered_map<AssetId, FrameIndex> assetNeeds;
    for (auto &[id, clip] : seq.clips) {
        const Clip &was = old.clips.at(id);
        const FrameIndex position = at(was.position);
        const FrameIndex length = std::max<FrameIndex>(1, at(was.end()) - position);
        clip.position = position;
        clip.in = at(was.in);
        clip.out = clip.in + length - 1;
        retimeEffects(clip.effects, at);
        for (Param &param : clip.sourceParams)
            retimeKeyframes(param.keyframes, at);
        for (std::optional<FadeSpec> *fade : {&clip.fadeIn, &clip.fadeOut})
            if (*fade)
                (*fade)->length = std::clamp<FrameIndex>(at((*fade)->length), 1, length);
        FrameIndex &need = assetNeeds[clip.asset];
        need = std::max(need, clip.out + 1);
    }
    for (Track &track : seq.tracks)
        retimeEffects(track.effects, at);
    retimeEffects(seq.effects, at);
    // Adjustment blocks: both ends rounded, like clips.
    for (AdjustmentBlock &block : seq.adjustmentBlocks) {
        const FrameIndex start = at(block.start);
        const FrameIndex end = at(block.end());
        block.start = start;
        block.length = std::max<FrameIndex>(1, end - start);
        retimeEffects(block.effects, at);
        for (std::optional<FadeSpec> *fade : {&block.fadeIn, &block.fadeOut})
            if (*fade)
                (*fade)->length = std::clamp<FrameIndex>(at((*fade)->length), 1, block.length);
    }
    for (Look &look : result.looks)
        retimeEffects(look.effects, at);

    // Dissolves: the overlap the rounded clips now have is the new length.
    std::vector<Transition> transitions;
    for (Transition transition : seq.transitions) {
        const Clip &a = seq.clips.at(transition.a);
        const Clip &b = seq.clips.at(transition.b);
        // Rounding is monotonic, so the overlap can shrink to nothing (the
        // clips then butt) but never go negative.
        const FrameIndex length = a.end() - b.position;
        if (length <= 0)
            continue; // too short to survive the new rate: a cut
        transition.length = length;
        transition.extendA = std::clamp<FrameIndex>(at(transition.extendA), 0, length);
        transition.extendB = length - transition.extendA;
        for (Param &param : transition.params)
            retimeKeyframes(param.keyframes, at);
        transitions.push_back(transition);
    }
    seq.transitions = std::move(transitions);

    for (Marker &marker : seq.markers)
        marker.at = at(marker.at);

    // Asset lengths at the new rate: rounded like everything else, and never
    // shorter than a clip now needs (rounding can ask for one frame more).
    for (Asset &asset : result.bin) {
        if (asset.info.lengthInSequenceFrames <= 0)
            continue;
        FrameIndex length = at(asset.info.lengthInSequenceFrames);
        if (auto need = assetNeeds.find(asset.id); need != assetNeeds.end())
            length = std::max(length, need->second);
        asset.info.lengthInSequenceFrames = length;
    }

    seq.profile.fps = fps;
    seq.profile.mltName.clear(); // a stock profile's name no longer describes it
    return result;
}

} // namespace ustudio::core
