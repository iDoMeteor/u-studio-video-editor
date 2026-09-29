// With AdwOverlaySplitView collapsed and its sidebar shown, the shield laid over the
// content (so that a click outside closes the sidebar) also takes every drag-and-drop
// event: dragging from the sidebar onto a GtkDropTarget in the content never drops.
// With the sidebar docked (not collapsed), the same drag drops.
//
// Build:  cc shield.c -o shield $(pkg-config --cflags --libs libadwaita-1)
// Run:    ./shield overlay    (or ./shield docked), then drag "Drag me" onto "Drop here";
//         or: ../../atspi-harness.sh probe.py ./shield overlay   (a synthetic XTest drag)
// Actual (libadwaita 1.9.2, GTK 4.22.4, X11): docked prints "dropped: dragged text";
//         overlay prints nothing.
#include <adwaita.h>

static const char *mode = "overlay";

static GdkContentProvider *on_prepare(GtkDragSource *source, double x, double y, gpointer data)
{
    return gdk_content_provider_new_typed(G_TYPE_STRING, "dragged text");
}

static gboolean on_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer data)
{
    g_print("dropped: %s\n", g_value_get_string(value));
    return TRUE;
}

static void on_activate(GtkApplication *app, gpointer data)
{
    GtkWidget *window = adw_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "shield repro");
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 500);

    GtkWidget *source = gtk_label_new("Drag me");
    gtk_widget_set_size_request(source, 200, 200);
    GtkDragSource *drag = gtk_drag_source_new();
    g_signal_connect(drag, "prepare", G_CALLBACK(on_prepare), NULL);
    gtk_widget_add_controller(source, GTK_EVENT_CONTROLLER(drag));

    GtkWidget *target = gtk_label_new("Drop here");
    gtk_widget_set_hexpand(target, TRUE);
    gtk_widget_set_vexpand(target, TRUE);
    GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(on_drop), NULL);
    gtk_widget_add_controller(target, GTK_EVENT_CONTROLLER(drop));

    GtkWidget *split = adw_overlay_split_view_new();
    adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(split), source);
    adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(split), target);
    // "overlay": collapsed, sidebar shown over the content (the shield is up).
    // "docked":  not collapsed, sidebar beside the content (control).
    adw_overlay_split_view_set_collapsed(ADW_OVERLAY_SPLIT_VIEW(split), g_str_equal(mode, "overlay"));
    adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(split), TRUE);

    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), split);
    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv)
{
    if (argc > 1)
        mode = argv[1];
    AdwApplication *app = adw_application_new("org.example.ShieldRepro", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), 1, argv);
    g_object_unref(app);
    return status;
}
