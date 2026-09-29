#include "core/commands.h"

#include "core/descriptor.h"

#include <algorithm>
#include <cstdint>

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

bool SetEffectMask::apply(core::Model &model)
{
    if (!model.hasEffect(m_id))
        return false;
    if (m_mask && m_mask->shape != "rectangle" && m_mask->shape != "ellipse")
        return false;
    m_old = model.effect(m_id).mask;
    model.setEffectMask(m_id, m_mask);
    return true;
}

void SetEffectMask::revert(core::Model &model)
{
    model.setEffectMask(m_id, m_old);
}

bool SetEffectMask::mergeWith(const core::Command &next)
{
    const auto *other = dynamic_cast<const SetEffectMask *>(&next);
    if (!other || m_gesture == 0 || other->m_gesture != m_gesture || other->m_id != m_id)
        return false;
    m_mask = other->m_mask;
    return true;
}

bool SetEffectMask::isNoOp() const
{
    return m_mask == m_old;
}

std::unique_ptr<core::Command> pasteEffects(const core::Model &model, const std::vector<Target> &targets,
                                            const std::vector<core::Effect> &effects, PasteMode mode, std::string label)
{
    std::vector<std::unique_ptr<core::Command>> commands;
    for (const Target &target : targets) {
        if (!model.hasEffectTarget(target))
            continue;
        if (mode == PasteMode::Replace)
            for (const core::Effect &existing : model.effects(target))
                if (existing.owner == kOwner)
                    commands.push_back(std::make_unique<RemoveEffect>(existing.id));
        for (core::Effect effect : effects) {
            effect.id = {};
            // At the end, whatever the removals above left (AddEffect clamps).
            commands.push_back(std::make_unique<AddEffect>(target, std::move(effect), SIZE_MAX));
        }
    }
    return std::make_unique<core::CompositeCommand>(std::move(label), std::move(commands));
}

std::unique_ptr<core::Command> setParamOnAll(const std::vector<core::EffectId> &effects, const core::Param &param,
                                             uint64_t gesture)
{
    std::vector<std::unique_ptr<core::Command>> commands;
    for (core::EffectId id : effects)
        commands.push_back(std::make_unique<SetParam>(id, param));
    return std::make_unique<core::CompositeCommand>("Set effect parameter", std::move(commands), gesture,
                                                    "Set effect parameter");
}

std::unique_ptr<core::Command> setMixOnAll(const core::Model &model, const std::vector<core::EffectId> &effects,
                                           double mix, uint64_t gesture)
{
    std::vector<std::unique_ptr<core::Command>> commands;
    for (core::EffectId id : effects) {
        if (!model.hasEffect(id))
            continue;
        core::KeyframedValue value = model.effect(id).mix;
        value.value = mix;
        commands.push_back(std::make_unique<SetMix>(id, value));
    }
    return std::make_unique<core::CompositeCommand>("Set effect mix", std::move(commands), gesture, "Set effect mix");
}

bool SaveLook::apply(core::Model &model)
{
    if (m_look.name.empty() || m_look.effects.empty())
        return false;
    for (core::Effect &effect : m_look.effects)
        effect.id = {};
    m_id = model.addLook(m_look, m_id.isValid() ? std::optional(m_id) : std::nullopt);
    return true;
}

void SaveLook::revert(core::Model &model)
{
    model.removeLook(m_id);
}

bool DeleteLook::apply(core::Model &model)
{
    if (!model.hasLook(m_id))
        return false;
    const auto &looks = model.project().looks;
    for (size_t i = 0; i < looks.size(); ++i)
        if (looks[i].id == m_id)
            m_index = i;
    m_removed = model.removeLook(m_id);
    return true;
}

void DeleteLook::revert(core::Model &model)
{
    // Model::addLook() appends: take the looks that followed it off, and
    // put everything back in the old order, each under its own id.
    std::vector<core::Look> after;
    const std::vector<core::Look> &looks = model.project().looks;
    for (size_t i = m_index; i < looks.size(); ++i)
        after.push_back(looks[i]);
    for (const core::Look &look : after)
        model.removeLook(look.id);
    model.addLook(m_removed, m_id);
    for (const core::Look &look : after)
        model.addLook(look, look.id);
}

} // namespace ustudio::effects
