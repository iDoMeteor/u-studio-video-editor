# 16 — Titles: a separate text design and animation tool

**Status:** proposal, 2026-09-23. Owner request the same day: an easy,
robust text editing and animation tool, separate from the editor, tackled
as soon as possible. Effects and transitions are planned in
[doc 15](15-effects-and-transitions.md); this doc shares its easing model
and its rule that only reusable, mature libraries do the heavy lifting.

## Why a separate tool, and why not an existing MLT text service

| Option | Verdict |
|---|---|
| `kdenlivetitle`, `qtext` producers | Rejected: Qt (ADR-007). |
| `glaxnimate` producer | Rejected: Qt. |
| MLT `text` / `dynamictext` filters, `pango` producer | Present and Qt-free, but static styling (one font, outline, background box) and no per-character animation. Good enough for burn-in timecode, not for a title designer. |
| `avfilter.drawtext` | Present; per-frame expressions exist but styling is limited and authoring it is not something to expose to users. |
| Our own renderer with Pango + Cairo, exposed as an MLT producer | **Chosen** ([ADR-012](adr/012-titles-mlt-module.md)). Pango does shaping, bidi, font fallback and line breaking; Cairo does paths, strokes and gradients. Both are mature and already in the process via GTK. We write layout of layers, animation evaluation and a thin producer. |

A separate application (not a dialog inside the editor) because:

- title design is a different activity with its own canvas, inspector and
  timeline, and would crowd the editor's window;
- a separate process isolates the editor from font and text-shaping
  crashes and lets the tool be used on its own for thumbnails and stills;
- it can be launched from the editor on a specific file and report back
  over GApplication's D-Bus actions, with no new dependency.

Working name: **u Studio Titles**, executable `u-studio-titles`, app id
`com.ustudio.Titles`.

## Architecture

All of it lives in `drop-ins/titles/` (see "Drop-in structure" below):

```
core/        TitleDocument model, .ustitle XML reader/writer, animation
             evaluator. std + libxml2 only (doc 02's core rules).
render/      libustudio-titlerender: draws a TitleDocument at time t into an
             RGBA buffer. Pango, PangoCairo, Cairo, fontconfig. No GTK, no MLT.
mltmodule/   libmltustudio.so: MLT module with one producer, `ustudio_title`,
             wrapping render/. MLT framework C API only. Linked into the
             curated module directory by FactoryPolicy (IP4).
app/         u-studio-titles: GTK4 + libadwaita app. Uses core/ and render/
             directly for its canvas. No MLT.
editor/      The editor side: treats a .ustitle file as an asset played by
             the `ustudio_title` producer.
```

One renderer, three consumers: the titles app's canvas, the editor's
preview (through the MLT producer) and export (the render CLI loads the same
module). What you see while designing is byte-for-byte what exports.

Dependencies: Pango, PangoCairo, Cairo and fontconfig are already loaded
via GTK4, but linking them into non-GTK layers is new, so ADR-012 records
it.

## Drop-in structure

Titles follow the same drop-in rules as effects
([ADR-013](adr/013-effects-and-titles-as-drop-in-modules.md); the
integration points IP1–IP6 are defined in
[doc 15](15-effects-and-transitions.md), "Drop-in structure"). The titles
*app* is already separate by design; this section is about the editor side.

```
drop-ins/titles/
  meson.build  README.md  register.{h,cpp}
  core/        TitleDocument, .ustitle XML, evaluator
  render/      titlerender (Pango/Cairo)
  mltmodule/   libmltustudio.so, `ustudio_title`
  app/         u-studio-titles executable
  editor/      .ustitle import handler, file watch, fields page,
               New Title / Bake title actions
  data/        templates/, brand.json
  tests/       core/, render/, engine/, editor/
```

The whole folder builds only when `dropin_titles` isn't `disabled` (ADR-014), and follows the same
self-containment rules as effects (dependencies point from `drop-ins/`
into `src/`, never back; deleting the folder removes the feature).

| IP | Titles use |
|---|---|
| IP1 | `Clip::sourceParams` (per-clip field values); a title asset is an ordinary `Asset` whose path ends in `.ustitle` |
| IP2 | `ustudio:field.*` on clip entries; the `ustudio_title` producer written like any other resource |
| IP3 | `makeProducer()` returns a per-clip `ustudio_title` producer with `length` and `field.*` set |
| IP4 | `libmltustudio.so` linked into the curated module directory |
| IP5 | import handler for `.ustitle`; inspector page with the clip's fields; action contributions (New Title, Bake title) |
| IP6 | none |

