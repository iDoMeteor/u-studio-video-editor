#pragma once

#include <adwaita.h>
#include <gtk/gtk.h>

#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/commands/undo_stack.h"
#include "core/model/model.h"
#include "engine/engine_sync.h"
#include "engine/playback_controller.h"
#include "engine/thumbnail_cache.h"
#include "engine/waveform_cache.h"

namespace ustudio::app {

// What an in-progress timeline drag is doing, decided in onTrackDragBegin
// from where the press landed (handle strip / clip edge / clip body /
// empty space) and finalized in onTrackDragEnd.
enum class TimelineDragMode
{
    None,
    TrackReorder,
    MoveClip,
    TrimClipStart,
    TrimClipEnd,
    // Dragging one edge of an EXISTING dissolve transition's hatch region
    // to grow/shrink it (Left = the edge at the earlier clip's position,
    // Right = the edge at the later clip's end) -- the other edge stays
    // fixed. Distinct from TrimClipStart/End: those touch a clip's own
    // in/out directly and refuse on overlap; these touch the transition's
    // extendA/extendB split via RemoveTransition+AddTransition.
    TransitionResizeLeft,
    TransitionResizeRight,
};

// What the inline name-edit popover (double-click a track label or a
// clip) is currently open for, if anything.
enum class InlineEditKind
{
    None,
    Track,
    Clip,
};

// A row/frame-index view of one model clip, rebuilt from Model on every
// refreshTimeline() call -- what the timeline actually draws and hit-tests
// against. `trackIndex` is a position in model->sequence().tracks (row 0 =
// top), not a TrackId; `id` is the model's stable ClipId, used to build
// Commands. Kept close to v1's ClipInfo shape deliberately, so the
// drag/draw/hit-test logic below (unchanged from v1) still applies to it.
struct ClipDisplay
{
    core::ClipId id;
    std::string name;
    std::string resource; // asset path, for waveform lookups
    int trackIndex = 0;
    int startFrame = 0;
    int frames = 0;
    int in = 0;
    int out = 0;
};

// The app shell: owns the project Model, its UndoStack, the EngineSync
// projection of it into MLT, a PlaybackController that plays whatever
// tractor EngineSync last built (doc 05/ADR-002), and every top-level
// widget. UI widgets are built imperatively in C++ (no .ui/GResource
// files) to keep the build to a single translation unit per concern and
// avoid an extra resource-compile step for this first milestone.
//
// Edit flow: a UI gesture builds a core::Command, UndoStack::execute()
// applies it to the model. Model::changed then drives the rest
// automatically (doc 02: "Model -> engine -> screen, never backwards"):
// EngineSync is subscribed to it and resyncs the tractor on its own
// (engine_sync.h), and its `rebuilt` signal (connected once, in this
// class's constructor) is what points PlaybackController at the new
// tractor -- no call site here needs to remember to do either.
class AppWindow
{
  public:
    explicit AppWindow(GtkApplication *app);

    GtkWidget *widget() const
    {
        return GTK_WIDGET(m_window);
    }

    // Stops the playback worker thread before Factory::close() runs on
    // quit (main.cpp's "shutdown" handler). AppWindow itself is never
    // destroyed, so its members' destructors are not the shutdown path.
    void prepareForShutdown();

