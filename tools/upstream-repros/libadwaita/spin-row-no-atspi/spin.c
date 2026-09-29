// An AdwSpinRow isn't in the AT-SPI tree at all: its AdwPreferencesGroup's
// list reports the rows around it, but not it, so a screen reader or an
// AT-SPI script can't find or change the value. An AdwComboRow beside it
// is there, and so is an AdwActionRow with a GtkSpinButton suffix.
//
// Build:  cc spin.c -o spin $(pkg-config --cflags --libs libadwaita-1)
// Run:    ../../atspi-harness.sh probe.py ./spin
// Expected: the list's three rows: "ComboRow", a spin button "SpinRow",
//           and "ActionRow" with its spin button.
// Actual (libadwaita 1.9.2, GTK 4.22.4, X11): the list has two children,
//           "ComboRow" and "ActionRow"; "SpinRow" is missing.
#include <adwaita.h>

static void activate(GtkApplication *app, gpointer data)
{
    GtkWidget *window = adw_application_window_new(app);
    GtkWidget *group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), "Group");

    GtkStringList *items = gtk_string_list_new((const char *[]){"One", "Two", NULL});
    GtkWidget *combo = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(combo), "ComboRow");
    adw_combo_row_set_model(ADW_COMBO_ROW(combo), G_LIST_MODEL(items));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), combo);

    GtkWidget *spin = adw_spin_row_new_with_range(0, 10, 1);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(spin), "SpinRow");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), spin);

    GtkWidget *action = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(action), "ActionRow");
    GtkWidget *button = gtk_spin_button_new_with_range(0, 10, 1);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, "ActionRow", -1);
    adw_action_row_add_suffix(ADW_ACTION_ROW(action), button);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), action);

    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), group);
    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv)
{
    AdwApplication *app = adw_application_new("org.example.SpinRowA11y", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    return g_application_run(G_APPLICATION(app), argc, argv);
}
