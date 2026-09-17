#include "app_window.h"

#include "util/log.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <thread>

namespace {
// Mirrors the tokens in style_css.h. Kept as plain hex here because clips
// are drawn with cairo inside a single GtkDrawingArea, not as separate
// widgets, so CSS selectors can't reach them.
constexpr double kClipFillR = 0x1b / 255.0, kClipFillG = 0x12 / 255.0, kClipFillB = 0x30 / 255.0;
constexpr double kClipBorderR = 0x34 / 255.0, kClipBorderG = 0x23 / 255.0, kClipBorderB = 0x57 / 255.0;
constexpr double kSelectedR = 0x19 / 255.0, kSelectedG = 0xe3 / 255.0, kSelectedB = 0xff / 255.0;
constexpr double kActiveTrackR = 0x19 / 255.0, kActiveTrackG = 0xe3 / 255.0, kActiveTrackB = 0xff / 255.0;
constexpr double kTrackRowHeight = 60.0;
constexpr double kHandleWidth = 22.0;
constexpr double kEdgeGrabWidth = 8.0;
constexpr double kDragClickThreshold = 3.0; // below this, a "drag" is really just a click
constexpr double kWaveformR = 0x9d / 255.0, kWaveformG = 0x4e / 255.0, kWaveformB = 0xff / 255.0; // brand violet
} // namespace

AppWindow::AppWindow(GtkApplication *app)
{
    m_engine = std::make_unique<MltEngine>();
    m_engine->setFrameCallback([this](std::vector<uint8_t> rgba, int width, int height, int frameNumber) {
        onFrameReady(std::move(rgba), width, height, frameNumber);
    });
    m_waveforms = std::make_unique<WaveformCache>([this] { onWaveformReady(); });

    buildUi(app);
    showStatus("Import a media file to begin.");
}

