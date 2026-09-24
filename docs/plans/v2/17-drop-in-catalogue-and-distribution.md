# 17 — Drop-in catalogue and distribution

**Status:** proposal, 2026-09-24. Owner direction the same day: plan every
candidate drop-in below, prioritise **audio polish** and **keying**, and
pull the rest in selectively as opportunity allows. Also: ship a core
editor with opt-in drop-ins.

Every entry follows the drop-in rules in
[doc 15, "Drop-in structure"](15-effects-and-transitions.md) and
[ADR-013](adr/013-effects-and-titles-as-drop-in-modules.md): one
self-contained folder under `drop-ins/`, reaching `src/` only through the
integration points IP1–IP6, never depending on another drop-in. How they
are built, packaged and loaded is in "Distribution" below and
[ADR-014](adr/014-drop-in-loading-and-distribution.md).

## Catalogue

Measured on the dev machine on 2026-09-24, with `frei0r-plugins` 2.5.6
installed: MLT exposes 616 filters (91 native, 333 libavfilter, 102
frei0r, 63 SoX, 8 LADSPA, 1 OpenCV), 57 transitions (49 of them frei0r
mixers) and 42 producers (12 frei0r generators). None of the 162 frei0r,
OpenCV, vid.stab, rubberband and rnnoise libraries links Qt (`ldd`); the
factory-policy test is still the runtime proof and must be rerun with
frei0r present.

| Drop-in | Priority | Built on | Installed today | New dependency | Effort |
|---|---|---|---|---|---|
| Audio polish | **1** | `loudness`, `dynamic_loudness`, `rnnoise`, SoX filters, `rbpitch`, `audiolevel` | yes | none | ~1.5 weeks |
| Keying | **2** | MLT `chroma`, avfilter `chromakey` / `colorkey`, frei0r keyers and spill filters | yes | none | ~1.5 weeks |
| Stabilise | as available | `vidstab` (vid.stab 1.1.1) | yes | none | ~1 week |
| Auto-captions | as available | whisper.cpp | no | `whisper-cpp` (ADR needed) | ~2 weeks |
| Motion tracking | as available | `opencv.tracker` | yes (OpenCV 4.13) | none | ~2 weeks, spike first |
| Speed ramps and freeze frames | as available | `timeremap` link, `freeze` | yes | none | ~2 weeks |
| Audio visualiser | as available | MLT `audiowave`; fallback: own Cairo producer | yes | none | ~1 week, spike first |
| AI generation | as available | provider HTTP APIs, local servers | no | libsoup, libsecret, json-glib (ADR-015) | see [doc 18](18-ai-generation.md) |
| Looks and LUTs | tracked in doc 15 | avfilter `lut3d`, `lift_gamma_gain` | yes | none | part of FX2 and FX5 |
| Audio plugin packs | tracked in doc 15 | LADSPA (LSP, SWH, Calf) | base `ladspa` only | runtime packages only | part of FX5 |
| OpenFX packs | tracked in doc 15 | MLT `openfx` host (experimental) | host only | runtime packages only | FX5 |
| Lottie animations | tracked in doc 16 | `rlottie` | no | `rlottie` (ADR needed) | titles T6 |

Rejected (doc 15 has the reasons): G'MIC, ML background removal, movit.

Every service name above was read from the installed MLT repository or its
YAML metadata. Parameter names are **not** listed here on purpose: each
drop-in's first task reads them from metadata (CLAUDE.md's
empirical-knowledge rule).

### Shared model need: effect ownership

Several drop-ins store their settings as ordinary `Effect` entries on clips
and tracks, which keeps project data in `src/` (IP1, IP2). So that two
drop-ins never both apply the same effect, IP1 gains `Effect::owner`: the
name of the drop-in that applies it (`"effects"`, `"audio-polish"`,
`"keying"`, …). Each drop-in's `EngineExtension` applies only its own
effects; the effects drop-in's Rack shows the others read-only with an
"Edit in …" link. An effect whose owner isn't loaded is kept in the project
and skipped at playback, with a status notice, like any missing drop-in.

### Audio polish (priority 1)

