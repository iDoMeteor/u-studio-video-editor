#include "undo_stack.h"

#include <cassert>

namespace ustudio::core {

bool UndoStack::execute(std::unique_ptr<Command> command)
{
    if (!command->apply(m_model))
        return false;

    m_redo.clear();

    if (!m_undo.empty() && m_undo.back()->mergeWith(*command)) {
        // `command`'s effect is already applied to the model (mergeWith
        // is asked to absorb it into the existing top entry, e.g. so a
        // slider drag is one undo step); the new object itself is
        // discarded here since the top entry now represents both.
    } else {
        m_undo.push_back(std::move(command));
        if (m_undo.size() > limit) {
            m_undo.erase(m_undo.begin());
            if (m_cleanDepth > 0)
                --m_cleanDepth; // the clean point just shifted down by one dropped entry
        }
    }

    changed.emit();
    return true;
}

bool UndoStack::undo()
{
    if (m_undo.empty())
        return false;

    std::unique_ptr<Command> command = std::move(m_undo.back());
    m_undo.pop_back();
    command->revert(m_model);
    m_redo.push_back(std::move(command));

    changed.emit();
    return true;
}

void UndoStack::clear()
{
    m_undo.clear();
    m_redo.clear();
    m_cleanDepth = 0;
    changed.emit();
}

bool UndoStack::redo()
{
    if (m_redo.empty())
        return false;

    std::unique_ptr<Command> command = std::move(m_redo.back());
    m_redo.pop_back();
    bool reapplied = command->apply(m_model);
    assert(reapplied && "UndoStack::redo: re-applying a previously-successful command failed");
    (void)reapplied;
    m_undo.push_back(std::move(command));

    changed.emit();
    return true;
}

} // namespace ustudio::core
