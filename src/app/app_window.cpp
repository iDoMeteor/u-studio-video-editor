#include "app_window.h"

#include "autosave.h"
#include "core/commands/composite_command.h"
#include "core/commands/primitives.h"
#include "core/commands/transaction.h"
#include "core/log.h"
#include "core/xml/reader.h"
#include "core/xml/writer.h"

#include <pango/pangocairo.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <system_error>
#include <thread>

namespace ustudio::app {

namespace Log = ustudio::core::Log;

namespace {
// Mirrors the tokens in style_css.h. Kept as plain hex here because clips
// are drawn with cairo inside a single GtkDrawingArea, not as separate
// widgets, so CSS selectors can't reach them.
constexpr double kClipFillR = 0x1b / 255.0, kClipFillG = 0x12 / 255.0, kClipFillB = 0x30 / 255.0;
constexpr double kClipBorderR = 0x34 / 255.0, kClipBorderG = 0x23 / 255.0, kClipBorderB = 0x57 / 255.0;
constexpr double kSelectedR = 0x19 / 255.0, kSelectedG = 0xe3 / 255.0, kSelectedB = 0xff / 255.0;
constexpr double kActiveTrackR = 0x19 / 255.0, kActiveTrackG = 0xe3 / 255.0, kActiveTrackB = 0xff / 255.0;
// --cyan-500 (claude-design-system/tokens/colors.css) -- same token/value
// as kSelected*/kActiveTrack* above, kept as its own named constant since
// the playhead is a distinct element that shouldn't silently follow if
// either of those two is ever retuned separately.
constexpr double kPlayheadR = 0x19 / 255.0, kPlayheadG = 0xe3 / 255.0, kPlayheadB = 0xff / 255.0;
// --warning (claude-design-system/tokens/colors.css) -- a locked track's
// row tint; warning/caution is the closest existing token to "you can't
// edit this", and reusing it keeps this from inventing an off-palette
// color for a single indicator.
constexpr double kLockedR = 0xff / 255.0, kLockedG = 0xc2 / 255.0, kLockedB = 0x4d / 255.0;
constexpr double kTrackRowHeight = 60.0;
constexpr double kHandleWidth = 22.0;
constexpr double kEdgeGrabWidth = 8.0;
constexpr double kDragClickThreshold = 3.0; // below this, a "drag" is really just a click
constexpr double kWaveformR = 0x9d / 255.0, kWaveformG = 0x4e / 255.0, kWaveformB = 0xff / 255.0; // brand violet
// window_fg_color (style.css) -- the design system's main light-on-dark
// text tone, used for every cairo-drawn label below (track names, a
// clip's track-title corner badge) since none of this canvas's text can
// be reached by a CSS selector (see the kClipFill* comment above).
constexpr double kLabelTextR = 0xff / 255.0, kLabelTextG = 0xef / 255.0, kLabelTextB = 0xfb / 255.0;
// A slim strip at the top of each track row, reserved for that track's
// name label -- carved out of the existing row height (kTrackRowHeight
// itself, and therefore every hit-test/drag calculation keyed on it,
// stays untouched; only where clips draw *within* their row shrinks).
constexpr double kTrackLabelHeight = 14.0;

// Single-line text, ellipsized to fit `maxWidth`, top-left anchored at
// (x, y) -- the one place this file draws text directly onto the cairo
// canvas rather than through a GTK label/CSS (track names, and a clip's
// track-title corner badge). Pango, not cairo's own "toy" text API
// (cairo_show_text), for real font shaping/metrics; "Sans"/"Monospace"
// are generic Pango family aliases always resolvable regardless of which
// of the design system's actual fonts (Space Grotesk, JetBrains Mono)
// happen to be installed (CLAUDE.md: "every rule must keep its generic
// fallback").
void drawLabel(cairo_t *cr, const std::string &text, double x, double y, double maxWidth, bool monospace = false)
{
    if (text.empty() || maxWidth <= 0)
        return;

    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, text.c_str(), -1);
    PangoFontDescription *desc = pango_font_description_from_string(monospace ? "Monospace 8" : "Sans 8");
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_width(layout, static_cast<int>(maxWidth * PANGO_SCALE));
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

