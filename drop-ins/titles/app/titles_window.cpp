#include "titles_window.h"

#include "view_settings.h"

#include "core/log.h"
#include "core/media/utf8_path.h"
#include "core/brand_kit.h"
#include "core/title_xml.h"

#include <algorithm>
#include <cmath>

namespace ustudio::titles::app {

namespace {

namespace Log = core::Log;

struct ActionEntry
{
    const char *name;
    const char *accel; // nullptr: none. Never a bare key: typing must not trigger one (audit A1)
};

// Every window action. Single keys (Delete, arrows) belong to the canvas's
// own key controller, so a text field never loses a keystroke to them.
constexpr ActionEntry kActions[] = {
    {"new", "<Control>n"},
    {"open", "<Control>o"},
    {"save", "<Control>s"},
    {"save-as", "<Control><Shift>s"},
    {"undo", "<Control>z"},
    {"redo", "<Control><Shift>z"},
    {"add-text", "<Control>t"},
    {"add-rectangle", nullptr},
    {"add-rounded", nullptr},
    {"add-ellipse", nullptr},
    {"add-line", nullptr},
    {"backdrop-checkerboard", nullptr},
    {"backdrop-colour", nullptr},
    {"backdrop-image", nullptr},
    {"toggle-guides", "<Control>semicolon"},
};

std::string fileName(const std::string &path)
{
    return core::utf8String(core::pathFromUtf8(path).filename());
}

// A GFile's path as UTF-8 (GLib's filename encoding may differ, ADR-017).
std::string utf8Path(GFile *file)
{
    char *raw = g_file_get_path(file);
    if (!raw)
        return {};
    char *utf8 = g_filename_to_utf8(raw, -1, nullptr, nullptr, nullptr);
    std::string out = utf8 ? utf8 : "";
    g_free(utf8);
    g_free(raw);
    return out;
}

GFile *fileFromUtf8(const std::string &path)
{
    char *raw = g_filename_from_utf8(path.c_str(), -1, nullptr, nullptr, nullptr);
    GFile *file = g_file_new_for_path(raw ? raw : path.c_str());
    g_free(raw);
    return file;
}

// The brand kit compiled into the app (data/brand.xml).
BrandKit loadBrandKit()
{
    GBytes *bytes = g_resources_lookup_data("/com/ustudio/Titles/brand.xml", G_RESOURCE_LOOKUP_FLAGS_NONE, nullptr);
    if (!bytes)
        return {};
    gsize size = 0;
    const auto *data = static_cast<const char *>(g_bytes_get_data(bytes, &size));
    auto kit = parseBrandKit(std::string_view(data, size));
    g_bytes_unref(bytes);
    if (!kit) {
        Log::warn("[titles] the brand kit doesn't read: " + kit.error());
        return {};
    }
    return *kit;
}

GtkFileFilter *titleFilter()
{
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Titles");
    gtk_file_filter_add_suffix(filter, "ustitle");
    return filter;
}

} // namespace

TitlesWindow::TitlesWindow(GtkApplication *app, const std::string &path, const std::string &backdrop)
{
    m_window = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
    gtk_window_set_default_size(GTK_WINDOW(m_window), 1440, 900);
    buildUi();

    for (const ActionEntry &entry : kActions) {
        GSimpleAction *action = entry.name == std::string("toggle-guides")
                                    ? g_simple_action_new_stateful(entry.name, nullptr, g_variant_new_boolean(TRUE))
                                    : g_simple_action_new(entry.name, nullptr);
        g_signal_connect(action, "activate", G_CALLBACK(onAction), this);
        g_action_map_add_action(G_ACTION_MAP(m_window), G_ACTION(action));
        g_object_unref(action);
        if (entry.accel) {
            const std::string detailed = std::string("win.") + entry.name;
            const char *accels[] = {entry.accel, nullptr};
            gtk_application_set_accels_for_action(app, detailed.c_str(), accels);
        }
    }
    g_signal_connect(m_window, "close-request", G_CALLBACK(onCloseRequest), this);
    g_signal_connect(m_window, "destroy", G_CALLBACK(onDestroy), this);

    const ViewSettings view = loadViewSettings();
    m_canvas->setGuidesVisible(view.guides);
    if (view.backdrop == Backdrop::Colour)
        m_canvas->setBackdropColour(view.colour);
    if (!backdrop.empty()) {
        GFile *file = fileFromUtf8(backdrop);
        GError *error = nullptr;
        GdkTexture *texture = gdk_texture_new_from_file(file, &error);
        g_object_unref(file);
        if (texture) {
            m_canvas->setBackdropImage(texture);
            g_object_unref(texture);
        } else {
            Log::warn("[titles] backdrop " + backdrop + " doesn't load: " + (error ? error->message : "?"));
            g_clear_error(&error);
        }
    }

    if (!path.empty())
        open(path);
    refresh();
    // The canvas has focus, not the layers list (a focused row in a
    // single-selection list selects itself).
    gtk_window_set_focus(GTK_WINDOW(m_window), m_canvas->widget());
}

TitlesWindow::~TitlesWindow() = default;

void TitlesWindow::buildUi()
{
    m_canvas = std::make_unique<TitleCanvas>(TitleCanvas::Callbacks{
        [this](const std::optional<std::string> &id) { selectionChanged(id); },
        [this](const std::string &id, double x, double y, bool final) { moveLayerTo(id, x, y, final); },
        [this](const std::string &id, const Rect &box, bool final) { resizeLayer(id, box, final); },
        [this](const std::string &id) { deleteLayer(id); },
        nullptr,
    });

    GtkWidget *header = adw_header_bar_new();
    m_title = ADW_WINDOW_TITLE(adw_window_title_new("", ""));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(m_title));