  private:
    void buildUi(GtkApplication *app);
    void installActions(GtkApplication *app);
    void onImportClicked();
    void onFileOpened(GObject *sourceObject, GAsyncResult *result);
    void onSaveClicked();
    void onSaveFinished(GObject *sourceObject, GAsyncResult *result);
    void onOpenProjectClicked();
    void onOpenProjectFinished(GObject *sourceObject, GAsyncResult *result);
    // Re-loads m_currentProjectPath from disk, discarding in-memory edits --
    // a quick way to pick up a fix or re-attempt opening the same file
    // without going through the file-picker dialog again. A no-op (with a
    // status message) if the project has never been saved/opened, since
    // there's nothing on disk yet to reload from. Confirms first if there
    // are unsaved changes (audit A2) -- unlike Open, which already goes
    // through a file-picker dialog the user actively navigates (its own
    // implicit "are you sure"), Reload and New Project replace the model
    // with a single click next to Save, with nothing else in the way.
    void onReloadProjectClicked();
    void performReload();
    // Resets to a brand new, empty, untitled project -- same effect as
    // Open Project loading a fresh Model::createEmpty(), just without a
    // file dialog. Does not touch whatever's on disk at
    // m_currentProjectPath. Confirms first if there are unsaved changes
    // -- see onReloadProjectClicked's own comment.
    void onNewProjectClicked();
    void performNewProject();
    // Shared by onReloadProjectClicked/onNewProjectClicked (audit A2): a
    // no-op (running `onConfirmed` immediately) if the project has no
    // unsaved changes; otherwise shows a "Discard unsaved changes?"
    // alert (matching offerRecoveryIfAny's own dialog conventions) and
    // runs `onConfirmed` only if the owner picks "Discard".
    void confirmDiscardIfDirty(std::function<void()> onConfirmed);
    // Audit A2: closing the window used to quit immediately with no
    // unsaved-changes prompt and no final autosave. GTK's "close-request"
    // signal lets a handler veto the close by returning GDK_EVENT_STOP;
    // this one does that whenever the project is dirty and shows a
    // Save/Discard/Cancel alert (confirmDiscardIfDirty's Discard/Cancel
    // pair doesn't fit here -- unlike Reload/New Project, closing loses
    // the work for good, so it needs a way to keep it too). The dialog is
    // async, so the confirmed paths destroy the window explicitly:
    // Discard calls gtk_window_destroy() directly (bypassing
    // close-request, so it can't loop back into this same prompt);
    // Save sets m_closeAfterSave and re-enters the normal Save flow,
    // which destroys the window itself once onSaveFinished() sees a
    // successful save with that flag set.
    gboolean onCloseRequest();
    void onRenderClicked();
    void onRenderFinished(GObject *sourceObject, GAsyncResult *result);
    void onAddTrackClicked();
    void onUndo();
    void onRedo();
    void onPlayToggled();
    void onSeekChanged();
    // J/K/L shuttle (doc 05): L accelerates forward (1x -> 2x -> 4x -> 8x),
    // J mirrors it in reverse, K stops. Repeated presses ramp the existing
    // direction's speed rather than resetting to 1x.
    void onShuttleForward();
    void onShuttleReverse();
    void onShuttleStop();
    void onStepForward();
    void onStepBackward();
    // Ctrl+Left/Right (10 frames) and Alt+Left/Right (1 minute) -- both
    // just call stepFrame() with a bigger delta; its seek() already clamps
    // to [0, totalFrames()-1], so "not a full minute left in that
    // direction" (the user's own spec) falls out for free.
    void onStepForward10();
    void onStepBackward10();
    void onStepForwardMinute();
    void onStepBackwardMinute();
    // fps() rounded to the nearest whole frame, times 60; falls back to
    // 25fps (matching the timecode label's own fallback) for the brief
    // window before any project/tractor exists.
    int oneMinuteInFrames() const;
    void onSeekHome();
    void onSeekEnd();
    // Jumps the playhead to the nearest clip boundary (a clip's start or
    // end) on the active track before/after the current frame -- the
    // timeline's own start (0) and end (m_playback->totalFrames()) count
    // as boundaries too, so there's always somewhere to land even on an
    // otherwise-empty or single-clip track. Doesn't pause playback,
    // matching onSeekHome/onSeekEnd's own precedent (a mid-playback jump
    // is a scrub, not a stop).
    void onSeekPreviousCut();
    void onSeekNextCut();
    // Every clip boundary on the active track, plus 0 and totalFrames(),
    // sorted and deduplicated -- shared by the two methods above.
    std::vector<int> cutBoundariesOnActiveTrack() const;
    // Moves which track row is "active" (where imports/splits/single-key
    // edits land) up or down by one, clamped to the track list -- the
    // same target onTimelineClicked's plain click already sets, just
    // reachable without the mouse.
    void onActiveTrackUp();
    void onActiveTrackDown();
    // I/O set the loop-in/loop-out at the current playhead (clamped so
    // in < out); there's no ruler widget yet to show the range visually
    // (that's UsTimelineView/ADR-008, M3), so m_loopStatusLabel is the only
    // feedback for now.
    void onSetLoopIn();
    void onSetLoopOut();
    void onClearLoopClicked();
    void onVolumeChanged();
    void onPreviewScaleChanged();
    void onSplitClicked();
    // nPress == 2 (double-click) on a track's name-label strip or on a
    // clip opens the inline name editor instead of the usual seek/select.
    void onTimelineClicked(int nPress, double x, double y);
    void onTimelineRightClicked(double x, double y);
    void onDeleteClipClicked();
    void onSplitAudioClicked();
    void onCloseGapClicked();
    void onRemoveTrackClicked();
    void onToggleLockClicked();
    void onEditTrackNameClicked();
    void onTrackVolumeChanged();
    void onWaveformReady();
    void onThumbnailReady();
    void onToggleMediaBrowserClicked();
    // Rebuilds m_mediaBrowserGrid from scratch against m_model.project().bin
    // -- called at every call site that can change the bin: after a
    // successful import, and after Open/Reload/New Project/Recover, which
    // each replace m_model wholesale (same call sites refreshTimeline()
    // itself already runs at, for the same reason).
    void refreshMediaBrowser();
    // Right-click on a media browser row: records which asset it landed
    // on (m_contextMenuAssetId) and pops m_mediaBrowserContextMenu at the
    // click point. `row` is the specific row widget the click landed on
    // (rows are rebuilt from scratch by refreshMediaBrowser(), so it's
    // passed in rather than cached); the popover itself stays parented
    // to the stable m_mediaBrowserPanel (see buildUi()'s own comment on
    // why not m_mediaBrowserList), so (x, y) -- relative to `row` -- is
    // translated into m_mediaBrowserPanel's coordinate space first.
    void onMediaBrowserRowRightClicked(core::AssetId assetId, GtkWidget *row, double x, double y);
    // "Remove from Project": RemoveAsset also removes every clip using
    // the asset (Model::check() invariant 5), so this is a single
    // undoable command, not a two-step confirm -- the file on disk is
    // untouched.
    void onRemoveAssetClicked();
    // "Delete File...": confirms (the file itself is not recoverable via
    // undo, unlike removing it from the project), then does both --
    // RemoveAsset via the undo stack, then an actual filesystem delete.
    // If the delete fails (permissions, already gone) the asset stays
    // removed from the project regardless; a status message says why.
    void onDeleteAssetFileClicked();
    // Drop target for dragging a media browser row onto the timeline
    // (GtkDropTarget on m_timeline, G_TYPE_INT64 carrying the AssetId's
    // value -- see refreshMediaBrowser()'s GtkDragSource on each row).
    // Inserts the asset's full length at the exact row/frame the drop
    // landed on; refuses (with a status message) if that space isn't
    // free or the target track is locked, same as any other insert.
    gboolean onTimelineDrop(const GValue *value, double x, double y);
    // Shared by import (a freshly probed asset not yet in the bin --
    // EngineSync::ProbedMedia, not yet a core::MediaInfo) and dragging an
    // existing bin asset onto the timeline (core::MediaInfo::isBoundless
    // ()): a still image or generator has no fixed duration of its own,
    // so its clip spans the rest of the *current* project length from
    // the insert point, falling back to a modest 10s when there's
    // nothing yet to cover -- see onFileOpened's own comment for why.
    core::FrameIndex effectiveInsertLength(bool isBoundless, core::FrameIndex knownLength,
                                           core::FrameIndex insertPos) const;
    // query-tooltip handler (GTK4's mechanism for a per-region tooltip on
    // a custom-drawn widget): true + gtk_tooltip_set_* if (x, y) is over a
    // clip, false to suppress the tooltip anywhere else.
    gboolean onTimelineQueryTooltip(int x, int y, GtkTooltip *tooltip);
    void onEditClipNameClicked();
    void onRemoveClipNameClicked();
    void onRemoveTransitionClicked();
    void onAddTransitionClicked();
    // Opens the inline name-edit popover anchored over track `row`'s
    // label strip, or over `clip`, pre-filled with its current name.
    // Enter or clicking away commits (a no-op Command if the text didn't
    // actually change -- see onInlineNameEditClosed); Escape cancels.
    void beginTrackNameEdit(int row);
    void beginClipNameEdit(const ClipDisplay &clip);
    void showInlineNameEditor(GdkRectangle anchor, const std::string &currentName);
    void onInlineNameEditClosed();
    // Returns TRUE (GDK_EVENT_STOP) only for Escape, so the popover's own
    // default key handling (which would otherwise treat Escape as "close",
    // i.e. commit via onInlineNameEditClosed) never runs for it.
    gboolean onInlineNameEditKeyPressed(guint keyval);
    // Returns true if the press hit something draggable (a track handle or
    // a clip) and the gesture should claim the sequence — denying the
    // competing click gesture on the same widget, which would otherwise
    // also treat this as a seek. A press on empty space returns false so
    // the plain click gesture still handles seeking there.
    bool onTrackDragBegin(double x, double y);
    void onTrackDragUpdate(double offsetX, double offsetY);
    void onTrackDragEnd(double offsetX, double offsetY);
    // Executes AddTransition(trackIdForRow(m_dragClipTrack), a, b,
    // extendA, extendB) via the undo stack and reports the result --
    // shared by TrimClipStart/TrimClipEnd's onTrackDragEnd handling, the
    // two places dragging a clip's edge past an exactly-touching
    // neighbour turns what would otherwise be a refused trim into a new
    // dissolve. Returns false (and shows no status) on failure, so the
    // caller falls back to its own "can't trim that far" message.
    bool onDragCreatedTransition(core::ClipId a, core::ClipId b, core::FrameIndex extendA, core::FrameIndex extendB);
    void onTimelineDraw(cairo_t *cr, int width, int height);
    void onFrameReady(std::vector<uint8_t> rgba, int width, int height, int frameNumber);