void AppWindow::buildUi(GtkApplication *app)
{
    m_window = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
    gtk_window_set_title(GTK_WINDOW(m_window), "u Studio Video Editor");
    gtk_window_set_default_size(GTK_WINDOW(m_window), 1100, 700);

    GtkWidget *toolbarView = adw_toolbar_view_new();

    GtkWidget *headerBar = adw_header_bar_new();
    GtkWidget *title = adw_window_title_new("u Studio", nullptr);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(headerBar), title);

    GtkWidget *importButton = gtk_button_new_with_label("Import…");
    gtk_widget_add_css_class(importButton, "suggested-action");
    g_signal_connect(importButton, "clicked", G_CALLBACK(&AppWindow::importClickedTrampoline), this);
    adw_header_bar_pack_start(ADW_HEADER_BAR(headerBar), importButton);

    GtkWidget *addTrackButton = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_set_tooltip_text(addTrackButton, "Add track");
    g_signal_connect(addTrackButton, "clicked", G_CALLBACK(&AppWindow::addTrackClickedTrampoline), this);
    adw_header_bar_pack_start(ADW_HEADER_BAR(headerBar), addTrackButton);

    GtkWidget *openButton = gtk_button_new_from_icon_name("document-open-symbolic");
    gtk_widget_set_tooltip_text(openButton, "Open project…");
    g_signal_connect(openButton, "clicked", G_CALLBACK(&AppWindow::openProjectClickedTrampoline), this);
    adw_header_bar_pack_end(ADW_HEADER_BAR(headerBar), openButton);

    GtkWidget *saveButton = gtk_button_new_from_icon_name("document-save-symbolic");
    gtk_widget_set_tooltip_text(saveButton, "Save project…");
    g_signal_connect(saveButton, "clicked", G_CALLBACK(&AppWindow::saveClickedTrampoline), this);
    adw_header_bar_pack_end(ADW_HEADER_BAR(headerBar), saveButton);

    GtkWidget *renderButton = gtk_button_new_with_label("Render…");
    gtk_widget_set_tooltip_text(renderButton, "Render the project to an MP4 file");
    g_signal_connect(renderButton, "clicked", G_CALLBACK(&AppWindow::renderClickedTrampoline), this);
    adw_header_bar_pack_end(ADW_HEADER_BAR(headerBar), renderButton);

    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbarView), headerBar);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_position(GTK_PANED(paned), 420);

    // Preview
    GtkWidget *previewFrame = gtk_frame_new(nullptr);
    gtk_widget_add_css_class(previewFrame, "preview-frame");
    m_preview = GTK_PICTURE(gtk_picture_new());
    gtk_picture_set_content_fit(m_preview, GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_hexpand(GTK_WIDGET(m_preview), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(m_preview), TRUE);
    gtk_frame_set_child(GTK_FRAME(previewFrame), GTK_WIDGET(m_preview));
    gtk_paned_set_start_child(GTK_PANED(paned), previewFrame);

    // Timeline + transport
    GtkWidget *bottomBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(bottomBox, 6);
    gtk_widget_set_margin_bottom(bottomBox, 6);
    gtk_widget_set_margin_start(bottomBox, 6);
    gtk_widget_set_margin_end(bottomBox, 6);

    m_timeline = GTK_DRAWING_AREA(gtk_drawing_area_new());
    gtk_widget_set_hexpand(GTK_WIDGET(m_timeline), TRUE);
    gtk_widget_set_size_request(GTK_WIDGET(m_timeline), -1, static_cast<int>(kTrackRowHeight));
    gtk_widget_add_css_class(GTK_WIDGET(m_timeline), "timeline-area");
    gtk_drawing_area_set_draw_func(m_timeline, &AppWindow::timelineDrawTrampoline, this, nullptr);

    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(&AppWindow::timelineClickTrampoline), this);
    gtk_widget_add_controller(GTK_WIDGET(m_timeline), GTK_EVENT_CONTROLLER(click));

    GtkGesture *rightClick = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rightClick), GDK_BUTTON_SECONDARY);
    g_signal_connect(rightClick, "pressed", G_CALLBACK(&AppWindow::timelineRightClickTrampoline), this);
    gtk_widget_add_controller(GTK_WIDGET(m_timeline), GTK_EVENT_CONTROLLER(rightClick));

    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(&AppWindow::trackDragBeginTrampoline), this);
    g_signal_connect(drag, "drag-update", G_CALLBACK(&AppWindow::trackDragUpdateTrampoline), this);
    g_signal_connect(drag, "drag-end", G_CALLBACK(&AppWindow::trackDragEndTrampoline), this);
    gtk_widget_add_controller(GTK_WIDGET(m_timeline), GTK_EVENT_CONTROLLER(drag));

    // One popover, three possible actions — onTimelineRightClicked decides
    // which single one is relevant (clip under the cursor -> Delete Clip;
    // gap under the cursor -> Close Gap; otherwise -> Remove Track) and
    // shows only that button.
    m_trackContextMenu = GTK_POPOVER(gtk_popover_new());
    gtk_widget_set_parent(GTK_WIDGET(m_trackContextMenu), GTK_WIDGET(m_timeline));
    GtkWidget *contextMenuBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    m_deleteClipButton = gtk_button_new_with_label("Delete Clip");
    gtk_widget_add_css_class(m_deleteClipButton, "flat");
    g_signal_connect(m_deleteClipButton, "clicked", G_CALLBACK(&AppWindow::deleteClipClickedTrampoline), this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_deleteClipButton);

    m_closeGapButton = gtk_button_new_with_label("Close Gap");
    gtk_widget_add_css_class(m_closeGapButton, "flat");
    g_signal_connect(m_closeGapButton, "clicked", G_CALLBACK(&AppWindow::closeGapClickedTrampoline), this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_closeGapButton);

    m_removeTrackButton = gtk_button_new_with_label("Remove Track");
    gtk_widget_add_css_class(m_removeTrackButton, "flat");
    g_signal_connect(m_removeTrackButton, "clicked", G_CALLBACK(&AppWindow::removeTrackClickedTrampoline), this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_removeTrackButton);

    gtk_popover_set_child(m_trackContextMenu, contextMenuBox);

    gtk_box_append(GTK_BOX(bottomBox), GTK_WIDGET(m_timeline));

    GtkWidget *transport = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    m_playButton = GTK_BUTTON(gtk_button_new_from_icon_name("media-playback-start-symbolic"));
    gtk_widget_add_css_class(GTK_WIDGET(m_playButton), "circular");
    g_signal_connect(m_playButton, "clicked", G_CALLBACK(&AppWindow::playToggledTrampoline), this);
    gtk_box_append(GTK_BOX(transport), GTK_WIDGET(m_playButton));

    GtkWidget *splitButton = gtk_button_new_from_icon_name("edit-cut-symbolic");
    gtk_widget_set_tooltip_text(splitButton, "Split at playhead");
    g_signal_connect(splitButton, "clicked", G_CALLBACK(&AppWindow::splitClickedTrampoline), this);
    gtk_box_append(GTK_BOX(transport), splitButton);

    m_seekScale = GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 1));
    gtk_scale_set_draw_value(m_seekScale, FALSE);
    gtk_widget_set_hexpand(GTK_WIDGET(m_seekScale), TRUE);
    g_signal_connect(m_seekScale, "value-changed", G_CALLBACK(&AppWindow::seekChangedTrampoline), this);
    gtk_box_append(GTK_BOX(transport), GTK_WIDGET(m_seekScale));

    m_timecodeLabel = GTK_LABEL(gtk_label_new("00:00:00:00"));
    gtk_widget_add_css_class(GTK_WIDGET(m_timecodeLabel), "timecode-label");
    gtk_box_append(GTK_BOX(transport), GTK_WIDGET(m_timecodeLabel));

    gtk_box_append(GTK_BOX(bottomBox), transport);

    m_statusLabel = GTK_LABEL(gtk_label_new(""));
    gtk_widget_set_halign(GTK_WIDGET(m_statusLabel), GTK_ALIGN_START);
    gtk_widget_add_css_class(GTK_WIDGET(m_statusLabel), "dim-label");
    gtk_box_append(GTK_BOX(bottomBox), GTK_WIDGET(m_statusLabel));

    gtk_paned_set_end_child(GTK_PANED(paned), bottomBox);

    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbarView), paned);
    adw_application_window_set_content(m_window, toolbarView);
}