    cairo_set_source_rgba(cr, kLabelTextR, kLabelTextG, kLabelTextB, 0.85);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

// Media-browser "length" column: an asset's own native duration, not
// tied to any sequence's fps -- "H:MM:SS", or "M:SS" under an hour.
std::string formatMediaLength(double seconds)
{
    if (seconds <= 0.0)
        return "—";
    int total = static_cast<int>(seconds + 0.5);
    int hours = total / 3600;
    int minutes = (total % 3600) / 60;
    int secs = total % 60;
    char buf[32];
    if (hours > 0)
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", hours, minutes, secs);
    else
        std::snprintf(buf, sizeof(buf), "%d:%02d", minutes, secs);
    return buf;
}

// Media-browser "fps" column -- a plain decimal (e.g. "30" or "29.97"),
// not a raw num/den fraction; trims to an integer when the ratio already
// is one (the overwhelmingly common case) rather than always showing a
// misleadingly precise ".00".
std::string formatMediaFps(core::Rational fps)
{
    if (fps.num <= 0 || fps.den <= 0)
        return "—";
    double value = static_cast<double>(fps.num) / fps.den;
    char buf[32];
    if (fps.num % fps.den == 0)
        std::snprintf(buf, sizeof(buf), "%d", fps.num / fps.den);
    else
        std::snprintf(buf, sizeof(buf), "%.2f", value);
    return buf;
}

// `length` is the caller's ADJUSTED length (still images get resized to
// cover the current timeline, or a 10s default -- see the call site),
// deliberately separate from `probed.length` (the raw probe result,
// meaningless for a still image beyond "MLT's pixbuf default").
core::Asset makeImportedAsset(const std::string &path, core::FrameIndex length,
                              const engine::EngineSync::ProbedMedia &probed, const core::Rational &sequenceFps)
{
    core::Asset asset;
    asset.path = path;
    auto slash = path.find_last_of('/');
    asset.displayName = (slash == std::string::npos) ? path : path.substr(slash + 1);
    asset.info.hasVideo = true;
    asset.info.hasAudio = probed.hasAudio;
    asset.info.isStillImage = probed.isStillImage;
    asset.info.lengthInSequenceFrames = length;
    asset.info.fps = probed.fps;
    asset.info.width = probed.width;
    asset.info.height = probed.height;
    // `length` is already measured in the sequence's own frames
    // (probeMedia opens its throwaway producer at that fps), so duration
    // follows directly from it -- no dependency on probed.fps (0 for a
    // still image/generator) being set at all.
    if (sequenceFps.num > 0 && sequenceFps.den > 0)
        asset.info.nativeDurationSeconds = static_cast<double>(length) * sequenceFps.den / sequenceFps.num;
    // "Format" (container) is a filename-extension read, not an MLT
    // property -- meta.media.* has no reliable container/format string
    // (verified empirically alongside probeMedia's fps/width/height
    // work), and the extension is exactly what a "format" column means
    // to a user browsing their imports anyway.
    auto dot = asset.displayName.find_last_of('.');
    asset.info.container = (dot == std::string::npos) ? std::string{} : asset.displayName.substr(dot + 1);
    asset.status = core::Asset::Status::Ready;
    return asset;
}
} // namespace

AppWindow::AppWindow(GtkApplication *app)
{
    // One starting track, matching v1's "track 0 always exists" default --
    // not through the UndoStack, since this is the pristine starting
    // state, not a user edit to undo back out of.
    m_model.addTrack(core::Track::Kind::Video, 0, "V1");

    m_engineSync = std::make_unique<engine::EngineSync>(m_model);

    m_playback = std::make_unique<engine::PlaybackController>();
    m_playback->setTractor(m_engineSync->tractorPtr());
    // The one place playback gets re-pointed at a rebuilt tractor: fires on
    // every EngineSync::rebuildAll(), whether triggered automatically by a
    // model edit or by an explicit reset() (Open Project, recovery load) --
    // no call site below needs its own setTractor() anymore.
    m_engineSync->rebuilt.connect([this] {
        m_playback->setTractor(m_engineSync->tractorPtr());
        m_lastEditMonotonicUsec = g_get_monotonic_time();
    });
    m_engineSync->mediaUnavailable.connect([this](const std::string &path) {
        showStatus("Couldn't open \"" + path + "\" — showing black in its place. The file may have moved or been "
                   "deleted.");
    });
    m_playback->setFrameCallback([this](std::vector<uint8_t> rgba, int width, int height, int frameNumber) {
        onFrameReady(std::move(rgba), width, height, frameNumber);
    });

    m_waveforms = std::make_unique<engine::WaveformCache>([this] { onWaveformReady(); });
    m_thumbnails = std::make_unique<engine::ThumbnailCache>([this] { onThumbnailReady(); });

    // Single source of truth for the undo/redo buttons and the title's
    // dirty mark (audit A1): every place that used to call
    // updateWindowTitle() by hand after touching the undo stack (import,
    // split, move, trim, Save, Open, Undo, Redo, Recover, ...) now just
    // goes through UndoStack::execute()/undo()/redo()/setCleanPoint()/
    // clear(), all of which emit `changed`, so nothing can forget to
    // refresh these after an ordinary edit the way manual call sites did.
    m_undoStack.changed.connect([this] { updateWindowTitle(); });

    gchar *sessionUuid = g_uuid_string_random();
    m_autosaveSessionId = sessionUuid;
    g_free(sessionUuid);

    buildUi(app);
    installActions(app);
    g_signal_connect(m_window, "notify::is-active", G_CALLBACK(&AppWindow::windowActiveChangedTrampoline), this);
    g_signal_connect(m_window, "close-request", G_CALLBACK(&AppWindow::closeRequestTrampoline), this);
    // Heartbeat, not a one-shot timer reset on every edit (doc 09: "2
    // minutes after the last command while dirty"): simpler to reason
    // about than adding/removing a GSource on every keystroke-equivalent,
    // and 10s of slack on a 2-minute threshold is immaterial.
    m_autosaveHeartbeatId = g_timeout_add_seconds(10, &AppWindow::autosaveHeartbeatTrampoline, this);

    refreshTimeline();
    updateWindowTitle();
    offerRecoveryIfAny();
    showStatus("Import a media file to begin.");
}

void AppWindow::prepareForShutdown()
{
    Log::info("[app] Preparing for shutdown");
    // Audit A2: a last-resort safety net. onCloseRequest() prompts before
    // a normal window close, but this fires unconditionally whenever the
    // GApplication actually quits -- including after that prompt's own
    // "Discard" (the autosave doesn't touch m_currentProjectPath, so
    // discarding still leaves a recovery point behind) and any path that
    // reaches shutdown without going through onCloseRequest at all.
    if (!m_undoStack.isClean())
        performAutosave();
    if (m_playback)
        m_playback->shutdown();
}

void AppWindow::buildUi(GtkApplication *app)
{
    m_window = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
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

    // Verified against the installed Adwaita symbolic icon set
    // (/usr/share/icons/Adwaita/symbolic/actions/sidebar-show-symbolic.svg)
    // rather than guessed -- CLAUDE.md's icon rule.
    GtkWidget *toggleMediaBrowserButton = gtk_button_new_from_icon_name("sidebar-show-symbolic");
    gtk_widget_set_tooltip_text(toggleMediaBrowserButton, "Media browser");
    g_signal_connect(toggleMediaBrowserButton, "clicked", G_CALLBACK(&AppWindow::toggleMediaBrowserClickedTrampoline),
                      this);
    adw_header_bar_pack_start(ADW_HEADER_BAR(headerBar), toggleMediaBrowserButton);

    m_undoButton = GTK_BUTTON(gtk_button_new_from_icon_name("edit-undo-symbolic"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(m_undoButton), "Undo (Ctrl+Z)");
    g_signal_connect(m_undoButton, "clicked", G_CALLBACK(&AppWindow::undoClickedTrampoline), this);
    adw_header_bar_pack_start(ADW_HEADER_BAR(headerBar), GTK_WIDGET(m_undoButton));

    m_redoButton = GTK_BUTTON(gtk_button_new_from_icon_name("edit-redo-symbolic"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(m_redoButton), "Redo (Ctrl+Shift+Z)");
    g_signal_connect(m_redoButton, "clicked", G_CALLBACK(&AppWindow::redoClickedTrampoline), this);
    adw_header_bar_pack_start(ADW_HEADER_BAR(headerBar), GTK_WIDGET(m_redoButton));

    GtkWidget *newProjectButton = gtk_button_new_from_icon_name("document-new-symbolic");
    gtk_widget_set_tooltip_text(newProjectButton, "New project (reset to an empty project)");
    g_signal_connect(newProjectButton, "clicked", G_CALLBACK(&AppWindow::newProjectClickedTrampoline), this);
    adw_header_bar_pack_end(ADW_HEADER_BAR(headerBar), newProjectButton);

    GtkWidget *reloadButton = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(reloadButton, "Reload project from disk");
    g_signal_connect(reloadButton, "clicked", G_CALLBACK(&AppWindow::reloadProjectClickedTrampoline), this);
    adw_header_bar_pack_end(ADW_HEADER_BAR(headerBar), reloadButton);

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

    // Preview row: [media browser panel | preview], side by side.
    GtkWidget *previewRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    // Media browser: a plain toggleable GtkBox, not GtkRevealer (unused
    // elsewhere in this codebase, and an animated slide would be
    // "decorative" per the design system's own glow/animation rule) --
    // gtk_widget_set_visible(FALSE) on a box child reclaims its layout
    // space immediately, which is all "collapsible" needs here.
    GtkWidget *mediaBrowserScroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(mediaBrowserScroller), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(mediaBrowserScroller, 320, -1);
    gtk_widget_add_css_class(mediaBrowserScroller, "media-browser-panel");
    m_mediaBrowserList = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 4));
    gtk_widget_set_margin_top(GTK_WIDGET(m_mediaBrowserList), 6);
    gtk_widget_set_margin_bottom(GTK_WIDGET(m_mediaBrowserList), 6);
    gtk_widget_set_margin_start(GTK_WIDGET(m_mediaBrowserList), 6);
    gtk_widget_set_margin_end(GTK_WIDGET(m_mediaBrowserList), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(mediaBrowserScroller), GTK_WIDGET(m_mediaBrowserList));
    m_mediaBrowserPanel = mediaBrowserScroller;
    gtk_widget_set_visible(m_mediaBrowserPanel, FALSE); // starts collapsed
    gtk_box_append(GTK_BOX(previewRow), m_mediaBrowserPanel);

    // Right-click menu for a media browser row. Parented once to
    // m_mediaBrowserPanel (the outer GtkScrolledWindow, like
    // m_trackContextMenu -> m_timeline below), deliberately NOT to
    // m_mediaBrowserList or to the row that was clicked: rows are
    // destroyed and rebuilt wholesale by refreshMediaBrowser() on every
    // bin change, and gtk_widget_set_parent() makes a popover a real
    // child in the generic widget tree -- parenting it to
    // m_mediaBrowserList would put it right in the path of
    // refreshMediaBrowser()'s own "walk every child of the list and
    // remove it" loop (confirmed empirically: it was, and got swept up
    // and destroyed by the very first refresh after buildUi(), leaving
    // this member dangling -- a real, reproduced use-after-free crash in
    // gtk_popover_set_pointing_to() the first time a row was right-
    // clicked, via coredumpctl + gdb backtrace, 2026-09-23). The outer
    // scroller is never touched by that loop -- only its one designated
    // child (m_mediaBrowserList, set via gtk_scrolled_window_set_child)
    // is -- so it's a safe, stable parent.
    m_mediaBrowserContextMenu = GTK_POPOVER(gtk_popover_new());
    gtk_widget_set_parent(GTK_WIDGET(m_mediaBrowserContextMenu), m_mediaBrowserPanel);
    GtkWidget *mediaContextBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    m_removeAssetButton = gtk_button_new_with_label("Remove from Project");
    gtk_widget_add_css_class(m_removeAssetButton, "flat");
    g_signal_connect(m_removeAssetButton, "clicked", G_CALLBACK(&AppWindow::removeAssetClickedTrampoline), this);
    gtk_box_append(GTK_BOX(mediaContextBox), m_removeAssetButton);
    m_deleteAssetFileButton = gtk_button_new_with_label("Move File to Trash…");
    gtk_widget_add_css_class(m_deleteAssetFileButton, "flat");
    g_signal_connect(m_deleteAssetFileButton, "clicked", G_CALLBACK(&AppWindow::deleteAssetFileClickedTrampoline),
                      this);
    gtk_box_append(GTK_BOX(mediaContextBox), m_deleteAssetFileButton);
    gtk_popover_set_child(m_mediaBrowserContextMenu, mediaContextBox);

    GtkWidget *previewFrame = gtk_frame_new(nullptr);
    gtk_widget_add_css_class(previewFrame, "preview-frame");
    gtk_widget_set_hexpand(previewFrame, TRUE);
    m_preview = GTK_PICTURE(gtk_picture_new());
    gtk_picture_set_content_fit(m_preview, GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_hexpand(GTK_WIDGET(m_preview), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(m_preview), TRUE);
    gtk_frame_set_child(GTK_FRAME(previewFrame), GTK_WIDGET(m_preview));
    gtk_box_append(GTK_BOX(previewRow), previewFrame);

    gtk_paned_set_start_child(GTK_PANED(paned), previewRow);

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

    // query-tooltip (GTK4's per-region-tooltip mechanism for a custom-drawn
    // widget) -- onTimelineQueryTooltip hit-tests (x, y) against m_clips.
    gtk_widget_set_has_tooltip(GTK_WIDGET(m_timeline), TRUE);
    g_signal_connect(m_timeline, "query-tooltip", G_CALLBACK(&AppWindow::timelineQueryTooltipTrampoline), this);

    // Drop target for dragging a media browser row onto the timeline
    // (refreshMediaBrowser() puts a matching GtkDragSource, carrying the
    // asset's AssetId::value as a G_TYPE_INT64, on each row).
    GtkDropTarget *dropTarget = gtk_drop_target_new(G_TYPE_INT64, GDK_ACTION_COPY);
    g_signal_connect(dropTarget, "drop", G_CALLBACK(&AppWindow::timelineDropTrampoline), this);
    gtk_widget_add_controller(GTK_WIDGET(m_timeline), GTK_EVENT_CONTROLLER(dropTarget));

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

    m_splitAudioButton = gtk_button_new_with_label("Split Audio");
    gtk_widget_add_css_class(m_splitAudioButton, "flat");
    g_signal_connect(m_splitAudioButton, "clicked", G_CALLBACK(&AppWindow::splitAudioClickedTrampoline), this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_splitAudioButton);

    m_closeGapButton = gtk_button_new_with_label("Close Gap");
    gtk_widget_add_css_class(m_closeGapButton, "flat");
    g_signal_connect(m_closeGapButton, "clicked", G_CALLBACK(&AppWindow::closeGapClickedTrampoline), this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_closeGapButton);

    // Volume + Lock/Unlock + Remove Track: all whole-track actions, shown
    // together whenever the right-click landed on empty track space (see
    // onTimelineRightClicked) rather than on a clip or a gap.
    GtkWidget *volumeRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(volumeRow), gtk_label_new("Track volume"));
    m_trackVolumeScale = GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.05));
    gtk_scale_set_draw_value(m_trackVolumeScale, FALSE);
    gtk_widget_set_hexpand(GTK_WIDGET(m_trackVolumeScale), TRUE);
    gtk_widget_set_size_request(GTK_WIDGET(m_trackVolumeScale), 120, -1);
    // Not focusable (audit A7): same reasoning as m_seekScale's own comment
    // further down this function -- GtkRange's own Left/Right/Home/End key
    // bindings would otherwise compete with the window-level transport
    // shortcuts once this widget (shown in the track right-click menu) has
    // focus.
    gtk_widget_set_focusable(GTK_WIDGET(m_trackVolumeScale), FALSE);
    g_signal_connect(m_trackVolumeScale, "value-changed", G_CALLBACK(&AppWindow::trackVolumeChangedTrampoline), this);
    gtk_box_append(GTK_BOX(volumeRow), GTK_WIDGET(m_trackVolumeScale));
    gtk_box_append(GTK_BOX(contextMenuBox), volumeRow);

    m_toggleLockButton = gtk_button_new_with_label("Lock Track");
    gtk_widget_add_css_class(m_toggleLockButton, "flat");
    g_signal_connect(m_toggleLockButton, "clicked", G_CALLBACK(&AppWindow::toggleLockClickedTrampoline), this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_toggleLockButton);

    m_removeTrackButton = gtk_button_new_with_label("Remove Track");
    gtk_widget_add_css_class(m_removeTrackButton, "flat");
    g_signal_connect(m_removeTrackButton, "clicked", G_CALLBACK(&AppWindow::removeTrackClickedTrampoline), this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_removeTrackButton);

    m_editTrackNameButton = gtk_button_new_with_label("Edit Track Name");
    gtk_widget_add_css_class(m_editTrackNameButton, "flat");
    g_signal_connect(m_editTrackNameButton, "clicked", G_CALLBACK(&AppWindow::editTrackNameClickedTrampoline), this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_editTrackNameButton);

    // Edit/Remove Clip Name: shown only when the right-click landed on a
    // clip (see onTimelineRightClicked); label text on
    // m_editClipNameButton ("Edit Clip Name" vs "Add Clip Name") and the
    // visibility of m_removeClipNameButton both depend on whether that
    // clip currently has a name.
    m_editClipNameButton = gtk_button_new_with_label("Edit Clip Name");
    gtk_widget_add_css_class(m_editClipNameButton, "flat");
    g_signal_connect(m_editClipNameButton, "clicked", G_CALLBACK(&AppWindow::editClipNameClickedTrampoline), this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_editClipNameButton);

    m_removeClipNameButton = gtk_button_new_with_label("Remove Clip Name");
    gtk_widget_add_css_class(m_removeClipNameButton, "flat");
    g_signal_connect(m_removeClipNameButton, "clicked", G_CALLBACK(&AppWindow::removeClipNameClickedTrampoline), this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_removeClipNameButton);

    // Shown when the right-click landed inside a dissolve transition's
    // overlap region (see onTimelineRightClicked) -- alongside whatever
    // clip buttons above also matched, since the overlap sits inside a
    // clip's own rectangle too.
    m_removeTransitionButton = gtk_button_new_with_label("Remove Transition");
    gtk_widget_add_css_class(m_removeTransitionButton, "flat");
    g_signal_connect(m_removeTransitionButton, "clicked", G_CALLBACK(&AppWindow::removeTransitionClickedTrampoline),
                      this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_removeTransitionButton);

    // Shown when the right-click landed near the boundary between two
    // touching, not-yet-linked clips (see onTimelineRightClicked) -- an
    // alternative to dragging a clip's edge past its neighbour.
    m_addTransitionButton = gtk_button_new_with_label("Add Transition");
    gtk_widget_add_css_class(m_addTransitionButton, "flat");
    g_signal_connect(m_addTransitionButton, "clicked", G_CALLBACK(&AppWindow::addTransitionClickedTrampoline), this);
    gtk_box_append(GTK_BOX(contextMenuBox), m_addTransitionButton);

    gtk_popover_set_child(m_trackContextMenu, contextMenuBox);

    // Shared inline name-edit popover, reused for both a track's label
    // strip and a clip (see InlineEditKind / m_inlineEditKind): one entry,
    // repositioned and refilled per use by showInlineNameEditor. Enter
    // (the entry's "activate") and clicking away (the popover's "closed",
    // which "activate" triggers by popping the popover down) both funnel
    // into onInlineNameEditClosed to commit; Escape sets m_inlineEditCancelled
    // first so that same "closed" handler discards instead.
    m_inlineNameEditPopover = GTK_POPOVER(gtk_popover_new());
    gtk_widget_set_parent(GTK_WIDGET(m_inlineNameEditPopover), GTK_WIDGET(m_timeline));
    m_inlineNameEditEntry = GTK_ENTRY(gtk_entry_new());
    gtk_widget_set_size_request(GTK_WIDGET(m_inlineNameEditEntry), 160, -1);
    g_signal_connect(m_inlineNameEditEntry, "activate", G_CALLBACK(&AppWindow::inlineNameEditActivateTrampoline),
                      this);
    GtkEventController *inlineEditKey = gtk_event_controller_key_new();
    g_signal_connect(inlineEditKey, "key-pressed", G_CALLBACK(&AppWindow::inlineNameEditKeyTrampoline), this);
    gtk_widget_add_controller(GTK_WIDGET(m_inlineNameEditEntry), inlineEditKey);
    gtk_popover_set_child(m_inlineNameEditPopover, GTK_WIDGET(m_inlineNameEditEntry));
    g_signal_connect(m_inlineNameEditPopover, "closed", G_CALLBACK(&AppWindow::inlineNameEditClosedTrampoline), this);

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
    // Not focusable: GtkRange's own key bindings would otherwise compete
    // with (and pre-empt, depending on focus) the window-level Left/Right/
    // Home/End actions below for frame-step/home/end -- there's no other
    // reason for this widget to hold keyboard focus, since seeking is
    // mouse-drag-driven.
    gtk_widget_set_focusable(GTK_WIDGET(m_seekScale), FALSE);
    g_signal_connect(m_seekScale, "value-changed", G_CALLBACK(&AppWindow::seekChangedTrampoline), this);
    gtk_box_append(GTK_BOX(transport), GTK_WIDGET(m_seekScale));

    m_timecodeLabel = GTK_LABEL(gtk_label_new("00:00:00:00"));
    gtk_widget_add_css_class(GTK_WIDGET(m_timecodeLabel), "timecode-label");
    gtk_box_append(GTK_BOX(transport), GTK_WIDGET(m_timecodeLabel));

    GtkWidget *volumeIcon = gtk_image_new_from_icon_name("audio-volume-high-symbolic");
    gtk_box_append(GTK_BOX(transport), volumeIcon);
    m_volumeScale = GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.05));
    gtk_scale_set_draw_value(m_volumeScale, FALSE);
    gtk_range_set_value(GTK_RANGE(m_volumeScale), 1.0);
    gtk_widget_set_size_request(GTK_WIDGET(m_volumeScale), 90, -1);
    gtk_widget_set_tooltip_text(GTK_WIDGET(m_volumeScale), "Volume");
    gtk_widget_set_focusable(GTK_WIDGET(m_volumeScale), FALSE); // audit A7 -- see m_seekScale's comment above
    g_signal_connect(m_volumeScale, "value-changed", G_CALLBACK(&AppWindow::volumeChangedTrampoline), this);
    gtk_box_append(GTK_BOX(transport), GTK_WIDGET(m_volumeScale));

    const char *previewScaleLabels[] = {"Auto", "Full", "Half", "Quarter", nullptr};
    GtkStringList *previewScaleModel = gtk_string_list_new(previewScaleLabels);
    m_previewScaleDropdown = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(previewScaleModel), nullptr));
    gtk_widget_set_tooltip_text(GTK_WIDGET(m_previewScaleDropdown), "Preview scale");
    // Audit A7: GtkDropDown handles Left/Right/Home/End itself while
    // focused (cycling/jumping between its own entries), the same
    // shortcut-stealing problem as the sliders above.
    gtk_widget_set_focusable(GTK_WIDGET(m_previewScaleDropdown), FALSE);
    g_signal_connect(m_previewScaleDropdown, "notify::selected", G_CALLBACK(&AppWindow::previewScaleChangedTrampoline),
                     this);
    gtk_box_append(GTK_BOX(transport), GTK_WIDGET(m_previewScaleDropdown));

    m_loopStatusLabel = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(m_loopStatusLabel), "dim-label");
    gtk_box_append(GTK_BOX(transport), GTK_WIDGET(m_loopStatusLabel));

    GtkWidget *clearLoopButton = gtk_button_new_from_icon_name("edit-clear-symbolic");
    gtk_widget_set_tooltip_text(clearLoopButton, "Clear loop (I/O set loop in/out at the playhead)");
    g_signal_connect(clearLoopButton, "clicked", G_CALLBACK(&AppWindow::clearLoopClickedTrampoline), this);
    gtk_box_append(GTK_BOX(transport), clearLoopButton);

    gtk_box_append(GTK_BOX(bottomBox), transport);

    m_statusLabel = GTK_LABEL(gtk_label_new(""));
    gtk_widget_set_halign(GTK_WIDGET(m_statusLabel), GTK_ALIGN_START);
    gtk_widget_add_css_class(GTK_WIDGET(m_statusLabel), "dim-label");
    gtk_box_append(GTK_BOX(bottomBox), GTK_WIDGET(m_statusLabel));

    gtk_paned_set_end_child(GTK_PANED(paned), bottomBox);

    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbarView), paned);
    adw_application_window_set_content(m_window, toolbarView);
}

void AppWindow::installActions(GtkApplication *app)
{
    GSimpleAction *undoAction = g_simple_action_new("undo", nullptr);
    g_signal_connect(undoAction, "activate", G_CALLBACK(&AppWindow::undoActionActivated), this);
    g_action_map_add_action(G_ACTION_MAP(m_window), G_ACTION(undoAction));
    g_object_unref(undoAction);

    GSimpleAction *redoAction = g_simple_action_new("redo", nullptr);
    g_signal_connect(redoAction, "activate", G_CALLBACK(&AppWindow::redoActionActivated), this);
    g_action_map_add_action(G_ACTION_MAP(m_window), G_ACTION(redoAction));
    g_object_unref(redoAction);

    const char *undoAccels[] = {"<Control>z", nullptr};
    gtk_application_set_accels_for_action(app, "win.undo", undoAccels);
    const char *redoAccels[] = {"<Control><Shift>z", nullptr};
    gtk_application_set_accels_for_action(app, "win.redo", redoAccels);

    // J/K/L shuttle, frame step, home/end, loop in/out (doc 05's M2
    // transport deliverables). No accelerator for play/pause toggle here:
    // the existing play button is mouse-only, and L already covers
    // "start playing forward" from the keyboard.
    addAction(app, "shuttle-forward", &AppWindow::shuttleForwardActivated, {"l"});
    addAction(app, "shuttle-reverse", &AppWindow::shuttleReverseActivated, {"j"});
    addAction(app, "shuttle-stop", &AppWindow::shuttleStopActivated, {"k"});
    addAction(app, "step-forward", &AppWindow::stepForwardActivated, {"Right"});
    addAction(app, "step-backward", &AppWindow::stepBackwardActivated, {"Left"});
    addAction(app, "seek-home", &AppWindow::seekHomeActivated, {"Home"});
    addAction(app, "seek-end", &AppWindow::seekEndActivated, {"End"});
    addAction(app, "loop-set-in", &AppWindow::loopSetInActivated, {"i"});
    addAction(app, "loop-set-out", &AppWindow::loopSetOutActivated, {"o"});
    // A/F: previous/next cut on the active track. S/D: active track
    // up/down -- the row edits/imports land on, same as clicking a row.
    addAction(app, "seek-previous-cut", &AppWindow::seekPreviousCutActivated, {"a"});
    addAction(app, "seek-next-cut", &AppWindow::seekNextCutActivated, {"f"});
    addAction(app, "active-track-up", &AppWindow::activeTrackUpActivated, {"s"});
    addAction(app, "active-track-down", &AppWindow::activeTrackDownActivated, {"d"});
    // Ctrl+Left/Right: jump 10 frames. Alt+Left/Right: jump 1 minute,
    // clamped to the timeline's start/end -- both reuse stepFrame(), whose
    // seek() already clamps to [0, totalFrames()-1], so "not a full minute
    // left in that direction" falls out for free rather than needing its
    // own clamping logic here.
    addAction(app, "step-forward-10", &AppWindow::stepForward10Activated, {"<Control>Right"});
    addAction(app, "step-backward-10", &AppWindow::stepBackward10Activated, {"<Control>Left"});
    addAction(app, "step-forward-minute", &AppWindow::stepForwardMinuteActivated, {"<Alt>Right"});
    addAction(app, "step-backward-minute", &AppWindow::stepBackwardMinuteActivated, {"<Alt>Left"});
}