Gating: with `dropin_titles=disabled`, or if the module fails to load, a title clip
plays as the black missing-media placeholder with a status notice, and its
data (asset, fields, timing) still round-trips unchanged.

Sequencing matches effects: `core/`, `render/`, `mltmodule/` and `app/`
are all new files inside `drop-ins/titles/` and can be built and tested now
(T0–T3 need no integration point). Editor-side integration (T1's editor
import, T4) waits for the integration points that land after the post-M3
audit.

## The document: `.ustitle`

XML via libxml2, like projects (ADR-004), versioned from 1. A title is a
small canvas with layers and a timeline split into three zones:

```xml
<ustitle version="1" width="1920" height="1080" fps="30/1">
  <timing intro="18" hold="60" outro="15" hold-mode="elastic"/>
  <field name="name" label="Name" default="Jay Doe"/>
  <field name="role" label="Role" default="Host"/>
  <layer id="bar" kind="shape" shape="rounded-rect" x="96" y="820" w="620" h="140" radius="12">
    <fill color="#1b1230" opacity="0.92"/>
    <behavior slot="in" id="wipe-right" duration="12" easing="cubic_out"/>
  </layer>
  <layer id="name" kind="text" x="128" y="838" w="560" fit="shrink">
    <text>{{name}}</text>
    <font family="Space Grotesk" weight="700" size="64" tracking="0.02"/>
    <fill gradient="linear" from="#ff3cc7" to="#19e3ff" angle="0"/>
    <shadow dx="0" dy="4" blur="12" color="#000000" opacity="0.5"/>
    <animator unit="character" order="forward" stagger="1">
      <key at="0" opacity="0" dy="24" easing="back_out"/>
      <key at="10" opacity="1" dy="0"/>
    </animator>
  </layer>
</ustitle>
```

### Elastic timing

Every title has an **intro**, a **hold** and an **outro**. On the editor's
timeline the clip can be any length: intro and outro keep their designed
timing and the hold stretches or shrinks to fit. Loop behaviours (pulse,
shimmer, float) repeat through the hold. This makes one lower third usable
for a two-second mention and a two-minute segment without re-animating
anything, and it is the main thing that makes the tool feel easy.

### Fields and dynamic text

- **Fields** (`{{name}}`, `{{role}}`) turn a title into a template. Each
  clip on the timeline stores its own field values, so one lower-third file
  serves every guest in a stream.
- **Dynamic fields** are evaluated per frame: `{{timecode}}`,
  `{{clip_time}}`, `{{countdown:mm:ss}}`, `{{date:%Y-%m-%d}}`.

### Layers and styling (v1)

| Layer | Features |
|---|---|
| Text | Pango attributes per run (family, weight, style, size, colour), tracking, line height, alignment, box with wrap or shrink-to-fit, fill (solid, linear or radial gradient), stroke, shadow, glow, background box with padding and corner radius |
| Shape | rectangle, rounded rectangle, ellipse, line; fill, stroke, gradient |
| Image | PNG (Cairo loads it natively); SVG later, since librsvg is another dependency |
| Group | transform and animate several layers together |

Cairo has no blur, so shadow and glow use a small separable box blur
(three passes approximate a Gaussian). It is the only image-processing code
the titles tool writes, and it is about forty lines.

## Animation model

- **Keyframes** on every layer property (position, scale, rotation,
  opacity, blur, tracking, colour), with the same `Easing` set doc 15 adds
  to the core model, so a "feel chip" means the same thing in both tools.
- **Text animators**, the After Effects idea that makes kinetic type
  practical: a range of units (character, word or line), an order (forward,
  reverse, centre-out, random with a stored seed), a stagger in frames, and
  a small keyframed curve of offsets (`dx`, `dy`, scale, rotation, opacity,
  blur, colour) applied to each unit on its own clock.
- **Behaviours** are named presets that generate keyframes and animators.
  They are what most users touch; "Detach to keyframes" converts one into
  plain keyframes for hand editing.

