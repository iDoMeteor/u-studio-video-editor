// An activatable GtkListBoxRow exposes no AT-SPI action, so an AT-SPI client can't
// activate it (row-activated). GtkExpander and GtkButton, next to it, expose
// "activate" and "click". libadwaita's AdwActionRow (activatable) and AdwExpanderRow
// are GtkListBoxRows too, so their activation and expansion are unreachable the same way.
//
// Build:  cc listrow.c -o listrow $(pkg-config --cflags --libs gtk4)
// Run:    ../../atspi-harness.sh probe.py ./listrow      (or inspect it with Accerciser)
// Actual (GTK 4.22.4, X11): the row is [list item] with actions=[]
#include <gtk/gtk.h>

static void on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
    g_print("row-activated\n");
}

static void on_activate(GtkApplication *app, gpointer data)
{
    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "listrow");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);

    GtkWidget *list = gtk_list_box_new();
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), gtk_label_new("Activatable row"));
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    gtk_list_box_append(GTK_LIST_BOX(list), row);
    g_signal_connect(list, "row-activated", G_CALLBACK(on_row_activated), NULL);
    gtk_box_append(GTK_BOX(box), list);

    GtkWidget *expander = gtk_expander_new("GtkExpander");
    gtk_expander_set_child(GTK_EXPANDER(expander), gtk_label_new("inside"));
    gtk_box_append(GTK_BOX(box), expander);
    gtk_box_append(GTK_BOX(box), gtk_button_new_with_label("GtkButton"));

    gtk_window_set_child(GTK_WINDOW(window), box);
    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv)
{
    GtkApplication *app = gtk_application_new("org.example.ListRow", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
