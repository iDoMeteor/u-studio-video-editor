# M4 MLT findings, for the upstream list (2026-09-25)

What M4 (0.45.0 to 0.50.0) learned about MLT 7.40 the hard way, each from a
standalone repro against `~/Repos/mlt` at `v7.40.0`. The first table is
what could go upstream, as a bug or a documentation fix; the second is
behaviour we build around and only document. It is written for whoever
files upstream issues; the engine comments and the README's
implementation notes say how our code handles each one.

## Worth reporting upstream

| # | Finding | Where | Kind | Our handling |
|---|---|---|---|---|
| U1 | `composite`'s `fill` defaults to 0, though its YAML says 1: the code reads it with `mlt_properties_get_int()`, so unset is 0 and a smaller picture draws at its own size in the corner | `transition_composite.c`, `transition_composite.yml` | Metadata or code bug | `EngineSync` and the writer set `fill=1` (0.46.1) |
| U2 | The `affine` filter makes its own `colour:0` background producer per instance, and `producer_colour` caches its last image: each filter keeps a frame-sized RGBA image (8 MB at 1080p) for as long as it lives. 300 transformed cuts that have played hold 2.4 GB | `filter_affine.c:59`, `producer_colour.c:127` | Memory growth | One shared background, a reference per filter (0.47.1). Projects played in `melt` still grow |
| U3 | The `affine` filter with `use_normalized=1` returns the profile's size whatever size is asked for; a `luma` then mixing it with a frame of the asked size gets two sizes and doesn't mix | `filter_affine.c` | Surprising contract | Consumers ask for the profile's size; tests pull frames at it |
| U4 | `pixbuf` with a `%04d` pattern reports a default length (15,000) rather than the number of files, and loops after the last one; `?begin=` works only with an explicit `pixbuf:` prefix (the default loader makes the producer invalid) | `producer_pixbuf.c` | Documentation, possibly a bug | Image sequences parked (doc 07) |
| U5 | `avformat` (image2) opens a `%04d.png` sequence but returns the last picture for every frame (`seekable=0`) | `producer_avformat.c` | Bug | No fallback for sequences |

## Behaviour we build around (documented, not bugs)

| # | Finding | Our handling |
|---|---|---|
| B1 | A chain of compositors (V1→V2, V2→V3) loses an upper clip's alpha; compositing every track onto track 0 keeps it (kdenlive's arrangement) | Tracks composite onto track 0 (0.47.1) |
| B2 | The `affine` transition as a track compositor costs 21 ms a frame for one untransformed 1080p track, against `composite`'s 5 (39 against 7 for four) | `composite` stays the compositor |
| B3 | The `affine` filter costs 16–22 ms per transformed 1080p track per frame (the interpolation, already sliced across cores, plus YUV→RGBA→YUV); three transformed tracks play at about 16 frames/s at Full, 22 at Half | Auto preview scale is Half once a clip is transformed (0.47.2); doc 19 MT4 |
| B4 | The `affine` filter needs `repeat_off=1` and `mirror_off=1` (it tiles and mirrors outside `rect` otherwise) and `use_normalized=1` (otherwise the canvas is the source's size) | `core::transformFilters()` sets them |
| B5 | A frame pulled bare interpolates rotated edges differently from a consumer (`rescale=bilinear`) | Byte-for-byte tests set `consumer.rescale` |
| B6 | `avformat` opens a single PNG or JPEG with length `INT_MAX` and `seekable=0` where gdk-pixbuf can't load it (Fedora 44's glycin sandbox in a container) | The probe treats that as a still (M4 A) |
| B7 | `g` passes through `avformat` to x264 (a keyframe interval for proxies) | Proxies use `g` (0.47.0) |

## Not MLT, found on the way (GTK 4.22, libadwaita 1.9)

- Application accelerators run before the focused widget's key handlers,
  so a focused widget never sees a key an action is bound to. The preview
  lifts the plain arrows from their actions while it has focus (0.48.0).
- A `GtkListView` binds about 200 rows when it fills, whatever the row
  height; filling it in two steps made it worse (365). The media browser
  makes binds cheap instead (0.50.0).
- An `AdwExpanderRow` page's scroll position set from the adjustment's
  `changed` signal is ignored (that signal comes inside the viewport's
  allocation); an idle sets it. Closing an `AdwDialog` resets its
  scrollers to 0 on the way out (0.49.0-beta.1).
- A modal `AdwDialog` dims and covers the window, so a dialog meant to be
  used beside the preview is a small window instead (0.48.0).