| Slot | Behaviours shipped in T3 |
|---|---|
| In | Fade, Rise, Drop, Pop (back easing), Typewriter (with cursor), Word by word, Blur in, Wipe (mask), Scramble (random glyphs resolving to the text, seeded), Split lines, Kinetic stack |
| Out | Mirrors of the In set, plus Collapse |
| Loop (hold) | Float, Pulse, Shimmer (gradient sweep), Wiggle (seeded noise), Glow breathe |

The evaluator lives in `drop-ins/titles/core/` as a pure function of the document,
time, field values and the unit count, so it is unit-tested without Pango
or a display. The renderer asks Pango for cluster positions
(`PangoLayoutIter`) and draws each unit with its own Cairo transform.

## Rendering and the MLT producer

- `ustudio_title` takes `resource` (the `.ustitle` path) and properties
  `field.<name>` for per-clip field values, and renders RGBA with alpha at
  the consumer's requested size.
- **Per-clip producers.** Today EngineSync shares one master producer per
  asset. Title clips differ per clip (field values, stretched hold), so each
  title clip gets its own producer with `length` set to the clip's length;
  the renderer maps clip frame to title time through the elastic timing.
- **Caching.** Layouts are rebuilt only when text, fields or fonts change.
  Static layers are cached as surfaces. During a hold with no loop
  behaviour, consecutive frames are identical and the previous buffer is
  returned without drawing, which is most frames of a long lower third.
- **Threading.** The producer runs on the consumer thread. Pango font maps
  and contexts must not be shared across threads, so the producer creates
  its own `PangoCairoFontMap`. T0 verifies this under the render CLI's
  threading as well.
- **Alpha.** Cairo's ARGB32 is premultiplied BGRA in native byte order; MLT's
  `rgba` is straight RGBA. The conversion is one loop, verified in T0 with
  a half-transparent test pixel.
- **Fonts.** fontconfig finds system fonts. A project may carry a `fonts/`
  folder next to the project file, registered with
  `FcConfigAppFontAddDir` in both the titles app and the producer. A missing
  font shows a warning in both tools and names the substituted family; it
  never fails silently. The Flatpak bundles the brand fonts (doc 12, M7).
- **Portability.** Stock `melt` does not know `ustudio_title`. "Bake title"
  in the editor renders a title clip to a QuickTime Animation or ProRes 4444
  file with alpha, or a PNG sequence, and swaps the clip's asset in one
  undoable command. The render CLI always loads our module, so exports never
  need baking.

## The titles app UX

Layout:

- **Canvas** in the centre, showing a **backdrop**: the editor's frame at
  the playhead when launched from the editor (passed as a PNG, so the titles
  app never needs MLT), otherwise a neutral checkerboard. Title-safe and
  action-safe guides, thirds, and snapping to guides, the canvas centre and
  other layers.
- **Layers** list on the left; drag to reorder; eye and lock toggles.
- **Inspector** on the right: style for the selected layer, with brand kit
  swatches and fonts at the top.
- **Animation strip** along the bottom: the three zones (intro, hold,
  outro) with draggable dividers, one bar per layer, behaviour chips on the
  bars. Expanding a bar shows its keyframes.

Interactions that make it easy:

1. **Type on the canvas.** Double-click text to edit it in place: an entry
   overlay sits exactly over the layer in the same font while typing; the
   canvas re-renders on commit.
2. **Drag a behaviour onto a layer.** The behaviour drawer shows each
   behaviour animating *your* text (rendered on a worker), not a generic
   sample.
3. **Loop preview.** Space plays intro, two seconds of hold, then outro, on
   repeat, so timing is judged in context without scrubbing.
4. **Brand kit.** Colours and fonts from the Unicorn Tears design tokens
   (copied into `drop-ins/titles/data/brand.json`, values only) are one click away;
   "Apply brand" restyles a whole title.
5. **Templates first.** New Title opens a gallery: lower third (one and two
   lines), name tag, "Live now" bug, chapter card, end card with call to
   action, countdown, quote card, social handle. Each is an ordinary
   `.ustitle` with fields.
6. **Shortcuts scoped to the canvas**, not application accelerators, so
   typing in any field never triggers them (audit A1, 2026-09-23).

## Editor integration

- **New Title** (header bar and `Shift+T`) opens the template gallery in the
  titles app. Saving there sends the file back through the editor's
  exported GApplication action `app.insert-title`, and the editor inserts it
  at the playhead on the active track.
