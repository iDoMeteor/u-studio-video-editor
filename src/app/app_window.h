#pragma once

#include <adwaita.h>
#include <gtk/gtk.h>

#include <memory>
#include <string>
#include <vector>

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

// The app shell: owns the single MltEngine instance and every top-level
// widget, and mediates between GTK signals and engine calls. UI widgets are
// built imperatively in C++ (no .ui/GResource files) to keep the build to a
// single translation unit per concern and avoid an extra resource-compile
// step for this first milestone.
class AppWindow
{
public:
    explicit AppWindow(GtkApplication *app);

    GtkWidget *widget() const { return GTK_WIDGET(m_window); }

private:
    void buildUi(GtkApplication *app);
    void onImportClicked();
    void onFileOpened(GObject *sourceObject, GAsyncResult *result);
    void onSaveClicked();
    void onSaveFinished(GObject *sourceObject, GAsyncResult *result);
    void onOpenProjectClicked();
    void onOpenProjectFinished(GObject *sourceObject, GAsyncResult *result);
    void onRenderClicked();
    void onRenderFinished(GObject *sourceObject, GAsyncResult *result);
    void onAddTrackClicked();
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

    void refreshTimeline();
    void refreshTransport(int frameNumber);
    void showStatus(const std::string &text);
    std::string formatTimecode(int frame) const;

    static void importClickedTrampoline(GtkButton *button, gpointer userData);
    static void fileOpenedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData);
    static void saveClickedTrampoline(GtkButton *button, gpointer userData);
    static void saveFinishedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData);
    static void openProjectClickedTrampoline(GtkButton *button, gpointer userData);
    static void openProjectFinishedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData);
    static void renderClickedTrampoline(GtkButton *button, gpointer userData);
    static void renderFinishedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData);
    static void addTrackClickedTrampoline(GtkButton *button, gpointer userData);
    static void playToggledTrampoline(GtkButton *button, gpointer userData);
    static void seekChangedTrampoline(GtkRange *range, gpointer userData);
    static void splitClickedTrampoline(GtkButton *button, gpointer userData);
    static void timelineDrawTrampoline(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer userData);
    static void timelineClickTrampoline(GtkGestureClick *gesture, int nPress, double x, double y, gpointer userData);
    static void
    timelineRightClickTrampoline(GtkGestureClick *gesture, int nPress, double x, double y, gpointer userData);
    static void deleteClipClickedTrampoline(GtkButton *button, gpointer userData);
    static void closeGapClickedTrampoline(GtkButton *button, gpointer userData);
    static void removeTrackClickedTrampoline(GtkButton *button, gpointer userData);
    static void trackDragBeginTrampoline(GtkGestureDrag *gesture, double x, double y, gpointer userData);
    static void trackDragUpdateTrampoline(GtkGestureDrag *gesture, double offsetX, double offsetY, gpointer userData);
    static void trackDragEndTrampoline(GtkGestureDrag *gesture, double offsetX, double offsetY, gpointer userData);

    AdwApplicationWindow *m_window = nullptr;
    GtkPicture *m_preview = nullptr;
    GtkDrawingArea *m_timeline = nullptr;
    GtkScale *m_seekScale = nullptr;
    GtkButton *m_playButton = nullptr;
    GtkLabel *m_timecodeLabel = nullptr;
    GtkLabel *m_statusLabel = nullptr;
    GtkPopover *m_trackContextMenu = nullptr;
    GtkWidget *m_deleteClipButton = nullptr;
    GtkWidget *m_closeGapButton = nullptr;
    GtkWidget *m_removeTrackButton = nullptr;

    std::unique_ptr<engine::MltEngine> m_engine;
    std::unique_ptr<engine::WaveformCache> m_waveforms;
    std::vector<engine::MltEngine::ClipInfo> m_clips;
    int m_selectedClip = -1;
    // Which track new imports/splits target; set by clicking a track's row.
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
    // manipulated by its *current* (track, start frame) — engine calls
    // address clips this way rather than by array index, since indices
    // shift under edits. The Preview fields are the live drag position/
    // length shown as a ghost while dragging, applied via an engine call
    // only on drag-end.
    int m_dragClipTrack = -1;
    int m_dragClipStartFrame = -1;
    int m_dragClipFrames = 0;
    int m_dragPreviewTrack = -1;
    int m_dragPreviewStartFrame = -1;
    int m_dragPreviewFrames = 0;

    bool m_suppressSeekSignal = false;
};

} // namespace ustudio::app
