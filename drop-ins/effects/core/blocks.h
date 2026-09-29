#pragma once

// Adjustment blocks (doc 15, "FX lane", FX4): effects on everything beneath
// them for a time range. A block on lane k sits above model row k (top = 0)
// and affects the video tracks at rows >= k; lane 0 is the FX lane above
// every track. Its fades ramp the block's effects in and out.

#include "core/commands/command.h"
#include "core/model/model.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ustudio::effects {

// Why `block` can't be where it says ("" when it can): a negative start, no
// length, a lane past the tracks, or overlapping another block (other than
// `self`) on its lane.
std::string blockProblem(const core::Model &model, const core::AdjustmentBlock &block,
                         std::optional<core::AdjustmentBlockId> self = std::nullopt);

// The block's effects as they play: core::blockEffects()
// (core/model/effect_native.h, shared with the project writer).

class AddAdjustmentBlock : public core::Command
{
  public:
    explicit AddAdjustmentBlock(core::AdjustmentBlock block) : m_block(std::move(block)) {}
    std::string label() const override
    {
        return "Add adjustment block";
    }
    bool apply(core::Model &model) override;
    void revert(core::Model &model) override;
    core::AdjustmentBlockId id() const
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
    bool apply(core::Model &model) override;
    void revert(core::Model &model) override;

  private:
    core::AdjustmentBlockId m_id;
    core::AdjustmentBlock m_removed;
};

// Moves, resizes or changes the lane of a block; a drag's changes (one
// gesture) merge. Fades shorten to fit a shorter block.
class SetAdjustmentBlockRange : public core::Command
{
  public:
    SetAdjustmentBlockRange(core::AdjustmentBlockId id, int lane, core::FrameIndex start, core::FrameIndex length,
                            uint64_t gesture = 0)
        : m_id(id), m_lane(lane), m_start(start), m_length(length), m_gesture(gesture)
    {}
    std::string label() const override
    {
        return "Move adjustment block";
    }
    bool apply(core::Model &model) override;
    void revert(core::Model &model) override;
    bool mergeWith(const core::Command &next) override;
    bool isNoOp() const override;

  private:
    core::AdjustmentBlockId m_id;
    int m_lane;
    core::FrameIndex m_start, m_length;
    uint64_t m_gesture;
    core::AdjustmentBlock m_old;
};

// A block's fade in and out (0 or nullopt: none); a drag's changes merge.
class SetAdjustmentBlockFades : public core::Command
{
  public:
    SetAdjustmentBlockFades(core::AdjustmentBlockId id, std::optional<core::FadeSpec> fadeIn,
                            std::optional<core::FadeSpec> fadeOut, uint64_t gesture = 0)
        : m_id(id), m_in(fadeIn), m_out(fadeOut), m_gesture(gesture)
    {}
    std::string label() const override
    {
        return "Fade adjustment block";
    }
    bool apply(core::Model &model) override;
    void revert(core::Model &model) override;
    bool mergeWith(const core::Command &next) override;
    bool isNoOp() const override;

  private:
    core::AdjustmentBlockId m_id;
    std::optional<core::FadeSpec> m_in, m_out, m_oldIn, m_oldOut;
    uint64_t m_gesture;
};

} // namespace ustudio::effects