    void refreshTimeline();
    void refreshTransport(int frameNumber);
    void refreshPlayButtonIcon();
    void refreshLoopStatusLabel();
    void updateWindowTitle();
    void showStatus(const std::string &text);
    std::string formatTimecode(int frame) const;
    // True if `path` names the same file as one of the project's own bin
    // assets (audit A6) -- Save and Render both refuse to write there
    // rather than overwrite a user's source media, per CLAUDE.md's data
    // safety rule. Compares canonicalized paths, not raw strings, so a
    // relative or symlinked spelling of the same file is still caught.
    bool pathIsProjectAsset(const std::string &path) const;

    // Row index (position in model->sequence().tracks) -> TrackId. Asserts
    // in debug builds if out of range; callers only pass rows the timeline
    // itself just drew, so this should never be reached with a stale one.
    core::TrackId trackIdForRow(int row) const;

    // Creates a stateless GSimpleAction named `name`, wires `activated` as
    // its "activate" handler with `this` as user data, adds it to the
    // window's action map, and binds `accels` to "win.<name>" -- the
    // boilerplate every one of installActions()'s calls below shares.
    void addAction(GtkApplication *app, const char *name,
                   void (*activated)(GSimpleAction *, GVariant *, gpointer),
                   std::initializer_list<const char *> accels);