void AppWindow::addAction(GtkApplication *app, const char *name,
                          void (*activated)(GSimpleAction *, GVariant *, gpointer),
                          std::initializer_list<const char *> accels)
{
    GSimpleAction *action = g_simple_action_new(name, nullptr);
    g_signal_connect(action, "activate", G_CALLBACK(activated), this);
    g_action_map_add_action(G_ACTION_MAP(m_window), G_ACTION(action));
    g_object_unref(action);

    std::vector<const char *> accelsWithNull(accels.begin(), accels.end());
    accelsWithNull.push_back(nullptr);
    gtk_application_set_accels_for_action(app, ("win." + std::string(name)).c_str(), accelsWithNull.data());
}

void AppWindow::setTransportActionsEnabled(bool enabled)
{
    static const char *kTransportActions[] = {
        "shuttle-forward", "shuttle-reverse",     "shuttle-stop",         "step-forward",
        "step-backward",   "seek-home",           "seek-end",             "loop-set-in",
        "loop-set-out",    "seek-previous-cut",   "seek-next-cut",        "active-track-up",
        "active-track-down", "step-forward-10",   "step-backward-10",    "step-forward-minute",
        "step-backward-minute",
    };
    for (const char *name : kTransportActions) {
        GAction *action = g_action_map_lookup_action(G_ACTION_MAP(m_window), name);
        g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
    }
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
            Log::debug(std::string("[app] Import file dialog closed without a selection: ") + error->message);
            g_error_free(error);
        }
        return;
    }

    char *path = g_file_get_path(file);
    if (path) {
        if (m_model.sequence().tracks.empty()) {
            showStatus("Add a track first.");
        } else {
            engine::EngineSync::ProbedMedia probed = m_engineSync->probeMedia(path);
            if (probed.length <= 0) {
                showStatus(std::string("Could not open media file: ") + path);
            } else {
                core::TrackId trackId = trackIdForRow(m_activeTrack);
                const core::Track &track = m_model.track(trackId);
                core::FrameIndex insertPos = track.clips.empty() ? 0 : m_model.clip(track.clips.back()).end();

                // A still image is boundless (MediaInfo::isBoundless()) --
                // MLT's own default (15000 frames via pixbuf, verified
                // empirically) has nothing to do with how long a clip cut
                // from it should be. Default to spanning the rest of the
                // *current* project length from the insert point, so
                // dropping a logo/watermark PNG onto an otherwise-empty top
                // track immediately covers the whole timeline, matching
                // what a still image is for -- no manual trim-to-fit
                // needed. Falls back to a modest 10s default when there's
                // nothing yet to cover (an empty project, or inserting
                // past the current end).
                core::FrameIndex length = effectiveInsertLength(probed.isStillImage, probed.length, insertPos);

                // AddAsset applies first (below) and, with no reuseId,
                // allocates exactly model.project().nextId as read here --
                // nothing else can allocate an id between this read and
                // that apply(), so InsertClip can be built against it
                // up front even though AddAsset hasn't run yet.
                core::AssetId predictedAssetId{m_model.project().nextId};

                std::vector<std::unique_ptr<core::Command>> steps;
                steps.push_back(std::make_unique<core::AddAsset>(
                    makeImportedAsset(path, length, probed, m_model.sequence().profile.fps)));
                steps.push_back(
                    std::make_unique<core::InsertClip>(trackId, predictedAssetId, insertPos, 0, length - 1));

                auto composite = std::make_unique<core::CompositeCommand>("Import clip", std::move(steps));

                if (m_undoStack.execute(std::move(composite))) {
                    refreshTimeline();
                    refreshMediaBrowser();
                    showStatus(std::string("Imported to track ") + std::to_string(m_activeTrack) + ": " + path);
                } else {
                    showStatus(std::string("Could not import: ") + path);
                }
            }
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
    // Audit A2: consumed unconditionally, regardless of how this save
    // turns out -- a cancelled or refused save must NOT close the window
    // (the user is left in the editor to sort it out), and clearing it up
    // front means a later, unrelated save can never inherit a stale
    // "close when done" from this one.
    bool closeAfterSave = m_closeAfterSave;
    m_closeAfterSave = false;

    GError *error = nullptr;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(sourceObject), result, &error);
    if (!file) {
        if (error)
            g_error_free(error);
        return;
    }

    char *path = g_file_get_path(file);
    if (path) {
        if (pathIsProjectAsset(path)) {
            showStatus(std::string("Refusing to save over a file already in this project's media: ") + path);
            g_free(path);
            g_object_unref(file);
            return;
        }
        // Audit T1 stop-gap: saveProject() doesn't run check() itself, and
        // loadProject() now refuses any file that fails it -- writing an
        // invalid model out would produce a file that looks saved but
        // can never be reopened. Checked here rather than inside
        // saveProject() (used by autosave and render too, where refusing
        // outright would be worse than writing best-effort) so this is
        // the one place a human actually sees and can act on the message.
        std::vector<std::string> problems = m_model.check();
        if (!problems.empty()) {
            Log::error("[app] refusing to save an invalid model: " + problems.front());
            showStatus("Can't save: the project has an internal inconsistency (" + problems.front() +
                      "). This is a bug -- please report it.");
            g_free(path);
            g_object_unref(file);
            return;
        }
        std::string err = core::saveProject(m_model, path);
        if (!err.empty()) {
            showStatus(err);
        } else {
            m_currentProjectPath = path;
            m_undoStack.setCleanPoint(); // emits changed -- updateWindowTitle() follows automatically
            // A successful manual Save is the one point A2 designates safe
            // to remove a recovered autosave: the recovered content now has
            // a durable copy of its own at `path`.
            if (!m_pendingAutosaveCleanupPath.empty()) {
                std::remove(m_pendingAutosaveCleanupPath.c_str());
                std::remove(m_pendingAutosaveCleanupMetaPath.c_str());
                m_pendingAutosaveCleanupPath.clear();
                m_pendingAutosaveCleanupMetaPath.clear();
            }
            showStatus(std::string("Saved: ") + path);
            if (closeAfterSave)
                gtk_window_destroy(GTK_WINDOW(m_window));
        }
        g_free(path);
    }
    g_object_unref(file);
}

void AppWindow::onOpenProjectClicked()
{
    confirmDiscardIfDirty([this] {
        GtkFileDialog *dialog = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dialog, "Open Project");
        gtk_file_dialog_open(dialog, GTK_WINDOW(m_window), nullptr, &AppWindow::openProjectFinishedTrampoline, this);
        g_object_unref(dialog);
    });
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
        auto loaded = core::loadProject(path);
        if (!loaded.has_value()) {
            showStatus(loaded.error());
        } else {
            m_model = std::move(*loaded); // m_undoStack/m_engineSync hold a reference to m_model, not a copy --
                                          // reassigning its contents leaves both still pointing at the right object
            m_currentProjectPath = path;
            m_undoStack.clear();
            m_undoStack.setCleanPoint(); // emits changed -- updateWindowTitle() follows automatically
            m_engineSync->reset(); // rebuilt.connect() (ctor) re-anchors playback automatically
            m_activeTrack = 0;
            m_selectedClip = -1;
            // Audit A1: a pending recovered-autosave cleanup is only
            // safe to act on once ITS OWN content has been durably
            // saved (onSaveFinished()'s own comment) -- switching away
            // to a different project via Open, same as New or Reload,
            // must forget it rather than let a later Save of THIS
            // project delete the still-only copy of whatever was
            // recovered. The files themselves are left alone; a later
            // launch (or offerRecoveryIfAny() looping later in this one)
            // can still find and offer them.
            m_pendingAutosaveCleanupPath.clear();
            m_pendingAutosaveCleanupMetaPath.clear();
            refreshTimeline();
            refreshMediaBrowser();
            showStatus(std::string("Opened: ") + path);
        }
        g_free(path);
    }
    g_object_unref(file);
}

void AppWindow::onReloadProjectClicked()
{
    if (m_currentProjectPath.empty()) {
        showStatus("Nothing to reload -- this project hasn't been saved or opened yet.");
        return;
    }
    confirmDiscardIfDirty([this] { performReload(); });
}

void AppWindow::performReload()
{
    auto loaded = core::loadProject(m_currentProjectPath);
    if (!loaded.has_value()) {
        showStatus(loaded.error());
        return;
    }

    m_model = std::move(*loaded); // m_undoStack/m_engineSync hold a reference to m_model, not a copy
    m_undoStack.clear();
    m_undoStack.setCleanPoint(); // emits changed -- updateWindowTitle() follows automatically
    m_engineSync->reset(); // rebuilt.connect() (ctor) re-anchors playback automatically
    m_activeTrack = 0;
    m_selectedClip = -1;
    // Audit A1 -- see onOpenProjectFinished's own comment.
    m_pendingAutosaveCleanupPath.clear();
    m_pendingAutosaveCleanupMetaPath.clear();
    refreshTimeline();
    refreshMediaBrowser();
    showStatus(std::string("Reloaded: ") + m_currentProjectPath);
}

void AppWindow::onNewProjectClicked()
{
    confirmDiscardIfDirty([this] { performNewProject(); });
}

void AppWindow::performNewProject()
{
    m_model = core::Model::createEmpty();
    // Pristine starting state, matching the constructor's own initial
    // track -- not through the UndoStack, since there's nothing to undo
    // back out of on a project that's just been reset.
    m_model.addTrack(core::Track::Kind::Video, 0, "V1");
    m_currentProjectPath.clear();
    m_undoStack.clear();
    m_undoStack.setCleanPoint(); // emits changed -- updateWindowTitle() follows automatically
    m_engineSync->reset(); // rebuilt.connect() (ctor) re-anchors playback automatically
    m_activeTrack = 0;
    m_selectedClip = -1;
    // Audit A1 -- see onOpenProjectFinished's own comment.
    m_pendingAutosaveCleanupPath.clear();
    m_pendingAutosaveCleanupMetaPath.clear();
    refreshTimeline();
    refreshMediaBrowser();
    showStatus("New project.");
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

    if (pathIsProjectAsset(path)) {
        showStatus(std::string("Refusing to render over a file already in this project's media: ") + path);
        return;
    }

    showStatus("Rendering to " + path + " … (this can take a while — the window will stay responsive)");

    // AppWindow itself is never destroyed during normal operation (see
    // main.cpp) — capturing `this` in this detached thread is safe on that
    // basis. `snapshot` is a deep copy of m_model taken HERE, synchronously
    // on the main thread, before the thread starts: core::Model has no
    // internal synchronization, so handing the render thread a reference to
    // the live m_model (which UndoStack::execute()/undo()/redo() mutate in
    // place on the main thread, with no lock) would be an unsynchronized
    // concurrent read/write the moment an edit happens mid-render. Copying
    // once up front instead means the render thread only ever touches its
    // own independent Model after this point -- a real render *queue* that
    // could serialize renders against edits is M6 territory, out of scope
    // here, but this closes the actual data race.
    std::thread([this, path, snapshot = m_model]() mutable {
        std::string err;
        bool ok = engine::renderProject(snapshot, path, err);

        struct Result
        {
            AppWindow *self;
            bool ok;
            std::string err;
            std::string path;
        };
        auto *renderResult = new Result{this, ok, std::move(err), path};
        g_idle_add(
            [](gpointer data) -> gboolean {
                std::unique_ptr<Result> r(static_cast<Result *>(data));
                if (r->ok)
                    r->self->showStatus("Rendered: " + r->path);
                else
                    r->self->showStatus("Render failed: " + r->err);
                return G_SOURCE_REMOVE;
            },
            renderResult);
    }).detach();
}

void AppWindow::onAddTrackClicked()
{
    // Inserted at row 0 (the top of the visual stack), matching v1's "a
    // new track always appears on top" -- see mltTrackOrder (doc 03):
    // video tracks are bottom-to-top by REVERSED model order, so a track
    // at model row 0 gets the highest MLT index, i.e. compositing wins.
    size_t trackNumber = m_model.sequence().tracks.size() + 1;
    auto cmd = std::make_unique<core::AddTrack>(core::Track::Kind::Video, 0, "V" + std::to_string(trackNumber));
    if (m_undoStack.execute(std::move(cmd))) {
        m_activeTrack = 0;
        refreshTimeline();
        showStatus("Added a track (now active).");
    }
}

void AppWindow::onUndo()
{
    if (m_undoStack.undo()) {
        m_selectedClip = -1;
        refreshTimeline();
        showStatus("Undid: " + m_undoStack.redoLabel());
    }
}

void AppWindow::onRedo()
{
    if (m_undoStack.redo()) {
        m_selectedClip = -1;
        refreshTimeline();
        showStatus("Redid: " + m_undoStack.undoLabel());
    }
}

void AppWindow::onPlayToggled()
{
    m_playback->togglePlay();
    refreshPlayButtonIcon();
}

void AppWindow::onSeekChanged()
{
    if (m_suppressSeekSignal)
        return;
    int frame = static_cast<int>(gtk_range_get_value(GTK_RANGE(m_seekScale)));
    m_playback->seek(frame);
}

void AppWindow::onShuttleForward()
{
    double current = m_playback->speed();
    double next = (current <= 0.0) ? 1.0 : std::min(current * 2.0, 8.0);
    m_playback->play(next);
    refreshPlayButtonIcon();
}

void AppWindow::onShuttleReverse()
{
    double current = m_playback->speed();
    double next = (current >= 0.0) ? -1.0 : std::max(current * 2.0, -8.0);
    m_playback->play(next);
    refreshPlayButtonIcon();
}

void AppWindow::onShuttleStop()
{
    m_playback->pause();
    refreshPlayButtonIcon();
}

void AppWindow::onStepForward()
{
    m_playback->stepFrame(1);
    refreshPlayButtonIcon();
}

void AppWindow::onStepBackward()
{
    m_playback->stepFrame(-1);
    refreshPlayButtonIcon();
}

void AppWindow::onStepForward10()
{
    m_playback->stepFrame(10);
    refreshPlayButtonIcon();
}

void AppWindow::onStepBackward10()
{
    m_playback->stepFrame(-10);
    refreshPlayButtonIcon();
}

// fps() is a profile property (always > 0 for a loaded project -- doc 09's
// loadProject() refuses a zero/negative frame rate outright), but this
// still falls back the same way the timecode label does (line ~2418) for
// the brief window before any project/tractor exists.
int AppWindow::oneMinuteInFrames() const
{
    double fps = m_playback->fps();
    return fps > 0.0 ? static_cast<int>(fps * 60.0 + 0.5) : 25 * 60;
}

void AppWindow::onStepForwardMinute()
{
    m_playback->stepFrame(oneMinuteInFrames());
    refreshPlayButtonIcon();
}

void AppWindow::onStepBackwardMinute()
{
    m_playback->stepFrame(-oneMinuteInFrames());
    refreshPlayButtonIcon();
}

void AppWindow::onSeekHome()
{
    m_playback->toHome();
}

void AppWindow::onSeekEnd()
{
    m_playback->toEnd();
}

// Every clip boundary (start and end) on the active track, plus the
// timeline's own start (0) and end, sorted and deduplicated -- shared by
// onSeekPreviousCut/onSeekNextCut so their "which frames count as a cut"
// definition can't drift apart.
std::vector<int> AppWindow::cutBoundariesOnActiveTrack() const
{
    const core::Track &track = m_model.track(trackIdForRow(m_activeTrack));
    // The LAST actually reachable frame, not the exclusive totalFrames()
    // itself: PlaybackController::seek() clamps to [0, totalFrames()-1],
    // so using the raw total here would make "next cut" silently re-seek
    // to the same already-clamped frame forever once at the end, instead
    // of ever reporting "no later cut".
    int lastFrame = std::max(m_playback->totalFrames() - 1, 0);
    std::vector<int> boundaries{0, lastFrame};
    for (core::ClipId clipId : track.clips) {
        const core::Clip &clip = m_model.clip(clipId);
        boundaries.push_back(static_cast<int>(clip.position));
        boundaries.push_back(static_cast<int>(clip.end()));
    }
    // A clip ending exactly at the sequence's own length (the common
    // case for whichever clip plays last) pushes the raw, EXCLUSIVE
    // clip.end() -- equal to totalFrames(), one past lastFrame -- so
    // clamp every entry before deduping, or that unreachable value
    // would slip back in as a distinct boundary past lastFrame,
    // reintroducing the exact bug lastFrame above exists to avoid.
    for (int &boundary : boundaries)
        boundary = std::clamp(boundary, 0, lastFrame);
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    return boundaries;
}