    const auto button = [](const char *icon, const char *action, const char *tooltip) {
        GtkWidget *b = gtk_button_new_from_icon_name(icon);
        gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
        gtk_widget_set_tooltip_text(b, tooltip);
        return b;
    };
    adw_header_bar_pack_start(ADW_HEADER_BAR(header),
                              button("document-open-symbolic", "win.open", "Open a title (Ctrl+O)"));
    GMenu *add = g_menu_new();
    g_menu_append(add, "Text", "win.add-text");
    g_menu_append(add, "Rectangle", "win.add-rectangle");
    g_menu_append(add, "Rounded Rectangle", "win.add-rounded");
    g_menu_append(add, "Ellipse", "win.add-ellipse");
    g_menu_append(add, "Line", "win.add-line");
    GtkWidget *addButton = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(addButton), "list-add-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(addButton), G_MENU_MODEL(add));
    gtk_widget_set_tooltip_text(addButton, "Add a layer");
    g_object_unref(add);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), addButton);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), button("edit-undo-symbolic", "win.undo", "Undo (Ctrl+Z)"));
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), button("edit-redo-symbolic", "win.redo", "Redo (Ctrl+Shift+Z)"));

    GMenu *menu = g_menu_new();
    GMenu *file = g_menu_new();
    g_menu_append(file, "New Title", "win.new");
    g_menu_append(file, "Save As…", "win.save-as");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(file));
    GMenu *view = g_menu_new();
    g_menu_append(view, "Show Safe Areas", "win.toggle-guides");
    g_menu_append(view, "Checkerboard Background", "win.backdrop-checkerboard");
    g_menu_append(view, "Background Colour…", "win.backdrop-colour");
    g_menu_append(view, "Background Picture…", "win.backdrop-image");
    g_menu_append_section(menu, "View", G_MENU_MODEL(view));
    GtkWidget *menuButton = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menuButton), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menuButton), G_MENU_MODEL(menu));
    gtk_widget_set_tooltip_text(menuButton, "Main menu");
    g_object_unref(file);
    g_object_unref(view);
    g_object_unref(menu);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menuButton);
    GtkWidget *saveButton = button("document-save-symbolic", "win.save", "Save (Ctrl+S)");
    gtk_widget_add_css_class(saveButton, "suggested-action");
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), saveButton);

    m_layers = std::make_unique<LayersPanel>(LayersPanel::Callbacks{
        [this](const std::optional<std::string> &id) { m_canvas->setSelection(id); },
        [this](const std::string &id, bool visible) {
            edit(visible ? "Show Layer" : "Hide Layer",
                 [&](TitleDocument &doc) { return updateLayer(doc, id, [&](Layer &l) { l.visible = visible; }); });
        },
        [this](const std::string &id, bool locked) {
            edit(locked ? "Lock Layer" : "Unlock Layer",
                 [&](TitleDocument &doc) { return updateLayer(doc, id, [&](Layer &l) { l.locked = locked; }); });
        },
        [this](const std::string &id, size_t index) {
            edit("Restack Layer", [&](TitleDocument &doc) { return moveLayer(doc, id, index); });
        },
        [this](const std::string &id) { deleteLayer(id); },
    });
    m_inspector = std::make_unique<Inspector>(
        Inspector::Callbacks{
            [this](const std::string &label, const std::string &id, const std::function<void(Layer &)> &change,
                   const std::string &mergeKey) {
                m_editFromInspector = true;
                edit(label, [&](TitleDocument &doc) { return updateLayer(doc, id, change); }, mergeKey);
                m_editFromInspector = false;
            },
            [this](const std::string &label, const std::function<void(TitleDocument &)> &change,
                   const std::string &mergeKey) {
                m_editFromInspector = true;
                edit(
                    label,
                    [&](TitleDocument &doc) {
                        change(doc);
                        return true;
                    },
                    mergeKey);
                m_editFromInspector = false;
            },
            [this] {
                if (!edit("Apply Brand", [&](TitleDocument &doc) { return applyBrand(doc, m_inspector->kit()); }))
                    toast("Already in the brand");
            },
            [this] { m_inspector->show(m_history.document(), m_canvas->selection()); },
        },
        loadBrandKit());

    m_toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    GtkWidget *panes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(panes), m_layers->widget());
    gtk_box_append(GTK_BOX(panes), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(panes), m_canvas->widget());
    gtk_box_append(GTK_BOX(panes), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(panes), m_inspector->widget());
    adw_toast_overlay_set_child(m_toasts, panes);
    GtkWidget *view_ = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view_), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view_), GTK_WIDGET(m_toasts));
    adw_application_window_set_content(m_window, view_);
}