void AppWindow::onImportClicked()
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Import Media");
    gtk_file_dialog_open(dialog, GTK_WINDOW(m_window), nullptr, &AppWindow::fileOpenedTrampoline, this);
    g_object_unref(dialog);
}

void AppWindow::onFileOpened(GObject *sourceObject, GAsyncResult *result)
{
    GError *error = nullptr;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(sourceObject), result, &error);
    if (!file) {
        if (error) {
            Log::debug(std::string("Import file dialog closed without a selection: ") + error->message);
            g_error_free(error);
        }
        return;
    }

    char *path = g_file_get_path(file);
    if (path) {
        std::string err;
        if (!m_engine->importClip(path, m_activeTrack, err)) {
            showStatus(err);
        } else {
            showStatus(std::string("Imported to track ") + std::to_string(m_activeTrack) + ": " + path);
            refreshTimeline();
        }
        g_free(path);
    }
    g_object_unref(file);
}

void AppWindow::onSaveClicked()
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save Project");
    gtk_file_dialog_set_initial_name(dialog, "project.ustudio");
    gtk_file_dialog_save(dialog, GTK_WINDOW(m_window), nullptr, &AppWindow::saveFinishedTrampoline, this);
    g_object_unref(dialog);
}

void AppWindow::onSaveFinished(GObject *sourceObject, GAsyncResult *result)
{
    GError *error = nullptr;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(sourceObject), result, &error);
    if (!file) {
        if (error)
            g_error_free(error);
        return;
    }

    char *path = g_file_get_path(file);
    if (path) {
        std::string err;
        if (!m_engine->saveProject(path, err))
            showStatus(err);
        else
            showStatus(std::string("Saved: ") + path);
        g_free(path);
    }
    g_object_unref(file);
}

void AppWindow::onOpenProjectClicked()
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open Project");
    gtk_file_dialog_open(dialog, GTK_WINDOW(m_window), nullptr, &AppWindow::openProjectFinishedTrampoline, this);
    g_object_unref(dialog);
}

void AppWindow::onOpenProjectFinished(GObject *sourceObject, GAsyncResult *result)
{
    GError *error = nullptr;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(sourceObject), result, &error);
    if (!file) {
        if (error)
            g_error_free(error);
        return;
    }

    char *path = g_file_get_path(file);
    if (path) {
        std::string err;
        if (!m_engine->loadProject(path, err)) {
            showStatus(err);
        } else {
            m_activeTrack = 0;
            m_selectedClip = -1;
            refreshTimeline();
            showStatus(std::string("Opened: ") + path);
        }
        g_free(path);
    }
    g_object_unref(file);
}

void AppWindow::onRenderClicked()
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Render Project");
    gtk_file_dialog_set_initial_name(dialog, "export.mp4");
    gtk_file_dialog_save(dialog, GTK_WINDOW(m_window), nullptr, &AppWindow::renderFinishedTrampoline, this);
    g_object_unref(dialog);
}

void AppWindow::onRenderFinished(GObject *sourceObject, GAsyncResult *result)
{
    GError *error = nullptr;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(sourceObject), result, &error);
    if (!file) {
        if (error)
            g_error_free(error);
        return;
    }

    char *pathC = g_file_get_path(file);
    g_object_unref(file);
    if (!pathC)
        return;
    std::string path = pathC;
    g_free(pathC);

    showStatus("Rendering to " + path + " … (this can take a while — the window will stay responsive)");

    // MltEngine outlives this window for the whole process, and AppWindow
    // itself is never destroyed during normal operation (see main.cpp) —
    // capturing `this`/m_engine in this detached thread is safe on that
    // basis, matching the same reasoning WaveformCache's worker relies on.
    std::thread([this, path]() {
        std::string err;
        bool ok = m_engine->renderProject(path, err);

        struct Result
        {
            AppWindow *self;
            bool ok;
            std::string err;
            std::string path;
        };
        auto *result = new Result{this, ok, std::move(err), path};
        g_idle_add(
            [](gpointer data) -> gboolean {
                std::unique_ptr<Result> r(static_cast<Result *>(data));
                if (r->ok)
                    r->self->showStatus("Rendered: " + r->path);
                else
                    r->self->showStatus("Render failed: " + r->err);
                return G_SOURCE_REMOVE;
            },
            result);
    }).detach();
}

