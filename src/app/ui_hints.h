#pragma once

#include <gtk/gtk.h>

#include <string>
#include <string_view>
#include <vector>

namespace ustudio::app {

// One registry for every piece of "how do I use this" text in the shell:
// widget tooltips and the Help dialog's Controls tab both read it, so a
// control's tooltip and its Help entry can't disagree, and neither can
// drift from its key binding -- a hint names its action and the shortcut
// is looked up in action_registry.h at display time instead of being typed
// into the text ("Undo (Ctrl+Z)" was, and would have gone stale the day
// rebinding lands).
//
// Drop-ins (ADR-013, doc 15's IP5) call registerHints() with their own
// table from registerDropIn(): their controls get tooltips through
// setTooltip() and show up under their own category in Help with no
// change to the shell.
//
// Tooltips that show *data* rather than explain a control (an asset row's
// file path, the timeline's per-clip name/timecode tooltip) are content,
// not hints, and are set directly.
struct HintSpec
{
    const char *id;       // "<area>.<control>", unique: "header.undo", "track-menu.mute"
    const char *category; // Help's grouping: "Header bar", "Transport", "Timeline", ...
    const char *title;    // what it is or does, sentence case: "Undo"
    const char *detail;   // optional one-line elaboration; nullptr if none
    const char *action;   // ActionSpec::name whose shortcut to show; nullptr if none
    const char *gesture;  // how to do it with the mouse, for canvas gestures with no
                          // widget of their own ("Drag a clip's edge"); nullptr if none
};

// The shell's own hints, then every registered drop-in table, in
// registration order.
const std::vector<HintSpec> &hintSpecs();

// Adds `hints` after the existing ones. A hint whose id is already
// registered is skipped with a warning, so a drop-in can't silently
// replace the shell's text. Main thread, before the window is built.
void registerHints(const std::vector<HintSpec> &hints);

const HintSpec *findHint(std::string_view id);

// "Ctrl+Z", or "" -- every accelerator of the named action, as GTK labels
// them (gtk_accelerator_get_label), joined with ", ".
std::string shortcutLabel(const char *actionName);

// "<title> (<shortcut>)" plus "\n<detail>" when there is one. An unknown id
// logs a warning and returns the id itself, so a typo is visible on hover.
std::string tooltipText(std::string_view id);

void setTooltip(GtkWidget *widget, std::string_view id);

} // namespace ustudio::app
