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

    // Never merge into the top entry when the stack is exactly at the
    // last clean point (m_undo.size() == m_cleanDepth, doc 13's C2):
    // isClean() only compares stack SIZE against that depth, so silently
    // absorbing a brand-new edit into the already-saved top entry (e.g. a
    // second, later drag on the same track that mergeWith() can't tell
    // apart from a continuation of the first) would leave the size
    // unchanged and isClean() would keep reporting "clean" even though the
    // model has genuinely changed since the save. Pushing a new entry
    // instead bumps the size and correctly flips isClean() to false.
    bool atCleanPoint = m_undo.size() == m_cleanDepth;
    if (!atCleanPoint && !m_undo.empty() && m_undo.back()->mergeWith(*command)) {
        // `command`'s effect is already applied to the model (mergeWith
        // is asked to absorb it into the existing top entry, e.g. so a
        // slider drag is one undo step); the new object itself is
        // discarded here since the top entry now represents both.
        if (m_undo.back()->isNoOp())
            m_undo.pop_back(); // the model is back where that entry began
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
    core::trace::Scope trace([&] { return "undo: " + m_undo.back()->label(); });

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
    core::trace::Scope trace([&] { return "redo: " + m_redo.back()->label(); });

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
