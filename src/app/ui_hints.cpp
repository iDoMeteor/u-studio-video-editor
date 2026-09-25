#include "ui_hints.h"

#include "action_registry.h"
#include "core/log.h"

#include <cstring>

namespace ustudio::app {

namespace Log = ustudio::core::Log;

namespace {

std::vector<HintSpec> &registry()
{
    // clang-format off
    static std::vector<HintSpec> hints = {
        // --- Header bar ---
        {"header.import",        "Header bar", "Import media",          "Several files at once; each lands after the previous one on the active track", "import", nullptr},
        {"header.add-track",     "Header bar", "Add track",             nullptr, nullptr, nullptr},
        {"header.media-browser", "Header bar", "Show or hide the media browser", nullptr, nullptr, nullptr},
        {"header.undo",          "Header bar", "Undo",                  nullptr, "undo", nullptr},
        {"header.redo",          "Header bar", "Redo",                  nullptr, "redo", nullptr},
        {"header.new-project",   "Header bar", "New project",           "Starts an empty, untitled project; asks first if there are unsaved changes", "new-project", nullptr},
        {"header.reload",        "Header bar", "Reload project from disk", "Asks first if there are unsaved changes", nullptr, nullptr},
        {"header.open",          "Header bar", "Open project…",         nullptr, "open-project", nullptr},
        {"header.recent",        "Header bar", "Recent projects",       nullptr, nullptr, nullptr},
        {"header.save",          "Header bar", "Save project",          "Saves to the project's own file, keeping its last 5 versions in .ustudio-backups beside it; right-click for Save As", "save", nullptr},
        {"header.render",        "Header bar", "Render the project to an MP4 file", "H.264 and AAC at the project's own size and frame rate", nullptr, nullptr},
        {"header.help",          "Header bar", "Help",                  nullptr, nullptr, nullptr},
        {"header.settings",      "Header bar", "Settings",              nullptr, nullptr, nullptr},

        // --- Transport ---
        {"transport.seek-home",       "Transport", "Go to start",           nullptr, "seek-home", nullptr},
        {"transport.shuttle-reverse", "Transport", "Shuttle reverse",       "Press again to speed up", "shuttle-reverse", nullptr},
        {"transport.step-backward",   "Transport", "Step back one frame",   "Keyboard Shortcuts has 10-frame and one-minute steps", "step-backward", nullptr},
        {"transport.play-pause",      "Transport", "Play/Pause",            nullptr, "play-pause", nullptr},
        {"transport.stop",            "Transport", "Stop",                  nullptr, "shuttle-stop", nullptr},
        {"transport.step-forward",    "Transport", "Step forward one frame", "Keyboard Shortcuts has 10-frame and one-minute steps", "step-forward", nullptr},
        {"transport.shuttle-forward", "Transport", "Shuttle forward",       "Press again to speed up", "shuttle-forward", nullptr},
        {"transport.seek-end",        "Transport", "Go to end",             nullptr, "seek-end", nullptr},
        {"transport.split",           "Transport", "Split the active track's clip at the playhead", nullptr, "split-at-playhead", nullptr},
        {"transport.ripple-mode",     "Transport", "Ripple mode",          "While on, moving a clip closes the gap it leaves and pushes later clips along where it lands", "ripple-mode", nullptr},
        {"transport.seek-bar",        "Transport", "Seek",                  "Drag to scrub, also while playing", nullptr, nullptr},
        {"transport.volume",          "Transport", "Playback volume",       "Only what you hear; not saved in the project", nullptr, nullptr},
        {"transport.preview-scale",   "Transport", "Preview scale",         "Lower scales play large media more smoothly", nullptr, nullptr},
        {"transport.clear-loop",      "Transport", "Clear loop",            "Loop in and out are set at the playhead from the keyboard (Set Loop In/Out)", nullptr, nullptr},

        // --- Timeline: canvas gestures, no widget of their own ---
        {"timeline.active-track", "Timeline", "Make a track active",  "Imports and splits land on it. The click also moves the playhead there and selects the clip under the pointer", nullptr, "Click anywhere in a row"},
        {"timeline.scrub",        "Timeline", "Scrub",                "The preview follows the pointer", nullptr, "Drag on empty track space"},
        {"timeline.move",         "Timeline", "Move a clip",          "Within a track or to another; snaps to clip edges and the playhead", nullptr, "Drag the clip"},
        {"timeline.trim",         "Timeline", "Trim a clip",          "Snaps like a move", nullptr, "Drag within ~8 px of a clip's edge"},
        {"timeline.dissolve",     "Timeline", "Add a dissolve",       "The drag distance is the dissolve's length", nullptr, "Drag a clip's edge past the clip it touches"},
        {"timeline.dissolve-resize", "Timeline", "Resize a dissolve", nullptr, nullptr, "Drag either edge of the hatched region"},
        {"timeline.select-more",  "Timeline", "Add a clip to the selection", "Ctrl toggles a clip in or out instead", nullptr, "Hold Shift and click it"},
        {"timeline.select-box",   "Timeline", "Select with a box",   "Every clip the box touches is added", nullptr, "Hold Shift and drag across empty track space"},
        {"timeline.select-all",   "Timeline", "Select all clips",    "Escape clears the selection", "select-all", nullptr},
        {"timeline.move-group",   "Timeline", "Move several clips",  "They keep their places relative to each other; dissolves between them survive a sideways move", nullptr, "Select them, then drag any one"},
        {"timeline.ripple-trim",  "Timeline", "Ripple trim",         "Later clips on the track follow, so no gap opens and nothing is overwritten", nullptr, "Hold Alt and drag a clip's edge"},
        {"timeline.slip",         "Timeline", "Slip a clip",         "Same place and length, different part of the source", nullptr, "Hold Shift and drag a clip's edge"},
        {"timeline.copy",         "Timeline", "Copy a clip",         "A click with Ctrl held toggles the clip in the selection instead", nullptr, "Hold Ctrl and drag the clip"},
        {"timeline.ripple-delete", "Timeline", "Ripple delete",      "Removes the selected clips and closes the gaps", "ripple-delete-selected", "Select, then"},
        {"timeline.marker",       "Timeline", "Add a marker",        "At the playhead; drawn on the ruler, and edges snap to it", "add-marker", nullptr},
        {"timeline.marker-remove", "Timeline", "Remove a marker",    "The one at (or right next to) the playhead", "remove-marker", nullptr},
        {"timeline.keyboard-select", "Timeline", "Select the next clip", "On the active track, moving the playhead to it; the previous one is under Keyboard Shortcuts", "select-next-clip", nullptr},
        {"timeline.nudge",        "Timeline", "Nudge the selection", "One frame at a time; with Shift, ten", "nudge-right", nullptr},
        {"timeline.pinch",        "Timeline", "Pinch to zoom",       nullptr, nullptr, "Pinch on a touchpad or touchscreen"},
        {"timeline.select-delete", "Timeline", "Delete clips",        "Every selected clip; leaves gaps, nothing else moves", "delete-selected-clip", "Select, then"},
        {"timeline.rename",       "Timeline", "Rename a clip or track", "Enter or click away to keep, Escape to cancel", nullptr, "Double-click the clip, or the track's name strip"},
        {"timeline.zoom",        "Timeline", "Zoom in",           "Keeps the frame under the pointer in place. Zoom out and Zoom to Fit are under Keyboard Shortcuts", "zoom-in", "Hold Ctrl and turn the mouse wheel, or press"},
        {"timeline.scroll",      "Timeline", "Scroll sideways",   "The playhead is followed a page at a time while playing", nullptr, "Hold Shift and turn the wheel, swipe sideways, or drag the scrollbar under the tracks"},
        {"timeline.reorder",      "Timeline", "Reorder tracks",       nullptr, nullptr, "Drag a track's handle at the left edge"},
        {"timeline.drop",         "Timeline", "Place media",          "Refused where the space isn't free or the track is locked", nullptr, "Drag from the media browser or the file manager onto a track"},
        {"timeline.menu",         "Timeline", "More actions",         "What's offered depends on what's under the pointer; see Timeline menu", nullptr, "Right-click a clip, a gap, a dissolve or empty track space"},

        // --- Timeline right-click menu ---
        {"track-menu.delete-clip",       "Timeline menu", "Delete clip",        "Leaves a gap; nothing else moves", nullptr, nullptr},
        {"track-menu.split-audio",       "Timeline menu", "Split audio",        "Moves the clip's audio to its own clip on an audio track", nullptr, nullptr},
        {"track-menu.close-gap",         "Timeline menu", "Close gap",          "Moves everything after the gap earlier; dissolves stay", nullptr, nullptr},
        {"track-menu.add-transition",    "Timeline menu", "Add transition",     "A half-second dissolve where two clips touch", nullptr, nullptr},
        {"track-menu.remove-transition", "Timeline menu", "Remove transition",  nullptr, nullptr, nullptr},
        {"track-menu.sync-audio",     "Timeline menu", "Sync tracks (audio)", "With two clips selected, right-click the one to keep still: the other moves so their sound lines up (within 5 seconds)", nullptr, nullptr},
        {"track-menu.volume",            "Timeline menu", "Track volume",       nullptr, nullptr, nullptr},
        {"track-menu.lock",              "Timeline menu", "Lock or unlock the track", "A locked track's clips can't be changed; it can still be reordered", nullptr, nullptr},
        {"track-menu.hide",              "Timeline menu", "Hide or show the track", "A hidden track isn't seen in the preview or the render; the tracks under it show through", nullptr, nullptr},
        {"track-menu.mute",              "Timeline menu", "Mute or unmute the track", "A muted track isn't heard in playback or the render", nullptr, nullptr},
        {"track-menu.remove-track",      "Timeline menu", "Remove track",       nullptr, nullptr, nullptr},
        {"track-menu.track-name",        "Timeline menu", "Edit track name",    nullptr, nullptr, nullptr},
        {"track-menu.clip-name",         "Timeline menu", "Add or edit the clip's name", "Clips cut from the same file can have different names", nullptr, nullptr},
        {"track-menu.remove-clip-name",  "Timeline menu", "Remove the clip's name", nullptr, nullptr, nullptr},

        // --- Media browser ---
        {"media.insert",       "Media browser", "Insert at the playhead", "On the active track", nullptr, "Double-click a row"},
        {"media.add-files",    "Media browser", "Add files without placing them", nullptr, nullptr, "Drop files from the file manager onto the browser"},
        {"media.remove",       "Media browser", "Remove from project", "Also removes every clip cut from it; undoable. The file is untouched", nullptr, nullptr},
        {"media.trash",        "Media browser", "Move file to Trash…", "Removes it from the project and moves the file to the desktop Trash", nullptr, nullptr},

        // --- Settings ---
        {"settings.autosave-delay",  "Settings", "Autosave delay",          "Minutes idle after an edit before an autosave is written; also written when the oldest unsaved edit reaches that age", nullptr, nullptr},
        {"settings.recent-projects", "Settings", "Recent projects list size", "How many projects the header bar's recent-projects menu lists", nullptr, nullptr},
        {"settings.preview-scale",   "Settings", "Default preview scale",   "The preview resolution the app starts with; the transport bar's own dropdown changes it for this session only", nullptr, nullptr},
        {"settings.shuttle-speed",   "Settings", "Maximum shuttle speed",   "The fastest the J/K/L shuttle ramps up to, in multiples of normal speed", nullptr, nullptr},
        {"settings.worker-threads", "Settings", "Worker threads", "Background threads for probing imports and loading and saving projects; Automatic uses half the CPU cores. Applies after restart", nullptr, nullptr},
    };
    // clang-format on
    return hints;
}

} // namespace

const std::vector<HintSpec> &hintSpecs()
{
    return registry();
}

void registerHints(const std::vector<HintSpec> &hints)
{
    for (const HintSpec &hint : hints) {
        if (findHint(hint.id) != nullptr) {
            Log::warn(std::string("[hints] duplicate hint id '") + hint.id + "' ignored");
            continue;
        }
        registry().push_back(hint);
    }
}

const HintSpec *findHint(std::string_view id)
{
    for (const HintSpec &hint : registry()) {
        if (id == hint.id)
            return &hint;
    }
    return nullptr;
}

std::string shortcutLabel(const char *actionName)
{
    std::string result;
    if (actionName == nullptr)
        return result;
    for (const ActionSpec &spec : actionSpecs()) {
        if (std::strcmp(spec.name, actionName) != 0)
            continue;
        // gtk_accelerator_get_label() turns a parsed accelerator back into
        // its display form ("<Control>s" -> "Ctrl+S"), the labels GTK
        // itself shows in menus, rather than a hand-formatted guess.
        for (const char *accel : spec.accels) {
            guint key = 0;
            GdkModifierType mods{};
            if (!gtk_accelerator_parse(accel, &key, &mods))
                continue;
            char *label = gtk_accelerator_get_label(key, mods);
            if (label != nullptr) {
                if (!result.empty())
                    result += ", ";
                result += label;
                g_free(label);
            }
        }
        break;
    }
    return result;
}

std::string tooltipText(std::string_view id)
{
    const HintSpec *hint = findHint(id);
    if (hint == nullptr) {
        Log::warn("[hints] no hint with id '" + std::string(id) + "'");
        return std::string(id);
    }
    std::string text = hint->title;
    if (std::string shortcut = shortcutLabel(hint->action); !shortcut.empty())
        text += " (" + shortcut + ")";
    if (hint->detail != nullptr)
        text += std::string("\n") + hint->detail;
    return text;
}

void setTooltip(GtkWidget *widget, std::string_view id)
{
    gtk_widget_set_tooltip_text(widget, tooltipText(id).c_str());
}

} // namespace ustudio::app
