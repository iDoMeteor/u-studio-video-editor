#pragma once

// What a part of the designer's window calls in its destructor. The window's
// C++ object is deleted in its "destroy" handler, before GTK disposes the
// widgets, and dispose still emits signals: a GtkListBox's remove_all emits
// row-selected (a demo crash, 2026-09-29), gestures end their drags, focus
// controllers see focus leave. So each part disconnects every handler that
// would call into it first.

#include <gtk/gtk.h>

#include <initializer_list>

namespace ustudio::titles::app {

// Disconnects, on `widget`, its event controllers and every widget under it,
// each handler whose user data is `data` or whose function is one of
// `functions` (for handlers that carry their own data, like a closure).
inline void disconnectFromTree(GtkWidget *widget, gpointer data, std::initializer_list<GCallback> functions = {})
{
    if (!widget)
        return;
    const auto disconnect = [&](gpointer object) {
        g_signal_handlers_disconnect_matched(object, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, data);
        for (GCallback function : functions)
            g_signal_handlers_disconnect_matched(object, G_SIGNAL_MATCH_FUNC, 0, 0, nullptr,
                                                 reinterpret_cast<gpointer>(function), nullptr);
    };
    disconnect(widget);
    GListModel *controllers = gtk_widget_observe_controllers(widget);
    for (guint i = 0, n = g_list_model_get_n_items(controllers); i < n; ++i) {
        gpointer controller = g_list_model_get_item(controllers, i);
        disconnect(controller);
        g_object_unref(controller);
    }
    g_object_unref(controllers);
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
        disconnectFromTree(child, data, functions);
}

} // namespace ustudio::titles::app