void AppWindow::onSeekPreviousCut()
{
    if (m_model.sequence().tracks.empty())
        return;
    std::vector<int> boundaries = cutBoundariesOnActiveTrack();

    // The largest boundary strictly before the current frame: lower_bound
    // finds the first boundary >= current (which, if current sits exactly
    // on one, is that same boundary, not the one before it), so the
    // previous distinct cut is always one step back from there.
    auto it = std::lower_bound(boundaries.begin(), boundaries.end(), m_playback->currentFrame());
    if (it == boundaries.begin()) {
        showStatus("No earlier cut on this track.");
        return;
    }
    m_playback->seek(*(it - 1));
}

void AppWindow::onSeekNextCut()
{
    if (m_model.sequence().tracks.empty())
        return;
    std::vector<int> boundaries = cutBoundariesOnActiveTrack();

    auto it = std::upper_bound(boundaries.begin(), boundaries.end(), m_playback->currentFrame());
    if (it == boundaries.end()) {
        showStatus("No later cut on this track.");
        return;
    }
    m_playback->seek(*it);
}

void AppWindow::onActiveTrackUp()
{
    int trackCount = static_cast<int>(m_model.sequence().tracks.size());
    if (trackCount <= 0)
        return;
    m_activeTrack = std::clamp(m_activeTrack - 1, 0, trackCount - 1);
    gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
}

void AppWindow::onActiveTrackDown()
{
    int trackCount = static_cast<int>(m_model.sequence().tracks.size());
    if (trackCount <= 0)
        return;
    m_activeTrack = std::clamp(m_activeTrack + 1, 0, trackCount - 1);
    gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
}

void AppWindow::onSetLoopIn()
{
    int frame = m_playback->currentFrame();
    auto range = m_playback->loopRange();
    int out = range ? range->second : std::max(m_playback->totalFrames() - 1, 0);
    if (frame >= out) {
        showStatus("Loop in must be before loop out.");
        return;
    }
    m_playback->setLoopRange(std::make_pair(frame, out));
    refreshLoopStatusLabel();
}

void AppWindow::onSetLoopOut()
{
    int frame = m_playback->currentFrame();
    auto range = m_playback->loopRange();
    int in = range ? range->first : 0;
    if (frame <= in) {
        showStatus("Loop out must be after loop in.");
        return;
    }
    m_playback->setLoopRange(std::make_pair(in, frame));
    refreshLoopStatusLabel();
}

void AppWindow::onClearLoopClicked()
{
    m_playback->setLoopRange(std::nullopt);
    refreshLoopStatusLabel();
}

void AppWindow::onVolumeChanged()
{
    m_playback->setVolume(gtk_range_get_value(GTK_RANGE(m_volumeScale)));
}

void AppWindow::onPreviewScaleChanged()
{
    switch (gtk_drop_down_get_selected(m_previewScaleDropdown)) {
    case 1:
        m_playback->setPreviewScale(engine::PlaybackController::PreviewScale::Full);
        break;
    case 2:
        m_playback->setPreviewScale(engine::PlaybackController::PreviewScale::Half);
        break;
    case 3:
        m_playback->setPreviewScale(engine::PlaybackController::PreviewScale::Quarter);
        break;
    default:
        m_playback->setPreviewScale(engine::PlaybackController::PreviewScale::Auto);
        break;
    }
}

void AppWindow::onSplitClicked()
{
    int frame = m_playback->currentFrame();
    for (const auto &clip : m_clips) {
        if (clip.trackIndex == m_activeTrack && frame > clip.startFrame && frame < clip.startFrame + clip.frames) {
            if (m_undoStack.execute(std::make_unique<core::SplitClip>(clip.id, frame))) {
                refreshTimeline();
            } else {
                showStatus("Couldn't split there.");
            }
            return;
        }
    }
    showStatus("Nothing to split on track " + std::to_string(m_activeTrack) + " at the current playhead.");
}

void AppWindow::onTimelineClicked(int nPress, double x, double y)
{
    int trackCount = static_cast<int>(m_model.sequence().tracks.size());
    if (trackCount <= 0)
        return;

    int row = static_cast<int>(y / kTrackRowHeight);
    m_activeTrack = std::clamp(row, 0, trackCount - 1);

    int total = m_playback->totalFrames();
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

        // Double-click opens the inline name editor instead of the usual
        // seek/select: on the label strip above a track, edit the
        // track's name; otherwise, on a clip, edit that clip's name. The
        // label-strip check must run FIRST: the clip hit-test above only
        // checks the frame/x range, not y, so it still matches a clip
        // sitting directly under the label strip in the same row.
        if (nPress == 2) {
            if (y - row * kTrackRowHeight <= kTrackLabelHeight) {
                beginTrackNameEdit(m_activeTrack);
                return;
            }
            if (m_selectedClip >= 0) {
                beginClipNameEdit(m_clips[static_cast<size_t>(m_selectedClip)]);
                return;
            }
        }

        m_playback->seek(frame);
    }

    gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
}

void AppWindow::onTimelineRightClicked(double x, double y)
{
    int trackCount = static_cast<int>(m_model.sequence().tracks.size());
    if (trackCount <= 0)
        return;

    int row = std::clamp(static_cast<int>(y / kTrackRowHeight), 0, trackCount - 1);
    m_contextMenuTrack = row;
    m_contextMenuClipStartFrame = -1;
    m_contextMenuGapStartFrame = -1;
    m_contextMenuTransitionId = core::TransitionId{};
    m_contextMenuAddTransitionA = core::ClipId{};
    m_contextMenuAddTransitionB = core::ClipId{};

    int total = m_playback->totalFrames();
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
        if (m_contextMenuClipStartFrame < 0) {
            // A gap: inside this track's own content range but not covered
            // by any clip (Model doesn't store gaps -- doc 03 -- so this is
            // just "no clip claims this frame, but something after it
            // does"). The gap's start is the END of whichever clip
            // immediately precedes it (or 0 if none) -- NOT the clicked
            // frame (audit A3): right-clicking anywhere inside a gap must
            // offer to close the WHOLE gap, not just the part after the
            // click. track.clips is kept sorted by position (Model::
            // sortTrackClips runs after every mutation), so the first clip
            // whose position is past `frame` is the one right after the
            // gap, and whatever ran immediately before it in this loop is
            // the one right before it.
            core::TrackId trackId = trackIdForRow(row);
            const core::Track &track = m_model.track(trackId);
            core::FrameIndex gapStart = 0;
            for (core::ClipId clipId : track.clips) {
                const core::Clip &candidate = m_model.clip(clipId);
                if (candidate.position > frame) {
                    m_contextMenuGapStartFrame = static_cast<int>(gapStart);
                    break;
                }
                gapStart = candidate.end();
            }
        }

        // A dissolve transition's overlap region: both the clip check
        // above and this one can match the same click (the overlap sits
        // inside BOTH clips' own rectangles) -- Remove Transition is
        // offered alongside whatever the clip check already found, not
        // instead of it.
        core::TrackId trackId = trackIdForRow(row);
        for (const core::Transition &t : m_model.sequence().transitions) {
            if (t.track != trackId)
                continue;
            // Audit T1 stop-gap: the core-level fix (RemoveClip/MoveClip/
            // ResizeClip/SplitClip/RemoveTrack now strip a clip's own
            // transitions before touching it) should make a dangling
            // transition impossible going forward, but this guard is
            // cheap insurance against a project saved before that fix
            // landed, or a future command that forgets to.
            if (!m_model.hasClip(t.a) || !m_model.hasClip(t.b))
                continue;
            const core::Clip &clipA = m_model.clip(t.a);
            const core::Clip &clipB = m_model.clip(t.b);
            if (frame >= clipB.position && frame < clipA.end()) {
                m_contextMenuTransitionId = t.id;
                break;
            }
        }

        // "Add Transition": near the exact pixel boundary between two
        // touching, not-yet-linked clips (same proximity width as the
        // drag-trim edge grab) -- offered independent of whatever the
        // clip/gap checks above found, since a boundary sits exactly on
        // the edge of both neighbouring clips' own rectangles.
        double contentWidth = widgetWidth - kHandleWidth;
        const core::Track &track = m_model.track(trackId);
        for (size_t i = 0; i + 1 < track.clips.size(); ++i) {
            const core::Clip &clipA = m_model.clip(track.clips[i]);
            const core::Clip &clipB = m_model.clip(track.clips[i + 1]);
            if (clipA.end() != clipB.position)
                continue; // a gap, not a touching boundary
            bool alreadyLinked =
                std::any_of(m_model.sequence().transitions.begin(), m_model.sequence().transitions.end(),
                           [&](const core::Transition &t2) { return t2.a == clipA.id && t2.b == clipB.id; });
            if (alreadyLinked)
                continue;
            double boundaryX = kHandleWidth + (static_cast<double>(clipA.end()) / total) * contentWidth;
            double distance = x > boundaryX ? x - boundaryX : boundaryX - x;
            if (distance <= kEdgeGrabWidth) {
                m_contextMenuAddTransitionA = clipA.id;
                m_contextMenuAddTransitionB = clipB.id;
                break;
            }
        }
    }

    gtk_widget_set_visible(m_removeTransitionButton, m_contextMenuTransitionId.isValid());
    gtk_widget_set_visible(m_addTransitionButton, m_contextMenuAddTransitionA.isValid());

    gtk_widget_set_visible(m_deleteClipButton, m_contextMenuClipStartFrame >= 0);

    // Doc 06: only offered when the clip actually has audio to pull out
    // and isn't already audio-only (splitting an audio-only clip would be
    // a no-op InsertClip of silence onto a second audio track).
    bool showSplitAudio = false;
    if (m_contextMenuClipStartFrame >= 0) {
        for (const auto &clip : m_clips) {
            if (clip.trackIndex == row && clip.startFrame == m_contextMenuClipStartFrame) {
                const core::Clip &modelClip = m_model.clip(clip.id);
                showSplitAudio = modelClip.videoEnabled && modelClip.audioEnabled &&
                                m_model.hasAsset(modelClip.asset) && m_model.asset(modelClip.asset).info.hasAudio;
                break;
            }
        }
    }
    gtk_widget_set_visible(m_splitAudioButton, showSplitAudio);

    gtk_widget_set_visible(m_closeGapButton, m_contextMenuGapStartFrame >= 0);

    // Edit Name / Remove Name: only offered on a clip. Label and the
    // Remove button's visibility both depend on whether it already has a
    // custom name (doc request: "if it has a name it should have
    // edit/remove").
    bool clipHasName = false;
    if (m_contextMenuClipStartFrame >= 0) {
        for (const auto &clip : m_clips) {
            if (clip.trackIndex == row && clip.startFrame == m_contextMenuClipStartFrame) {
                clipHasName = !clip.name.empty();
                break;
            }
        }
    }
    gtk_widget_set_visible(m_editClipNameButton, m_contextMenuClipStartFrame >= 0);
    gtk_button_set_label(GTK_BUTTON(m_editClipNameButton), clipHasName ? "Edit Clip Name" : "Add Clip Name");
    gtk_widget_set_visible(m_removeClipNameButton, clipHasName);

    bool onEmptyTrackSpace = m_contextMenuClipStartFrame < 0 && m_contextMenuGapStartFrame < 0;
    gtk_widget_set_visible(m_removeTrackButton, onEmptyTrackSpace);
    gtk_widget_set_visible(m_toggleLockButton, onEmptyTrackSpace);
    gtk_widget_set_visible(m_editTrackNameButton, onEmptyTrackSpace);
    gtk_widget_set_visible(gtk_widget_get_parent(GTK_WIDGET(m_trackVolumeScale)), onEmptyTrackSpace);
    if (onEmptyTrackSpace) {
        const core::Track &track = m_model.track(trackIdForRow(row));
        gtk_button_set_label(GTK_BUTTON(m_toggleLockButton), track.locked ? "Unlock Track" : "Lock Track");
        m_suppressTrackVolumeSignal = true;
        gtk_range_set_value(GTK_RANGE(m_trackVolumeScale), track.volume);
        m_suppressTrackVolumeSignal = false;
    }

    GdkRectangle rect{static_cast<int>(x), static_cast<int>(row * kTrackRowHeight), 1,
                      static_cast<int>(kTrackRowHeight)};
    gtk_popover_set_pointing_to(m_trackContextMenu, &rect);
    gtk_popover_popup(m_trackContextMenu);
}

void AppWindow::onDeleteClipClicked()
{
    gtk_popover_popdown(m_trackContextMenu);
    if (m_contextMenuClipStartFrame < 0)
        return;

    core::ClipId clipId;
    for (const auto &clip : m_clips) {
        if (clip.trackIndex == m_contextMenuTrack && clip.startFrame == m_contextMenuClipStartFrame) {
            clipId = clip.id;
            break;
        }
    }

    if (clipId.isValid() && m_undoStack.execute(std::make_unique<core::RemoveClip>(clipId))) {
        m_selectedClip = -1;
        refreshTimeline();
        showStatus("Deleted clip — gap left behind. Right-click the gap to close it.");
    } else {
        showStatus("Couldn't delete that clip.");
    }
}

void AppWindow::onSplitAudioClicked()
{
    gtk_popover_popdown(m_trackContextMenu);
    if (m_contextMenuClipStartFrame < 0)
        return;

    core::ClipId clipId;
    for (const auto &clip : m_clips) {
        if (clip.trackIndex == m_contextMenuTrack && clip.startFrame == m_contextMenuClipStartFrame) {
            clipId = clip.id;
            break;
        }
    }

    if (clipId.isValid() && m_undoStack.execute(std::make_unique<core::SplitAudio>(clipId))) {
        refreshTimeline();
        showStatus("Split audio to its own track.");
    } else {
        showStatus("Couldn't split that clip's audio.");
    }
}

void AppWindow::onCloseGapClicked()
{
    gtk_popover_popdown(m_trackContextMenu);
    if (m_contextMenuGapStartFrame < 0) {
        m_contextMenuTrack = -1;
        return;
    }

    core::TrackId trackId = trackIdForRow(m_contextMenuTrack);
    const core::Track &track = m_model.track(trackId);
    core::FrameIndex gapStart = m_contextMenuGapStartFrame;

    // The gap's length: distance from gapStart to the next clip's start
    // (there must be one, or onTimelineRightClicked wouldn't have offered
    // "Close Gap" for this position at all).
    core::FrameIndex gapEnd = gapStart;
    bool foundNext = false;
    for (core::ClipId clipId : track.clips) {
        core::FrameIndex position = m_model.clip(clipId).position;
        if (position > gapStart) {
            gapEnd = position;
            foundNext = true;
            break;
        }
    }

    if (!foundNext) {
        showStatus("Couldn't close that gap.");
        m_contextMenuTrack = -1;
        return;
    }

    core::FrameIndex gapLength = gapEnd - gapStart;
    std::vector<std::unique_ptr<core::Command>> moves;
    for (core::ClipId clipId : track.clips) {
        const core::Clip &clip = m_model.clip(clipId);
        if (clip.position >= gapEnd)
            moves.push_back(std::make_unique<core::MoveClip>(clipId, trackId, clip.position - gapLength));
    }

    bool ok =
        !moves.empty() && m_undoStack.execute(std::make_unique<core::CompositeCommand>("Close gap", std::move(moves)));
    if (ok) {
        m_selectedClip = -1;
        refreshTimeline();
        showStatus("Closed gap.");
    } else {
        showStatus("Couldn't close that gap.");
    }
    m_contextMenuTrack = -1;
}

