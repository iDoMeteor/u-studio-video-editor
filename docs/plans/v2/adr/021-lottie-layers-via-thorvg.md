# ADR-021: Lottie animations are a titles layer, drawn by ThorVG inside the titles drop-in

**Status:** Accepted (owner, 2026-09-28, through the VE Strategist: "the
owner said YES to T6 Lottie"; the Strategist approved ThorVG over rlottie
the same day). Extends ADR-012 (titles are an MLT module) and ADR-013
(drop-ins); follows ADR-016 (threads), ADR-017 (Windows) and ADR-020
(template packs).

## Context

Doc 16 has carried T6 since the titles plan: import Lottie animations
(Bodymovin JSON, exported from After Effects, Figma plug-ins and
LottieFiles) as layers of a title, so an animated logo sting, emoji or
icon can sit in a lower third without a video file with alpha. Doc 17
named `rlottie` as the likely library and said it needed an ADR.

Lottie files come from strangers (LottieFiles, template packs, clients),
so they're untrusted input, and they're JSON, which the titles drop-in
doesn't parse today.

What we found before deciding (2026-09-28, from the Fedora 44 packages and
the libraries' sources):

| | rlottie | ThorVG |
|---|---|---|
| Version, activity | 0.2, the last release (2020) | 1.0.6 (2026), active |
| In Fedora | `rlottie-devel` (main repo) | `thorvg-devel` (updates) |
| Licence | MIT AND FTL AND BSD-3-Clause AND MPL (one MPL file; FTL asks for a credit) | MIT |
| API | C, render a frame number into ARGB32 | C (`thorvg_capi.h`), `tvg_animation_set_frame()` with fractional frames, software canvas into premultiplied `TVG_COLORSPACE_ARGB8888` (Cairo's ARGB32) |
| Lottie coverage | no text layers, no expressions | text layers; expressions through a bundled JavaScript engine when built with `extra=lottie_exp` |
| Images in a file | `data:` URIs decoded by a bundled stb_image; any other path read from disk | `data:` URIs, or files when built with `file=true` |
| Fedora's build | `-Dcache=false -Dmodule=false` | `-Dloaders=all -Dbindings=capi`, `extra` left at its default, so **expressions are on** |

## Decision

1. **A Lottie animation is a layer kind of a title** (`kind="lottie"`),
   beside text, shape and image. It lives in the titles drop-in only:
   `drop-ins/titles/core/` validates the file (std only), and
   `drop-ins/titles/render/` draws it with **ThorVG** through its C API.
   ThorVG is linked by the titles render library, and so by
   `u-studio-titles` and the titles MLT module; `src/` never includes or
   links it (ADR-013). ThorVG is optional at build time: without it, a
   Lottie layer draws as nothing and the title warns, as with any layer a
   version can't draw.
2. **Why ThorVG, not rlottie:**
   - It's maintained, and rlottie hasn't had a release since 2020. A
     library that parses untrusted files needs its fixes to keep coming.
   - Its licence is MIT alone.
   - It covers more of Lottie (text layers, more shape features), and its
     fractional frames suit exact timing.
   - Its C API renders premultiplied ARGB straight into our buffers.

   rlottie's one advantage, no expression engine at all, we get from
   ThorVG by building it ourselves without one (decision 8) and by
   refusing expressions before it sees a file (decision 3).
3. **Our own validator sees every Lottie file first.** It refuses the
   whole file, with a reason the user can read, when the file:
   - is over 8 MB, nests deeper than 64, or has over 1,000,000 values;
   - lacks a sane header: `v`; `fr` from 1 to 120; `ip < op`; at most 10
     minutes long; `w` and `h` from 1 to 8192;
   - has over 1,000 layers or 256 assets, precompositions included, or a
     precomposition that refers to itself;
   - **uses an expression** (a string `x` on an animated property), so no
     script ever reaches ThorVG's engine even where it's built with one;
   - **names an external asset**: every image must be an embedded
     `data:image/png` or `data:image/jpeg` base64 URI whose decoded bytes
     are at most 16 MB, sniffed as that type, and at most 8192 × 8192 by
     their header. Fonts must be embedded or left to the system: a font
     path or URL is refused. ThorVG gets the data with an empty resource
     path, so there's nothing to resolve against anyway.

   Nothing is fetched and nothing is executed. The validator is a small
   std-only JSON scanner in `core/`, not a general parser. The tests
   throw a corpus of hostile files at it: deep nesting, huge numbers,
   bad UTF-8, external image and font paths, `..`, expressions,
   self-referencing precompositions and oversized pictures.
4. **Where the file lives.** The layer's `src` is the `.json` file,
   copied beside the title like pictures (`<title name> images/`) when
   added, and so moved, packed and templated like one. dotLottie
   (`.lottie`, a zip) and Telegram's `.tgs` (gzip) aren't read in T6.
5. **Timing is frame-exact and deterministic.** At title frame `t`, a
   title rate `num/den`, a layer `speed` `s` (0.25 to 4, stored as a
   decimal and read as an exact fraction), and the animation's `ip`, `op`
   and `fr`, the frame shown is `ip + t · den · s · fr / num`, worked out
   in exact integer arithmetic and passed to `tvg_animation_set_frame()`
   as the nearest float. With `loop="loop"` it wraps within `[ip, op)`;
   with `loop="once"` it holds the last frame after the end. The same
   inputs give the same frame on every thread and in every process, so
   the editor's producer, the designer, the bake and the render tool draw
   the same pixels. That's tested byte for byte.
6. **Threads.** ThorVG is initialised once per process with
   `tvg_engine_init(0)`: no worker threads of its own, since titlerender
   already runs on its own worker threads (ADR-016). Each thread keeps its
   own canvases and animations in a small per-thread cache (at most 8,
   keyed by path, file fingerprint and render size), so no ThorVG object
   is used by two threads. A standalone repro confirms that separate
   canvases on separate threads are safe before this is relied on (per
   CLAUDE.md). If they aren't, calls go through one titles-drop-in mutex.
7. **Size, fit, colour.** The layer has a box like an image layer and
   the same `fit` choices. The animation is rendered at the box's device
   pixel size (`tvg_picture_set_size`), so it stays sharp at any scale.
   Its pixels are premultiplied sRGB, composited like an image layer,
   with the layer's opacity, transform, reveal, blur and shadow.
8. **Packaging.** ThorVG isn't in the GNOME Flatpak runtime, so the
   manifest gets a module that builds 1.0.6 from its release tarball
   with meson and
   `-Dengines=cpu -Dloaders=lottie,png,jpg,ttf -Dextra= -Dfile=false
   -Dbindings=capi -Dtools= -Dtests=false -Dthreads=false`.
   That leaves out:
   - **expressions** (no JavaScript engine);
   - **file access**: we always load from memory, so the shipped library
     can't open a file even if the validator missed something;
   - OpenMP, and ThorVG's own threads.

   VE Installers own the manifest. The package ships ThorVG's `LICENSE`.
   Fedora dev builds link the system `thorvg-devel`, which has
   expressions and file access built in, and rely on decision 3 there.
   On Windows (ADR-017), ThorVG builds with meson and MSVC or MinGW.
9. **Format version.** `.ustitle` goes to format version 2, written only
   when a title has a Lottie layer, so every other title stays readable
   by older versions. An older version refuses a version-2 file with its
   existing "saved by a newer version" message rather than silently
   losing the layer.
10. **Template packs** (ADR-020) may carry Lottie files as
    `lottie/<name>.json`. They're sniffed as JSON objects with `v`, `fr`,
    `ip`, `op` and `layers`, listed in `pack.xml` with a SHA-256 like any
    file, and validated by decision 3 before anything is installed.

## Consequences

- One more library in the titles drop-in's closure, optional at build
  time.
- Lottie files that use expressions are refused. Most stickers and logo
  stings don't use them; files exported with expressions need them baked
  into keyframes first (Bodymovin can). The refusal message says so.
- ThorVG decodes embedded PNG and JPEG pictures from untrusted files. We
  bound their size and dimensions before ThorVG sees them, and keep
  ThorVG current through the Flatpak module.
- A title with a Lottie layer can't be opened by versions before T6.
  That's by design (decision 9).

## Alternatives considered

- **rlottie** (the first candidate): see decision 2. Unmaintained since
  2020, and under more licences.
- **Pre-rendering to a video with alpha on import**: loses resolution
  independence and editability, and bloats projects. The bake (T2e)
  already gives a video when one is wanted.
- **Skottie (Skia)**: brings Skia, far too large for one layer kind.