For voice-led streams and promos: clean speech and platform-ready loudness
in one click.

- **Clean up voice** on a clip or track: `rnnoise` denoise, a high-pass and
  gentle compression (SoX), in a fixed, tested order.
- **Target loudness** on the sequence: presets for streaming platforms
  (-14 LUFS), podcast (-16) and broadcast (-23), with a custom value.
  `loudness` measures in an analysis pass (a render-tool subcommand, IP6, so
  the UI never blocks); `dynamic_loudness` is the real-time alternative for
  live preview. Which one to use by default is the spike's first question.
- **Meters**: per-track and master level meters from `audiolevel`, in the
  inspector while playing.
- Integration points: IP1 (`Effect::owner`), IP3 (`decorateCut`,
  `decoratePlaylist`, `decorateTractor`), IP5 (inspector page, actions),
  IP6 (loudness analysis).
- Acceptance: a generated `noise:` + `tone:` voice-like test clip comes out
  of export within 1 LU of the chosen target; preview and export match;
  disabling the drop-in leaves the project playable and unchanged on save.

### Keying (priority 2)

Green and blue screen for streamer and product shots.

- Pick the key colour with an eyedropper on the preview (IP5 preview
  overlay), then three sliders: tolerance, edge softness, spill removal.
- **Matte view** toggle to see the alpha channel while tuning.
- Engine: one keyer chosen by the spike from `chroma`, `avfilter.chromakey`
  and the frei0r keyers, judged on edge quality and speed at 1080p; spill
  suppression from frei0r. The keyed clip goes on an upper track and
  composites over the lower tracks through the normal alpha path.
- Integration points: IP1, IP3 (`decorateCut`), IP5 (inspector page,
  preview overlay, actions).
- Acceptance: a generated green-background test clip keys to full
  transparency outside the subject within the tolerance; matte view and
  preview/export parity.

### Stabilise

- "Stabilise" on a clip runs `vidstab`'s analysis out of process (IP6),
  with progress; the result file lives in the project's cache folder and is
  referenced by a relativised path. Then smoothing and zoom sliders.
- Integration points: IP1, IP3, IP5, IP6.

### Auto-captions

- Speech to text with whisper.cpp, run as a render-tool subcommand (IP6,
  CPU-heavy, out of process). The model file is chosen by the user in
  Settings; the app never downloads it (no network in the core editor).
- Output: an `.srt` file next to the project plus captions on a caption
  track, rendered with MLT's `subtitle` filter so this drop-in doesn't
  depend on titles. The titles drop-in can import the same `.srt` (T5) for
  styled captions.
- Integration points: IP1, IP3, IP5, IP6. Needs an ADR for `whisper-cpp`.

### Motion tracking

- Draw a box on the preview, track with `opencv.tracker`, and store the
  result as keyframes on the clip (IP1 data). First consumers: a blur or
  pixelate that follows the box (privacy), and exposing the track so other
  drop-ins can bind a position to it later.
- Spike first: tracker algorithms, accuracy on real footage, and speed.
- Integration points: IP1 (track data), IP3, IP5, IP6 (analysis).

### Speed ramps and freeze frames

- Keyframed playback speed per clip through MLT's `timeremap` link, and
  freeze-frame at the playhead through `freeze`.
- Not purely additive: speed changes a clip's length relative to its source
  span, which touches the model's length rules (IP1) and every command that
  assumes `length = out - in + 1`. Needs its own design review before it
  starts. Doc 12 had this post-2.0; it moves here as an optional drop-in.

### Audio visualiser

- Waveform, bars or spectrum over a clip or as a generator clip, styled
  with brand colours, for podcast and music promos.
- Spike first: whether MLT's `audiowave` is good enough; otherwise the
  drop-in ships its own small Cairo-drawn MLT producer (the same pattern as
  titles' `ustudio_title`, but its own copy, since drop-ins don't share code).

## Distribution

Goal: ship a core editor, with drop-ins opt-in, and keep every build honest
about what it contains. Decided in
[ADR-014](adr/014-drop-in-loading-and-distribution.md).

### Two ways to build a drop-in

