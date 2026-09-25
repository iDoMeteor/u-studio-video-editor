#pragma once

// The test drop-in's core layer (IP1's consumer): effect commands built only
// on Model's IP1 mutators, the way drop-ins/effects/core will (doc 15,
// "Commands"). Each apply() validates first and changes nothing when it
// refuses; each revert() restores exactly.

#include "core/commands/command.h"
#include "core/model/model.h"

#include <optional>
#include <string>

namespace ustudio::testdropin {

using core::Model;
using Target = core::Model::EffectTarget;

class AddEffect : public core::Command
{
  public:
    AddEffect(Target target, core::Effect effect, size_t index)
        : m_target(target), m_effect(std::move(effect)), m_index(index)
    {}
    std::string label() const override
    {
        return "Add effect";
    }
    bool apply(Model &model) override;
    void revert(Model &model) override;
    core::EffectId effectId() const
    {
        return m_id;
    }

  private:
    Target m_target;
    core::Effect m_effect;
    size_t m_index;
    core::EffectId m_id;
};

class RemoveEffect : public core::Command
{
  public:
    explicit RemoveEffect(core::EffectId id) : m_id(id) {}
    std::string label() const override
    {
        return "Remove effect";
    }
    bool apply(Model &model) override;
    void revert(Model &model) override;

  private:
    core::EffectId m_id;
    Target m_target;
    size_t m_index = 0;
    core::Effect m_removed;
};

class MoveEffect : public core::Command
{
  public:
    MoveEffect(core::EffectId id, size_t index) : m_id(id), m_index(index) {}
    std::string label() const override
    {
        return "Move effect";
    }
    bool apply(Model &model) override;
    void revert(Model &model) override;

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
        return "Enable effect";
    }
    bool apply(Model &model) override;
    void revert(Model &model) override;

  private:
    core::EffectId m_id;
    bool m_enabled;
    bool m_old = true;
};

// Changes an existing parameter; merges while dragging the same one.
class SetParam : public core::Command
{
  public:
    SetParam(core::EffectId id, core::Param param) : m_id(id), m_param(std::move(param)) {}
    std::string label() const override
    {
        return "Set parameter";
    }
    bool apply(Model &model) override;
    void revert(Model &model) override;
    bool mergeWith(const core::Command &next) override;

  private:
    core::EffectId m_id;
    core::Param m_param;
    core::Param m_old;
};

class SetMix : public core::Command
{
  public:
    SetMix(core::EffectId id, core::KeyframedValue mix) : m_id(id), m_mix(std::move(mix)) {}
    std::string label() const override
    {
        return "Set mix";
    }
    bool apply(Model &model) override;
    void revert(Model &model) override;

  private:
    core::EffectId m_id;
    core::KeyframedValue m_mix, m_old;
};

class SetMask : public core::Command
{
  public:
    SetMask(core::EffectId id, std::optional<core::EffectMask> mask) : m_id(id), m_mask(std::move(mask)) {}
    std::string label() const override
    {
        return "Set mask";
    }
    bool apply(Model &model) override;
    void revert(Model &model) override;

  private:
    core::EffectId m_id;
    std::optional<core::EffectMask> m_mask, m_old;
};

class AddAdjustmentBlock : public core::Command
{
  public:
    explicit AddAdjustmentBlock(core::AdjustmentBlock block) : m_block(std::move(block)) {}
    std::string label() const override
    {
        return "Add adjustment block";
    }
    bool apply(Model &model) override;
    void revert(Model &model) override;
    core::AdjustmentBlockId blockId() const
    {
        return m_id;
    }

  private:
    core::AdjustmentBlock m_block;
    core::AdjustmentBlockId m_id;
};

class RemoveAdjustmentBlock : public core::Command
{
  public:
    explicit RemoveAdjustmentBlock(core::AdjustmentBlockId id) : m_id(id) {}
    std::string label() const override
    {
        return "Remove adjustment block";
    }
    bool apply(Model &model) override;
    void revert(Model &model) override;

  private:
    core::AdjustmentBlockId m_id;
    core::AdjustmentBlock m_removed;
};

class MoveAdjustmentBlock : public core::Command
{
  public:
    MoveAdjustmentBlock(core::AdjustmentBlockId id, int lane, core::FrameIndex start, core::FrameIndex length)
        : m_id(id), m_lane(lane), m_start(start), m_length(length)
    {}
    std::string label() const override
    {
        return "Move adjustment block";
    }
    bool apply(Model &model) override;
    void revert(Model &model) override;

  private:
    core::AdjustmentBlockId m_id;
    int m_lane;
    core::FrameIndex m_start, m_length;
    int m_oldLane = 0;
    core::FrameIndex m_oldStart = 0, m_oldLength = 0;
};

class AddLook : public core::Command
{
  public:
    explicit AddLook(core::Look look) : m_look(std::move(look)) {}
    std::string label() const override
    {
        return "Save look";
    }
    bool apply(Model &model) override;
    void revert(Model &model) override;

  private:
    core::Look m_look;
    core::LookId m_id;
};

class SetClipSource : public core::Command
{
  public:
    SetClipSource(core::ClipId clip, std::vector<core::Param> params) : m_clip(clip), m_params(std::move(params)) {}
    std::string label() const override
    {
        return "Edit source";
    }
    bool apply(Model &model) override;
    void revert(Model &model) override;

  private:
    core::ClipId m_clip;
    std::vector<core::Param> m_params, m_old;
};

} // namespace ustudio::testdropin