void AppWindow::onEditClipNameClicked()
{
    gtk_popover_popdown(m_trackContextMenu);
    if (m_contextMenuClipStartFrame < 0)
        return;

    for (const auto &clip : m_clips) {
        if (clip.trackIndex == m_contextMenuTrack && clip.startFrame == m_contextMenuClipStartFrame) {
            beginClipNameEdit(clip);
            return;
        }
    }
}

void AppWindow::onRemoveClipNameClicked()
{
    gtk_popover_popdown(m_trackContextMenu);
    if (m_contextMenuClipStartFrame < 0)
        return;

    for (const auto &clip : m_clips) {
        if (clip.trackIndex == m_contextMenuTrack && clip.startFrame == m_contextMenuClipStartFrame) {
            if (m_undoStack.execute(std::make_unique<core::RenameClip>(clip.id, std::string{}))) {
                refreshTimeline();
                showStatus("Removed clip name.");
            } else {
                showStatus("Couldn't remove that clip's name.");
            }
            return;
        }
    }
}

void AppWindow::onRemoveTransitionClicked()
{
    gtk_popover_popdown(m_trackContextMenu);
    if (!m_contextMenuTransitionId.isValid())
        return;

    if (m_undoStack.execute(std::make_unique<core::RemoveTransition>(m_contextMenuTransitionId))) {
        refreshTimeline();
        showStatus("Removed dissolve.");
    } else {
        showStatus("Couldn't remove that transition.");
    }
}

void AppWindow::onAddTransitionClicked()
{
    gtk_popover_popdown(m_trackContextMenu);
    if (!m_contextMenuAddTransitionA.isValid() || !m_contextMenuAddTransitionB.isValid())
        return;

    core::TrackId trackId = trackIdForRow(m_contextMenuTrack);
    const core::Clip &clipA = m_model.clip(m_contextMenuAddTransitionA);
    const core::Clip &clipB = m_model.clip(m_contextMenuAddTransitionB);

    // Default length: about half a second, split between both clips' own
    // handles -- unlike a drag-created transition (which attributes the
    // whole length to whichever edge was actually dragged), there's no
    // single side to prefer here, so try half from each, and hand
    // whatever one side can't use to the other (clamped again there).
    core::FrameIndex targetLength =
        std::max<core::FrameIndex>(1, static_cast<core::FrameIndex>(m_playback->fps() * 0.5 + 0.5));
    core::FrameIndex handleA = 0;
    if (m_model.hasAsset(clipA.asset)) {
        const core::Asset &asset = m_model.asset(clipA.asset);
        handleA = asset.info.isBoundless()
                    ? targetLength
                    : std::max<core::FrameIndex>(0, asset.info.lengthInSequenceFrames - 1 - clipA.out);
    }
    core::FrameIndex handleB = clipB.in; // source starts at 0, so `in` itself is the available head room

    core::FrameIndex extendA = std::min(targetLength / 2, handleA);
    core::FrameIndex remaining = targetLength - extendA;
    core::FrameIndex extendB = std::min(remaining, handleB);
    core::FrameIndex shortfall = remaining - extendB;
    if (shortfall > 0)
        extendA = std::min(handleA, extendA + shortfall);

    if (extendA + extendB <= 0) {
        showStatus("Couldn't add a transition there — neither clip has spare source frames.");
        return;
    }

    if (m_undoStack.execute(std::make_unique<core::AddTransition>(trackId, clipA.id, clipB.id, extendA, extendB))) {
        refreshTimeline();
        showStatus("Created a " + std::to_string(extendA + extendB) + "-frame dissolve.");
    } else {
        showStatus("Couldn't add a transition there.");
    }
}

void AppWindow::onRemoveTrackClicked()
{
    gtk_popover_popdown(m_trackContextMenu);

    if (m_contextMenuTrack < 0)
        return;

    core::TrackId trackId = trackIdForRow(m_contextMenuTrack);
    if (m_undoStack.execute(std::make_unique<core::RemoveTrack>(trackId))) {
        int trackCount = static_cast<int>(m_model.sequence().tracks.size());
        m_activeTrack = std::clamp(m_activeTrack, 0, std::max(trackCount - 1, 0));
        m_selectedClip = -1;
        refreshTimeline();
        showStatus("Removed track " + std::to_string(m_contextMenuTrack) + ".");
    } else {
        showStatus("Couldn't remove that track.");
    }
    m_contextMenuTrack = -1;
}

void AppWindow::onToggleLockClicked()
{
    gtk_popover_popdown(m_trackContextMenu);
    if (m_contextMenuTrack < 0)
        return;

    core::TrackId trackId = trackIdForRow(m_contextMenuTrack);
    const core::Track &track = m_model.track(trackId);
    bool newLocked = !track.locked;
    if (m_undoStack.execute(std::make_unique<core::SetTrackFlags>(trackId, track.muted, track.hidden, newLocked))) {
        refreshTimeline();
        showStatus(newLocked ? "Track locked." : "Track unlocked.");
    } else {
        showStatus("Couldn't change that track's lock.");
    }
    m_contextMenuTrack = -1;
}

void AppWindow::onEditTrackNameClicked()
{
    gtk_popover_popdown(m_trackContextMenu);
    if (m_contextMenuTrack < 0)
        return;
    beginTrackNameEdit(m_contextMenuTrack);
}

void AppWindow::onTrackVolumeChanged()
{
    if (m_suppressTrackVolumeSignal || m_contextMenuTrack < 0)
        return;
    core::TrackId trackId = trackIdForRow(m_contextMenuTrack);
    double volume = gtk_range_get_value(GTK_RANGE(m_trackVolumeScale));
    // Not gated on hasTrack/success feedback: a slider drag fires many of
    // these, and SetTrackVolume::mergeWith coalesces them into one undo
    // step already -- a status message per tick would just be noise.
    m_undoStack.execute(std::make_unique<core::SetTrackVolume>(trackId, volume));
}

