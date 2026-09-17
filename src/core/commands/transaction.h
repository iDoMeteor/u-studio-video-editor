#pragma once

#include "command.h"
#include "core/model/model.h"

#include <memory>
#include <vector>

namespace ustudio::core {

// Groups primitive commands into one undo step, used by composite
// commands and by the app layer for "batch" gestures (doc 04). Wraps the
// group in BatchBegin/BatchEnd so the engine can defer rebuilding a track
// until the whole group lands.
class Transaction
{
  public:
    explicit Transaction(Model &model) : m_model(model)
    {
        m_model.notify(BatchBegin{});
    }

    // Safety net: if neither commit() nor rollback() was called (e.g. an
    // early return), unwind rather than leave a half-applied group.
    ~Transaction()
    {
        if (!m_committed && !m_rolledBack)
            rollback();
    }

    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;

    // Applies `command`; on failure, rolls back everything applied so far
    // in this transaction (including `command`'s partial effect, which
    // apply() guarantees is none) and returns false.
    bool run(std::unique_ptr<Command> command)
    {
        if (!command->apply(m_model)) {
            rollback();
            return false;
        }
        m_applied.push_back(std::move(command));
        return true;
    }

    void commit()
    {
        m_committed = true;
        m_model.notify(BatchEnd{});
    }

    void rollback()
    {
        if (m_rolledBack)
            return;
        for (auto it = m_applied.rbegin(); it != m_applied.rend(); ++it)
            (*it)->revert(m_model);
        m_applied.clear();
        m_rolledBack = true;
        m_model.notify(BatchEnd{});
    }

  private:
    Model &m_model;
    std::vector<std::unique_ptr<Command>> m_applied;
    bool m_committed = false;
    bool m_rolledBack = false;
};

} // namespace ustudio::core
