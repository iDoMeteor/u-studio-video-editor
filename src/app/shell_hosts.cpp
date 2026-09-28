// Doc 15 IP5: AppWindow as the ShellHost drop-ins build their UI on
// (shell_host.h). Split out of app_window.cpp to keep the hosts together;
// every host starts empty, and until a drop-in adds to it the window is
// built and behaves exactly as without this file.

#include "app_window.h"

#include "core/log.h"
#include "core/media/fingerprint.h"
#include "core/media/utf8_path.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace ustudio::app {

namespace Log = ustudio::core::Log;

bool AppWindow::execute(std::unique_ptr<core::Command> command)
{
    if (!m_undoStack.execute(std::move(command)))
        return false;
    refreshTimeline();
    return true;
}

core::FrameIndex AppWindow::currentFrame() const
{
    return m_engine->currentFrame();
}

ShellSelection AppWindow::currentSelection() const
{
    ShellSelection selection;
    for (core::ClipId clip : m_timelineController.selection().clips())
        if (m_model.hasClip(clip))
            selection.clips.push_back(clip);
    std::sort(selection.clips.begin(), selection.clips.end(), [this](core::ClipId a, core::ClipId b) {
        const core::Clip &x = m_model.clip(a), &y = m_model.clip(b);
        return x.position != y.position ? x.position < y.position : a.value < b.value;
    });
    if (m_activeTrack >= 0 && static_cast<size_t>(m_activeTrack) < m_model.sequence().tracks.size())
        selection.track = trackIdForRow(m_activeTrack);
    return selection;
}

void AppWindow::noteSelectionForShell()
{
    if (!m_hasShellExtensions || m_shellSelectionIdleId != 0 || currentSelection() == m_lastShellSelection)
        return;
    m_shellSelectionIdleId = g_idle_add(&AppWindow::shellSelectionIdleTrampoline, this);
}

void AppWindow::addInspectorPage(const InspectorPage &page)
{
    if (!m_inspectorSplit) {
        // The first page wraps the window's content in a split view with the
        // inspector on the right. Its toggle floats in the content's top
        // right corner rather than joining the header bar, which is already
        // as wide as the default 1100 px window allows (one more button
        // there widened the whole window, 2026-09-25).
        registerHints({{"inspector.toggle", "Inspector", "Show or hide the inspector",
                        "Pages that drop-ins add: effects, titles, …", nullptr, nullptr}});
        m_inspectorSplit = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
        adw_overlay_split_view_set_sidebar_position(m_inspectorSplit, GTK_PACK_END);
        // Floating over the content, hidden until toggled: side by side it
        // needs the content's minimum (about 885 px) plus its own, wider
        // than the default window, and docking it only when there's room
        // takes an AdwBreakpoint, which drops the window's content-based
        // minimum size for everyone -- a decision of its own (doc 15's
        // REVIEW note), not part of adding the host.
        adw_overlay_split_view_set_collapsed(m_inspectorSplit, TRUE);
        adw_overlay_split_view_set_show_sidebar(m_inspectorSplit, FALSE);
        adw_overlay_split_view_set_min_sidebar_width(m_inspectorSplit, 240);
        adw_overlay_split_view_set_max_sidebar_width(m_inspectorSplit, 360);

        GtkWidget *content = gtk_overlay_new();
        g_object_ref(m_mainPaned);
        adw_toolbar_view_set_content(m_toolbarView, GTK_WIDGET(m_inspectorSplit));
        gtk_overlay_set_child(GTK_OVERLAY(content), m_mainPaned);
        g_object_unref(m_mainPaned);
        adw_overlay_split_view_set_content(m_inspectorSplit, content);

        GtkWidget *toggle = gtk_toggle_button_new();
        gtk_button_set_icon_name(GTK_BUTTON(toggle), "sidebar-show-right-symbolic");
        gtk_widget_add_css_class(toggle, "osd");
        gtk_widget_add_css_class(toggle, "circular");
        gtk_widget_set_halign(toggle, GTK_ALIGN_END);
        gtk_widget_set_valign(toggle, GTK_ALIGN_START);
        gtk_widget_set_margin_top(toggle, 8);
        gtk_widget_set_margin_end(toggle, 8);
        gtk_accessible_update_property(GTK_ACCESSIBLE(toggle), GTK_ACCESSIBLE_PROPERTY_LABEL, "Inspector", -1);
        setTooltip(toggle, "inspector.toggle");
        g_object_bind_property(m_inspectorSplit, "show-sidebar", toggle, "active",
                               static_cast<GBindingFlags>(G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE));
        gtk_overlay_add_overlay(GTK_OVERLAY(content), toggle);

        GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_add_css_class(sidebar, "inspector");
        m_inspectorStack = ADW_VIEW_STACK(adw_view_stack_new());
        gtk_widget_set_vexpand(GTK_WIDGET(m_inspectorStack), TRUE);
        GtkWidget *switcher = adw_view_switcher_new();
        adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_SWITCHER_POLICY_NARROW);
        adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher), m_inspectorStack);
        gtk_box_append(GTK_BOX(sidebar), switcher);
        gtk_box_append(GTK_BOX(sidebar), GTK_WIDGET(m_inspectorStack));
        adw_overlay_split_view_set_sidebar(m_inspectorSplit, sidebar);
    }
    adw_view_stack_add_titled_with_icon(m_inspectorStack, page.widget, page.id, page.title, page.iconName);
}