| Mode | Build option | How it loads | Used for |
|---|---|---|---|
| `builtin` | `-Ddropin_<name>=builtin` | Linked into the app; registered through the generated `drop_ins.h` | development, tests, drop-ins bundled with core |
| `module` | `-Ddropin_<name>=module` | Built as `libustudio-dropin-<name>.so`; found and loaded at startup | opt-in drop-ins shipped separately |
| off | `-Ddropin_<name>=disabled` | not built | the default for anything not yet past its gate |

A module exports one C function, `ustudio_drop_in_describe()`, returning its
name, version, the drop-in API version it was built against, and its
`contributeFactoryPaths()` / `registerDropIn()` entry points. The API
version is an integer in `src/` that bumps whenever `DropInHost` or an
integration point changes. A module built for a different API version, or
from a different app release, is refused with a clear message: drop-ins are
built from the same repository and released with the app, so exact
matching is simple and avoids C++ ABI surprises.

### What ships where

| Package | Contains |
|---|---|
| Core | the editor and the render tool; no drop-ins, no frei0r |
| One package per drop-in | its module, data and runtime dependencies. **Effects** carries `frei0r-plugins`; **titles** carries its MLT module and the titles app; optional drop-ins carry theirs (e.g. whisper.cpp for auto-captions) |

Owner decision (2026-09-24): effects and titles are opt-in like every
other drop-in, so frei0r is never a hard dependency of the editor
(ADR-014 narrows ADR-011). Suggested default install for most users: core
plus effects plus titles, offered together in GNOME Software, but each
removable.

- **Flatpak (reference artifact, M7).** The app declares an extension
  point, `com.ustudio.VideoEditor.DropIn`, mounted under the app's
  extensions directory with one subdirectory per drop-in, not downloaded
  automatically. Each optional drop-in is its own Flatpak extension, e.g.
  `com.ustudio.VideoEditor.DropIn.AudioPolish`, installed from GNOME
  Software or `flatpak install`. The exact manifest keys are verified
  against flatpak-builder's documentation in M7, not written from memory
  here. Flatpak permissions belong to the app, not extensions, so no
  extension can add network access; see doc 18 for how AI generation
  handles that.
- **Fedora RPM.** The core package plus subpackages named
  `u-studio-video-editor-dropin-<name>`, each requiring its own runtime
  dependencies.
- **From source.** `meson setup` with the `dropin_*` options.

### Finding, enabling and trusting drop-ins

- The app loads modules only from trusted locations: `$libdir/u-studio/drop-ins/`
  and the Flatpak extension mount. Never from a project's folder, never
  from the user's home directory. A `USTUDIO_DROPIN_PATH` override exists
  for development and logs a warning when used.
- Settings gains a **Drop-ins** page: installed drop-ins with version and
  an enable switch each (stored in GSettings, applied on next start;
  unloading native code at runtime isn't safe). Drop-ins not installed are
  listed with how to get them: a link that opens GNOME Software on the
  extension, or the install command. The app itself never downloads or
  installs anything (CLAUDE.md: no network requests).
- A project that uses a drop-in that isn't installed or enabled opens
  normally, keeps that drop-in's data unchanged on save, plays without its
  effect, and says which drop-in is missing.

### Testing

- Each drop-in's tests pass in `builtin` and `module` mode.
- The core suite passes with every optional drop-in disabled.
- A test loads a module with a wrong API version and checks it is refused.

## Order of work

1. After the post-M3 audit and the integration points (doc 12), add the
   loader and the Drop-ins settings page (ADR-014) alongside IP5.
2. Audio polish, then keying.
3. The rest as opportunity allows; each starts with its spike and, where
   listed, its dependency ADR.

## Decisions needed from the owner

1. ~~Effects and titles in core or opt-in~~: decided, opt-in (2026-09-24).
2. ADRs for `whisper-cpp` (auto-captions) and `rlottie` (Lottie), when
   those drop-ins are pulled.
3. ~~Individually or as one "extras" bundle~~: decided, **individually**,
   one Flatpak extension per drop-in (owner, 2026-09-24).
