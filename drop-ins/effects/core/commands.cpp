#include "core/commands.h"

#include <algorithm>

namespace ustudio::effects {

namespace {

bool keyframesSorted(const std::vector<core::Keyframe> &keyframes)
{
    for (size_t i = 1; i < keyframes.size(); ++i)
        if (keyframes[i].at <= keyframes[i - 1].at)
            return false;
    return true;
}

bool mixValid(const core::KeyframedValue &mix)
{
    if (mix.value < 0.0 || mix.value > 1.0 || !keyframesSorted(mix.keyframes))
        return false;
    return std::all_of(mix.keyframes.begin(), mix.keyframes.end(),
                       [](const core::Keyframe &k) { return k.value >= 0.0 && k.value <= 1.0; });
}

} // namespace

std::string AddEffect::label() const
{
    return "Add " + (m_effect.displayName.empty() ? std::string("effect") : m_effect.displayName);
}

bool AddEffect::apply(core::Model &model)
{
    if (!model.hasEffectTarget(m_target) || m_effect.service.empty())
        return false;
    for (const core::Param &param : m_effect.params)
        if (!keyframesSorted(param.keyframes))
            return false;
    if (!mixValid(m_effect.mix))
        return false;
    const size_t index = std::min(m_index, model.effects(m_target).size());
    m_id = model.addEffect(m_target, m_effect, index, m_id.isValid() ? std::optional(m_id) : std::nullopt);
    return true;
}

void AddEffect::revert(core::Model &model)
{
    model.removeEffect(m_id);
}

bool RemoveEffect::apply(core::Model &model)
{
    auto found = model.findEffect(m_id);
    if (!found)
        return false;
    m_target = found->first;
    m_index = found->second;
    m_removed = model.removeEffect(m_id);
    return true;
}

void RemoveEffect::revert(core::Model &model)
{
    model.addEffect(m_target, m_removed, m_index, m_id);
}

bool MoveEffect::apply(core::Model &model)
{
    auto found = model.findEffect(m_id);
    if (!found || found->second == m_index || m_index >= model.effects(found->first).size())
        return false;
    m_oldIndex = found->second;
    model.moveEffect(m_id, m_index);
    return true;
}

void MoveEffect::revert(core::Model &model)
{
    model.moveEffect(m_id, m_oldIndex);
}

bool SetEffectEnabled::apply(core::Model &model)
{
    if (!model.hasEffect(m_id) || model.effect(m_id).enabled == m_enabled)
        return false;
    m_old = model.effect(m_id).enabled;
    model.setEffectEnabled(m_id, m_enabled);
    return true;
}

void SetEffectEnabled::revert(core::Model &model)
{
    model.setEffectEnabled(m_id, m_old);
}

std::string SetParam::label() const
{
    return m_param.keyframes.empty() ? "Set effect parameter" : "Set keyframes";
}

bool SetParam::apply(core::Model &model)
{
    if (!model.hasEffect(m_id) || m_param.name.empty() || !keyframesSorted(m_param.keyframes))
        return false;
    // Keyframes animate numbers only (core::Keyframe::value).
    if (!m_param.keyframes.empty() && !std::holds_alternative<double>(m_param.value))
        return false;
    const core::Effect &effect = model.effect(m_id);
    auto it = std::find_if(effect.params.begin(), effect.params.end(),
                           [&](const core::Param &p) { return p.name == m_param.name; });
    if (it != effect.params.end()) {
        m_old = *it;
        m_place.reset();
    } else {
        m_old.reset();
        m_place = model.findEffect(m_id);
        m_oldEffect = effect;
    }
    model.setEffectParam(m_id, m_param);
    return true;
}

void SetParam::revert(core::Model &model)
{
    if (m_old) {
        model.setEffectParam(m_id, *m_old);
        return;
    }
    model.removeEffect(m_id);
    model.addEffect(m_place->first, m_oldEffect, m_place->second, m_id);
}

bool SetParam::mergeWith(const core::Command &next)
{
    const auto *other = dynamic_cast<const SetParam *>(&next);
    if (!other || m_gesture == 0 || other->m_gesture != m_gesture || other->m_id != m_id ||
        other->m_param.name != m_param.name)
        return false;
    m_param = other->m_param; // keep the old value: the gesture's start
    return true;
}

bool SetParam::isNoOp() const
{
    return m_old && *m_old == m_param;
}

bool SetMix::apply(core::Model &model)
{
    if (!model.hasEffect(m_id) || !mixValid(m_mix))
        return false;
    m_old = model.effect(m_id).mix;
    model.setEffectMix(m_id, m_mix);
    return true;
}

void SetMix::revert(core::Model &model)
{
    model.setEffectMix(m_id, m_old);
}

bool SetMix::mergeWith(const core::Command &next)
{
    const auto *other = dynamic_cast<const SetMix *>(&next);
    if (!other || m_gesture == 0 || other->m_gesture != m_gesture || other->m_id != m_id)
        return false;
    m_mix = other->m_mix;
    return true;
}

bool SetMix::isNoOp() const
{
    return m_old == m_mix;
}

} // namespace ustudio::effects