void AppWindow::onAddTrackClicked()
{
    int newTrack = m_engine->addTrack();
    m_activeTrack = newTrack;
    refreshTimeline();
    showStatus("Added track " + std::to_string(newTrack) + " (now active).");
}

void AppWindow::onPlayToggled()
{
    m_engine->togglePlay();
    const char *icon = m_engine->isPlaying() ? "media-playback-pause-symbolic" : "media-playback-start-symbolic";
    gtk_button_set_icon_name(m_playButton, icon);
}

void AppWindow::onSeekChanged()
{
    if (m_suppressSeekSignal)
        return;
    int frame = static_cast<int>(gtk_range_get_value(GTK_RANGE(m_seekScale)));
    m_engine->seek(frame);
}

void AppWindow::onSplitClicked()
{
    int frame = m_engine->currentFrame();
    if (m_engine->splitAt(m_activeTrack, frame))
        refreshTimeline();
    else
        showStatus("Nothing to split on track " + std::to_string(m_activeTrack) + " at the current playhead.");
}

void AppWindow::onTimelineClicked(double x, double y)
{
    int trackCount = m_engine->trackCount();
    if (trackCount <= 0)
        return;

    int row = static_cast<int>(y / kTrackRowHeight);
    m_activeTrack = std::clamp(row, 0, trackCount - 1);

    int total = m_engine->totalFrames();
    int widgetWidth = gtk_widget_get_width(GTK_WIDGET(m_timeline));
    // The handle strip is drag-only (see onTrackDragBegin); a plain click
    // there just selects the row's track without also seeking.
    if (x >= kHandleWidth && total > 0 && widgetWidth > 0) {
        double fraction = std::clamp((x - kHandleWidth) / (widgetWidth - kHandleWidth), 0.0, 1.0);
        int frame = static_cast<int>(fraction * total);

        m_selectedClip = -1;
        for (size_t i = 0; i < m_clips.size(); ++i) {
            const auto &clip = m_clips[i];
            if (clip.trackIndex == m_activeTrack && frame >= clip.startFrame && frame < clip.startFrame + clip.frames) {
                m_selectedClip = static_cast<int>(i);
                break;
            }
        }

        m_engine->seek(frame);
    }

    gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
}

void AppWindow::onTimelineRightClicked(double x, double y)
{
    int trackCount = m_engine->trackCount();
    if (trackCount <= 0)
        return;

    int row = std::clamp(static_cast<int>(y / kTrackRowHeight), 0, trackCount - 1);
    m_contextMenuTrack = row;
    m_contextMenuClipStartFrame = -1;
    m_contextMenuGapStartFrame = -1;

    int total = m_engine->totalFrames();
    int widgetWidth = gtk_widget_get_width(GTK_WIDGET(m_timeline));
    int frame = -1;
    if (total > 0 && widgetWidth > kHandleWidth && x >= kHandleWidth) {
        double contentWidth = widgetWidth - kHandleWidth;
        frame = static_cast<int>(((x - kHandleWidth) / contentWidth) * total);
    }
    m_contextMenuFrame = frame;

    if (frame >= 0) {
        for (const auto &clip : m_clips) {
            if (clip.trackIndex == row && frame >= clip.startFrame && frame < clip.startFrame + clip.frames) {
                m_contextMenuClipStartFrame = clip.startFrame;
                break;
            }
        }
        if (m_contextMenuClipStartFrame < 0 && m_engine->isGapAt(row, frame))
            m_contextMenuGapStartFrame = frame;
    }

    gtk_widget_set_visible(m_deleteClipButton, m_contextMenuClipStartFrame >= 0);
    gtk_widget_set_visible(m_closeGapButton, m_contextMenuGapStartFrame >= 0);
    gtk_widget_set_visible(m_removeTrackButton, m_contextMenuClipStartFrame < 0 && m_contextMenuGapStartFrame < 0);

    GdkRectangle rect{
        static_cast<int>(x), static_cast<int>(row * kTrackRowHeight), 1, static_cast<int>(kTrackRowHeight)};
    gtk_popover_set_pointing_to(m_trackContextMenu, &rect);
    gtk_popover_popup(m_trackContextMenu);
}

void AppWindow::onDeleteClipClicked()
{
    gtk_popover_popdown(m_trackContextMenu);
    if (m_contextMenuClipStartFrame < 0)
        return;

    if (m_engine->deleteClip(m_contextMenuTrack, m_contextMenuClipStartFrame)) {
        m_selectedClip = -1;
        refreshTimeline();
        showStatus("Deleted clip — gap left behind. Right-click the gap to close it.");
    } else {
        showStatus("Couldn't delete that clip.");
    }
}

