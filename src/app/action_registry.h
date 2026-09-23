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

const std::vector<ActionSpec> &actionSpecs();

} // namespace ustudio::app