void TitlesWindow::selectionChanged(const std::optional<std::string> &id)
{
    const TitleDocument &doc = m_history.document();
    m_layers->show(doc, id);
    m_inspector->show(doc, id);
}

void TitlesWindow::refresh()
{
    const TitleDocument &doc = m_history.document();
    m_canvas->setDocument(doc); // may clear a selection whose layer is gone (selectionChanged)
    m_layers->show(doc, m_canvas->selection());
    if (!m_editFromInspector)
        m_inspector->show(doc, m_canvas->selection());
    const std::string name = m_path.empty() ? "Untitled Title" : fileName(m_path);
    const std::string title = (m_history.isDirty() ? "• " : "") + name;
    adw_window_title_set_title(m_title, title.c_str());
    const std::string subtitle = std::to_string(doc.width) + "×" + std::to_string(doc.height) + " · " +
                                 std::to_string(doc.fpsNum / std::max(1, doc.fpsDen)) + " fps";
    adw_window_title_set_subtitle(m_title, subtitle.c_str());
    gtk_window_set_title(GTK_WINDOW(m_window), (name + " – U Stu Titles").c_str());
    const auto enable = [this](const char *action, bool on) {
        g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(m_window), action)), on);
    };
    if (g_action_map_lookup_action(G_ACTION_MAP(m_window), "undo")) {
        enable("undo", m_history.canUndo());
        enable("redo", m_history.canRedo());
    }
}

void TitlesWindow::toast(const std::string &text)
{
    AdwToast *t = adw_toast_new(text.c_str());
    adw_toast_set_timeout(t, 4);
    adw_toast_overlay_add_toast(m_toasts, t);
    Log::info("[titles] " + text);
}

bool TitlesWindow::edit(const std::string &label, const std::function<bool(TitleDocument &)> &change,
                        const std::string &mergeKey)
{
    if (!m_history.apply(label, change, mergeKey))
        return false;
    refresh();
    return true;
}

void TitlesWindow::open(const std::string &path)
{
    auto read = readTitle(path);
    if (!read) {
        toast("Couldn't open " + fileName(path) + ": " + read.error());
        return;
    }
    m_history.reset(std::move(read->document));
    m_path = path;
    m_canvas->setSelection(std::nullopt);
    refresh();
    for (const std::string &warning : read->warnings)
        toast(warning);
}

void TitlesWindow::save(const std::string &path)
{
    const std::string error = saveTitle(m_history.document(), path);
    if (!error.empty()) {
        toast("Couldn't save: " + error);
        return;
    }
    m_path = path;
    m_history.markSaved();
    refresh();
    toast("Saved " + fileName(path));
}

void TitlesWindow::chooseSavePath()
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save Title");
    gtk_file_dialog_set_initial_name(dialog, m_path.empty() ? "Title.ustitle" : fileName(m_path).c_str());
    // The title's own folder, else Videos (else home): never wherever the
    // process happened to start.
    GFile *folder = nullptr;
    if (!m_path.empty()) {
        GFile *file = fileFromUtf8(m_path);
        folder = g_file_get_parent(file);
        g_object_unref(file);
    } else {
        const char *videos = g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS);
        folder = g_file_new_for_path(videos ? videos : g_get_home_dir());
    }
    if (folder) {
        gtk_file_dialog_set_initial_folder(dialog, folder);
        g_object_unref(folder);
    }
    GtkFileFilter *filter = titleFilter();
    gtk_file_dialog_set_default_filter(dialog, filter);
    g_object_unref(filter);
    gtk_file_dialog_save(
        dialog, GTK_WINDOW(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer self) {
            GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, nullptr);
            if (!file)
                return;
            std::string path = utf8Path(file);
            g_object_unref(file);
            if (!isTitleFile(path))
                path += ".ustitle";
            static_cast<TitlesWindow *>(self)->save(path);
        },
        this);
    g_object_unref(dialog);
}

