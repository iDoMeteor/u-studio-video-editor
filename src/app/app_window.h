#pragma once

#include <adwaita.h>
#include <gtk/gtk.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/commands/undo_stack.h"
#include "core/model/model.h"
#include "engine/engine_sync.h"
#include "engine/mlt_engine.h"
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
// projection of it into MLT, a playback-only MltEngine that plays whatever
// tractor EngineSync last built, and every top-level widget. UI widgets are
// built imperatively in C++ (no .ui/GResource files) to keep the build to a
// single translation unit per concern and avoid an extra resource-compile
// step for this first milestone.
//
// Edit flow: a UI gesture builds a core::Command, UndoStack::execute()
// applies it to the model, then EngineSync::rebuildAll() projects the new
// model state into a fresh tractor and MltEngine::setTractor() points
// playback at it (doc 02: "Model -> engine -> screen, never backwards").
class AppWindow
{
  public:
    explicit AppWindow(GtkApplication *app);

    GtkWidget *widget() const
    {
        return GTK_WIDGET(m_window);
    }

  private:
    void buildUi(GtkApplication *app);
    void installActions(GtkApplication *app);
    void onImportClicked();
    void onFileOpened(GObject *sourceObject, GAsyncResult *result);
    void onSaveClicked();
    void onSaveFinished(GObject *sourceObject, GAsyncResult *result);
    void onOpenProjectClicked();
    void onOpenProjectFinished(GObject *sourceObject, GAsyncResult *result);
    void onRenderClicked();
    void onRenderFinished(GObject *sourceObject, GAsyncResult *result);
    void onAddTrackClicked();
    void onUndo();
    void onRedo();
    void onPlayToggled();
    void onSeekChanged();
    void onSplitClicked();
    void onTimelineClicked(double x, double y);
    void onTimelineRightClicked(double x, double y);
    void onDeleteClipClicked();
    void onCloseGapClicked();
    void onRemoveTrackClicked();
    void onWaveformReady();
    // Returns true if the press hit something draggable (a track handle or
    // a clip) and the gesture should claim the sequence — denying the
    // competing click gesture on the same widget, which would otherwise
    // also treat this as a seek. A press on empty space returns false so
    // the plain click gesture still handles seeking there.
    bool onTrackDragBegin(double x, double y);
    void onTrackDragUpdate(double offsetX, double offsetY);
    void onTrackDragEnd(double offsetX, double offsetY);
    void onTimelineDraw(cairo_t *cr, int width, int height);
    void onFrameReady(std::vector<uint8_t> rgba, int width, int height, int frameNumber);

    // Re-projects the model into MLT (EngineSync::rebuildAll()) and points
    // playback at the new tractor. Called after every command that changes
    // the model.
    void syncEngine();
    void refreshTimeline();
    void refreshTransport(int frameNumber);
    void updateWindowTitle();
    void showStatus(const std::string &text);
    std::string formatTimecode(int frame) const;

    // Row index (position in model->sequence().tracks) -> TrackId. Asserts
    // in debug builds if out of range; callers only pass rows the timeline
    // itself just drew, so this should never be reached with a stale one.
    core::TrackId trackIdForRow(int row) const;

    static void importClickedTrampoline(GtkButton *button, gpointer userData);
    static void fileOpenedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData);
    static void saveClickedTrampoline(GtkButton *button, gpointer userData);
    static void saveFinishedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData);
    static void openProjectClickedTrampoline(GtkButton *button, gpointer userData);
    static void openProjectFinishedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData);
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
    static void closeGapClickedTrampoline(GtkButton *button, gpointer userData);
    static void removeTrackClickedTrampoline(GtkButton *button, gpointer userData);
    static void trackDragBeginTrampoline(GtkGestureDrag *gesture, double x, double y, gpointer userData);
    static void trackDragUpdateTrampoline(GtkGestureDrag *gesture, double offsetX, double offsetY, gpointer userData);
    static void trackDragEndTrampoline(GtkGestureDrag *gesture, double offsetX, double offsetY, gpointer userData);
    static void undoActionActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);
    static void redoActionActivated(GSimpleAction *action, GVariant *parameter, gpointer userData);

    AdwApplicationWindow *m_window = nullptr;
    GtkPicture *m_preview = nullptr;
    GtkDrawingArea *m_timeline = nullptr;
    GtkScale *m_seekScale = nullptr;
    GtkButton *m_playButton = nullptr;
    GtkButton *m_undoButton = nullptr;
    GtkButton *m_redoButton = nullptr;
    GtkLabel *m_timecodeLabel = nullptr;
    GtkLabel *m_statusLabel = nullptr;
    GtkPopover *m_trackContextMenu = nullptr;
    GtkWidget *m_deleteClipButton = nullptr;
    GtkWidget *m_closeGapButton = nullptr;
    GtkWidget *m_removeTrackButton = nullptr;

    core::Model m_model = core::Model::createEmpty();
    core::UndoStack m_undoStack{m_model};
    std::unique_ptr<engine::EngineSync> m_engineSync;
    std::unique_ptr<engine::MltEngine> m_playback;
    std::unique_ptr<engine::WaveformCache> m_waveforms;
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

    void onAutosaveHeartbeat();
    void performAutosave();
    void offerRecoveryIfAny();
    void onWindowActiveChanged();
    static gboolean autosaveHeartbeatTrampoline(gpointer userData);
    static void windowActiveChangedTrampoline(GObject *object, GParamSpec *pspec, gpointer userData);
};

} // namespace ustudio::app
