#pragma once

#include "command.h"
#include "core/model/model.h"
#include "core/model/signal.h"

#include <memory>
#include <string>
#include <vector>

namespace ustudio::core {

class UndoStack
{
  public:
    explicit UndoStack(Model &model) : m_model(model) {}

    // Applies `command`; on success, pushes it (or merges it into the top
    // of the undo stack) and clears the redo stack. On refusal (apply()
    // returned false), the model is untouched and nothing is pushed.
    bool execute(std::unique_ptr<Command> command);

    bool undo();
    bool redo();
    // Drops all history without touching the model -- for "Open Project":
    // the new project's edits must never be undoable back into the one
    // that was just replaced.
    void clear();
    bool canUndo() const
    {
        return !m_undo.empty();
    }
    bool canRedo() const
    {
        return !m_redo.empty();
    }
    std::string undoLabel() const
    {
        return m_undo.empty() ? std::string{} : m_undo.back()->label();
    }
    std::string redoLabel() const
    {
        return m_redo.empty() ? std::string{} : m_redo.back()->label();
    }

    // Dirty-flag tracking (Qt QUndoStack-style depth marker): isClean() is
    // true exactly when the undo stack is back at the depth it was at the
    // last setCleanPoint() call (e.g. the last save).
    void setCleanPoint()
    {
        m_cleanDepth = m_undo.size();
    }
    bool isClean() const
    {
        return m_undo.size() == m_cleanDepth;
    }

    Signal<> changed;
    size_t limit = 500; // oldest entries dropped beyond this

  private:
    Model &m_model;
    std::vector<std::unique_ptr<Command>> m_undo;
    std::vector<std::unique_ptr<Command>> m_redo;
    size_t m_cleanDepth = 0;
};

} // namespace ustudio::core