- **Double-click a title clip** to open it in the titles app. The editor
  watches the file (`GFileMonitor`) and rebuilds just that asset's producers
  when it changes, reusing the missing-media reset path.
- **Fields in the editor.** Selecting a title clip shows its fields in the
  editor's Rack (doc 15), so changing a guest's name never requires opening
  the titles app. Field values are stored per clip in the model
  (`Clip::sourceParams`, a string map written as `ustudio:field.*`).
- Title clips are drawn with their own colour and a text snippet on the
  timeline, and are boundless (like stills), with the elastic hold making any
  length valid.
- Effects from doc 15 apply to title clips like any other clip, so glow,
  glitch or grain on a title is free.

## Phases

The titles track runs alongside the M3 wrap-up, M4 and the FX track.
Everything up to T3 is module-internal; only the editor-side parts need
integration points (listed per phase).

### T0 — Spikes (about 3–5 days)

Integration points: none.


- A custom MLT module loaded from the curated directory, with YAML metadata
  that `Mlt::Repository::metadata()` returns.
- Pango rendering on the consumer thread with a private font map;
  4K frame time for a two-layer lower third; no leaks across 10k frames.
- Premultiplied-to-straight alpha into MLT `rgba`.
- `ustudio_title` round-trip through MLT's `xml` producer and the render CLI.
- `FcConfigAppFontAddDir` visible to Pango in both processes.
- GApplication action invoked from a second process.

Acceptance: each item has a recorded finding and a kept repro.

### T1 — Format, renderer, producer (about 2 weeks)

Integration points: IP1, IP2, IP3 `makeProducer()`, IP4, IP5 import
handler. The format, renderer and producer can be built and tested before
those land; only "the editor imports a `.ustitle`" waits for them.


`drop-ins/titles/core` model, XML reader/writer, evaluator (keyframes only, no
animators yet); `titlerender` with text, shapes, fills, stroke, shadow;
`libmltustudio` producer; the editor imports a `.ustitle` as an asset,
with per-clip producers, elastic timing and file-watch reload.

Acceptance:

- [ ] A hand-written `.ustitle` plays in the editor and renders through
      `u-studio-render` with identical frame hashes.
- [ ] Stretching the clip changes only the hold.
- [ ] Editing the file on disk updates the editor within a second.

### T2 — The titles app (about 2–3 weeks)

Integration points: none (separate executable).


Canvas with backdrop, guides and snapping; layers; inspector; on-canvas
typing; shapes and images; brand kit; save and open; launch from the
editor on a file.

Acceptance:

- [ ] A lower third can be designed from a blank canvas without touching
      the XML.
- [ ] Nothing typed into any text field triggers a shortcut.

### T3 — Animation (about 2 weeks)

Integration points: none.


Keyframes with the shared easing set, text animators, the behaviour
library, the animation strip with elastic zones, loop preview, animated
behaviour thumbnails.

Acceptance:

- [ ] Every shipped behaviour renders identically in the titles app, the
      editor and export.
- [ ] Evaluator unit tests cover every easing and every animator order.

### T4 — Templates and editor workflow (about 1–2 weeks)

Integration points: IP5 inspector page and action contributions.


Fields and dynamic fields, the template gallery, fields in the editor's
Rack, New Title and insert-at-playhead over D-Bus, Bake title.

Acceptance:

- [ ] Ten lower thirds with different names come from one template file.
- [ ] A baked title plays in stock `melt`.

### Later

- **T5 Captions:** SRT/VTT import into title clips using a caption
  template. MLT's `subtitle` producer and filter can serve as a stop-gap
  meanwhile (T0 checks whether its internal text renderer falls back to
  `pango` when `qtext` is excluded; the library contains both names).
- **T6 Lottie:** import Lottie animations as layers via `rlottie` (not
  installed here; needs its own ADR).

## Decisions needed from the owner

1. Separate process (recommended) versus a second window inside the editor.
2. The name "u Studio Titles".
3. Whether brand fonts may be bundled in the Flatpak (licensing check on
   Space Grotesk, JetBrains Mono and Anton, all believed to be OFL).
4. Priority order between the titles track and the FX track if only one
   engineer is available (suggested: FX0 and T0 spikes first, then
   alternate).
