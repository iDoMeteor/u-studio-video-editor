#pragma once

#include <adwaita.h>
#include <gtk/gtk.h>

#include <initializer_list>
#include <memory>
#include <optional>
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
    // there's nothing on disk yet to reload from.
    void onReloadProjectClicked();
    // Resets to a brand new, empty, untitled project -- same effect as
    // Open Project loading a fresh Model::createEmpty(), just without a
    // file dialog. Does not touch whatever's on disk at
    // m_currentProjectPath.
    void onNewProjectClicked();
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
    void onSeekHome();
    void onSeekEnd();
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

    static void importClickedTrampoline(GtkButton *button, gpointer userData);
    static void fileOpenedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData);
    static void saveClickedTrampoline(GtkButton *button, gpointer userData);
    static void saveFinishedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData);
    static void openProjectClickedTrampoline(GtkButton *button, gpointer userData);
    static void openProjectFinishedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData);
    static void reloadProjectClickedTrampoline(GtkButton *button, gpointer userData);
    static void newProjectClickedTrampoline(GtkButton *button, gpointer userData);
    static void toggleMediaBrowserClickedTrampoline(GtkButton *button, gpointer userData);
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
    static void seekHomeActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void seekEndActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void loopSetInActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void loopSetOutActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
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
    // (same row) listing every imported asset as a grid row (thumbnail,
    // name, length, fps, format). m_mediaBrowserPanel is the whole
    // collapsible widget (a GtkScrolledWindow); m_mediaBrowserGrid is
    // rebuilt from scratch by refreshMediaBrowser() each time the bin
    // changes (mirrors refreshTimeline()'s own call-site-driven resync --
    // see its own comment for why this app doesn't subscribe to
    // Model::changed directly).
    GtkWidget *m_mediaBrowserPanel = nullptr;
    GtkGrid *m_mediaBrowserGrid = nullptr;
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
    // nothing pending.
    std::string m_pendingAutosaveCleanupPath;
    std::string m_pendingAutosaveCleanupMetaPath;

    void onAutosaveHeartbeat();
    void performAutosave();
    void offerRecoveryIfAny();
    void onWindowActiveChanged();
    static gboolean autosaveHeartbeatTrampoline(gpointer userData);
    static void windowActiveChangedTrampoline(GObject *object, GParamSpec *pspec, gpointer userData);
};

} // namespace ustudio::app
