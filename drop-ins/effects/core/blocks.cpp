#include "core/blocks.h"

#include "core/model/animation.h"

#include <algorithm>
#include <set>

namespace ustudio::effects {

std::string blockProblem(const core::Model &model, const core::AdjustmentBlock &block,
                         std::optional<core::AdjustmentBlockId> self)
{
    if (block.start < 0)
        return "starts before the sequence";
    if (block.length <= 0)
        return "has no length";
    const int tracks = static_cast<int>(model.sequence().tracks.size());
    if (block.lane < 0 || block.lane > std::max(tracks - 1, 0))
        return "is on a lane past the tracks";
    for (const core::AdjustmentBlock &other : model.sequence().adjustmentBlocks) {
        if (self && other.id == *self)
            continue;
        if (other.lane == block.lane && other.start < block.end() && block.start < other.end())
            return "overlaps another block on its lane";
    }
    return {};
}

namespace {

// The fade envelope at `frame` of a block `length` long.
double envelope(const core::AdjustmentBlock &block, double frame)
{
    double gain = 1.0;
    const double last = static_cast<double>(block.length - 1);
    if (block.fadeIn && block.fadeIn->length > 0)
        gain = std::min(gain, frame / static_cast<double>(block.fadeIn->length));
    if (block.fadeOut && block.fadeOut->length > 0)
        gain = std::min(gain, (last - frame) / static_cast<double>(block.fadeOut->length));
    return std::clamp(gain, 0.0, 1.0);
}

} // namespace

std::vector<core::Effect> blockEffects(const core::AdjustmentBlock &block)
{
    std::vector<core::Effect> effects = block.effects;
    const bool fadeIn = block.fadeIn && block.fadeIn->length > 0;
    const bool fadeOut = block.fadeOut && block.fadeOut->length > 0;
    if (!fadeIn && !fadeOut)
        return effects;
    // Keys where the envelope bends and wherever the mix had its own, each
    // the mix there times the envelope (linear between: the envelope is
    // linear, and the mix's own keys are kept as points).
    const core::FrameIndex last = block.length - 1;
    for (core::Effect &effect : effects) {
        std::set<core::FrameIndex> at{0, last};
        if (fadeIn)
            at.insert(std::min(block.fadeIn->length, last));
        if (fadeOut)
            at.insert(std::max<core::FrameIndex>(last - block.fadeOut->length, 0));
        for (const core::Keyframe &key : effect.mix.keyframes)
            if (key.at >= 0 && key.at <= last)
                at.insert(key.at);
        std::vector<core::Keyframe> keys;
        for (core::FrameIndex frame : at) {
            const double mix = effect.mix.keyframes.empty()
                                   ? effect.mix.value
                                   : core::easedValue(effect.mix.keyframes, static_cast<double>(frame));
            keys.push_back({frame, mix * envelope(block, static_cast<double>(frame)), core::Easing::Linear});
        }
        effect.mix.keyframes = std::move(keys);
    }
    return effects;
}

bool AddAdjustmentBlock::apply(core::Model &model)
{
    if (!blockProblem(model, m_block).empty())
        return false;
    // The first apply allocates the ids; redo keeps them (the block's and
    // its effects', which the model assigned).
    m_id = model.addAdjustmentBlock(m_block, m_id.isValid() ? std::optional(m_id) : std::nullopt);
    m_block = model.adjustmentBlock(m_id);
    return true;
}

void AddAdjustmentBlock::revert(core::Model &model)
{
    model.removeAdjustmentBlock(m_id);
}

bool RemoveAdjustmentBlock::apply(core::Model &model)
{
    if (!model.hasAdjustmentBlock(m_id))
        return false;
    m_removed = model.removeAdjustmentBlock(m_id);
    return true;
}

void RemoveAdjustmentBlock::revert(core::Model &model)
{
    model.addAdjustmentBlock(m_removed, m_id);
}

bool SetAdjustmentBlockRange::apply(core::Model &model)
{
    if (!model.hasAdjustmentBlock(m_id))
        return false;
    core::AdjustmentBlock moved = model.adjustmentBlock(m_id);
    m_old = moved;
    moved.lane = m_lane;
    moved.start = m_start;
    moved.length = m_length;
    if (!blockProblem(model, moved, m_id).empty())
        return false;
    model.setAdjustmentBlockRange(m_id, m_lane, m_start, m_length);
    // Fades never longer than the block.
    auto fit = [&](std::optional<core::FadeSpec> fade) {
        if (fade)
            fade->length = std::min(fade->length, m_length);
        return fade;
    };
    if (fit(m_old.fadeIn) != m_old.fadeIn || fit(m_old.fadeOut) != m_old.fadeOut)
        model.setAdjustmentBlockFades(m_id, fit(m_old.fadeIn), fit(m_old.fadeOut));
    return true;
}

void SetAdjustmentBlockRange::revert(core::Model &model)
{
    model.setAdjustmentBlockRange(m_id, m_old.lane, m_old.start, m_old.length);
    model.setAdjustmentBlockFades(m_id, m_old.fadeIn, m_old.fadeOut);
}

bool SetAdjustmentBlockRange::mergeWith(const core::Command &next)
{
    const auto *other = dynamic_cast<const SetAdjustmentBlockRange *>(&next);
    if (!other || m_gesture == 0 || other->m_gesture != m_gesture || other->m_id != m_id)
        return false;
    m_lane = other->m_lane;
    m_start = other->m_start;
    m_length = other->m_length;
    return true;
}

bool SetAdjustmentBlockRange::isNoOp() const
{
    return m_old.lane == m_lane && m_old.start == m_start && m_old.length == m_length;
}

bool SetAdjustmentBlockFades::apply(core::Model &model)
{
    if (!model.hasAdjustmentBlock(m_id))
        return false;
    const core::AdjustmentBlock &block = model.adjustmentBlock(m_id);
    auto valid = [&](const std::optional<core::FadeSpec> &fade) {
        return !fade || (fade->length >= 0 && fade->length <= block.length);
    };
    if (!valid(m_in) || !valid(m_out))
        return false;
    m_oldIn = block.fadeIn;
    m_oldOut = block.fadeOut;
    model.setAdjustmentBlockFades(m_id, m_in, m_out);
    return true;
}

void SetAdjustmentBlockFades::revert(core::Model &model)
{
    model.setAdjustmentBlockFades(m_id, m_oldIn, m_oldOut);
}

bool SetAdjustmentBlockFades::mergeWith(const core::Command &next)
{
    const auto *other = dynamic_cast<const SetAdjustmentBlockFades *>(&next);
    if (!other || m_gesture == 0 || other->m_gesture != m_gesture || other->m_id != m_id)
        return false;
    m_in = other->m_in;
    m_out = other->m_out;
    return true;
}

bool SetAdjustmentBlockFades::isNoOp() const
{
    return m_in == m_oldIn && m_out == m_oldOut;
}

} // namespace ustudio::effects
