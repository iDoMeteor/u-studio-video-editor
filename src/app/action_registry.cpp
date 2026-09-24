#include "action_registry.h"

#include "app_window.h"

namespace ustudio::app {

const std::vector<ActionSpec> &actionSpecs()
{
    // clang-format off
    static const std::vector<ActionSpec> kSpecs = {
        // --- Playback (doc 05's M2 transport deliverables) ---
        // Space toggles play/pause, same as every other video editor --
        // shuttle-forward (L) below already covers "start playing
        // forward" too, but Space is the one everyone reaches for first.
        {"play-pause",          "Play/Pause",                 "Playback", {"space"},              &AppWindow::playPauseActivated},
        {"shuttle-forward",     "Shuttle Forward (accelerate)", "Playback", {"l"},                 &AppWindow::shuttleForwardActivated},
        {"shuttle-reverse",     "Shuttle Reverse (accelerate)", "Playback", {"j"},                 &AppWindow::shuttleReverseActivated},
        {"shuttle-stop",        "Shuttle Stop",               "Playback", {"k"},                   &AppWindow::shuttleStopActivated},
        {"step-forward",        "Step Forward One Frame",     "Playback", {"Right"},               &AppWindow::stepForwardActivated},
        {"step-backward",       "Step Backward One Frame",    "Playback", {"Left"},                &AppWindow::stepBackwardActivated},
        {"step-forward-10",     "Step Forward 10 Frames",     "Playback", {"<Control>Right"},      &AppWindow::stepForward10Activated},
        {"step-backward-10",    "Step Backward 10 Frames",    "Playback", {"<Control>Left"},       &AppWindow::stepBackward10Activated},
        {"step-forward-minute", "Step Forward 1 Minute",      "Playback", {"<Alt>Right"},          &AppWindow::stepForwardMinuteActivated},
        {"step-backward-minute","Step Backward 1 Minute",     "Playback", {"<Alt>Left"},           &AppWindow::stepBackwardMinuteActivated},
        {"seek-home",            "Seek to Start",             "Playback", {"Home"},                &AppWindow::seekHomeActivated},
        {"seek-end",             "Seek to End",                "Playback", {"End"},                &AppWindow::seekEndActivated},
        {"loop-set-in",          "Set Loop In",                "Playback", {"i"},                  &AppWindow::loopSetInActivated},
        {"loop-set-out",         "Set Loop Out",               "Playback", {"o"},                  &AppWindow::loopSetOutActivated},

        // --- Editing ---
        {"undo",                 "Undo",                       "Editing", {"<Control>z"},          nullptr},
        {"redo",                 "Redo",                       "Editing", {"<Control><Shift>z"},   nullptr},
        // A/F: previous/next cut on the active track. S/D: active track
        // up/down -- the row edits/imports land on, same as clicking a row.
        {"seek-previous-cut",    "Seek to Previous Cut",       "Editing", {"a"},                    &AppWindow::seekPreviousCutActivated},
        {"seek-next-cut",        "Seek to Next Cut",           "Editing", {"f"},                    &AppWindow::seekNextCutActivated},
        {"active-track-up",      "Active Track Up",            "Editing", {"s"},                    &AppWindow::activeTrackUpActivated},
        {"active-track-down",    "Active Track Down",          "Editing", {"d"},                    &AppWindow::activeTrackDownActivated},
        {"delete-selected-clip", "Delete Selected Clip",       "Editing", {"Delete"},               &AppWindow::deleteSelectedClipActivated},
        {"split-at-playhead",    "Split Clip at Playhead",     "Editing", {"x"},                    &AppWindow::splitAtPlayheadActivated},
        {"select-all",           "Select All Clips",           "Editing", {"<Control>a"},           &AppWindow::selectAllActivated},
        {"clear-selection",      "Clear Selection",            "Editing", {"Escape"},               &AppWindow::clearSelectionActivated},

        // --- Timeline view (doc 06) ---
        {"zoom-in",              "Zoom In",                    "Timeline", {"plus", "equal", "KP_Add"}, &AppWindow::zoomInActivated},
        {"zoom-out",             "Zoom Out",                   "Timeline", {"minus", "KP_Subtract"},   &AppWindow::zoomOutActivated},
        {"zoom-fit",             "Zoom to Fit",                "Timeline", {"0"},                      &AppWindow::zoomFitActivated},

        // --- Project ---
        {"save",         "Save",              "Project", {"<Control>s"},             &AppWindow::saveActionActivated},
        {"save-as",       "Save As…",          "Project", {"<Control><Shift>s"},      &AppWindow::saveAsActivated},
        {"open-project",  "Open Project…",     "Project", {"<Control>o"},             &AppWindow::openProjectActionActivated},
        {"new-project",   "New Project",       "Project", {"<Control>n"},             &AppWindow::newProjectActionActivated},
        {"import",        "Import…",           "Project", {"<Control>i"},             &AppWindow::importActionActivated},
    };
    // clang-format on
    return kSpecs;
}

} // namespace ustudio::app
