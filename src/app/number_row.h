#pragma once

#include <adwaita.h>

namespace ustudio::app {

// A number setting: an AdwActionRow titled `title` with a GtkSpinButton
// suffix labelled with the same title, returned. Not an AdwSpinRow:
// libadwaita 1.9.2 leaves that out of the AT-SPI tree, so screen readers
// and scripts can't reach it (VE Text's repro; docs/developer/notes/
// gtk-upstream.md). The `app-no-spin-row` test keeps it out of src/app.
inline GtkSpinButton *newNumberRow(const char *title, double min, double max, double step)
{
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    GtkWidget *spin = gtk_spin_button_new_with_range(min, max, step);
    gtk_widget_set_valign(spin, GTK_ALIGN_CENTER);
    gtk_accessible_update_property(GTK_ACCESSIBLE(spin), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), spin);
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), spin);
    g_object_set_data(G_OBJECT(spin), "number-row", row);
    return GTK_SPIN_BUTTON(spin);
}

// The row newNumberRow() put `spin` in: what goes into a preferences group,
// and what takes the subtitle, tooltip and prefixes.
inline GtkWidget *numberRowOf(GtkSpinButton *spin)
{
    return static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(spin), "number-row"));
}

} // namespace ustudio::app