void AppWindow::onCloseGapClicked()
{
    gtk_popover_popdown(m_trackContextMenu);
    if (m_contextMenuGapStartFrame < 0)
        return;

    if (m_engine->closeGap(m_contextMenuTrack, m_contextMenuGapStartFrame)) {
        m_selectedClip = -1;
        refreshTimeline();
        showStatus("Closed gap.");
    } else {
        showStatus("Couldn't close that gap.");
    }
}

void AppWindow::onRemoveTrackClicked()
{
    gtk_popover_popdown(m_trackContextMenu);

    if (m_contextMenuTrack < 0)
        return;

    if (m_engine->removeTrack(m_contextMenuTrack)) {
        int trackCount = m_engine->trackCount();
        m_activeTrack = std::clamp(m_activeTrack, 0, std::max(trackCount - 1, 0));
        m_selectedClip = -1;
        refreshTimeline();
        showStatus("Removed track " + std::to_string(m_contextMenuTrack) + ".");
    } else {
        showStatus("Can't remove the last remaining track.");
    }
    m_contextMenuTrack = -1;
}

bool AppWindow::onTrackDragBegin(double x, double y)
{
    m_dragMode = TimelineDragMode::None;
    m_dragStartX = x;
    m_dragStartY = y;

    int trackCount = m_engine->trackCount();
    if (trackCount <= 0)
        return false;

    if (x < kHandleWidth) {
        m_dragMode = TimelineDragMode::TrackReorder;
        m_draggingTrack = std::clamp(static_cast<int>(y / kTrackRowHeight), 0, trackCount - 1);
        m_dragHoverRow = m_draggingTrack;
        gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
        return true;
    }

    int total = m_engine->totalFrames();
    int widgetWidth = gtk_widget_get_width(GTK_WIDGET(m_timeline));
    if (total <= 0 || widgetWidth <= kHandleWidth)
        return false;

    double contentWidth = widgetWidth - kHandleWidth;
    int row = static_cast<int>(y / kTrackRowHeight);
    int frameAtX = static_cast<int>(((x - kHandleWidth) / contentWidth) * total);

    for (size_t i = 0; i < m_clips.size(); ++i) {
        const auto &clip = m_clips[i];
        if (clip.trackIndex != row)
            continue;
        if (frameAtX < clip.startFrame || frameAtX >= clip.startFrame + clip.frames)
            continue;

        double clipLeftX = kHandleWidth + (static_cast<double>(clip.startFrame) / total) * contentWidth;
        double clipRightX = kHandleWidth + (static_cast<double>(clip.startFrame + clip.frames) / total) * contentWidth;

        m_dragClipTrack = clip.trackIndex;
        m_dragClipStartFrame = clip.startFrame;
        m_dragClipFrames = clip.frames;
        m_dragPreviewTrack = clip.trackIndex;
        m_dragPreviewStartFrame = clip.startFrame;
        m_dragPreviewFrames = clip.frames;
        m_selectedClip = static_cast<int>(i);
        m_activeTrack = clip.trackIndex;

        if (x - clipLeftX < kEdgeGrabWidth)
            m_dragMode = TimelineDragMode::TrimClipStart;
        else if (clipRightX - x < kEdgeGrabWidth)
            m_dragMode = TimelineDragMode::TrimClipEnd;
        else
            m_dragMode = TimelineDragMode::MoveClip;

        gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
        return true;
    }

    return false; // empty space -- let the plain click gesture seek there
}

void AppWindow::onTrackDragUpdate(double offsetX, double offsetY)
{
    if (m_dragMode == TimelineDragMode::None)
        return;

    int trackCount = m_engine->trackCount();

    if (m_dragMode == TimelineDragMode::TrackReorder) {
        double currentY = m_dragStartY + offsetY;
        m_dragHoverRow = std::clamp(static_cast<int>(currentY / kTrackRowHeight), 0, std::max(trackCount - 1, 0));
        gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
        return;
    }

    int total = m_engine->totalFrames();
    int widgetWidth = gtk_widget_get_width(GTK_WIDGET(m_timeline));
    double contentWidth = std::max(widgetWidth - kHandleWidth, 1.0);
    if (total <= 0)
        return;
    int deltaFrames = static_cast<int>(offsetX / contentWidth * total);

    if (m_dragMode == TimelineDragMode::MoveClip) {
        m_dragPreviewTrack = std::clamp(
            static_cast<int>((m_dragStartY + offsetY) / kTrackRowHeight), 0, std::max(trackCount - 1, 0));
        m_dragPreviewStartFrame = std::max(0, m_dragClipStartFrame + deltaFrames);
        m_dragPreviewFrames = m_dragClipFrames;
    } else if (m_dragMode == TimelineDragMode::TrimClipStart) {
        int fixedEnd = m_dragClipStartFrame + m_dragClipFrames;
        int newStart = std::clamp(m_dragClipStartFrame + deltaFrames, 0, fixedEnd - 1);
        m_dragPreviewTrack = m_dragClipTrack;
        m_dragPreviewStartFrame = newStart;
        m_dragPreviewFrames = fixedEnd - newStart;
    } else if (m_dragMode == TimelineDragMode::TrimClipEnd) {
        int newEnd = std::max(m_dragClipStartFrame + 1, m_dragClipStartFrame + m_dragClipFrames + deltaFrames);
        m_dragPreviewTrack = m_dragClipTrack;
        m_dragPreviewStartFrame = m_dragClipStartFrame;
        m_dragPreviewFrames = newEnd - m_dragClipStartFrame;
    }

    gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
}

