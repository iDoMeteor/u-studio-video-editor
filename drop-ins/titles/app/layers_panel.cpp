#include "layers_panel.h"

#include <adwaita.h>

namespace ustudio::titles::app {

namespace {

struct Handler
{
    std::function<void()> fn;
};
void freeHandler(gpointer data, GClosure *)
{
    delete static_cast<Handler *>(data);
}
void onSignal(GObject *, gpointer data)
{
    static_cast<Handler *>(data)->fn();
}
void onEvent(gpointer object, const char *signal, std::function<void()> fn)
{
    g_signal_connect_data(object, signal, G_CALLBACK(onSignal), new Handler{std::move(fn)}, freeHandler,
                          G_CONNECT_DEFAULT);
}

// A dropped row: which layer it carries.
struct DropHandler
{
    std::function<void(const std::string &)> fn;
};
void freeDropHandler(gpointer data, GClosure *)
{
    delete static_cast<DropHandler *>(data);
}
gboolean onDrop(GtkDropTarget *, const GValue *value, double, double, gpointer data)
{
    if (!G_VALUE_HOLDS_STRING(value))
        return FALSE;
    static_cast<DropHandler *>(data)->fn(g_value_get_string(value));
    return TRUE;
}

const char *iconFor(const Layer &layer)
{
    if (layer.kind == LayerKind::Text)
        return "format-text-plain-symbolic";
    if (layer.kind == LayerKind::Image)
        return "image-x-generic-symbolic";
    switch (layer.shape) {
    case ShapeKind::Ellipse:
        return "media-record-symbolic";
    case ShapeKind::Line:
        return "list-remove-symbolic";
    default:
        return "media-playback-stop-symbolic";
    }
}

std::string labelFor(const Layer &layer)
{
    if (layer.kind == LayerKind::Text && !layer.text.empty()) {
        std::string text = layer.text.substr(0, layer.text.find('\n'));
        if (text.size() > 28)
            text = text.substr(0, 27) + "…";
        return text;
    }
    if (layer.kind == LayerKind::Image && !layer.src.empty())
        return layer.src.substr(layer.src.find_last_of("/\\") == std::string::npos ? 0
                                                                                   : layer.src.find_last_of("/\\") + 1);
    return layer.id;
}

GtkWidget *toggle(const char *onIcon, const char *offIcon, bool on, const char *tooltip)
{
    GtkWidget *button = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(button), on ? onIcon : offIcon);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), on);
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(button, tooltip);
    return button;
}

} // namespace

LayersPanel::LayersPanel(Callbacks callbacks) : m_callbacks(std::move(callbacks))
{
    m_root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_ref_sink(m_root);
    gtk_widget_set_size_request(m_root, 240, -1);
    gtk_widget_set_name(m_root, "title-layers");

    GtkWidget *heading = gtk_label_new("Layers");
    gtk_widget_add_css_class(heading, "heading");
    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    gtk_widget_set_margin_start(heading, 12);
    gtk_widget_set_margin_top(heading, 12);
    gtk_widget_set_margin_bottom(heading, 6);
    gtk_box_append(GTK_BOX(m_root), heading);

    m_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(m_list), GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(m_list, "navigation-sidebar");
    g_signal_connect(m_list, "row-selected", G_CALLBACK(onRowSelected), this);
    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), m_list);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_box_append(GTK_BOX(m_root), scroller);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(bar, 6);
    gtk_widget_set_margin_end(bar, 6);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
    const auto barButton = [&](const char *icon, const char *tooltip, std::function<void()> fn) {
        GtkWidget *button = gtk_button_new_from_icon_name(icon);
        gtk_widget_add_css_class(button, "flat");
        gtk_widget_set_tooltip_text(button, tooltip);
        onEvent(button, "clicked", std::move(fn));
        gtk_box_append(GTK_BOX(bar), button);
    };
    barButton("go-up-symbolic", "Raise the layer", [this] { restackBy(1); });
    barButton("go-down-symbolic", "Lower the layer", [this] { restackBy(-1); });
    barButton("user-trash-symbolic", "Delete the layer", [this] {
        if (m_selection && m_callbacks.remove)
            m_callbacks.remove(*m_selection);
    });
    gtk_box_append(GTK_BOX(m_root), bar);
}

