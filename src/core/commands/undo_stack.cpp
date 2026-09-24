#include "undo_stack.h"

#include "core/trace.h"

#include <cassert>

namespace ustudio::core {

bool UndoStack::execute(std::unique_ptr<Command> command)
{
    core::trace::Scope trace([&] { return "execute: " + command->label(); });
    if (!command->apply(m_model))
        return false;

    m_redo.clear();

    // Never merge into the top entry at the clean point (doc 13's C2): the
    // saved entry stays one undo step on its own, so undoing the new edit
    // gets back to exactly what was saved.
    if (!isClean() && !m_undo.empty() && m_undo.back().command->mergeWith(*command)) {
        // `command`'s effect is already applied to the model (mergeWith
        // is asked to absorb it into the existing top entry, e.g. so a
        // slider drag is one undo step); the new object itself is
        // discarded here since the top entry now represents both.
        if (m_undo.back().command->isNoOp())
            m_undo.pop_back(); // the model is back where that entry began
        else
            m_undo.back().serial = m_nextState++; // same entry, new content
    } else {
        m_undo.push_back({std::move(command), m_nextState++});
        if (m_undo.size() > limit) {
            // The state after the dropped entry is now the bottom one.
            m_baseState = m_undo.front().serial;
            m_undo.erase(m_undo.begin());
        }
    }

    changed.emit();
    return true;
}

bool UndoStack::undo()
{
    if (m_undo.empty())
        return false;
    core::trace::Scope trace([&] { return "undo: " + m_undo.back().command->label(); });

    Entry entry = std::move(m_undo.back());
    m_undo.pop_back();
    entry.command->revert(m_model);
    m_redo.push_back(std::move(entry));

    changed.emit();
    return true;
}

void UndoStack::clear()
{
    m_undo.clear();
    m_redo.clear();
    // A new base: nothing saved against the old history can match it.
    m_baseState = m_nextState++;
    m_cleanState = m_baseState;
    changed.emit();
}

bool UndoStack::redo()
{
    if (m_redo.empty())
        return false;
    core::trace::Scope trace([&] { return "redo: " + m_redo.back().command->label(); });

    Entry entry = std::move(m_redo.back());
    m_redo.pop_back();
    bool reapplied = entry.command->apply(m_model);
    assert(reapplied && "UndoStack::redo: re-applying a previously-successful command failed");
    (void)reapplied;
    m_undo.push_back(std::move(entry));

    changed.emit();
    return true;
}

} // namespace ustudio::core