void AppWindow::addActions(const std::vector<ActionSpec> &specs, gpointer target)
{
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(m_window));
    for (const ContributedAction &action : contributeActions(specs, target)) {
        GSimpleAction *simple = g_simple_action_new(action.spec.name, nullptr);
        g_signal_connect(simple, "activate", G_CALLBACK(action.spec.activated), action.target);
        g_action_map_add_action(G_ACTION_MAP(m_window), G_ACTION(simple));
        g_object_unref(simple);
        if (app)
            setAccelsForAction(app, action.spec.name, action.spec.accels);
        // A shortcut with no Ctrl, Alt or Super would type in a text field.
        for (const char *accel : action.spec.accels) {
            guint key = 0;
            GdkModifierType mods{};
            if (gtk_accelerator_parse(accel, &key, &mods) &&
                !(mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK))) {
                m_typingKeyActions.push_back(action.spec.name);
                break;
            }
        }
    }
}

void AppWindow::addHints(const std::vector<HintSpec> &hints)
{
    registerHints(hints);
}

void AppWindow::addPreviewOverlay(GtkWidget *overlay)
{
    if (!m_previewOverlay) {
        // The first overlay stacks the preview picture under a GtkOverlay,
        // the same size, inside the same frame.
        m_previewOverlay = GTK_OVERLAY(gtk_overlay_new());
        g_object_ref(m_preview);
        gtk_frame_set_child(GTK_FRAME(m_previewFrame), GTK_WIDGET(m_previewOverlay));
        gtk_overlay_set_child(m_previewOverlay, GTK_WIDGET(m_preview));
        g_object_unref(m_preview);
    }
    gtk_overlay_add_overlay(m_previewOverlay, overlay);
    m_previewOverlays.push_back(overlay);
}

PreviewMapping AppWindow::previewMapping() const
{
    const core::Profile &profile = m_model.sequence().profile;
    // The shown image's own aspect (a scaled preview keeps it); the
    // profile's until the first frame arrives.
    double aspect = displayAspectOf(profile);
    if (GdkPaintable *paintable = gtk_picture_get_paintable(m_preview)) {
        const double own = gdk_paintable_get_intrinsic_aspect_ratio(paintable);
        if (own > 0.0)
            aspect = own;
    }
    return mapPreview(gtk_widget_get_width(GTK_WIDGET(m_preview)), gtk_widget_get_height(GTK_WIDGET(m_preview)),
                      profile.width, profile.height, aspect);
}

void AppWindow::redrawPreviewOverlays()
{
    for (GtkWidget *overlay : m_previewOverlays)
        gtk_widget_queue_draw(overlay);
}

