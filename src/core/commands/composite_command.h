#pragma once

#include "command.h"
#include "core/model/model.h"

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
        : m_label(std::move(label)), m_commands(std::move(commands))
    {}

    std::string label() const override
    {
        return m_label;
    }

    bool apply(Model &model) override
    {
        size_t appliedCount = 0;
        for (auto &command : m_commands) {
            if (!command->apply(model)) {
                for (size_t i = appliedCount; i-- > 0;)
                    m_commands[i]->revert(model);
                return false;
            }
            ++appliedCount;
        }
        return true;
    }

    void revert(Model &model) override
    {
        for (size_t i = m_commands.size(); i-- > 0;)
            m_commands[i]->revert(model);
    }

  private:
    std::string m_label;
    std::vector<std::unique_ptr<Command>> m_commands;
};

} // namespace ustudio::core
