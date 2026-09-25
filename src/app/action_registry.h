#pragma once

#include <gio/gio.h>

#include <vector>

namespace ustudio::app {

// One entry per window-scoped GAction ("win.<name>") this app installs.
// installActions() (app_window.cpp) and the Help dialog's Keyboard
// Shortcuts tab (showHelpDialog()) both read this single table instead of
// keeping separate lists, so the two can never drift out of sync -- the
// point of it existing at all (the owner asked to "stay modular" here
// specifically because a hotkey-rebinding feature is coming later). That
// feature's natural shape on top of this: a Settings-backed lookup
// ("has the user overridden this action's accel?") consulted wherever
// `accels` is read below, rather than a restructure of this table or its
// two consumers.
//
// `activated == nullptr` marks undo/redo: installActions() constructs and
// wires those two GSimpleActions itself (their enabled/disabled state
// tracks UndoStack::canUndo/canRedo via m_undoButton/m_redoButton's
// sensitivity, not this table), but they still belong here so the
// Shortcuts tab lists them and so installActions() can set their default
// accelerators from the same place as every other action's, instead of a
// separate hardcoded pair.
struct ActionSpec
{
    const char *name;
    const char *label;
    const char *category;
    std::vector<const char *> accels;
    void (*activated)(GSimpleAction *, GVariant *, gpointer);
};

// The shell's own actions.
const std::vector<ActionSpec> &actionSpecs();

// Doc 15 IP5: actions a drop-in adds through ShellHost::addActions(), each
// activated with the drop-in's own `target` as user data (the shell's pass
// the window). Strings must outlive the process -- static tables, like
// HintSpec's. Main thread.
struct ContributedAction
{
    ActionSpec spec;
    gpointer target;
};

// Records the acceptable ones and returns them. Refused, with a warning: no
// name, label, category or handler, or a name already taken. An
// accelerator another action already has is dropped (warned), so a drop-in
// can't take Space from the shell.
std::vector<ContributedAction> contributeActions(const std::vector<ActionSpec> &specs, gpointer target);
const std::vector<ContributedAction> &contributedActions();

// Every action: the shell's table, then the contributions in order. What
// Help's Keyboard Shortcuts tab and shortcutLabel() read.
std::vector<ActionSpec> allActionSpecs();

// Tests only: forget the contributions.
void clearContributedActions();

} // namespace ustudio::app