bool AppWindow::onTrackDragBegin(double x, double y)
{
    m_dragMode = TimelineDragMode::None;
    m_dragStartX = x;
    m_dragStartY = y;

    int trackCount = static_cast<int>(m_model.sequence().tracks.size());
    if (trackCount <= 0)
        return false;

    if (x < kHandleWidth) {
        m_dragMode = TimelineDragMode::TrackReorder;
        m_draggingTrack = std::clamp(static_cast<int>(y / kTrackRowHeight), 0, trackCount - 1);
        m_dragHoverRow = m_draggingTrack;
        gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
        return true;
    }

    int total = m_playback->totalFrames();
    int widgetWidth = gtk_widget_get_width(GTK_WIDGET(m_timeline));
    if (total <= 0 || widgetWidth <= kHandleWidth)
        return false;

    double contentWidth = widgetWidth - kHandleWidth;
    int row = static_cast<int>(y / kTrackRowHeight);
    int frameAtX = static_cast<int>(((x - kHandleWidth) / contentWidth) * total);

    // An existing transition's own edges sit exactly on the drawn edges
    // of the two clips it links (clipA's right edge == clipB's left edge
    // == the overlap boundary), so this must run BEFORE the plain clip
    // edge-detection below: dragging there should resize the transition,
    // not attempt (and always fail) an ordinary trim into occupied space.
    core::TrackId rowTrackId = trackIdForRow(row);
    for (const core::Transition &t : m_model.sequence().transitions) {
        if (t.track != rowTrackId)
            continue;
        // Audit T1 stop-gap -- see onTimelineRightClicked's own comment.
        if (!m_model.hasClip(t.a) || !m_model.hasClip(t.b))
            continue;
        const core::Clip &clipA = m_model.clip(t.a);
        const core::Clip &clipB = m_model.clip(t.b);
        double leftX = kHandleWidth + (static_cast<double>(clipB.position) / total) * contentWidth;
        double rightX = kHandleWidth + (static_cast<double>(clipA.end()) / total) * contentWidth;

        bool nearLeft = x - leftX < kEdgeGrabWidth && leftX - x < kEdgeGrabWidth;
        bool nearRight = x - rightX < kEdgeGrabWidth && rightX - x < kEdgeGrabWidth;
        if (!nearLeft && !nearRight)
            continue;

        m_dragTransitionId = t.id;
        m_dragTransitionRow = row;
        m_dragTransitionPreviewLeftFrame = static_cast<int>(clipB.position);
        m_dragTransitionPreviewRightFrame = static_cast<int>(clipA.end());
        m_activeTrack = row;
        m_dragMode = nearLeft ? TimelineDragMode::TransitionResizeLeft : TimelineDragMode::TransitionResizeRight;
        gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
        return true;
    }

    for (size_t i = 0; i < m_clips.size(); ++i) {
        const auto &clip = m_clips[i];
        if (clip.trackIndex != row)
            continue;
        if (frameAtX < clip.startFrame || frameAtX >= clip.startFrame + clip.frames)
            continue;

        double clipLeftX = kHandleWidth + (static_cast<double>(clip.startFrame) / total) * contentWidth;
        double clipRightX = kHandleWidth + (static_cast<double>(clip.startFrame + clip.frames) / total) * contentWidth;

        m_dragClipId = clip.id;
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

    int trackCount = static_cast<int>(m_model.sequence().tracks.size());

    if (m_dragMode == TimelineDragMode::TrackReorder) {
        double currentY = m_dragStartY + offsetY;
        m_dragHoverRow = std::clamp(static_cast<int>(currentY / kTrackRowHeight), 0, std::max(trackCount - 1, 0));
        gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
        return;
    }

    int total = m_playback->totalFrames();
    int widgetWidth = gtk_widget_get_width(GTK_WIDGET(m_timeline));
    double contentWidth = std::max(widgetWidth - kHandleWidth, 1.0);
    if (total <= 0)
        return;
    int deltaFrames = static_cast<int>(offsetX / contentWidth * total);

    if (m_dragMode == TimelineDragMode::MoveClip) {
        m_dragPreviewTrack =
            std::clamp(static_cast<int>((m_dragStartY + offsetY) / kTrackRowHeight), 0, std::max(trackCount - 1, 0));
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
    } else if (m_dragMode == TimelineDragMode::TransitionResizeLeft) {
        // The right edge stays put; clamp so the left edge can't cross it
        // (and can't go negative) -- final handle-availability validation
        // happens at drag-end (AddTransition), this is just the preview.
        if (m_model.hasTransition(m_dragTransitionId)) {
            const core::Transition &t = m_model.transition(m_dragTransitionId);
            int rightEdge = static_cast<int>(m_model.clip(t.a).end());
            m_dragTransitionPreviewLeftFrame =
                std::clamp(static_cast<int>(m_model.clip(t.b).position) + deltaFrames, 0, rightEdge - 1);
        }
    } else if (m_dragMode == TimelineDragMode::TransitionResizeRight) {
        if (m_model.hasTransition(m_dragTransitionId)) {
            const core::Transition &t = m_model.transition(m_dragTransitionId);
            int leftEdge = static_cast<int>(m_model.clip(t.b).position);
            m_dragTransitionPreviewRightFrame =
                std::max(leftEdge + 1, static_cast<int>(m_model.clip(t.a).end()) + deltaFrames);
        }
    }

    gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
}

void AppWindow::onTrackDragEnd(double offsetX, double offsetY)
{
    bool trivial = std::abs(offsetX) < kDragClickThreshold && std::abs(offsetY) < kDragClickThreshold;
    TimelineDragMode mode = m_dragMode;

    if (mode == TimelineDragMode::TrackReorder) {
        int trackCount = static_cast<int>(m_model.sequence().tracks.size());
        double currentY = m_dragStartY + offsetY;
        int targetRow = std::clamp(static_cast<int>(currentY / kTrackRowHeight), 0, std::max(trackCount - 1, 0));
        if (targetRow != m_draggingTrack) {
            core::TrackId trackId = trackIdForRow(m_draggingTrack);
            if (m_undoStack.execute(std::make_unique<core::MoveTrack>(trackId, static_cast<size_t>(targetRow)))) {
                m_activeTrack = targetRow;
                m_selectedClip = -1;
                showStatus("Moved track " + std::to_string(m_draggingTrack) + " to " + std::to_string(targetRow) + ".");
            }
        }
    } else if (mode == TimelineDragMode::MoveClip || mode == TimelineDragMode::TrimClipStart ||
               mode == TimelineDragMode::TrimClipEnd) {
        if (trivial) {
            // Not really a drag -- treat as the plain click it was.
            onTimelineClicked(1, m_dragStartX, m_dragStartY);
        } else if (mode == TimelineDragMode::MoveClip) {
            core::TrackId destTrack = trackIdForRow(m_dragPreviewTrack);
            const core::Clip &draggedClip = m_model.clip(m_dragClipId);
            // Audit C3: a drag whose pixel offset cleared the trivial
            // threshold (so it wasn't caught above) but still lands back
            // on the clip's own track and position isn't a move -- issuing
            // MoveClip anyway would strip any dissolve on this clip for
            // an edit that changes nothing. Model::MoveClip::apply()
            // refuses this too (belt and suspenders), but skipping it
            // here also avoids the wrong "that space is occupied" message
            // for what's actually a no-op.
            if (destTrack == draggedClip.track && m_dragPreviewStartFrame == draggedClip.position) {
                // Nothing to do -- already exactly where it started.
            } else if (m_undoStack.execute(
                          std::make_unique<core::MoveClip>(m_dragClipId, destTrack, m_dragPreviewStartFrame))) {
                m_activeTrack = m_dragPreviewTrack;
            } else {
                showStatus("Can't move the clip there — that space is occupied.");
            }
        } else if (mode == TimelineDragMode::TrimClipStart) {
            const core::Clip &clip = m_model.clip(m_dragClipId);
            core::FrameIndex delta = m_dragPreviewStartFrame - m_dragClipStartFrame;
            core::FrameIndex newIn = clip.in + delta;
            if (!m_undoStack.execute(
                    std::make_unique<core::ResizeClip>(m_dragClipId, newIn, clip.out, m_dragPreviewStartFrame))) {
                // Dragging left past the START of the clip immediately
                // before this one (on the same track, exactly touching --
                // AddTransition's own precondition) creates a dissolve
                // there instead of just refusing: the overlap the user
                // just dragged into becomes the transition's length,
                // pulled entirely from this clip's own head handle.
                bool handled = false;
                if (delta < 0) {
                    for (const auto &other : m_clips) {
                        if (other.trackIndex == m_dragClipTrack &&
                            other.startFrame + other.frames == m_dragClipStartFrame) {
                            handled = onDragCreatedTransition(other.id, m_dragClipId, 0, -delta);
                            break;
                        }
                    }
                }
                if (!handled)
                    showStatus("Can't trim the clip that far — space is occupied or the source has no more frames.");
            }
        } else if (mode == TimelineDragMode::TrimClipEnd) {
            const core::Clip &clip = m_model.clip(m_dragClipId);
            core::FrameIndex newOut = m_dragPreviewStartFrame + m_dragPreviewFrames - 1;
            // No ripple: a following clip immediately after this one
            // refuses the trim rather than shifting out of the way (doc
            // 04's RippleTrim, a composite command, isn't built yet --
            // M3/timeline territory) UNLESS it's exactly touching, in
            // which case the overlap becomes a dissolve instead (see
            // onDragCreatedTransition) -- move the following clip first
            // to just trim past a gap.
            if (!m_undoStack.execute(
                    std::make_unique<core::ResizeClip>(m_dragClipId, clip.in, newOut, clip.position))) {
                bool handled = false;
                if (newOut > clip.out) {
                    for (const auto &other : m_clips) {
                        if (other.trackIndex == m_dragClipTrack &&
                            other.startFrame == m_dragClipStartFrame + m_dragClipFrames) {
                            handled = onDragCreatedTransition(m_dragClipId, other.id, newOut - clip.out, 0);
                            break;
                        }
                    }
                }
                if (!handled) {
                    showStatus("Can't trim the clip that far — move the next clip out of the way first, or the "
                               "source has no more frames.");
                }
            }
        }
    } else if (mode == TimelineDragMode::TransitionResizeLeft || mode == TimelineDragMode::TransitionResizeRight) {
        if (trivial) {
            // Not really a drag -- treat as the plain click it was.
            onTimelineClicked(1, m_dragStartX, m_dragStartY);
        } else if (m_model.hasTransition(m_dragTransitionId)) {
            const core::Transition &current = m_model.transition(m_dragTransitionId);
            core::TrackId trackId = current.track;
            core::ClipId aId = current.a, bId = current.b;
            core::FrameIndex extendA = current.extendA;
            core::FrameIndex extendB = current.extendB;

            // Only the edge actually being dragged moves; the other stays
            // at its current position -- re-derive the fixed touch point
            // (where the two clips would meet with no transition at all)
            // from whichever side ISN'T moving, then compute the new
            // extend* for the side that is.
            if (mode == TimelineDragMode::TransitionResizeLeft) {
                core::FrameIndex touchPoint = m_model.clip(bId).position + extendB;
                extendB = std::max<core::FrameIndex>(0, touchPoint - m_dragTransitionPreviewLeftFrame);
            } else {
                core::FrameIndex touchPoint = m_model.clip(aId).end() - extendA;
                extendA = std::max<core::FrameIndex>(0, m_dragTransitionPreviewRightFrame - touchPoint);
            }

            bool ok;
            if (extendA + extendB > 0) {
                std::vector<std::unique_ptr<core::Command>> steps;
                steps.push_back(std::make_unique<core::RemoveTransition>(m_dragTransitionId));
                steps.push_back(std::make_unique<core::AddTransition>(trackId, aId, bId, extendA, extendB));
                ok = m_undoStack.execute(
                    std::make_unique<core::CompositeCommand>("Resize transition", std::move(steps)));
            } else {
                // Dragged all the way to nothing -- just remove it.
                ok = m_undoStack.execute(std::make_unique<core::RemoveTransition>(m_dragTransitionId));
            }
            if (ok) {
                showStatus(extendA + extendB > 0
                              ? "Resized dissolve to " + std::to_string(extendA + extendB) + " frames."
                              : "Removed dissolve.");
            } else {
                showStatus("Couldn't resize that transition — not enough source frames.");
            }
        }
    }

    m_dragMode = TimelineDragMode::None;
    m_draggingTrack = -1;
    m_dragHoverRow = -1;
    m_dragClipTrack = -1;
    m_dragClipStartFrame = -1;
    m_dragTransitionId = core::TransitionId{};
    m_dragTransitionRow = -1;
    refreshTimeline();
}

bool AppWindow::onDragCreatedTransition(core::ClipId a, core::ClipId b, core::FrameIndex extendA,
                                        core::FrameIndex extendB)
{
    core::TrackId trackId = trackIdForRow(m_dragClipTrack);
    if (!m_undoStack.execute(std::make_unique<core::AddTransition>(trackId, a, b, extendA, extendB)))
        return false;
    showStatus("Created a " + std::to_string(extendA + extendB) + "-frame dissolve.");
    return true;
}

void AppWindow::onTimelineDraw(cairo_t *cr, int width, int height)
{
    (void)height; // row layout is driven by kTrackRowHeight, not the widget's actual allocation
    int trackCount = static_cast<int>(m_model.sequence().tracks.size());
    if (trackCount <= 0)
        return;

    double contentWidth = std::max(width - kHandleWidth, 1.0);

    // Row backgrounds: a faint tint on the active track, a stronger one on
    // the current drag drop-target row, plus separators and a grip icon in
    // the handle strip.
    for (int t = 0; t < trackCount; ++t) {
        double rowY = t * kTrackRowHeight;
        bool locked = m_model.track(trackIdForRow(t)).locked;

        if (locked) {
            // Flat tint, not a glow (design system: glow is selection/focus
            // only) -- just enough to notice a row is different at a glance
            // without a per-track header widget to put a lock icon on yet.
            cairo_set_source_rgba(cr, kLockedR, kLockedG, kLockedB, 0.08);
            cairo_rectangle(cr, 0, rowY, width, kTrackRowHeight);
            cairo_fill(cr);
        }

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
        // strip, signaling "drag here to reorder" -- tinted the same
        // warning color as the row when locked, since dragging a clip is
        // blocked there but reordering the track itself still isn't.
        if (locked)
            cairo_set_source_rgb(cr, kLockedR, kLockedG, kLockedB);
        else
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

        // Track name label, in the slim strip reserved at the top of the
        // row (double-click to edit -- onTimelineClicked). Skipped while
        // this exact row is mid-inline-edit: the popover positioned over
        // it already shows (and lets you change) the text.
        const core::Track &modelTrack = m_model.track(trackIdForRow(t));
        if (!modelTrack.name.empty() && !(m_inlineEditKind == InlineEditKind::Track && m_inlineEditTrackRow == t)) {
            drawLabel(cr, modelTrack.name, kHandleWidth + 4, rowY + 1, width - kHandleWidth - 8);
        }
    }

    int total = m_playback->totalFrames();
    if (total <= 0)
        return;

    auto drawClipRect = [&](int trackIndex, int startFrame, int frames, const std::string &clipName, bool selected,
                            bool ghost) {
        double x = kHandleWidth + (static_cast<double>(startFrame) / total) * contentWidth;
        double w = (static_cast<double>(frames) / total) * contentWidth;
        double rowY = trackIndex * kTrackRowHeight;
        double clipTop = rowY + kTrackLabelHeight + 2.0;
        double clipHeight = kTrackRowHeight - kTrackLabelHeight - 6.0;

        cairo_set_source_rgba(cr, kClipFillR, kClipFillG, kClipFillB, ghost ? 0.5 : 1.0);
        cairo_rectangle(cr, x + 1, clipTop, std::max(w - 2, 1.0), clipHeight);
        cairo_fill_preserve(cr);

        if (selected)
            cairo_set_source_rgba(cr, kSelectedR, kSelectedG, kSelectedB, ghost ? 0.7 : 1.0);
        else
            cairo_set_source_rgba(cr, kClipBorderR, kClipBorderG, kClipBorderB, ghost ? 0.7 : 1.0);
        cairo_set_line_width(cr, selected ? 2.0 : 1.0);
        cairo_stroke(cr);

        // Corner badge: the clip's own name if it has one (the more
        // specific identifier once set), else the owning track's name, so
        // which track a clip belongs to is still visible without looking
        // back at the row label (e.g. once scrolled) when the clip has no
        // name of its own. Skipped for the drag-preview ghost outline,
        // where it would just be clutter.
        if (!ghost) {
            const core::Track &owningTrack = m_model.track(trackIdForRow(trackIndex));
            const std::string &label = !clipName.empty() ? clipName : owningTrack.name;
            if (!label.empty())
                drawLabel(cr, label, x + 4, clipTop + 1, std::max(w - 8, 1.0));
        }
    };

    // Waveform: fetches cached peaks (kicking off async computation if not
    // yet available — see WaveformCache) and draws a vertical-bar envelope
    // across the clip's rectangle. Skipped for the actively-dragged clip
    // (its in/out are changing every frame during a trim, which would just
    // thrash the cache) — reasonable to not bother re-drawing a waveform
    // for the ~second a drag lasts.
    auto drawWaveform = [&](const ClipDisplay &clip, double x, double w) {
        if (clip.resource.empty())
            return;
        const std::vector<float> *peaks =
            m_waveforms->peaksFor(clip.resource, clip.in, clip.out, m_model.sequence().profile.fps);
        if (!peaks || peaks->empty())
            return;

        double rowY = clip.trackIndex * kTrackRowHeight;
        double clipTop = rowY + kTrackLabelHeight + 2.0;
        double clipHeight = kTrackRowHeight - kTrackLabelHeight - 6.0;
        double midY = clipTop + clipHeight / 2.0;
        double maxBarHalfHeight = (clipHeight - 4.0) / 2.0;
        size_t peakCount = peaks->size();
        int pixelWidth = std::max(static_cast<int>(w), 1);

        cairo_set_source_rgba(cr, kWaveformR, kWaveformG, kWaveformB, 0.85);
        cairo_set_line_width(cr, 1.0);
        for (int px = 0; px < pixelWidth; ++px) {
            size_t startIdx =
                static_cast<size_t>((static_cast<double>(px) / pixelWidth) * static_cast<double>(peakCount));
            size_t endIdx = std::min(
                peakCount, std::max(startIdx + 1, static_cast<size_t>((static_cast<double>(px + 1) / pixelWidth) *
                                                                      static_cast<double>(peakCount))));

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
        bool isDragged = m_dragMode != TimelineDragMode::None && clip.id == m_dragClipId;
        bool selected = (static_cast<int>(i) == m_selectedClip);

        if (isDragged && m_dragMode == TimelineDragMode::MoveClip)
            continue; // drawn as a ghost at the preview position instead, below

        int drawTrack = isDragged ? m_dragPreviewTrack : clip.trackIndex;
        int drawStart = isDragged ? m_dragPreviewStartFrame : clip.startFrame;
        int drawFrames = isDragged ? m_dragPreviewFrames : clip.frames;
        drawClipRect(drawTrack, drawStart, drawFrames, clip.name, selected, false);

        if (!isDragged) {
            double x = kHandleWidth + (static_cast<double>(clip.startFrame) / total) * contentWidth;
            double w = (static_cast<double>(clip.frames) / total) * contentWidth;
            drawWaveform(clip, x, w);
        }
    }

    if (m_dragMode == TimelineDragMode::MoveClip)
        drawClipRect(m_dragPreviewTrack, m_dragPreviewStartFrame, m_dragPreviewFrames, std::string{}, true, true);

    // Dissolve transitions: a diagonal-hatch overlay on the overlap
    // region between the two clips a transition links -- geometry keyed
    // off the CURRENT (already-extended) clip state, same source as
    // drawClipRect/drawWaveform above, not the transition's own
    // extendA/extendB (those only matter to AddTransition/
    // RemoveTransition, not drawing). A transition being CREATED by a
    // drag (TrimClipStart/End overlapping a neighbour) isn't drawn here
    // -- it doesn't exist until AddTransition runs at drag-end, and the
    // live resize ghost already shows the overlap forming. A transition
    // being RESIZED (TransitionResizeLeft/Right) DOES already exist, so
    // its hatch is drawn live at the drag preview position instead of
    // its last-committed one.
    for (const core::Transition &t : m_model.sequence().transitions) {
        if (!m_model.hasClip(t.a) || !m_model.hasClip(t.b))
            continue; // mid-undo-step transient state; refreshTimeline() will catch up
        const core::Track &owningTrack = m_model.track(t.track);
        int row = -1;
        for (int r = 0; r < trackCount; ++r) {
            if (trackIdForRow(r) == owningTrack.id) {
                row = r;
                break;
            }
        }
        if (row < 0)
            continue;

        const core::Clip &clipA = m_model.clip(t.a);
        const core::Clip &clipB = m_model.clip(t.b);
        bool isBeingResized = t.id == m_dragTransitionId && (m_dragMode == TimelineDragMode::TransitionResizeLeft ||
                                                             m_dragMode == TimelineDragMode::TransitionResizeRight);
        core::FrameIndex overlapStartFrame = isBeingResized ? m_dragTransitionPreviewLeftFrame : clipB.position;
        core::FrameIndex overlapEndFrame = isBeingResized ? m_dragTransitionPreviewRightFrame : clipA.end();
        double overlapX = kHandleWidth + (static_cast<double>(overlapStartFrame) / total) * contentWidth;
        double overlapEndX = kHandleWidth + (static_cast<double>(overlapEndFrame) / total) * contentWidth;
        double rowY = row * kTrackRowHeight;
        double clipTop = rowY + kTrackLabelHeight + 2.0;
        double clipHeight = kTrackRowHeight - kTrackLabelHeight - 6.0;

        cairo_save(cr);
        cairo_rectangle(cr, overlapX, clipTop, std::max(overlapEndX - overlapX, 1.0), clipHeight);
        cairo_clip(cr);
        cairo_set_source_rgba(cr, kSelectedR, kSelectedG, kSelectedB, 0.6);
        cairo_set_line_width(cr, 1.5);
        constexpr double kHatchSpacing = 7.0;
        for (double sx = overlapX - clipHeight; sx < overlapEndX; sx += kHatchSpacing) {
            cairo_move_to(cr, sx, clipTop + clipHeight);
            cairo_line_to(cr, sx + clipHeight, clipTop);
        }
        cairo_stroke(cr);
        cairo_restore(cr);
    }

    // Playhead: a vertical line at the current frame, spanning every
    // track row, drawn last so it sits on top of clips/waveforms/hatch
    // overlays -- the only at-a-glance answer to "where are we" on a
    // multi-track timeline (previously only the seek bar below the
    // preview showed this). refreshTransport() queues the redraw that
    // keeps this in sync with playback, not just with edits.
    int currentFrame = m_playback->currentFrame();
    double playheadX = kHandleWidth + (static_cast<double>(currentFrame) / total) * contentWidth;
    cairo_set_source_rgb(cr, kPlayheadR, kPlayheadG, kPlayheadB);
    cairo_set_line_width(cr, 2.0);
    cairo_move_to(cr, playheadX, 0);
    cairo_line_to(cr, playheadX, trackCount * kTrackRowHeight);
    cairo_stroke(cr);
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

void AppWindow::onThumbnailReady()
{
    // Audit A4: a thumbnail finishing while the panel is hidden has
    // nothing on screen to update -- rebuilding it anyway means N full
    // rebuilds (destroying and recreating every row) for N assets
    // imported at once, none of them visible. onToggleMediaBrowserClicked
    // already runs its own refreshMediaBrowser() when the panel goes
    // from hidden to visible, which picks up everything that finished
    // in the meantime in a single rebuild.
    if (!gtk_widget_get_visible(m_mediaBrowserPanel))
        return;
    refreshMediaBrowser();
}

void AppWindow::onToggleMediaBrowserClicked()
{
    bool visible = gtk_widget_get_visible(m_mediaBrowserPanel);
    gtk_widget_set_visible(m_mediaBrowserPanel, !visible);
    if (!visible)
        refreshMediaBrowser(); // was hidden -- may be stale/never built
}

void AppWindow::refreshMediaBrowser()
{
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(m_mediaBrowserList));
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(m_mediaBrowserList, child);
        child = next;
    }

    const auto &bin = m_model.project().bin;
    for (const core::Asset &asset : bin) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        // Right-click (context menu) and drag-to-timeline both need to
        // know which asset this row is for; stashed on the row itself
        // rather than captured per-signal-connection since the row (and
        // everything on it) is torn down and rebuilt wholesale on every
        // refreshMediaBrowser() anyway.
        g_object_set_data(G_OBJECT(row), "ustudio-asset-id",
                          reinterpret_cast<void *>(static_cast<uintptr_t>(asset.id.value)));

        GtkWidget *thumbCell = gtk_picture_new();
        gtk_widget_set_size_request(thumbCell, 120, 68);
        gtk_picture_set_content_fit(GTK_PICTURE(thumbCell), GTK_CONTENT_FIT_CONTAIN);
        // thumbnailFor() kicks off a background job on a miss and returns
        // nullptr; onThumbnailReady() re-calls refreshMediaBrowser() once
        // it's ready, so a still-loading row just shows an empty cell for
        // one redraw cycle rather than a placeholder icon.
        const engine::ThumbnailCache::Data *thumb = m_thumbnails->thumbnailFor(asset.path);
        if (thumb && thumb->width > 0 && thumb->height > 0) {
            GBytes *bytes = g_bytes_new(thumb->rgba.data(), thumb->rgba.size());
            GdkTexture *texture = gdk_memory_texture_new(thumb->width, thumb->height, GDK_MEMORY_R8G8B8A8, bytes,
                                                          static_cast<gsize>(thumb->width) * 4);
            g_bytes_unref(bytes);
            gtk_picture_set_paintable(GTK_PICTURE(thumbCell), GDK_PAINTABLE(texture));
            g_object_unref(texture);
        }
        gtk_box_append(GTK_BOX(row), thumbCell);

        GtkWidget *nameLabel = gtk_label_new(asset.displayName.c_str());
        gtk_label_set_ellipsize(GTK_LABEL(nameLabel), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_xalign(GTK_LABEL(nameLabel), 0.0);
        gtk_widget_set_size_request(nameLabel, 140, -1);
        gtk_widget_set_tooltip_text(nameLabel, asset.path.c_str());
        gtk_box_append(GTK_BOX(row), nameLabel);

        GtkWidget *lengthLabel = gtk_label_new(formatMediaLength(asset.info.nativeDurationSeconds).c_str());
        gtk_widget_add_css_class(lengthLabel, "dim-label");
        gtk_widget_set_size_request(lengthLabel, 60, -1);
        gtk_label_set_xalign(GTK_LABEL(lengthLabel), 0.0);
        gtk_box_append(GTK_BOX(row), lengthLabel);

        GtkWidget *fpsLabel = gtk_label_new(formatMediaFps(asset.info.fps).c_str());
        gtk_widget_add_css_class(fpsLabel, "dim-label");
        gtk_widget_set_size_request(fpsLabel, 40, -1);
        gtk_label_set_xalign(GTK_LABEL(fpsLabel), 0.0);
        gtk_box_append(GTK_BOX(row), fpsLabel);

        std::string formatText = asset.info.container;
        for (char &c : formatText)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        GtkWidget *formatLabel = gtk_label_new(formatText.empty() ? "—" : formatText.c_str());
        gtk_widget_add_css_class(formatLabel, "dim-label");
        gtk_widget_set_size_request(formatLabel, 50, -1);
        gtk_label_set_xalign(GTK_LABEL(formatLabel), 0.0);
        gtk_box_append(GTK_BOX(row), formatLabel);

        GtkGesture *rowRightClick = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rowRightClick), GDK_BUTTON_SECONDARY);
        g_signal_connect(rowRightClick, "pressed", G_CALLBACK(&AppWindow::mediaBrowserRowRightClickTrampoline),
                         this);
        gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(rowRightClick));

        // Drag-to-timeline: content is just the AssetId's value (a
        // G_TYPE_INT64), which onTimelineDrop looks back up against
        // m_model.project().bin -- the row itself never needs to travel,
        // only which asset it names.
        GtkDragSource *dragSource = gtk_drag_source_new();
        gtk_drag_source_set_actions(dragSource, GDK_ACTION_COPY);
        GdkContentProvider *content =
            gdk_content_provider_new_typed(G_TYPE_INT64, static_cast<gint64>(asset.id.value));
        gtk_drag_source_set_content(dragSource, content);
        g_object_unref(content);
        gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(dragSource));

        gtk_box_append(m_mediaBrowserList, row);
    }
}