void AppWindow::addTimelineOverlay(timeline::TimelineOverlayProvider *provider)
{
    m_timelineOverlays.push_back(provider);
    refreshTimeline(); // its lanes change the timeline's height
}

void AppWindow::redrawTimeline()
{
    refreshTimeline();
}

bool AppWindow::overlayClaimsPress(double x, double y, int nPress)
{
    if (m_timelineOverlays.empty())
        return false;
    const timeline::RowLayout layout = rowLayout();
    for (timeline::TimelineOverlayProvider *overlay : m_timelineOverlays) {
        if (overlay->pressed(m_model, m_viewport, layout, x, y, nPress)) {
            gtk_widget_queue_draw(GTK_WIDGET(m_timeline));
            return true;
        }
    }
    return false;
}

std::string AppWindow::projectFolder() const
{
    std::error_code ec;
    if (!m_currentProjectPath.empty())
        return core::utf8String(core::pathFromUtf8(m_currentProjectPath).parent_path());
    const std::string folder = m_settings->defaultProjectFolder();
    if (!folder.empty() && std::filesystem::is_directory(core::pathFromUtf8(folder), ec))
        return folder;
    return {};
}

void AppWindow::assetChangedOnDisk(core::AssetId id)
{
    if (!m_model.hasAsset(id))
        return;
    const core::Asset &asset = m_model.asset(id);
    const std::string fingerprint = core::fileFingerprint(asset.path);
    const auto status = fingerprint.empty() ? core::Asset::Status::Missing : core::Asset::Status::Ready;
    if (fingerprint == asset.fileFingerprint && status == asset.status)
        return;
    // A new fingerprint changes the bin, so the next build is a full one
    // (EngineSync::setProject()), and drop-in producers are made afresh.
    m_model.setAssetSource(id, asset.path, fingerprint, status);
    m_engine->publish(m_model.snapshot());
    refreshTimeline();
    refreshMediaBrowser();
    refreshMissingBanner();
}

void AppWindow::addImportHandler(ImportHandler handler)
{
    for (const std::string &extension : handler.extensions) {
        for (const ImportHandler &existing : m_importHandlers) {
            if (std::find(existing.extensions.begin(), existing.extensions.end(), extension) !=
                existing.extensions.end()) {
                Log::warn("[import] refused a handler for ." + extension + ": another drop-in has it");
                return;
            }
        }
    }
    m_importHandlers.push_back(std::move(handler));
}

std::vector<std::string> AppWindow::importWithHandlers(std::vector<std::string> paths,
                                                       std::optional<core::TrackId> trackId,
                                                       std::optional<core::FrameIndex> &position)
{
    std::vector<std::string> rest;
    for (std::string &path : paths) {
        std::string extension = core::utf8String(core::pathFromUtf8(path).extension());
        if (!extension.empty())
            extension.erase(0, 1);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto handler = std::find_if(m_importHandlers.begin(), m_importHandlers.end(), [&](const ImportHandler &h) {
            return std::find(h.extensions.begin(), h.extensions.end(), extension) != h.extensions.end();
        });
        if (extension.empty() || handler == m_importHandlers.end()) {
            rest.push_back(std::move(path));
            continue;
        }
        auto result = handler->import(path, trackId, position);
        if (!result) {
            showStatus(result.error());
            continue;
        }
        if (*result && position)
            position = **result;
        showStatus("Imported " + path);
    }
    return rest;
}

// --- GTK signal trampolines ---

gboolean AppWindow::shellSelectionIdleTrampoline(gpointer userData)
{
    auto *self = static_cast<AppWindow *>(userData);
    self->m_shellSelectionIdleId = 0;
    ShellSelection now = self->currentSelection();
    if (now != self->m_lastShellSelection) {
        self->m_lastShellSelection = std::move(now);
        self->m_shellSelectionChanged.emit();
    }
    return G_SOURCE_REMOVE;
}

} // namespace ustudio::app