LayersPanel::~LayersPanel()
{
    g_object_unref(m_root);
}

void LayersPanel::show(const TitleDocument &doc, const std::optional<std::string> &selection)
{
    m_doc = doc;
    m_selection = selection;
    m_updating = true;
    gtk_list_box_remove_all(GTK_LIST_BOX(m_list));
    // Topmost first, as they stack on the canvas.
    for (size_t i = doc.layers.size(); i-- > 0;) {
        GtkWidget *row = makeRow(doc.layers[i], i);
        gtk_list_box_append(GTK_LIST_BOX(m_list), row);
        if (selection && doc.layers[i].id == *selection)
            gtk_list_box_select_row(GTK_LIST_BOX(m_list), GTK_LIST_BOX_ROW(row));
    }
    m_updating = false;
}

GtkWidget *LayersPanel::makeRow(const Layer &layer, size_t index)
{
    GtkWidget *row = gtk_list_box_row_new();
    g_object_set_data_full(G_OBJECT(row), "layer-id", g_strdup(layer.id.c_str()), g_free);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(iconFor(layer)));
    GtkWidget *label = gtk_label_new(labelFor(layer).c_str());
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_hexpand(label, TRUE);
    if (!layer.visible)
        gtk_widget_add_css_class(label, "dim-label");
    gtk_box_append(GTK_BOX(box), label);

    const std::string id = layer.id;
    GtkWidget *eye = toggle("view-reveal-symbolic", "view-conceal-symbolic", layer.visible, "Show or hide");
    onEvent(eye, "toggled", [this, eye, id] {
        if (!m_updating && m_callbacks.setVisible)
            m_callbacks.setVisible(id, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(eye)));
    });
    gtk_box_append(GTK_BOX(box), eye);
    GtkWidget *lock = toggle("changes-prevent-symbolic", "changes-allow-symbolic", layer.locked,
                             "Lock (can't be picked on the canvas)");
    onEvent(lock, "toggled", [this, lock, id] {
        if (!m_updating && m_callbacks.setLocked)
            m_callbacks.setLocked(id, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lock)));
    });
    gtk_box_append(GTK_BOX(box), lock);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);

    // Drag a row onto another to take its place in the stack.
    GtkDragSource *source = gtk_drag_source_new();
    gtk_drag_source_set_actions(source, GDK_ACTION_MOVE);
    GValue value = G_VALUE_INIT;
    g_value_init(&value, G_TYPE_STRING);
    g_value_set_string(&value, id.c_str());
    GdkContentProvider *content = gdk_content_provider_new_for_value(&value);
    g_value_unset(&value);
    gtk_drag_source_set_content(source, content);
    g_object_unref(content);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(source));
    GtkDropTarget *target = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_MOVE);
    g_signal_connect_data(target, "drop", G_CALLBACK(onDrop),
                          new DropHandler{[this, index](const std::string &dropped) {
                              if (m_callbacks.restack)
                                  m_callbacks.restack(dropped, index);
                          }},
                          freeDropHandler, G_CONNECT_DEFAULT);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(target));
    return row;
}

void LayersPanel::restackBy(int delta)
{
    if (!m_selection || !m_callbacks.restack)
        return;
    for (size_t i = 0; i < m_doc.layers.size(); ++i) {
        if (m_doc.layers[i].id != *m_selection)
            continue;
        const auto target = static_cast<long long>(i) + delta;
        if (target >= 0 && target < static_cast<long long>(m_doc.layers.size()))
            m_callbacks.restack(*m_selection, static_cast<size_t>(target));
        return;
    }
}

// --- GTK trampolines ---------------------------------------------------------

void LayersPanel::onRowSelected(GtkListBox *, GtkListBoxRow *row, gpointer self)
{
    auto *panel = static_cast<LayersPanel *>(self);
    if (panel->m_updating || !panel->m_callbacks.selected)
        return;
    const char *id = row ? static_cast<const char *>(g_object_get_data(G_OBJECT(row), "layer-id")) : nullptr;
    panel->m_callbacks.selected(id ? std::optional<std::string>(id) : std::nullopt);
}

} // namespace ustudio::titles::app