void AppWindow::onTrackDragEnd(double offsetX, double offsetY)
{
    bool trivial = std::abs(offsetX) < kDragClickThreshold && std::abs(offsetY) < kDragClickThreshold;
    TimelineDragMode mode = m_dragMode;

    if (mode == TimelineDragMode::TrackReorder) {
        int trackCount = m_engine->trackCount();
        double currentY = m_dragStartY + offsetY;
        int targetRow = std::clamp(static_cast<int>(currentY / kTrackRowHeight), 0, std::max(trackCount - 1, 0));
        if (targetRow != m_draggingTrack && m_engine->moveTrack(m_draggingTrack, targetRow)) {
            m_activeTrack = targetRow;
            m_selectedClip = -1;
            showStatus("Moved track " + std::to_string(m_draggingTrack) + " to " + std::to_string(targetRow) + ".");
        }
    } else if (mode == TimelineDragMode::MoveClip || mode == TimelineDragMode::TrimClipStart
               || mode == TimelineDragMode::TrimClipEnd) {
        if (trivial) {
            // Not really a drag -- treat as the plain click it was.
            onTimelineClicked(m_dragStartX, m_dragStartY);
        } else if (mode == TimelineDragMode::MoveClip) {
            if (m_engine->moveClip(
                    m_dragClipTrack, m_dragClipStartFrame, m_dragPreviewTrack, m_dragPreviewStartFrame)) {
                m_activeTrack = m_dragPreviewTrack;
            } else {
                showStatus("Can't move the clip there — that space is occupied.");
            }
        } else if (mode == TimelineDragMode::TrimClipStart) {
            if (!m_engine->trimClipStart(m_dragClipTrack, m_dragClipStartFrame, m_dragPreviewStartFrame))
                showStatus("Can't trim the clip that far — space is occupied or the source has no more frames.");
        } else if (mode == TimelineDragMode::TrimClipEnd) {
            int newEnd = m_dragPreviewStartFrame + m_dragPreviewFrames;
            if (!m_engine->trimClipEnd(m_dragClipTrack, m_dragClipStartFrame, newEnd))
                showStatus("Can't trim the clip that far — the source has no more frames.");
        }
    }

    m_dragMode = TimelineDragMode::None;
    m_draggingTrack = -1;
    m_dragHoverRow = -1;
    m_dragClipTrack = -1;
    m_dragClipStartFrame = -1;
    refreshTimeline();
}