void AppWindow::onMediaBrowserRowRightClicked(core::AssetId assetId, GtkWidget *row, double x, double y)
{
    m_contextMenuAssetId = assetId;

    graphene_point_t local = GRAPHENE_POINT_INIT(static_cast<float>(x), static_cast<float>(y));
    graphene_point_t inPanel{};
    if (!gtk_widget_compute_point(row, m_mediaBrowserPanel, &local, &inPanel))
        inPanel = local; // row is always a live descendant of m_mediaBrowserPanel; kept as a harmless fallback

    GdkRectangle rect{static_cast<int>(inPanel.x), static_cast<int>(inPanel.y), 1, 1};
    gtk_popover_set_pointing_to(m_mediaBrowserContextMenu, &rect);
    gtk_popover_popup(m_mediaBrowserContextMenu);
}

void AppWindow::onRemoveAssetClicked()
{
    gtk_popover_popdown(m_mediaBrowserContextMenu);
    if (!m_contextMenuAssetId.isValid() || !m_model.hasAsset(m_contextMenuAssetId))
        return;
    std::string name = m_model.asset(m_contextMenuAssetId).displayName;

    if (m_undoStack.execute(std::make_unique<core::RemoveAsset>(m_contextMenuAssetId))) {
        refreshTimeline();
        refreshMediaBrowser();
        showStatus("Removed " + name + " from the project.");
    } else {
        showStatus("Can't remove " + name + ": used by a clip on a locked track.");
    }
}

void AppWindow::onDeleteAssetFileClicked()
{
    gtk_popover_popdown(m_mediaBrowserContextMenu);
    if (!m_contextMenuAssetId.isValid() || !m_model.hasAsset(m_contextMenuAssetId))
        return;
    const core::Asset &asset = m_model.asset(m_contextMenuAssetId);

    int clipCount = 0;
    for (const auto &[clipId, clip] : m_model.sequence().clips)
        if (clip.asset == m_contextMenuAssetId)
            ++clipCount;

    std::string body = "Move \"" + asset.displayName + "\" to Trash?";
    if (clipCount > 0)
        body += " It's used by " + std::to_string(clipCount) + (clipCount == 1 ? " clip" : " clips") +
                " in this project -- those will be removed too.";

    AdwDialog *dialog = adw_alert_dialog_new("Move file to Trash?", body.c_str());
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "delete", "Move to Trash");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "delete", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");

    struct DeleteContext
    {
        AppWindow *self;
        core::AssetId assetId;
        std::string path;
        std::string displayName;
    };
    auto *ctx = new DeleteContext{this, m_contextMenuAssetId, asset.path, asset.displayName};

    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(dialog), GTK_WIDGET(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer userData) {
            std::unique_ptr<DeleteContext> owned(static_cast<DeleteContext *>(userData));
            const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
            if (!response || std::string(response) != "delete")
                return;

            AppWindow *self = owned->self;
            if (!self->m_model.hasAsset(owned->assetId)) {
                self->showStatus("Can't delete " + owned->displayName + ": no longer in the project.");
                return;
            }

            if (!self->m_undoStack.execute(std::make_unique<core::RemoveAsset>(owned->assetId))) {
                self->showStatus("Can't delete " + owned->displayName + ": used by a clip on a locked track.");
                return;
            }
            self->refreshTimeline();
            self->refreshMediaBrowser();

            // The project no longer references it either way (RemoveAsset
            // above already succeeded); a failed trash just leaves the
            // now-orphaned file on disk. Audit A3: this used to be
            // std::filesystem::remove -- a permanent unlink with no
            // recovery path -- despite the dialog only asking about
            // removing it from the *project*. g_file_trash() (GIO,
            // already a dependency; CLAUDE.md's "never execute/shell-
            // interpolate a project-file-derived path" rule is why this
            // isn't system("gio trash ...")) moves it to the desktop's
            // Trash instead, so it's still recoverable the normal way a
            // GNOME user expects.
            std::error_code ec;
            if (!std::filesystem::exists(owned->path, ec)) {
                self->showStatus("Removed " + owned->displayName + " (file was already gone).");
                return;
            }
            GFile *file = g_file_new_for_path(owned->path.c_str());
            GError *error = nullptr;
            if (!g_file_trash(file, nullptr, &error)) {
                Log::error("[app] could not move " + owned->path + " to Trash: " + error->message);
                self->showStatus("Removed " + owned->displayName +
                                 " from the project, but could not move the file to Trash: " + error->message);
                g_error_free(error);
            } else {
                self->showStatus("Moved " + owned->displayName + " to Trash.");
            }
            g_object_unref(file);
        },
        ctx);
}

gboolean AppWindow::onTimelineDrop(const GValue *value, double x, double y)
{
    int trackCount = static_cast<int>(m_model.sequence().tracks.size());
    if (trackCount <= 0 || !G_VALUE_HOLDS_INT64(value))
        return FALSE;

    core::AssetId assetId{static_cast<uint64_t>(g_value_get_int64(value))};
    if (!m_model.hasAsset(assetId))
        return FALSE;
    const core::Asset &asset = m_model.asset(assetId);

    int row = std::clamp(static_cast<int>(y / kTrackRowHeight), 0, trackCount - 1);
    core::TrackId trackId = trackIdForRow(row);
    if (m_model.track(trackId).locked) {
        showStatus("Can't drop onto a locked track.");
        return FALSE;
    }

    int widgetWidth = gtk_widget_get_width(GTK_WIDGET(m_timeline));
    if (widgetWidth <= kHandleWidth || x < kHandleWidth)
        return FALSE;

    int total = m_playback->totalFrames();
    double contentWidth = widgetWidth - kHandleWidth;
    core::FrameIndex dropFrame =
        total > 0 ? static_cast<core::FrameIndex>(((x - kHandleWidth) / contentWidth) * total) : 0;

    core::FrameIndex length =
        effectiveInsertLength(asset.info.isBoundless(), asset.info.lengthInSequenceFrames, dropFrame);

    if (!m_model.isRangeFree(trackId, dropFrame, dropFrame + length)) {
        showStatus("Can't drop " + asset.displayName + " there: it would overlap another clip.");
        return FALSE;
    }

    if (m_undoStack.execute(std::make_unique<core::InsertClip>(trackId, assetId, dropFrame, 0, length - 1))) {
        refreshTimeline();
        showStatus("Added " + asset.displayName + " to track " + std::to_string(row) + ".");
        return TRUE;
    }
    return FALSE;
}

core::FrameIndex AppWindow::effectiveInsertLength(bool isBoundless, core::FrameIndex knownLength,
                                                  core::FrameIndex insertPos) const
{
    if (!isBoundless)
        return knownLength;
    core::FrameIndex timelineLength = m_model.sequence().length();
    if (timelineLength > insertPos)
        return timelineLength - insertPos;
    const core::Rational &fps = m_model.sequence().profile.fps;
    double fpsValue = fps.den > 0 ? static_cast<double>(fps.num) / fps.den : 30.0;
    return static_cast<core::FrameIndex>(fpsValue * 10.0);
}

void AppWindow::beginTrackNameEdit(int row)
{
    int trackCount = static_cast<int>(m_model.sequence().tracks.size());
    if (row < 0 || row >= trackCount)
        return;

    m_inlineEditKind = InlineEditKind::Track;
    m_inlineEditTrackRow = row;

    const core::Track &track = m_model.track(trackIdForRow(row));
    GdkRectangle anchor{static_cast<int>(kHandleWidth), static_cast<int>(row * kTrackRowHeight), 160,
                        static_cast<int>(kTrackLabelHeight)};
    showInlineNameEditor(anchor, track.name);
}

void AppWindow::beginClipNameEdit(const ClipDisplay &clip)
{
    m_inlineEditKind = InlineEditKind::Clip;
    m_inlineEditClipId = clip.id;

    int total = m_playback->totalFrames();
    int widgetWidth = gtk_widget_get_width(GTK_WIDGET(m_timeline));
    int anchorX = static_cast<int>(kHandleWidth);
    int anchorW = 160;
    if (total > 0 && widgetWidth > kHandleWidth) {
        double contentWidth = widgetWidth - kHandleWidth;
        anchorX = static_cast<int>(kHandleWidth + (static_cast<double>(clip.startFrame) / total) * contentWidth);
        anchorW = std::max(static_cast<int>((static_cast<double>(clip.frames) / total) * contentWidth), 40);
    }
    double rowY = clip.trackIndex * kTrackRowHeight + kTrackLabelHeight;
    GdkRectangle anchor{anchorX, static_cast<int>(rowY), anchorW,
                        static_cast<int>(kTrackRowHeight - kTrackLabelHeight)};
    showInlineNameEditor(anchor, clip.name);
}

void AppWindow::showInlineNameEditor(GdkRectangle anchor, const std::string &currentName)
{
    m_inlineEditCancelled = false;
    setTransportActionsEnabled(false);
    gtk_editable_set_text(GTK_EDITABLE(m_inlineNameEditEntry), currentName.c_str());
    gtk_popover_set_pointing_to(m_inlineNameEditPopover, &anchor);
    gtk_popover_popup(m_inlineNameEditPopover);
    gtk_widget_grab_focus(GTK_WIDGET(m_inlineNameEditEntry));
}

void AppWindow::onInlineNameEditClosed()
{
    setTransportActionsEnabled(true);

    InlineEditKind kind = m_inlineEditKind;
    m_inlineEditKind = InlineEditKind::None;

    if (kind == InlineEditKind::None || m_inlineEditCancelled) {
        m_inlineEditCancelled = false;
        gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
        return;
    }

    std::string newName = gtk_editable_get_text(GTK_EDITABLE(m_inlineNameEditEntry));

    if (kind == InlineEditKind::Track) {
        core::TrackId trackId = trackIdForRow(m_inlineEditTrackRow);
        if (trackId.isValid() && newName != m_model.track(trackId).name)
            m_undoStack.execute(std::make_unique<core::RenameTrack>(trackId, newName));
    } else {
        if (m_model.hasClip(m_inlineEditClipId) && newName != m_model.clip(m_inlineEditClipId).name)
            m_undoStack.execute(std::make_unique<core::RenameClip>(m_inlineEditClipId, newName));
    }

    refreshTimeline();
}

