# Titles (T0 spikes)

[Docs home](../../README.md) › [Developer docs](../README.md) › [Implementation notes](README.md) › Titles

The T0 spikes of [doc 16](../../plans/v2/16-titles-tool.md) (ADR-012), run
2026-09-27 against MLT 7.40, Pango 1.5x and GLib 2.8x (Fedora 44). Each was
a standalone repro: a minimal `ustudio_title` MLT producer module (Pango
and Cairo, about 100 lines of C++) loaded from a curated module directory.
Nothing is wired into `src/`.

| # | Question | Answer |
|---|---|---|
| 1 | A custom MLT module from the curated directory, with YAML metadata | **Yes.** The module exports `mlt_register` with `MLT_REGISTER` and `MLT_REGISTER_METADATA`; symlinked into a curated directory (every module but `qt6` and `glaxnimate-qt6`), `Mlt::Factory::init(dir)` loads it and `Repository::metadata(producer, "ustudio_title")` returns its YAML. The metadata callback reads the file itself (`mlt_properties_parse_yaml`), so the drop-in says where its YAML lives. No Qt mapped in the process. |
| 2 | Pango on the consumer thread with a private font map; 4K cost; leaks | **Yes.** A `thread_local` `pango_cairo_font_map_new()` per rendering thread (Pango's default font map isn't safe across threads). A two-layer lower third (a translucent bar and a line of text) at 3840×2160 takes **12.7 ms a frame** on a worker thread, including the conversion below. **No growth** over 10,000 frames (RSS 144,452 KB before and after). |
| 3 | Premultiplied to straight alpha into MLT `rgba` | **Yes.** Cairo's `ARGB32` is premultiplied BGRA (little-endian); one loop divides by alpha and reorders. A 60% bar of `#FC3CBA` reads back as 253/60/187 at alpha 153. |
| 4 | `ustudio_title` through MLT's `xml` | **Yes** for the `xml` consumer and producer: `mlt_service`, `text` and `font` survive and the reloaded producer renders. Through `u-studio-render`, it needs the module shipped as a drop-in (its module directory joins the curated one, IP4), which is T1's packaging. |
| 5 | `FcConfigAppFontAddDir` visible to Pango | **Yes.** A font directory added with `FcConfigAppFontAddDir(nullptr, dir)` before the first layout is used by Pango's font maps: "UStuTestFnt Bold" (a renamed DejaVu Sans copy, not installed) resolved to that family. Each process adds it itself, so the editor and `u-studio-render` both call it at startup. |
| 6 | A GApplication action invoked from a second process | **Yes.** `gapplication action com.ustudio.T0Spike open-title "'/path/x.ustitle'"` reached the running app's `open-title` action (parameter type `s`) over its session-bus name: the way a titles tool can ask the editor to open or refresh a title. No `.desktop` file or D-Bus activation is needed while the app runs. |
