#include "effect_commands.h"

#include <algorithm>

namespace ustudio::testdropin {

namespace {
bool keyframesSorted(const std::vector<core::Keyframe> &keyframes)
{
    for (size_t i = 1; i < keyframes.size(); ++i)
        if (keyframes[i].at <= keyframes[i - 1].at)
            return false;
    return true;
}
} // namespace

bool AddEffect::apply(Model &model)
{
    if (!model.hasEffectTarget(m_target))
        return false;
    m_id = model.addEffect(m_target, m_effect, m_index, m_id.isValid() ? std::optional(m_id) : std::nullopt);
    return true;
}

void AddEffect::revert(Model &model)
{
    model.removeEffect(m_id);
}

bool RemoveEffect::apply(Model &model)
{
    auto found = model.findEffect(m_id);
    if (!found)
        return false;
    m_target = found->first;
    m_index = found->second;
    m_removed = model.removeEffect(m_id);
    return true;
}

void RemoveEffect::revert(Model &model)
{
    model.addEffect(m_target, m_removed, m_index, m_id);
}

bool MoveEffect::apply(Model &model)
{
    auto found = model.findEffect(m_id);
    if (!found || found->second == m_index || m_index >= model.effects(found->first).size())
        return false;
    m_oldIndex = found->second;
    model.moveEffect(m_id, m_index);
    return true;
}

void MoveEffect::revert(Model &model)
{
    model.moveEffect(m_id, m_oldIndex);
}

bool SetEffectEnabled::apply(Model &model)
{
    if (!model.hasEffect(m_id))
        return false;
    m_old = model.effect(m_id).enabled;
    model.setEffectEnabled(m_id, m_enabled);
    return true;
}

void SetEffectEnabled::revert(Model &model)
{
    model.setEffectEnabled(m_id, m_old);
}

bool SetParam::apply(Model &model)
{
    if (!model.hasEffect(m_id) || !keyframesSorted(m_param.keyframes))
        return false;
    const auto &params = model.effect(m_id).params;
    auto it = std::find_if(params.begin(), params.end(), [&](const core::Param &p) { return p.name == m_param.name; });
    if (it == params.end())
        return false; // parameters come with the effect; this only changes them
    m_old = *it;
    model.setEffectParam(m_id, m_param);
    return true;
}

void SetParam::revert(Model &model)
{
    model.setEffectParam(m_id, m_old);
}

bool SetParam::mergeWith(const core::Command &next)
{
    const auto *other = dynamic_cast<const SetParam *>(&next);
    if (!other || other->m_id != m_id || other->m_param.name != m_param.name)
        return false;
    m_param = other->m_param; // keep m_old: the drag's start
    return true;
}

bool SetMix::apply(Model &model)
{
    if (!model.hasEffect(m_id) || !keyframesSorted(m_mix.keyframes) || m_mix.value < 0.0 || m_mix.value > 1.0)
        return false;
    m_old = model.effect(m_id).mix;
    model.setEffectMix(m_id, m_mix);
    return true;
}

void SetMix::revert(Model &model)
{
    model.setEffectMix(m_id, m_old);
}

bool SetMask::apply(Model &model)
{
    if (!model.hasEffect(m_id))
        return false;
    m_old = model.effect(m_id).mask;
    model.setEffectMask(m_id, m_mask);
    return true;
}

void SetMask::revert(Model &model)
{
    model.setEffectMask(m_id, m_old);
}

namespace {
bool laneFree(const Model &model, int lane, core::FrameIndex start, core::FrameIndex length,
              core::AdjustmentBlockId ignore = {})
{
    for (const core::AdjustmentBlock &other : model.sequence().adjustmentBlocks)
        if (other.id != ignore && other.lane == lane && other.start < start + length && start < other.end())
            return false;
    return true;
}
} // namespace

bool AddAdjustmentBlock::apply(Model &model)
{
    if (m_block.lane < 0 || m_block.start < 0 || m_block.length <= 0 ||
        !laneFree(model, m_block.lane, m_block.start, m_block.length))
        return false;
    m_id = model.addAdjustmentBlock(m_block, m_id.isValid() ? std::optional(m_id) : std::nullopt);
    m_block = model.adjustmentBlock(m_id); // with its effects' ids, for redo
    return true;
}

void AddAdjustmentBlock::revert(Model &model)
{
    model.removeAdjustmentBlock(m_id);
}

bool RemoveAdjustmentBlock::apply(Model &model)
{
    if (!model.hasAdjustmentBlock(m_id))
        return false;
    m_removed = model.removeAdjustmentBlock(m_id);
    return true;
}

void RemoveAdjustmentBlock::revert(Model &model)
{
    model.addAdjustmentBlock(m_removed, m_id);
}

bool MoveAdjustmentBlock::apply(Model &model)
{
    if (!model.hasAdjustmentBlock(m_id) || m_lane < 0 || m_start < 0 || m_length <= 0 ||
        !laneFree(model, m_lane, m_start, m_length, m_id))
        return false;
    const core::AdjustmentBlock &block = model.adjustmentBlock(m_id);
    m_oldLane = block.lane;
    m_oldStart = block.start;
    m_oldLength = block.length;
    model.setAdjustmentBlockRange(m_id, m_lane, m_start, m_length);
    return true;
}

void MoveAdjustmentBlock::revert(Model &model)
{
    model.setAdjustmentBlockRange(m_id, m_oldLane, m_oldStart, m_oldLength);
}

bool AddLook::apply(Model &model)
{
    m_id = model.addLook(m_look, m_id.isValid() ? std::optional(m_id) : std::nullopt);
    for (const core::Look &look : model.project().looks)
        if (look.id == m_id)
            m_look = look; // with its effects' ids, for redo
    return true;
}

void AddLook::revert(Model &model)
{
    model.removeLook(m_id);
}

bool SetClipSource::apply(Model &model)
{
    if (!model.hasClip(m_clip))
        return false;
    for (const core::Param &param : m_params)
        if (!keyframesSorted(param.keyframes))
            return false;
    m_old = model.clip(m_clip).sourceParams;
    model.setClipSourceParams(m_clip, m_params);
    return true;
}

void SetClipSource::revert(Model &model)
{
    model.setClipSourceParams(m_clip, m_old);
}

} // namespace ustudio::testdropin
