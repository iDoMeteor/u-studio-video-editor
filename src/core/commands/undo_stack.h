#pragma once

#include "command.h"
#include "core/model/model.h"
#include "core/model/signal.h"

#include <cstdint>
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
        return m_undo.empty() ? std::string{} : m_undo.back().command->label();
    }
    std::string redoLabel() const
    {
        return m_redo.empty() ? std::string{} : m_redo.back().command->label();
    }

    // Names the model's current state for dirty tracking: the serial of
    // the top undo entry, renewed whenever that entry's effect changes (a
    // merge), or of the base state below the stack when it's empty. Two
    // moments share a state() exactly when undo/redo alone moves between
    // them, so an async save (doc 19 MT1) can capture it on submit and mark
    // that state clean on completion: if the user edited meanwhile, the
    // project stays dirty, and undoing back to the saved state makes it
    // clean again. (A depth marker can't do this: undo below the save
    // point, then a new edit, reaches the saved depth with other content.)
    using State = uint64_t;
    State state() const
    {
        return m_undo.empty() ? m_baseState : m_undo.back().serial;
    }

    // Dirty-flag tracking: isClean() is true exactly when the model is in
    // the state of the last setCleanPoint() (e.g. the last save). Emits
    // `changed` too (audit A1): this flips isClean() exactly like an
    // execute()/undo()/redo() does, so a UI that only listens to `changed`
    // to refresh its dirty marker stays correct after a save.
    void setCleanPoint()
    {
        setCleanPoint(state());
    }
    // An async save's completion: `saved` is state() when it was submitted.
    void setCleanPoint(State saved)
    {
        m_cleanState = saved;
        changed.emit();
    }
    bool isClean() const
    {
        return state() == m_cleanState;
    }
    // Forces isClean() to false until the next setCleanPoint() (audit A2):
    // recovered content is unsaved even though its stack, like a freshly
    // opened project's, is empty.
    void markDirty()
    {
        m_cleanState = kNoState;
        changed.emit();
    }

    Signal<> changed;
    size_t limit = 500; // oldest entries dropped beyond this

  private:
    struct Entry
    {
        std::unique_ptr<Command> command;
        State serial;
    };
    static constexpr State kNoState = 0; // never a real state

    Model &m_model;
    std::vector<Entry> m_undo;
    std::vector<Entry> m_redo;
    State m_nextState = 2;
    State m_baseState = 1; // the state below the bottom entry
    State m_cleanState = 1;
};

} // namespace ustudio::core