void AppWindow::onTimelineDraw(cairo_t *cr, int width, int height)
{
    (void)height; // row layout is driven by kTrackRowHeight, not the widget's actual allocation
    int trackCount = m_engine->trackCount();
    if (trackCount <= 0)
        return;

    double contentWidth = std::max(width - kHandleWidth, 1.0);

    // Row backgrounds: a faint tint on the active track, a stronger one on
    // the current drag drop-target row, plus separators and a grip icon in
    // the handle strip.
    for (int t = 0; t < trackCount; ++t) {
        double rowY = t * kTrackRowHeight;

        if (m_draggingTrack >= 0 && t == m_dragHoverRow) {
            cairo_set_source_rgba(cr, kSelectedR, kSelectedG, kSelectedB, 0.12);
            cairo_rectangle(cr, 0, rowY, width, kTrackRowHeight);
            cairo_fill(cr);
        } else if (t == m_activeTrack) {
            cairo_set_source_rgba(cr, kActiveTrackR, kActiveTrackG, kActiveTrackB, 0.07);
            cairo_rectangle(cr, 0, rowY, width, kTrackRowHeight);
            cairo_fill(cr);
        }

        if (t > 0) {
            cairo_set_source_rgb(cr, kClipBorderR, kClipBorderG, kClipBorderB);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, 0, rowY);
            cairo_line_to(cr, width, rowY);
            cairo_stroke(cr);
        }

        // Grip icon: three short horizontal lines centered in the handle
        // strip, signaling "drag here to reorder".
        cairo_set_source_rgb(cr, kClipBorderR, kClipBorderG, kClipBorderB);
        cairo_set_line_width(cr, 2.0);
        double gripCenterX = kHandleWidth / 2.0;
        double gripCenterY = rowY + kTrackRowHeight / 2.0;
        for (int line = -1; line <= 1; ++line) {
            double ly = gripCenterY + line * 5.0;
            cairo_move_to(cr, gripCenterX - 5.0, ly);
            cairo_line_to(cr, gripCenterX + 5.0, ly);
            cairo_stroke(cr);
        }
        cairo_move_to(cr, kHandleWidth, rowY);
        cairo_line_to(cr, kHandleWidth, rowY + kTrackRowHeight);
        cairo_stroke(cr);
    }

    int total = m_engine->totalFrames();
    if (total <= 0)
        return;

    auto drawClipRect = [&](int trackIndex, int startFrame, int frames, bool selected, bool ghost) {
        double x = kHandleWidth + (static_cast<double>(startFrame) / total) * contentWidth;
        double w = (static_cast<double>(frames) / total) * contentWidth;
        double rowY = trackIndex * kTrackRowHeight;

        cairo_set_source_rgba(cr, kClipFillR, kClipFillG, kClipFillB, ghost ? 0.5 : 1.0);
        cairo_rectangle(cr, x + 1, rowY + 4, std::max(w - 2, 1.0), kTrackRowHeight - 8.0);
        cairo_fill_preserve(cr);

        if (selected)
            cairo_set_source_rgba(cr, kSelectedR, kSelectedG, kSelectedB, ghost ? 0.7 : 1.0);
        else
            cairo_set_source_rgba(cr, kClipBorderR, kClipBorderG, kClipBorderB, ghost ? 0.7 : 1.0);
        cairo_set_line_width(cr, selected ? 2.0 : 1.0);
        cairo_stroke(cr);
    };

    // Waveform: fetches cached peaks (kicking off async computation if not
    // yet available — see WaveformCache) and draws a vertical-bar envelope
    // across the clip's rectangle. Skipped for the actively-dragged clip
    // (its in/out are changing every frame during a trim, which would just
    // thrash the cache) — reasonable to not bother re-drawing a waveform
    // for the ~second a drag lasts.
    auto drawWaveform = [&](const MltEngine::ClipInfo &clip, double x, double w) {
        if (clip.resource.empty())
            return;
        const std::vector<float> *peaks = m_waveforms->peaksFor(clip.resource, clip.in, clip.out);
        if (!peaks || peaks->empty())
            return;

        double rowY = clip.trackIndex * kTrackRowHeight;
        double midY = rowY + kTrackRowHeight / 2.0;
        double maxBarHalfHeight = (kTrackRowHeight - 12.0) / 2.0;
        size_t peakCount = peaks->size();
        int pixelWidth = std::max(static_cast<int>(w), 1);

        cairo_set_source_rgba(cr, kWaveformR, kWaveformG, kWaveformB, 0.85);
        cairo_set_line_width(cr, 1.0);
        for (int px = 0; px < pixelWidth; ++px) {
            size_t startIdx = static_cast<size_t>((static_cast<double>(px) / pixelWidth) * peakCount);
            size_t endIdx =
                std::min(peakCount, std::max(startIdx + 1, static_cast<size_t>((static_cast<double>(px + 1) / pixelWidth) * peakCount)));

            float peak = 0.0f;
            for (size_t k = startIdx; k < endIdx; ++k)
                peak = std::max(peak, (*peaks)[k]);

            double barHalf = std::max(static_cast<double>(peak) * maxBarHalfHeight, 1.0);
            double colX = x + px + 0.5;
            cairo_move_to(cr, colX, midY - barHalf);
            cairo_line_to(cr, colX, midY + barHalf);
            cairo_stroke(cr);
        }
    };

    for (size_t i = 0; i < m_clips.size(); ++i) {
        const auto &clip = m_clips[i];
        bool isDragged = m_dragMode != TimelineDragMode::None && clip.trackIndex == m_dragClipTrack
            && clip.startFrame == m_dragClipStartFrame;
        bool selected = (static_cast<int>(i) == m_selectedClip);

        if (isDragged && m_dragMode == TimelineDragMode::MoveClip)
            continue; // drawn as a ghost at the preview position instead, below

        int drawTrack = isDragged ? m_dragPreviewTrack : clip.trackIndex;
        int drawStart = isDragged ? m_dragPreviewStartFrame : clip.startFrame;
        int drawFrames = isDragged ? m_dragPreviewFrames : clip.frames;
        drawClipRect(drawTrack, drawStart, drawFrames, selected, false);

        if (!isDragged) {
            double x = kHandleWidth + (static_cast<double>(clip.startFrame) / total) * contentWidth;
            double w = (static_cast<double>(clip.frames) / total) * contentWidth;
            drawWaveform(clip, x, w);
        }
    }

    if (m_dragMode == TimelineDragMode::MoveClip)
        drawClipRect(m_dragPreviewTrack, m_dragPreviewStartFrame, m_dragPreviewFrames, true, true);
}

