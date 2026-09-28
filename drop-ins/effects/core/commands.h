#pragma once

// The effects drop-in's commands (doc 15, "Commands"), built only on
// Model's IP1 mutators. Each apply() validates first and changes nothing
// when it refuses; each revert() restores the model exactly (the property
// test in tests/test_core.cpp). Keyframes are part of a parameter, so
// setting, moving or re-easing one is a SetParam with the new list; FX2
// adds the convenience wrappers its keyframe UI needs.

#include "core/commands/command.h"
#include "core/model/model.h"

#include <cstdint>
#include <optional>
#include <string>

namespace ustudio::effects {

using Target = core::Model::EffectTarget;

class AddEffect : public core::Command
{
  public:
    AddEffect(Target target, core::Effect effect, size_t index)
        : m_target(target), m_effect(std::move(effect)), m_index(index)
    {}
    std::string label() const override;
    bool apply(core::Model &model) override;
    void revert(core::Model &model) override;
    core::EffectId effectId() const
    {
        return m_id;
    }

  private:
    Target m_target;
    core::Effect m_effect;
    size_t m_index;
    core::EffectId m_id; // kept across redo
};

class RemoveEffect : public core::Command
{
  public:
    explicit RemoveEffect(core::EffectId id) : m_id(id) {}
    std::string label() const override
    {
        return "Remove effect";
    }
    bool apply(core::Model &model) override;
    void revert(core::Model &model) override;

  private:
    core::EffectId m_id;
    Target m_target;
    size_t m_index = 0;
    core::Effect m_removed;
};

// Reorders within the effect's own stack.
class MoveEffect : public core::Command
{
  public:
    MoveEffect(core::EffectId id, size_t index) : m_id(id), m_index(index) {}
    std::string label() const override
    {
        return "Reorder effects";
    }
    bool apply(core::Model &model) override;
    void revert(core::Model &model) override;

  private:
    core::EffectId m_id;
    size_t m_index;
    size_t m_oldIndex = 0;
};

class SetEffectEnabled : public core::Command
{
  public:
    SetEffectEnabled(core::EffectId id, bool enabled) : m_id(id), m_enabled(enabled) {}
    std::string label() const override
    {
        return m_enabled ? "Enable effect" : "Bypass effect";
    }
    bool apply(core::Model &model) override;
    void revert(core::Model &model) override;

  private:
    core::EffectId m_id;
    bool m_enabled;
    bool m_old = true;
};

// Sets a parameter (value and keyframes), adding it when the effect doesn't
// carry it yet (a parameter left to the service's default until now).
// Commands with the same nonzero `gesture` on the same parameter merge:
// one slider drag, one undo step (doc 15, "One gesture, one undo").
class SetParam : public core::Command
{
  public:
    SetParam(core::EffectId id, core::Param param, uint64_t gesture = 0)
        : m_id(id), m_param(std::move(param)), m_gesture(gesture)
    {}
    std::string label() const override;
    bool apply(core::Model &model) override;
    void revert(core::Model &model) override;
    bool mergeWith(const core::Command &next) override;
    bool isNoOp() const override;

  private:
    core::EffectId m_id;
    core::Param m_param;
    uint64_t m_gesture;
    std::optional<core::Param> m_old; // none: the effect didn't have it
    // Adding a parameter can only be undone by restoring the whole effect
    // (Model has no remove-parameter mutator): where it was, as it was.
    std::optional<std::pair<Target, size_t>> m_place;
    core::Effect m_oldEffect;
};

// The wet/dry mix (0-1, keyframable). Merges like SetParam.
class SetMix : public core::Command
{
  public:
    SetMix(core::EffectId id, core::KeyframedValue mix, uint64_t gesture = 0)
        : m_id(id), m_mix(std::move(mix)), m_gesture(gesture)
    {}
    std::string label() const override
    {
        return "Set effect mix";
    }
    bool apply(core::Model &model) override;
    void revert(core::Model &model) override;
    bool mergeWith(const core::Command &next) override;
    bool isNoOp() const override;

  private:
    core::EffectId m_id;
    core::KeyframedValue m_mix, m_old;
    uint64_t m_gesture;
};

} // namespace ustudio::effects
