**Title:** AdwSpinRow is missing from the AT-SPI tree

## Steps to reproduce

1. Build and run the example below: an `AdwPreferencesGroup` holding an `AdwComboRow`, an `AdwSpinRow`, and an `AdwActionRow` with a `GtkSpinButton` suffix.

   ```sh
   cc spin.c -o spin $(pkg-config --cflags --libs libadwaita-1)
   GTK_A11Y=atspi ./spin
   ```
2. Look at the window's accessibility tree (Accerciser, or the AT-SPI probe beside the example: `probe.py`).

<details>
<summary>spin.c</summary>

See `spin.c` in this folder.

</details>

## Current behavior

The group's list has two children, the combo row and the action row. The `AdwSpinRow` isn't there at all, so a screen reader can't reach it and AT-SPI tools can't find or set its value.

## Expected behavior

The spin row is exposed like the others: a spin button named "SpinRow" with its value.

## Version information

libadwaita 1.9.2, GTK 4.22.4, Fedora 44, X11 (Xvfb); not checked on Wayland.

## Additional information

An `AdwActionRow` with a `GtkSpinButton` suffix (labelled with the row's title) is exposed correctly, which is what we use instead.