void TitlesWindow::chooseOpenPath()
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open Title");
    GtkFileFilter *filter = titleFilter();
    gtk_file_dialog_set_default_filter(dialog, filter);
    g_object_unref(filter);
    gtk_file_dialog_open(
        dialog, GTK_WINDOW(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer self) {
            GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, nullptr);
            if (!file)
                return;
            const std::string path = utf8Path(file);
            g_object_unref(file);
            auto *window = static_cast<TitlesWindow *>(self);
            if (window->m_history.isDirty() || !window->m_path.empty()) {
                // Unsaved or another file here: its own window.
                auto *other = new TitlesWindow(gtk_window_get_application(window->window()), path, {});
                gtk_window_present(other->window());
            } else {
                window->open(path);
            }
        },
        this);
    g_object_unref(dialog);
}

void TitlesWindow::chooseBackdropColour()
{
    GtkColorDialog *dialog = gtk_color_dialog_new();
    gtk_color_dialog_set_title(dialog, "Background Colour");
    gtk_color_dialog_set_with_alpha(dialog, FALSE);
    const GdkRGBA initial = loadViewSettings().colour;
    gtk_color_dialog_choose_rgba(
        dialog, GTK_WINDOW(m_window), &initial, nullptr,
        [](GObject *source, GAsyncResult *result, gpointer self) {
            GdkRGBA *colour = gtk_color_dialog_choose_rgba_finish(GTK_COLOR_DIALOG(source), result, nullptr);
            if (!colour)
                return;
            auto *window = static_cast<TitlesWindow *>(self);
            window->m_canvas->setBackdropColour(*colour);
            ViewSettings view = loadViewSettings();
            view.backdrop = Backdrop::Colour;
            view.colour = *colour;
            saveViewSettings(view);
            gdk_rgba_free(colour);
        },
        this);
    g_object_unref(dialog);
}

void TitlesWindow::chooseBackdropImage()
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Background Picture");
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Pictures");
    for (const char *type : {"image/png", "image/jpeg", "image/webp"})
        gtk_file_filter_add_mime_type(filter, type);
    gtk_file_dialog_set_default_filter(dialog, filter);
    g_object_unref(filter);
    gtk_file_dialog_open(
        dialog, GTK_WINDOW(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer self) {
            GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, nullptr);
            if (!file)
                return;
            auto *window = static_cast<TitlesWindow *>(self);
            GError *error = nullptr;
            GdkTexture *texture = gdk_texture_new_from_file(file, &error);
            g_object_unref(file);
            if (!texture) {
                window->toast(std::string("Couldn't load that picture: ") + (error ? error->message : "?"));
                g_clear_error(&error);
                return;
            }
            window->m_canvas->setBackdropImage(texture);
            g_object_unref(texture);
        },
        this);
    g_object_unref(dialog);
}

void TitlesWindow::addLayerOf(Layer layer, const std::string &label)
{
    const std::string id = layer.id;
    if (edit(label, [&](TitleDocument &doc) { return addLayer(doc, layer); }))
        m_canvas->setSelection(id);
}

bool TitlesWindow::confirmClose()
{
    if (!m_history.isDirty() || m_closing)
        return true;
    AdwDialog *dialog = adw_alert_dialog_new("Save changes?", nullptr);
    adw_alert_dialog_format_body(ADW_ALERT_DIALOG(dialog), "“%s” has changes that aren't saved.",
                                 m_path.empty() ? "Untitled Title" : fileName(m_path).c_str());
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "_Cancel", "discard", "_Discard", "save",
                                   "_Save", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "discard", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "save", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "save");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(dialog), GTK_WIDGET(m_window), nullptr,
        [](GObject *source, GAsyncResult *result, gpointer self) {
            auto *window = static_cast<TitlesWindow *>(self);
            const std::string response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
            if (response == "cancel")
                return;
            if (response == "save") {
                if (window->m_path.empty()) {
                    window->chooseSavePath();
                    return; // closes on the next attempt, once saved
                }
                window->save(window->m_path);
                if (window->m_history.isDirty())
                    return; // the save failed; the toast says why
            }
            window->m_closing = true;
            gtk_window_close(window->window());
        },
        this);
    return false;
}

