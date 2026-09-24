#pragma once

#include "core/model/model.h"

#include <string>

namespace ustudio::core {

// Every state change the user can trigger is a Command: the unit of undo,
// of engine synchronisation, and of testing (doc 04).
//
// apply() is atomic: validate everything first, then mutate. If it
// returns false, the model must be untouched. revert() after a
// successful apply() must restore the model to bit-for-bit the state it
// was in before -- this is a property test (Model::operator==, see
// tests/core/test_commands.cpp).
//
// Commands capture what they need to revert AT APPLY TIME (e.g. MoveClip
// records the old track/position when applied, not when constructed).
// That is why commands are objects, not closure pairs.
//
// doc 04 also specifies record() for the edit journal (doc 09,
// autosave/recovery). That lands with autosave in a later M1 slice --
// omitted here rather than adding an API nothing calls yet.
class Command
{
  public:
    virtual ~Command() = default;

    virtual std::string label() const = 0; // "Move clip", shown in the Edit menu
    virtual bool apply(Model &) = 0;       // false = refused; nothing changed
    virtual void revert(Model &) = 0;      // must exactly undo a successful apply

    // Coalescing: e.g. a slider drag is many SetParam commands that
    // should be one undo step. Default: never merge.
    virtual bool mergeWith(const Command &next)
    {
        (void)next;
        return false;
    }

    // After a merge: true if the entry now changes nothing (a drag that
    // ended where it began). UndoStack then drops it, since an entry that
    // changes nothing can't be redone (post-M3 audit P7). Default: never.
    virtual bool isNoOp() const
    {
        return false;
    }
};

} // namespace ustudio::core