gboolean AppWindow::onInlineNameEditKeyPressed(guint keyval)
{
    if (keyval == GDK_KEY_Escape) {
        m_inlineEditCancelled = true;
        gtk_popover_popdown(m_inlineNameEditPopover);
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

gboolean AppWindow::onTimelineQueryTooltip(int x, int y, GtkTooltip *tooltip)
{
    for (const auto &clip : m_clips) {
        double rowY = clip.trackIndex * kTrackRowHeight;
        int total = m_playback->totalFrames();
        int widgetWidth = gtk_widget_get_width(GTK_WIDGET(m_timeline));
        if (total <= 0 || widgetWidth <= kHandleWidth)
            continue;
        double contentWidth = widgetWidth - kHandleWidth;
        double clipX = kHandleWidth + (static_cast<double>(clip.startFrame) / total) * contentWidth;
        double clipW = (static_cast<double>(clip.frames) / total) * contentWidth;

        if (x < clipX || x >= clipX + clipW || y < rowY || y >= rowY + kTrackRowHeight)
            continue;

        std::string name = clip.name.empty() ? std::string("(unnamed)") : clip.name;
        std::string text = name + "\n" + formatTimecode(clip.startFrame) + " – " +
                            formatTimecode(clip.startFrame + clip.frames) + "\nLength: " +
                            formatTimecode(clip.frames) + " (" + std::to_string(clip.frames) + " frames)\nSource: " +
                            (clip.resource.empty() ? std::string("(none)") : clip.resource);
        gtk_tooltip_set_text(tooltip, text.c_str());
        return TRUE;
    }
    return FALSE;
}

core::TrackId AppWindow::trackIdForRow(int row) const
{
    const auto &tracks = m_model.sequence().tracks;
    if (row < 0 || row >= static_cast<int>(tracks.size())) {
        Log::error("[app] trackIdForRow: row " + std::to_string(row) + " out of range (" + std::to_string(tracks.size()) +
                   " tracks)");
        return core::TrackId{};
    }
    return tracks[static_cast<size_t>(row)].id;
}

void AppWindow::refreshTimeline()
{
    m_clips.clear();
    const core::Sequence &seq = m_model.sequence();
    for (size_t row = 0; row < seq.tracks.size(); ++row) {
        const core::Track &track = seq.tracks[row];
        for (core::ClipId clipId : track.clips) {
            const core::Clip &clip = m_model.clip(clipId);
            ClipDisplay display;
            display.id = clipId;
            display.name = clip.name;
            display.resource = m_model.hasAsset(clip.asset) ? m_model.asset(clip.asset).path : std::string{};
            display.trackIndex = static_cast<int>(row);
            display.startFrame = static_cast<int>(clip.position);
            display.frames = static_cast<int>(clip.length());
            display.in = static_cast<int>(clip.in);
            display.out = static_cast<int>(clip.out);
            m_clips.push_back(std::move(display));
        }
    }

    int trackCount = static_cast<int>(seq.tracks.size());
    int height = std::max(trackCount, 1) * static_cast<int>(kTrackRowHeight);
    gtk_widget_set_size_request(GTK_WIDGET(m_timeline), -1, height);

    int total = m_playback->totalFrames();
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
    // Keeps the timeline's playhead line in sync with playback, not just
    // with edits -- this runs once per displayed frame (onFrameReady's
    // own comment), both while playing and after a seek (seek() purges
    // and requests a fresh frame, which comes back through the same
    // callback), so nothing else needs to separately queue this redraw.
    gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
}

void AppWindow::refreshPlayButtonIcon()
{
    const char *icon = m_playback->isPlaying() ? "media-playback-pause-symbolic" : "media-playback-start-symbolic";
    gtk_button_set_icon_name(m_playButton, icon);
}

void AppWindow::refreshLoopStatusLabel()
{
    auto range = m_playback->loopRange();
    if (!range) {
        gtk_label_set_text(m_loopStatusLabel, "");
        return;
    }
    std::string text = "Loop " + formatTimecode(range->first) + " – " + formatTimecode(range->second);
    gtk_label_set_text(m_loopStatusLabel, text.c_str());
}

bool AppWindow::pathIsProjectAsset(const std::string &path) const
{
    std::error_code ec;
    std::filesystem::path candidate = std::filesystem::weakly_canonical(path, ec);
    if (ec)
        candidate = path; // weakly_canonical only fails on a genuinely unusable path; compare as given then
    for (const core::Asset &asset : m_model.project().bin) {
        std::error_code assetEc;
        std::filesystem::path assetPath = std::filesystem::weakly_canonical(asset.path, assetEc);
        if (assetEc)
            assetPath = asset.path;
        if (candidate == assetPath)
            return true;
    }
    return false;
}

void AppWindow::updateWindowTitle()
{
    std::string title = m_undoStack.isClean() ? "u Studio Video Editor" : "u Studio Video Editor •";
    gtk_window_set_title(GTK_WINDOW(m_window), title.c_str());
    gtk_widget_set_sensitive(GTK_WIDGET(m_undoButton), m_undoStack.canUndo());
    gtk_widget_set_sensitive(GTK_WIDGET(m_redoButton), m_undoStack.canRedo());
}

void AppWindow::showStatus(const std::string &text)
{
    gtk_label_set_text(m_statusLabel, text.c_str());
    // Every user-visible outcome (import result, save/open/render success
    // or failure, split/delete/close-gap/lock/volume messages, refusals)
    // goes through this one function -- logging it here, once, instead of
    // at each of the dozens of call sites is the only way to get a
    // reliable trail of "what did the user just do" leading up to a crash
    // without relying on someone remembering to log at every future call
    // site too.
    Log::debug("[app] status: " + text);
}

std::string AppWindow::formatTimecode(int frame) const
{
    double fps = m_playback->fps();
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

void AppWindow::performAutosave()
{
    std::string dir = autosave::directory();
    if (dir.empty())
        return;

    std::string base = autosave::baseNameFor(m_currentProjectPath, m_autosaveSessionId);
    std::string autosavePath = dir + "/" + base + ".ustudio";
    std::string metaPath = dir + "/" + base + ".meta";

    if (!core::saveProject(m_model, autosavePath).empty())
        return; // silent: an autosave failure shouldn't interrupt the user

    autosave::Meta meta;
    meta.originalPath = m_currentProjectPath;
    meta.timestampUnix = static_cast<int64_t>(std::time(nullptr));
    meta.ownerPid = static_cast<int64_t>(getpid()); // audit A5: lets a later launch skip a still-live owner
    // audit A3: cross-checked against ownerPid in ownerAlive() so a
    // later, unrelated process reusing this same pid isn't mistaken for
    // this instance still being alive.
    meta.ownerStartTime = autosave::processStartTime(meta.ownerPid);
    autosave::writeMeta(metaPath, meta);

    m_lastAutosaveMonotonicUsec = g_get_monotonic_time();
    Log::debug("[app] Autosaved to " + autosavePath);
}

void AppWindow::onAutosaveHeartbeat()
{
    if (m_undoStack.isClean())
        return;
    // Already covered this edit -- avoid re-writing the same state every
    // 10s while the user is away with nothing new to capture.
    if (m_lastAutosaveMonotonicUsec >= m_lastEditMonotonicUsec)
        return;

    constexpr gint64 kAutosaveDelayUsec = 120 * G_USEC_PER_SEC; // doc 09: 2 minutes since the last command
    if (g_get_monotonic_time() - m_lastEditMonotonicUsec >= kAutosaveDelayUsec)
        performAutosave();
}

void AppWindow::onWindowActiveChanged()
{
    // doc 09: autosave "on focus loss" too, not just the 2-minute timer.
    if (!gtk_window_is_active(GTK_WINDOW(m_window)) && !m_undoStack.isClean())
        performAutosave();
}

void AppWindow::confirmDiscardIfDirty(std::function<void()> onConfirmed)
{
    if (m_undoStack.isClean()) {
        onConfirmed();
        return;
    }

    AdwDialog *dialog =
        adw_alert_dialog_new("Discard unsaved changes?", "This project has unsaved changes that will be lost.");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "discard", "Discard");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "discard", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");

    struct DiscardConfirmContext
    {
        std::function<void()> onConfirmed;
    };
    auto *ctx = new DiscardConfirmContext{std::move(onConfirmed)};

    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(dialog), GTK_WIDGET(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer userData) {
            std::unique_ptr<DiscardConfirmContext> owned(static_cast<DiscardConfirmContext *>(userData));
            const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
            if (response && std::string(response) == "discard")
                owned->onConfirmed();
        },
        ctx);
}

gboolean AppWindow::onCloseRequest()
{
    if (m_undoStack.isClean())
        return GDK_EVENT_PROPAGATE;

    AdwDialog *dialog = adw_alert_dialog_new(
        "Save changes before closing?", "This project has unsaved changes that will be lost if you don't save them.");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "discard", "Discard");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "save", "Save");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "discard", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "save", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "save");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");

    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(dialog), GTK_WIDGET(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer userData) {
            auto *self = static_cast<AppWindow *>(userData);
            const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
            if (!response)
                return;
            std::string chosen = response;
            if (chosen == "discard") {
                // Bypasses close-request entirely -- calling
                // gtk_window_close() again here would just re-enter this
                // same prompt.
                gtk_window_destroy(GTK_WINDOW(self->m_window));
            } else if (chosen == "save") {
                self->m_closeAfterSave = true;
                self->onSaveClicked();
            }
            // "cancel": nothing to do -- the close is already vetoed by
            // onCloseRequest()'s GDK_EVENT_STOP.
        },
        this);

    return GDK_EVENT_STOP;
}

void AppWindow::offerRecoveryIfAny()
{
    // Excludes every candidate already offered (and answered) THIS
    // launch -- recovering doesn't delete the file (see
    // m_pendingAutosaveCleanupPath's own comment), so without this the
    // very next call below would just find the same one again, forever,
    // and any OTHER independently-orphaned autosave would never surface
    // at all (2026-09-23: exactly how a real, richer autosave went
    // unmentioned and unrecovered alongside newer, emptier ones).
    auto found = autosave::findRecoverable(m_offeredAutosaveMetaPaths);
    if (!found)
        return;
    bool isFirstOfferThisLaunch = m_offeredAutosaveMetaPaths.empty();
    m_offeredAutosaveMetaPaths.insert(found->metaPath);

    auto timestamp = static_cast<std::time_t>(found->meta.timestampUnix);
    char timeBuf[64] = {};
    std::tm tmBuf{};
    localtime_r(&timestamp, &tmBuf);
    std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M", &tmBuf);

    std::string article = isFirstOfferThisLaunch ? "An" : "Another";
    std::string body = found->meta.originalPath.empty()
                          ? article + " unsaved, untitled project from " + timeBuf + " was found."
                          : article + " unsaved version of “" + found->meta.originalPath + "” from " + timeBuf +
                                " was found.";
    if (!isFirstOfferThisLaunch)
        body += " Recovering it will replace what you just recovered.";

    AdwDialog *dialog = adw_alert_dialog_new("Recover unsaved work?", body.c_str());
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "discard", "Discard");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "recover", "Recover");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "recover", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "recover");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "discard");

    struct RecoveryContext
    {
        AppWindow *self;
        autosave::Recoverable found;
    };
    auto *ctx = new RecoveryContext{this, *found};

    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(dialog), GTK_WIDGET(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer userData) {
            std::unique_ptr<RecoveryContext> owned(static_cast<RecoveryContext *>(userData));
            const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);

            if (response && std::string(response) == "recover") {
                auto loaded = core::loadProject(owned->found.autosavePath);
                if (loaded.has_value()) {
                    owned->self->m_model = std::move(*loaded);
                    owned->self->m_currentProjectPath = owned->found.meta.originalPath;
                    owned->self->m_undoStack.clear();
                    // markDirty(), not setCleanPoint() (audit A2): recovered
                    // content is unsaved relative to m_currentProjectPath (or
                    // has no target at all), but clear() alone already makes
                    // an empty stack report clean by definition (0 == 0) --
                    // there's no "depth" to NOT reset back to that would
                    // otherwise leave it dirty. markDirty() forces isClean()
                    // false until an explicit, later setCleanPoint() (a real
                    // Save) says otherwise.
                    owned->self->m_undoStack.markDirty(); // emits changed -- updateWindowTitle() follows
                    owned->self->m_engineSync->reset(); // rebuilt.connect() (ctor) re-anchors playback automatically
                    owned->self->m_activeTrack = 0;
                    owned->self->m_selectedClip = -1;
                    owned->self->refreshTimeline();
                    owned->self->refreshMediaBrowser();
                    owned->self->showStatus("Recovered unsaved work.");
                    // NOT deleted here: this session's own autosaves go to a
                    // filename keyed on its own (fresh) session id, never
                    // this recovered file's, so nothing else will ever clean
                    // it up. Kept as the only durable copy of the recovered
                    // work until a manual Save succeeds (onSaveFinished()),
                    // so a second crash before that Save doesn't lose it
                    // again.
                    owned->self->m_pendingAutosaveCleanupPath = owned->found.autosavePath;
                    owned->self->m_pendingAutosaveCleanupMetaPath = owned->found.metaPath;
                } else {
                    // Load failed -- leave the autosave files untouched
                    // entirely rather than destroying what may be the only
                    // copy of that work; a later launch gets another chance
                    // to recover them.
                    owned->self->showStatus("Couldn't recover: " + loaded.error());
                }
            } else {
                // An affirmative "no" from the owner (doc 09: "discarded
                // ones are deleted") -- safe to remove immediately, unlike
                // the recover-success case above.
                owned->self->showStatus("Discarded the recovered autosave.");
                std::remove(owned->found.autosavePath.c_str());
                std::remove(owned->found.metaPath.c_str());
            }

            // Check again: m_offeredAutosaveMetaPaths now excludes the one
            // just handled, so this surfaces any OTHER independently-
            // orphaned autosave instead of leaving it to be silently
            // outranked by whichever one this call happened to find
            // first. A no-op the overwhelming majority of the time (one
            // recoverable file is the normal case).
            owned->self->offerRecoveryIfAny();
        },
        ctx);
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

void AppWindow::reloadProjectClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onReloadProjectClicked();
}

void AppWindow::newProjectClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onNewProjectClicked();
}

void AppWindow::toggleMediaBrowserClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onToggleMediaBrowserClicked();
}

void AppWindow::mediaBrowserRowRightClickTrampoline(GtkGestureClick *gesture, int, double x, double y,
                                                    gpointer userData)
{
    GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    auto assetIdValue =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g_object_get_data(G_OBJECT(row), "ustudio-asset-id")));
    static_cast<AppWindow *>(userData)->onMediaBrowserRowRightClicked(core::AssetId{assetIdValue}, row, x, y);
}

void AppWindow::removeAssetClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onRemoveAssetClicked();
}

void AppWindow::deleteAssetFileClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onDeleteAssetFileClicked();
}

gboolean AppWindow::timelineDropTrampoline(GtkDropTarget *, const GValue *value, double x, double y,
                                           gpointer userData)
{
    return static_cast<AppWindow *>(userData)->onTimelineDrop(value, x, y);
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

void AppWindow::undoClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onUndo();
}

void AppWindow::redoClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onRedo();
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

void AppWindow::timelineClickTrampoline(GtkGestureClick *, int nPress, double x, double y, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onTimelineClicked(nPress, x, y);
}

void AppWindow::timelineRightClickTrampoline(GtkGestureClick *, int, double x, double y, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onTimelineRightClicked(x, y);
}

void AppWindow::deleteClipClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onDeleteClipClicked();
}

void AppWindow::splitAudioClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSplitAudioClicked();
}

void AppWindow::closeGapClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onCloseGapClicked();
}

void AppWindow::removeTrackClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onRemoveTrackClicked();
}

void AppWindow::toggleLockClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onToggleLockClicked();
}

void AppWindow::editTrackNameClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onEditTrackNameClicked();
}

void AppWindow::editClipNameClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onEditClipNameClicked();
}

void AppWindow::removeClipNameClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onRemoveClipNameClicked();
}

void AppWindow::removeTransitionClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onRemoveTransitionClicked();
}

void AppWindow::addTransitionClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onAddTransitionClicked();
}

gboolean AppWindow::timelineQueryTooltipTrampoline(GtkWidget *, int x, int y, gboolean, GtkTooltip *tooltip,
                                                   gpointer userData)
{
    return static_cast<AppWindow *>(userData)->onTimelineQueryTooltip(x, y, tooltip);
}

void AppWindow::inlineNameEditActivateTrampoline(GtkEntry *, gpointer userData)
{
    // Pops the popover down; its "closed" signal (inlineNameEditClosedTrampoline)
    // does the actual commit, so Enter and click-away share one code path.
    gtk_popover_popdown(static_cast<AppWindow *>(userData)->m_inlineNameEditPopover);
}

void AppWindow::inlineNameEditClosedTrampoline(GtkPopover *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onInlineNameEditClosed();
}

gboolean AppWindow::inlineNameEditKeyTrampoline(GtkEventControllerKey *, guint keyval, guint, GdkModifierType,
                                                gpointer userData)
{
    return static_cast<AppWindow *>(userData)->onInlineNameEditKeyPressed(keyval);
}

void AppWindow::trackVolumeChangedTrampoline(GtkRange *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onTrackVolumeChanged();
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

void AppWindow::undoActionActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onUndo();
}

void AppWindow::redoActionActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onRedo();
}

void AppWindow::shuttleForwardActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onShuttleForward();
}

void AppWindow::shuttleReverseActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onShuttleReverse();
}

void AppWindow::shuttleStopActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onShuttleStop();
}

void AppWindow::stepForwardActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onStepForward();
}

void AppWindow::stepBackwardActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onStepBackward();
}

void AppWindow::seekHomeActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSeekHome();
}

void AppWindow::seekEndActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSeekEnd();
}

void AppWindow::loopSetInActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSetLoopIn();
}

void AppWindow::loopSetOutActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSetLoopOut();
}

void AppWindow::seekPreviousCutActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSeekPreviousCut();
}

void AppWindow::seekNextCutActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onSeekNextCut();
}

void AppWindow::activeTrackUpActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onActiveTrackUp();
}

void AppWindow::activeTrackDownActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onActiveTrackDown();
}

void AppWindow::stepForward10Activated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onStepForward10();
}

void AppWindow::stepBackward10Activated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onStepBackward10();
}

void AppWindow::stepForwardMinuteActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onStepForwardMinute();
}

void AppWindow::stepBackwardMinuteActivated(GSimpleAction *, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onStepBackwardMinute();
}

void AppWindow::clearLoopClickedTrampoline(GtkButton *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onClearLoopClicked();
}

void AppWindow::volumeChangedTrampoline(GtkRange *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onVolumeChanged();
}

void AppWindow::previewScaleChangedTrampoline(GtkDropDown *, GParamSpec *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onPreviewScaleChanged();
}

gboolean AppWindow::autosaveHeartbeatTrampoline(gpointer userData)
{
    static_cast<AppWindow *>(userData)->onAutosaveHeartbeat();
    return G_SOURCE_CONTINUE;
}

void AppWindow::windowActiveChangedTrampoline(GObject *, GParamSpec *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->onWindowActiveChanged();
}

gboolean AppWindow::closeRequestTrampoline(GtkWindow *, gpointer userData)
{
    return static_cast<AppWindow *>(userData)->onCloseRequest();
}

} // namespace ustudio::app