void AppWindow::onFrameReady(std::vector<uint8_t> rgba, int width, int height, int frameNumber)
{
    if (!rgba.empty() && width > 0 && height > 0) {
        GBytes *bytes = g_bytes_new(rgba.data(), rgba.size());
        GdkTexture *texture = gdk_memory_texture_new(width, height, GDK_MEMORY_R8G8B8A8, bytes, width * 4);
        g_bytes_unref(bytes);
        gtk_picture_set_paintable(m_preview, GDK_PAINTABLE(texture));
        g_object_unref(texture);
    }

    refreshTransport(frameNumber);
}

void AppWindow::onWaveformReady()
{
    gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
}

void AppWindow::refreshTimeline()
{
    m_clips = m_engine->clips();

    int trackCount = m_engine->trackCount();
    int height = std::max(trackCount, 1) * static_cast<int>(kTrackRowHeight);
    gtk_widget_set_size_request(GTK_WIDGET(m_timeline), -1, height);

    int total = m_engine->totalFrames();
    m_suppressSeekSignal = true;
    gtk_range_set_range(GTK_RANGE(m_seekScale), 0, std::max(total - 1, 0));
    m_suppressSeekSignal = false;

    gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
}

void AppWindow::refreshTransport(int frameNumber)
{
    m_suppressSeekSignal = true;
    gtk_range_set_value(GTK_RANGE(m_seekScale), frameNumber);
    m_suppressSeekSignal = false;

    gtk_label_set_text(m_timecodeLabel, formatTimecode(frameNumber).c_str());
}

void AppWindow::showStatus(const std::string &text)
{
    gtk_label_set_text(m_statusLabel, text.c_str());
}

std::string AppWindow::formatTimecode(int frame) const
{
    double fps = m_engine->fps();
    int fpsInt = fps > 0.0 ? static_cast<int>(fps + 0.5) : 25;
    if (fpsInt <= 0)
        fpsInt = 25;

    int totalSeconds = frame / fpsInt;
    int frames = frame % fpsInt;
    int hours = totalSeconds / 3600;
    int minutes = (totalSeconds % 3600) / 60;
    int seconds = totalSeconds % 60;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d:%02d", hours, minutes, seconds, frames);
    return buf;
}

// ---- GTK/GObject trampolines: static C-linkage-compatible callbacks that
// forward straight into the owning AppWindow instance. ----

void AppWindow::importClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onImportClicked();
}

void AppWindow::fileOpenedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onFileOpened(sourceObject, result);
}

void AppWindow::saveClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSaveClicked();
}

void AppWindow::saveFinishedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSaveFinished(sourceObject, result);
}

void AppWindow::openProjectClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onOpenProjectClicked();
}

void AppWindow::openProjectFinishedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onOpenProjectFinished(sourceObject, result);
}

void AppWindow::renderClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onRenderClicked();
}

void AppWindow::renderFinishedTrampoline(GObject *sourceObject, GAsyncResult *result, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onRenderFinished(sourceObject, result);
}

void AppWindow::addTrackClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onAddTrackClicked();
}

void AppWindow::playToggledTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onPlayToggled();
}

void AppWindow::seekChangedTrampoline(GtkRange *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSeekChanged();
}

void AppWindow::splitClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSplitClicked();
}

void AppWindow::timelineDrawTrampoline(GtkDrawingArea *, cairo_t *cr, int width, int height, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onTimelineDraw(cr, width, height);
}

void AppWindow::timelineClickTrampoline(GtkGestureClick *, int, double x, double y, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onTimelineClicked(x, y);
}

void AppWindow::timelineRightClickTrampoline(GtkGestureClick *, int, double x, double y, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onTimelineRightClicked(x, y);
}

void AppWindow::deleteClipClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onDeleteClipClicked();
}

void AppWindow::closeGapClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onCloseGapClicked();
}

void AppWindow::removeTrackClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onRemoveTrackClicked();
}

void AppWindow::trackDragBeginTrampoline(GtkGestureDrag *gesture, double x, double y, gpointer userData)
{
    if (static_cast<AppWindow *>(userData)->onTrackDragBegin(x, y))
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

void AppWindow::trackDragUpdateTrampoline(GtkGestureDrag *, double offsetX, double offsetY, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onTrackDragUpdate(offsetX, offsetY);
}

void AppWindow::trackDragEndTrampoline(GtkGestureDrag *, double offsetX, double offsetY, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onTrackDragEnd(offsetX, offsetY);
}
