// The items of a GtkPopoverMenu (built from a GMenu, shown by a GtkMenuButton) have an
// empty accessible name over AT-SPI. Each item is "labelled-by" its label, but that
// target's name is empty too, and the label isn't in the tree. The items do expose
// "click", which activates them.
//
// Build:  cc menuitem.c -o menuitem $(pkg-config --cflags --libs gtk4)
// Run:    ../../atspi-harness.sh probe.py ./menuitem      (or inspect it with Accerciser)
// Actual (GTK 4.22.4, X11): [menu item] name='' labelled-by=[''] actions=['click'], twice
#include <gtk/gtk.h>

static void on_item(GSimpleAction *action, GVariant *parameter, gpointer data)
{
    g_print("activated %s\n", g_action_get_name(G_ACTION(action)));
}

static void on_activate(GtkApplication *app, gpointer data)
{
    static const GActionEntry entries[] = {{"first", on_item}, {"second", on_item}};
    g_action_map_add_action_entries(G_ACTION_MAP(app), entries, G_N_ELEMENTS(entries), NULL);
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "First item", "app.first");
    g_menu_append(menu, "Second item", "app.second");

    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "menuitem");
    GtkWidget *button = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(button), "Menu");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(button), G_MENU_MODEL(menu));
    gtk_window_set_child(GTK_WINDOW(window), button);
    gtk_window_set_default_size(GTK_WINDOW(window), 400, 300);
    gtk_window_present(GTK_WINDOW(window));
    g_object_unref(menu);
}

int main(int argc, char **argv)
{
    GtkApplication *app = gtk_application_new("org.example.MenuItem", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