    // Audit A1: the transport actions installActions() binds to bare
    // letters and Left/Right/Home/End (+ Ctrl/Alt variants) are global
    // window accelerators, so they fire even while the inline track/clip
    // name GtkEntry has focus -- typing "a" there seeks to the previous
    // cut instead of inserting the letter. showInlineNameEditor() disables
    // them for the popover's lifetime; onInlineNameEditClosed() re-enables
    // them. Undo/redo (Ctrl+Z/Ctrl+Shift+Z) are deliberately not in this
    // set: they're modifier combos, not bare keys a text entry would ever
    // want to consume itself.
    void setTransportActionsEnabled(bool enabled);

    static void importClickedTrampoline(GtkButton *button, gpointer userData);
    static void fileOpenedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData);
    static void saveClickedTrampoline(GtkButton *button, gpointer userData);
    static void saveFinishedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData);
    static void openProjectClickedTrampoline(GtkButton *button, gpointer userData);
    static void openProjectFinishedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData);
    static void reloadProjectClickedTrampoline(GtkButton *button, gpointer userData);
    static void newProjectClickedTrampoline(GtkButton *button, gpointer userData);
    static void toggleMediaBrowserClickedTrampoline(GtkButton *button, gpointer userData);
    static void mediaBrowserRowRightClickTrampoline(GtkGestureClick *gesture, int nPress, double x, double y,
                                                    gpointer userData);
    static void removeAssetClickedTrampoline(GtkButton *button, gpointer userData);
    static void deleteAssetFileClickedTrampoline(GtkButton *button, gpointer userData);
    static gboolean timelineDropTrampoline(GtkDropTarget *target, const GValue *value, double x, double y,
                                           gpointer userData);
    static void renderClickedTrampoline(GtkButton *button, gpointer userData);
    static void renderFinishedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData);
    static void addTrackClickedTrampoline(GtkButton *button, gpointer userData);
    static void undoClickedTrampoline(GtkButton *button, gpointer userData);
    static void redoClickedTrampoline(GtkButton *button, gpointer userData);
    static void playToggledTrampoline(GtkButton *button, gpointer userData);
    static void seekChangedTrampoline(GtkRange *range, gpointer userData);
    static void splitClickedTrampoline(GtkButton *button, gpointer userData);
    static void timelineDrawTrampoline(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer userData);
    static void timelineClickTrampoline(GtkGestureClick *gesture, int nPress, double x, double y, gpointer userData);
    static void timelineRightClickTrampoline(GtkGestureClick *gesture, int nPress, double x, double y,
                                             gpointer userData);
    static void deleteClipClickedTrampoline(GtkButton *button, gpointer userData);
    static void splitAudioClickedTrampoline(GtkButton *button, gpointer userData);
    static void closeGapClickedTrampoline(GtkButton *button, gpointer userData);
    static void removeTrackClickedTrampoline(GtkButton *button, gpointer userData);
    static void toggleLockClickedTrampoline(GtkButton *button, gpointer userData);
    static void editTrackNameClickedTrampoline(GtkButton *button, gpointer userData);
    static void trackVolumeChangedTrampoline(GtkRange *range, gpointer userData);
    static void trackDragBeginTrampoline(GtkGestureDrag *gesture, double x, double y, gpointer userData);
    static void trackDragUpdateTrampoline(GtkGestureDrag *gesture, double offsetX, double offsetY, gpointer userData);
    static void trackDragEndTrampoline(GtkGestureDrag *gesture, double offsetX, double offsetY, gpointer userData);
    static void undoActionActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void redoActionActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void shuttleForwardActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void shuttleReverseActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void shuttleStopActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void stepForwardActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void stepBackwardActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void stepForward10Activated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void stepBackward10Activated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void stepForwardMinuteActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void stepBackwardMinuteActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void seekHomeActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void seekEndActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void loopSetInActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void loopSetOutActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void seekPreviousCutActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void seekNextCutActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void activeTrackUpActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void activeTrackDownActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void clearLoopClickedTrampoline(GtkButton *button, gpointer userData);
    static void volumeChangedTrampoline(GtkRange *range, gpointer userData);
    static void previewScaleChangedTrampoline(GtkDropDown *dropdown, GParamSpec *pspec, gpointer userData);
    static gboolean timelineQueryTooltipTrampoline(GtkWidget *widget, int x, int y, gboolean keyboardMode,
                                                   GtkTooltip *tooltip, gpointer userData);
    static void editClipNameClickedTrampoline(GtkButton *button, gpointer userData);
    static void removeClipNameClickedTrampoline(GtkButton *button, gpointer userData);
    static void removeTransitionClickedTrampoline(GtkButton *button, gpointer userData);
    static void addTransitionClickedTrampoline(GtkButton *button, gpointer userData);
    static void inlineNameEditActivateTrampoline(GtkEntry *entry, gpointer userData);
    static void inlineNameEditClosedTrampoline(GtkPopover *popover, gpointer userData);
    static gboolean inlineNameEditKeyTrampoline(GtkEventControllerKey *controller, guint keyval, guint keycode,
                                                GdkModifierType state, gpointer userData);

    AdwApplicationWindow *m_window = nullptr;
    GtkPicture *m_preview = nullptr;
    // Media browser: a collapsible panel to the left of the preview
    // (same row) listing every imported asset as a row (thumbnail, name,
    // length, fps, format). m_mediaBrowserPanel is the whole collapsible
    // widget (a GtkScrolledWindow); m_mediaBrowserList is rebuilt from
    // scratch by refreshMediaBrowser() each time the bin changes (mirrors
    // refreshTimeline()'s own call-site-driven resync -- see its own
    // comment for why this app doesn't subscribe to Model::changed
    // directly). A plain vertical GtkBox of per-row GtkBoxes, not a
    // GtkGrid: each row needs to be one widget a right-click gesture and
    // a drag source can attach to (a GtkGrid has no such per-row widget,
    // only per-cell ones), so each row lays out its own cells at fixed
    // widths to keep columns aligned across rows instead.
    GtkWidget *m_mediaBrowserPanel = nullptr;
    GtkBox *m_mediaBrowserList = nullptr;
    // Two-button popover ("Remove from Project" / "Delete File...") for
    // whichever row was last right-clicked (m_contextMenuAssetId) --
    // reparented onto that row each time (see
    // onMediaBrowserRowRightClicked's own comment).
    GtkPopover *m_mediaBrowserContextMenu = nullptr;
    GtkWidget *m_removeAssetButton = nullptr;
    GtkWidget *m_deleteAssetFileButton = nullptr;
    core::AssetId m_contextMenuAssetId;
    GtkDrawingArea *m_timeline = nullptr;
    GtkScale *m_seekScale = nullptr;
    GtkButton *m_playButton = nullptr;
    GtkButton *m_undoButton = nullptr;
    GtkButton *m_redoButton = nullptr;
    GtkLabel *m_timecodeLabel = nullptr;
    GtkLabel *m_statusLabel = nullptr;
    GtkLabel *m_loopStatusLabel = nullptr;
    GtkScale *m_volumeScale = nullptr;
    GtkDropDown *m_previewScaleDropdown = nullptr;
    GtkPopover *m_trackContextMenu = nullptr;
    GtkWidget *m_deleteClipButton = nullptr;
    GtkWidget *m_splitAudioButton = nullptr;
    GtkWidget *m_closeGapButton = nullptr;
    GtkWidget *m_removeTrackButton = nullptr;
    GtkWidget *m_toggleLockButton = nullptr;
    GtkWidget *m_editTrackNameButton = nullptr;
    GtkWidget *m_editClipNameButton = nullptr;
    GtkWidget *m_removeClipNameButton = nullptr;
    GtkWidget *m_removeTransitionButton = nullptr;
    GtkWidget *m_addTransitionButton = nullptr;
    GtkScale *m_trackVolumeScale = nullptr;
    // "value-changed" fires while merely repositioning the slider to the
    // right-clicked track's current volume (see onTimelineRightClicked) --
    // this suppresses turning that into a spurious SetTrackVolume command,
    // matching m_suppressSeekSignal's existing role for the seek scale.
    bool m_suppressTrackVolumeSignal = false;

    core::Model m_model = core::Model::createEmpty();
    core::UndoStack m_undoStack{m_model};
    std::unique_ptr<engine::EngineSync> m_engineSync;
    std::unique_ptr<engine::PlaybackController> m_playback;
    std::unique_ptr<engine::WaveformCache> m_waveforms;
    std::unique_ptr<engine::ThumbnailCache> m_thumbnails;
    std::vector<ClipDisplay> m_clips;
    int m_selectedClip = -1;
    // Which track row new imports/splits target; set by clicking a track's
    // row. A row index into model.sequence().tracks, not a TrackId.
    int m_activeTrack = 0;
    // What a right-click's context menu is currently open for: the track
    // row, the frame position clicked, and — if the click landed on a clip
    // or a gap — that clip's/gap's start frame (-1 if neither applies).
    // Which popover buttons are visible is decided from these at
    // right-click time (see onTimelineRightClicked).
    int m_contextMenuTrack = -1;
    int m_contextMenuFrame = -1;
    int m_contextMenuClipStartFrame = -1;
    int m_contextMenuGapStartFrame = -1;
    // Set alongside the above when the right-click landed inside a
    // dissolve transition's overlap region (invalid/default otherwise).
    core::TransitionId m_contextMenuTransitionId;
    // Set alongside the above when the right-click landed near the exact
    // boundary between two touching, not-yet-linked clips (invalid/
    // default otherwise) -- offers "Add Transition" there.
    core::ClipId m_contextMenuAddTransitionA, m_contextMenuAddTransitionB;

    // --- Inline track/clip name editing (double-click, or the context
    // menu's Edit Name) ---
    GtkPopover *m_inlineNameEditPopover = nullptr;
    GtkEntry *m_inlineNameEditEntry = nullptr;
    InlineEditKind m_inlineEditKind = InlineEditKind::None;
    int m_inlineEditTrackRow = -1;      // valid when m_inlineEditKind == Track
    core::ClipId m_inlineEditClipId;    // valid when m_inlineEditKind == Clip
    // Set by Escape (onInlineNameEditKeyPressed) just before popping the
    // popover down, so the "closed" signal that follows knows to discard
    // the entry's text instead of committing it.
    bool m_inlineEditCancelled = false;

    // --- Timeline drag state (track reorder, clip move, clip trim) ---
    TimelineDragMode m_dragMode = TimelineDragMode::None;
    double m_dragStartX = 0.0;
    double m_dragStartY = 0.0;
    // TrackReorder: the track being dragged and the row currently under the
    // cursor, drawn as a drop-target hint.
    int m_draggingTrack = -1;
    int m_dragHoverRow = -1;
    // MoveClip/TrimClipStart/TrimClipEnd: identifies the clip being
    // manipulated by its model ClipId. The Preview fields are the live drag
    // position/length shown as a ghost while dragging, applied via a
    // Command only on drag-end.
    core::ClipId m_dragClipId;
    int m_dragClipTrack = -1;
    int m_dragClipStartFrame = -1;
    int m_dragClipFrames = 0;
    int m_dragPreviewTrack = -1;
    int m_dragPreviewStartFrame = -1;
    int m_dragPreviewFrames = 0;

    // TransitionResizeLeft/Right: which transition, its row, and the live
    // preview position of the edge actually being dragged (the other edge
    // stays at the transition's current, unchanged position throughout).
    core::TransitionId m_dragTransitionId;
    int m_dragTransitionRow = -1;
    int m_dragTransitionPreviewLeftFrame = 0;
    int m_dragTransitionPreviewRightFrame = 0;

    bool m_suppressSeekSignal = false;

    // --- Autosave / recovery (doc 09) ---
    // Empty until the first successful Save/Save As/Open; drives both the
    // autosave filename (autosave::baseNameFor) and where a normal Save
    // writes.
    std::string m_currentProjectPath;
    // Fresh per launch (a UUID) -- makes two untitled sessions autosave to
    // different files instead of overwriting each other, per doc 09.
    std::string m_autosaveSessionId;
    gint64 m_lastEditMonotonicUsec = 0;
    gint64 m_lastAutosaveMonotonicUsec = 0;
    guint m_autosaveHeartbeatId = 0;
    // Set after a successful "Recover" (audit A2): this session's own
    // future autosaves go to a filename keyed on m_autosaveSessionId (a
    // fresh UUID per launch), never the recovered file's, so nothing else
    // would ever clean the old one up. Deleted only once a manual Save
    // actually lands the recovered content somewhere durable -- not right
    // after recovery, when it would be the only copy of that work again if
    // the app crashed a second time before the user got to Save. Empty =
    // nothing pending. Cleared WITHOUT deleting anything by New Project/
    // Open/Reload (audit A1): switching away to a different project
    // before that Save happens must forget this pending cleanup, or a
    // later Save of the NEW project would delete the recovered
    // content's still-only copy out from under it. The files themselves
    // stay on disk either way -- a later launch, or offerRecoveryIfAny()
    // looping later in this one, can still find and offer them.
    std::string m_pendingAutosaveCleanupPath;
    std::string m_pendingAutosaveCleanupMetaPath;
    // Every .meta path offerRecoveryIfAny() has already asked about this
    // launch (recovering doesn't remove the file -- see the comment
    // above -- so without this the same one would just be found again).
    // Lets it loop through EVERY independently-orphaned autosave instead
    // of only ever surfacing whichever one findRecoverable() happens to
    // return first, so an older one with real content never goes silently
    // unmentioned just because a newer, emptier one also exists.
    std::set<std::string> m_offeredAutosaveMetaPaths;
    // Set by onCloseRequest()'s "Save" response, consumed by the very next
    // onSaveFinished() (success or not -- see that method's own comment)
    // so a save the user triggers manually in between never accidentally
    // closes the window.
    bool m_closeAfterSave = false;

    void onAutosaveHeartbeat();
    void performAutosave();
    void offerRecoveryIfAny();
    void onWindowActiveChanged();
    static gboolean autosaveHeartbeatTrampoline(gpointer userData);
    static void windowActiveChangedTrampoline(GObject *object, GParamSpec *pspec, gpointer userData);
    static gboolean closeRequestTrampoline(GtkWindow *window, gpointer userData);
};

} // namespace ustudio::app