void TitlesWindow::moveLayerTo(const std::string &id, double x, double y, bool final)
{
    // By how much the layer's box moves; its position and any position
    // keyframes move with it.
    const std::vector<LayerGeometry> geometry =
        measureLayers(m_history.document(), static_cast<double>(m_history.document().timing.intro), {});
    auto it = std::find_if(geometry.begin(), geometry.end(), [&](const LayerGeometry &g) { return g.id == id; });
    if (it == geometry.end())
        return;
    const double dx = x - it->box.x, dy = y - it->box.y;
    edit(
        "Move Layer",
        [&](TitleDocument &doc) {
            return updateLayer(doc, id, [&](Layer &layer) {
                layer.x += dx;
                layer.y += dy;
                for (PropertyTrack &track : layer.animation)
                    for (TitleKey &key : track.keys) {
                        if (track.property == Property::X)
                            key.key.value += dx;
                        else if (track.property == Property::Y)
                            key.key.value += dy;
                    }
            });
        },
        "move:" + id);
    if (final)
        m_history.closeStep();
}

void TitlesWindow::resizeLayer(const std::string &id, const Rect &box, bool final)
{
    edit(
        "Resize Layer",
        [&](TitleDocument &doc) {
            return updateLayer(doc, id, [&](Layer &layer) {
                layer.x = box.x;
                layer.w = box.w;
                // A text layer's height follows its text unless it had a box.
                if (layer.kind == LayerKind::Shape || layer.h > 0.0) {
                    layer.y = box.y;
                    layer.h = box.h;
                }
            });
        },
        "resize:" + id);
    if (final)
        m_history.closeStep();
}

void TitlesWindow::deleteLayer(const std::string &id)
{
    edit("Delete Layer", [&](TitleDocument &doc) { return removeLayer(doc, id); });
}

// --- GTK trampolines ---------------------------------------------------------

gboolean TitlesWindow::onCloseRequest(GtkWindow *, gpointer self)
{
    return static_cast<TitlesWindow *>(self)->confirmClose() ? FALSE : TRUE;
}

void TitlesWindow::onDestroy(GtkWidget *, gpointer self)
{
    delete static_cast<TitlesWindow *>(self);
}

void TitlesWindow::onAction(GSimpleAction *action, GVariant *, gpointer self)
{
    auto *window = static_cast<TitlesWindow *>(self);
    const std::string name = g_action_get_name(G_ACTION(action));
    const TitleDocument &doc = window->m_history.document();
    if (name == "new") {
        auto *other = new TitlesWindow(gtk_window_get_application(window->window()), {}, {});
        gtk_window_present(other->window());
    } else if (name == "open") {
        window->chooseOpenPath();
    } else if (name == "save") {
        if (window->m_path.empty())
            window->chooseSavePath();
        else
            window->save(window->m_path);
    } else if (name == "save-as") {
        window->chooseSavePath();
    } else if (name == "undo") {
        if (window->m_history.undo())
            window->refresh();
    } else if (name == "redo") {
        if (window->m_history.redo())
            window->refresh();
    } else if (name == "add-text") {
        window->addLayerOf(makeTextLayer(doc, "Your text"), "Add Text");
    } else if (name == "add-rectangle") {
        window->addLayerOf(makeShapeLayer(doc, ShapeKind::Rect), "Add Rectangle");
    } else if (name == "add-rounded") {
        window->addLayerOf(makeShapeLayer(doc, ShapeKind::RoundedRect), "Add Rounded Rectangle");
    } else if (name == "add-ellipse") {
        window->addLayerOf(makeShapeLayer(doc, ShapeKind::Ellipse), "Add Ellipse");
    } else if (name == "add-line") {
        window->addLayerOf(makeShapeLayer(doc, ShapeKind::Line), "Add Line");
    } else if (name == "backdrop-checkerboard") {
        window->m_canvas->setBackdropImage(nullptr);
        ViewSettings view = loadViewSettings();
        view.backdrop = Backdrop::Checkerboard;
        saveViewSettings(view);
    } else if (name == "backdrop-colour") {
        window->chooseBackdropColour();
    } else if (name == "backdrop-image") {
        window->chooseBackdropImage();
    } else if (name == "toggle-guides") {
        GVariant *state = g_action_get_state(G_ACTION(action));
        const bool on = !g_variant_get_boolean(state);
        g_variant_unref(state);
        g_simple_action_set_state(action, g_variant_new_boolean(on));
        window->m_canvas->setGuidesVisible(on);
        ViewSettings view = loadViewSettings();
        view.guides = on;
        saveViewSettings(view);
    }
}

} // namespace ustudio::titles::app
