# Titles (T0 spikes and T1 renderer)

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

## T1: the renderer

Found while building `drop-ins/titles/render/` (2026-09-27, Pango 1.57,
Cairo 1.18, fontconfig 2.17):

- **Hinting off, or the preview isn't the export.** With Cairo's default
  font options, glyph outlines and metrics snap to the output's pixel grid,
  so a half-size preview lays text out differently from the full-size
  frame. The renderer lays out in canvas pixels with
  `CAIRO_HINT_STYLE_NONE`, `CAIRO_HINT_METRICS_OFF` and
  `pango_context_set_round_glyph_positions(FALSE)`, then scales. A
  960×540 render's ink box is the 1920×1080 one halved, to within 3 px
  (`titles-render`).
- **`FcConfigAppFontAddDir()` returns true for a directory that doesn't
  exist**, so `addFontDirectory()` checks for the directory itself. A font
  map made before the call doesn't see the new fonts. Each rendering
  thread rebuilds its font map when a directory has been added since its
  last render.
- **Which font was really used**: `pango_context_load_font()` then
  `pango_font_describe()` gives the family Pango picked. A missing
  "Space Grotesk" comes back as "Noto Sans" on Fedora 44. Generic names
  ("Sans", "monospace") never match their result and aren't warned about.
- **Cost** (release build, one thread): doc 16's lower third with a
  shadowed gradient name, a subtitle, an outlined bar and a radial dot
  takes 4.3 ms at 1920×1080 and 13.9 ms at 3840×2160. A debug (`-O0`)
  build is about four times slower, mostly in `std::vector` fills and the
  blur's loops, so don't judge speed on one.
- Text goes to Pango with `pango_layout_set_text()`, never as markup, so a
  field value like `<b>` or `&` shows as typed.

## T1: the producer in the editor's graph

- **Make the producer through `loader`, not the factory.** A producer made
  with `mlt_factory_producer(profile, "ustudio_title", path)` has no
  normalising filters, so nothing converts its `mlt_image_rgba` frames to
  the format the compositor asks for. The `composite` transition read the
  RGBA bytes as YUV 4:2:2, and a fully transparent title came out as an
  opaque green frame, (0, 136, 0), which is Y = U = V = 0 converted to RGB.
  `Mlt::Producer(profile, "loader", "ustudio_title:<path>")` splits the
  service off at the first colon (`producer_loader.c`, `create_producer()`)
  and attaches the normalisers like any media file. Repro:
  `titles-engine`, "a title clip plays over the track below it".
- **Metadata without a YAML file.** `mlt_repository_register_metadata()`'s
  callback may build the properties itself. MLT caches the result on the
  service and frees it with `mlt_properties_close`, so the module stays one
  file wherever it's installed.
- **A self-contained module.** `libmltustudio.so` links the titles core,
  the renderer and the parts of `src/core` they use statically, with
  `-Wl,--exclude-libs,ALL`, so it exports only `mlt_register`. It loads into
  the editor, `u-studio-render` or `melt` whatever they export. The drop-in
  module `libustudio-dropin-titles.so` is the opposite: it resolves core
  and engine against the program, which links those libraries whole when
  any drop-in is a module.
- **Boundless assets grow.** `InsertClip` extends a boundless asset's
  `lengthInSequenceFrames` to its furthest clip, after which it's no longer
  boundless and a longer trim is refused. Stills avoid this with
  `isStillImage`, and so do titles.
- **Watching a file saved atomically.** `g_file_monitor_file()` on the title
  reports a write-then-rename save as several events. A 150 ms settle timer
  turns them into one reload, and the fingerprint (size and mtime in ns)
  decides whether anything changed.
