#pragma once

#include "command.h"
#include "core/model/model.h"

#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace ustudio::core {

// Runs a fixed list of primitive commands as one undo step (doc 04's
// "composite commands" pattern). Unlike Transaction (core/commands/
// transaction.h), which consumes its commands on a single use and is meant
// for one-shot UI batch gestures, CompositeCommand keeps its sub-commands
// for its own whole lifetime so undo/redo can apply()/revert() them
// repeatedly -- each sub-command's own reuseId-on-redo logic just works,
// unmodified.
class CompositeCommand : public Command
{
  public:
    CompositeCommand(std::string label, std::vector<std::unique_ptr<Command>> commands)
        : m_label(std::move(label)),
          m_commands(std::make_move_iterator(commands.begin()), std::make_move_iterator(commands.end()))
    {}
    // A batch member: another CompositeCommand with the same non-zero
    // `mergeKey` executed right after this one joins it as one undo step
    // (a multi-file import is one step, not one per file), which is then
    // labelled `batchLabel`.
    CompositeCommand(std::string label, std::vector<std::unique_ptr<Command>> commands, uint64_t mergeKey,
                     std::string batchLabel)
        : m_label(std::move(label)),
          m_commands(std::make_move_iterator(commands.begin()), std::make_move_iterator(commands.end())),
          m_mergeKey(mergeKey), m_batchLabel(std::move(batchLabel))
    {}

    std::string label() const override
    {
        return m_merged ? m_batchLabel : m_label;
    }

    // `next` is already applied (UndoStack::execute()); taking its commands
    // after ours keeps revert() in reverse order across the whole batch.
    bool mergeWith(const Command &next) override
    {
        const auto *other = dynamic_cast<const CompositeCommand *>(&next);
        if (m_mergeKey == 0 || !other || other->m_mergeKey != m_mergeKey)
            return false;
        // Shared, not moved: `next` is const here, and UndoStack drops it
        // straight after a merge.
        m_commands.insert(m_commands.end(), other->m_commands.begin(), other->m_commands.end());
        m_merged = true;
        return true;
    }

    // Wrapped in BatchBegin/BatchEnd (doc 04/05) so a listener projecting
    // model events into MLT (EngineSync) coalesces the whole group into one
    // resync instead of one per sub-command.
    bool apply(Model &model) override
    {
        model.notify(BatchBegin{});
        size_t appliedCount = 0;
        bool ok = true;
        for (auto &command : m_commands) {
            if (!command->apply(model)) {
                for (size_t i = appliedCount; i-- > 0;)
                    m_commands[i]->revert(model);
                ok = false;
                break;
            }
            ++appliedCount;
        }
        model.notify(BatchEnd{});
        return ok;
    }

    void revert(Model &model) override
    {
        model.notify(BatchBegin{});
        for (size_t i = m_commands.size(); i-- > 0;)
            m_commands[i]->revert(model);
        model.notify(BatchEnd{});
    }

  private:
    std::string m_label;
    // Shared so a batch can take a merged member's commands (mergeWith()).
    std::vector<std::shared_ptr<Command>> m_commands;
    uint64_t m_mergeKey = 0;
    std::string m_batchLabel;
    bool m_merged = false;
};

} // namespace ustudio::core
