# 15 — Effects and transitions

**Status:** proposal, 2026-09-23. Owner directive the same day: effects and
transitions are to be tackled as soon as possible, **full frei0r
compatibility is mandatory**, the UX should be original and intuitive, and
the backend should reuse mature plugin ecosystems instead of writing
effects ourselves. This doc is the plan for that. Titles and text animation
are a separate tool with their own plan in
[doc 16](16-titles-tool.md).

This doc supersedes most of [doc 08](08-effects-and-compositing.md):

| Doc 08 section | Status after this doc |
|---|---|
| Vocabulary | Still valid; extended below with *look*, *FX lane* and *recipe* |
| Compositing without Qt or frei0r (ADR-006) | Replaced. [ADR-011](adr/011-frei0r-required-and-effect-families.md) supersedes ADR-006 |
| Effect catalogue (curated JSON only) | Replaced by the generated registry plus curated overlays below |
| Keyframes | Extended: MLT's full easing set, keyframe offset rule for split cuts |
| Transitions (dissolves) | Still valid for dissolves (already built); generalised to recipes |
| Effect panel | Replaced by the Effect Rack and Effect Browser |
| Preview-side manipulators | Kept, scheduled as phase FX4 |

## Goals

1. Every frei0r filter, mixer and generator installed on the system is
   usable on clips, tracks and the whole sequence, with a generated UI,
   keyframes and undo, and renders identically in preview and export.
2. Reuse, don't write: MLT's own modules, FFmpeg's libavfilter and frei0r
   cover colour, blur, keying, stylise, distortion, LUTs and audio.
   We write glue, metadata overlays and UI, not image processing.
3. A plugin can never take the editor down silently. Every native plugin
   is health-checked out of process before it is offered.
4. Transitions become a small, data-driven library (dissolves, wipes,
   motion, blend-mode crossfades) authored as recipes, not code.
5. The UX is built around seeing results before committing to them and
   manipulating values directly on the preview.

Non-goals for this plan: GPU *effects* (`movit` filters as user effects;
GPU compositing and transforms are ADR-019's), a node graph, writing new
image-processing effects, per-effect speed/time remapping (a clip property,
doc 08), and titles (doc 16).

## What is on this machine today

Measured 2026-09-23 against the installed MLT 7.40.0 with the Qt modules
excluded exactly as `FactoryPolicy` does (standalone program in the session
scratchpad listing `Mlt::Repository` services):

| Family | Services found | Notes |
|---|---|---|
| MLT native (core, plus, plusgpl, kdenlive, oldfilm, vid.stab, normalize, rnnoise) | 91 filters | `affine`, `luma`, `mask_start`, `mask_apply`, `shape`, `lift_gamma_gain`, `chroma`, `spot_remover`, `loudness`, `dynamic_loudness`, `vidstab`, `text`, `dynamictext`, `subtitle`, `timer`, `strobe`, `gradientmap`, `hslprimaries`, `hslrange` all present |
| FFmpeg libavfilter (`avfilter.*`) | 333 filters | includes `lut3d`, `eq`, `curves`, `colorbalance`, `chromakey`, `gblur`, `loudnorm`, `drawtext` |
| SoX (`sox.*`) | 63 filters | audio |
| LADSPA (`ladspa.*`) | 8 filters, 2 producers | only the base `ladspa` package's plugins |
| VST2 (LinuxVST) | host present | `filter_vst2.yml`, no plugins installed |
| LV2 | **not built** | no LV2 service or metadata in this MLT build |
| OpenFX 1.5 | host present, tagged *experimental* | searches `OFX_PLUGIN_PATH`, `/usr/OFX/Plugins`, `/usr/local/OFX/Plugins`; no plugins installed |
| frei0r | **module present, 0 plugins** | `frei0r-plugins` is not installed; module searches `FREI0R_PATH`, then `/usr/lib64/frei0r-1` and others |
| movit (GPU) | 17 filters, 3 transitions | needs a GL context on the consumer thread; we supply our own (EGL), ADR-019 |
| Transitions | `affine`, `composite`, `luma`, `matte`, `mix`, `movit.*` | no frei0r mixers until frei0r is installed |
| Luma wipe images | **none** | `/usr/share/mlt-7/lumas` does not exist on this install |

MLT's frei0r module ships data we rely on rather than reinvent:
`blacklist.txt` (plugins MLT refuses: `perspective`),
`not_thread_safe.txt` (19 plugins, e.g. `baltan`, `delay0r`, `nervous`),
`aliases.yaml` (renamed plugins, e.g. `frei0r.alphaspot` →
`frei0r.alpha0ps_alphaspot`), `param_name_map.yaml` (legacy parameter
names mapped to parameter indices), and `filter_cairoblend_mode.yml`
(per-clip blend mode for the `cairoblend` compositor).

Two traps found in the metadata, both of which the engine must handle
explicitly:

- `mask_apply` defaults its `transition` to **`qtblend`**, a Qt service
  excluded by ADR-007. Every use must set `transition=affine` (or
  `frei0r.cairoblend` when present).
- `mask_start` defaults its `filter` to `frei0r.alphaspot`, which does not
  exist until frei0r is installed, and has since been renamed per
  `aliases.yaml`.

## Backend: plugin families

| Family | Decision | Why |
|---|---|---|
| **frei0r** | **Required by this drop-in** (ADR-011, narrowed by ADR-014). Ships in the effects package, never in core. | Owner directive. Roughly 130 mature, GPL, CPU-only plugins (exact count confirmed in FX0) covering stylise, colour, keying, distortion, generators and blend-mode mixers. MLT already hosts it. |
| **MLT native** | Required (already are) | Zero cost; `affine`, `luma`, `mask_*`, `lift_gamma_gain`, `loudness`, `vidstab` are best-in-class for MLT. |
| **libavfilter** | Required (already present via `avformat`) | Largest library available; the only one with `.cube` LUTs (`lut3d`), proper curves and EBU R128 loudness. |
| **LADSPA** | Supported; plugin packs optional | Host is present. Real value comes from LSP, Calf, x42 or SWH packs; offer, don't require. |
| **VST2 (LinuxVST)** | Supported, hidden behind a preference | Host present; licensing of individual plugins varies. |
| **OpenFX** | Phase FX5, behind an *experimental* preference | MLT 7.40 ships a host (tagged experimental). Natron's `openfx-misc` set would add a large, mature collection, but packaging and host stability are unproven. Health scan mandatory. |
| **movit** | Compositing and transforms: ADR-019. As user effects: not planned | The consumer-thread objection is answered: our own EGL context, made current from `consumer-thread-started`, works under PlaybackController's consumer (ADR-019). Effects stay CPU inside the GPU graph. |
| **Qt modules** (`qtblend`, `qtext`, `kdenlivetitle`, `glaxnimate`) | Rejected | ADR-007. |
| G'MIC | Rejected | Its video host is Qt (`gmic-qt`) and MLT has no G'MIC module. |
| LV2 | Not possible with this MLT build | No LV2 service compiled in. Revisit if Fedora or our Flatpak MLT enables it. |
| Lottie (`rlottie`) | Considered for doc 16, not here | Animation playback belongs to the titles tool. |

### Keeping ADR-007's no-Qt guarantee

Plugins are native code dlopened into our process, so the no-Qt rule
extends to them. `FactoryPolicy` gains the same curated-directory treatment
for plugin search paths that it already applies to MLT modules:

- Build a curated `frei0r` directory of symlinks and export `FREI0R_PATH`
  pointing at it before `Mlt::Factory::init()`; likewise `OFX_PLUGIN_PATH`
  when OpenFX is enabled. Anything that links Qt, and anything the health
  scan quarantined as crashing, is simply not linked in.
- The factory-policy test extends to "load every registered frei0r and
  OpenFX service, pull one frame, assert zero `libQt` mappings". The
  `frei0r-plugins-opencv` subpackage is the one to check first (OpenCV's
  GUI module may pull in Qt, depending on how Fedora builds it; verify with
  `ldd`, don't assume).

## Architecture

```
(all paths under drop-ins/effects/, see "Drop-in structure")

data/overlays/*.json      curated names, groups, ranges, featured flags
data/transitions/*.json   transition recipes
data/lumas/               generated wipe maps (build step)
        │
engine/EffectRegistry ──── enumerates Mlt::Repository + metadata()
        │                   merges overlays, applies health results
        ▼
core/EffectDescriptor     plain structs: id, family, category,
        │                  params (kind, range, default, animatable)
        ▼
app/ Effect Browser, Effect Rack, param widgets   (no MLT, per doc 02)

render subcommand: u-studio-render --probe-effect <service>  (child process health + cost)
```

Layer placement follows doc 02 inside the drop-in: only its `engine/`
sees MLT; its `core/` holds `descriptor.h` (pure data) so its `app/` can
build UI without MLT types. The registry result is cached on disk
(`$XDG_CACHE_HOME/ustudio/effects-<mlt-version>-<plugin-set-hash>.json`) so
startup doesn't re-parse 500+ YAML documents each launch.

### Descriptor normalisation

Every family describes parameters differently. The registry maps them to one
set of kinds, each with exactly one widget:

| Source type | Kind | Widget |
|---|---|---|
| MLT `float`, frei0r `double` | `Scalar` (with display range and unit) | draggable number + slider |
| MLT `integer` | `Integer` | draggable number |
| MLT `boolean`, frei0r `bool` | `Toggle` | switch |
| MLT `color`, frei0r `color` | `Color` | swatch + eyedropper on preview |
| frei0r `position`, MLT `rect` / geometry | `Point` / `Rect` | on-preview handles (FX4) + numeric fallback |
| MLT `string` with `values:` | `Choice` | dropdown |
| MLT `string` (file) | `File` | file chooser with LUT/luma filters |
| frei0r `string`, MLT `string` | `Text` | entry |

frei0r doubles are conventionally normalised to 0–1 even when they mean
degrees or pixels. The overlay file maps them to display units, so users
see "Radius 12 px" rather than "0.0468". Without an overlay, the generic
0–1 slider is still fully functional; overlays are polish, never a
prerequisite for compatibility.

### Curated overlays

One JSON file per family, keyed by MLT service id:

```json
{
  "frei0r.glow": {
    "name": "Glow",
    "category": "Light",
    "tags": ["bloom", "soft"],
    "featured": true,
    "params": {
      "0": { "name": "Blur", "display": { "from": [0, 1], "to": [0, 100], "unit": "%" } }
    }
  }
}
```

Whether frei0r parameters are addressed by index (`"0"`) or by name is not
assumed: `param_name_map.yaml` suggests MLT 7 moved to indices, and phase
FX0 verifies it before any overlay is written.

### Health and cost probe

The editor never discovers a crashing plugin by crashing. On first launch,
and whenever the plugin set's hash changes, a background job runs
`u-studio-render --probe-effect <service>` for each service not yet probed
(bounded parallelism, low priority). The child:

1. builds a two-second `noise:` / `tone:` source at the sequence profile;
2. attaches the effect with defaults, then with each scalar at its min and
   max;
3. renders to the `null` consumer with a timeout;
4. prints JSON: `ok`, `crashed`, `timed_out`, or `bad_output` (all-black or
   NaN audio), plus milliseconds per 1080p frame.

Results land in `$XDG_CACHE_HOME/ustudio/effect-health.json`. Quarantined
effects are hidden unless "Show unstable effects" is on. Cost feeds the
UI's cost badges. This pulls a small, well-defined piece of the M6 render
CLI forward; the full CLI stays in M6.

## Drop-in structure

Effects are built as a drop-in module ([ADR-013](adr/013-effects-and-titles-as-drop-in-modules.md)):
self-contained code that plugs into the editor through a short, named list
of integration points, can be developed while M3 and M4 are in flight, and
can be switched off at build time without changing anything else. The
titles tool follows the same rules ([doc 16](16-titles-tool.md), "Drop-in
structure").

### Where the code lives

Every drop-in lives in its own folder under a top-level `drop-ins/`
directory (beside `src/`, `tests/` and `data/`), and that folder holds
everything about it: code for each layer, data, tests, build file and
documentation.

```
drop-ins/
  meson.build             one subdir() per drop-in whose build option is on
  effects/
    meson.build           its libraries, tests and data install; nothing else
    README.md             what it does, the integration points it uses, how to test it alone
    register.{h,cpp}      the single entry point: registerDropIn(DropInHost&)
    core/                 descriptors, easing helpers, looks, effect commands   (core rules)
    engine/               EffectRegistry, overlay loader, EngineExtension, probe (engine rules)
    app/                  Effect Rack, Effect Browser, parameter widgets, keyframe UI (app rules)
    data/                 overlays/, transitions/, lumas/ (generator)
    tests/                core/, engine/, app/
  titles/                 same shape, doc 16
```

Rules that keep a drop-in self-contained:

- **Dependencies point one way.** A drop-in may include headers from
  `src/`; nothing in `src/` includes anything from `drop-ins/`. The only
  link back is a file meson generates, `drop_ins.h`, listing the
  `registerDropIn()` functions of drop-ins built in, which `app/main.cpp`
  and `render/main.cpp` call at startup. Drop-ins built as loadable modules
  are found and registered at startup instead ([ADR-014](adr/014-drop-in-loading-and-distribution.md),
  [doc 17](17-drop-in-catalogue-and-distribution.md), "Distribution").
- **Deleting the folder removes the feature.** Removing
  `drop-ins/effects/` and its build option leaves a working editor that
  still loads and saves every project (IP1 and IP2 live in `src/`).
- **Doc 02's layer rules apply inside each drop-in.** Its `core/` may not
  include GTK, GLib or MLT; its `app/` may not include MLT. The meson
  boundary check extends to `drop-ins/*/app/` and `drop-ins/*/core/`.
- **Drop-ins don't depend on each other.** Anything two drop-ins need
  (the `Easing` type, for example) goes into `src/` through an integration
  point.
- **Data installs per drop-in**, to `$datadir/u-studio/drop-ins/<name>/`.
- The whole folder builds only when its option isn't `disabled`
  (`-Ddropin_effects=builtin|module|disabled`, likewise `dropin_titles`;
  ADR-014).

**As built (step 0 of the integration points, 2026-09-25):**

- `src/dropins/`: `api.h` (`DROPIN_API_VERSION`, the C
  `UStudioDropInDescription` a drop-in returns: API version, name, app
  version, description, `contributeFactoryPaths`, `registerDropIn`),
  `dropin_host.h` (`DropInHost`, pure virtual, which IP3–IP6 extend as
  they land; `FactoryPaths` for IP4) and `registry.{h,cpp}`
  (`DropInRegistry`: built-ins, modules via GModule from
  `$libdir/u-studio/drop-ins/` or the `USTUDIO_DROPIN_PATH` development
  override, refusals with reasons).
- `drop-ins/meson.build` generates `drop_ins.h` into the build tree:
  `builtinDropIns()`, listing each built-in's `ustudio_dropin_<name>_describe()`.
  A module exports `ustudio_drop_in_describe()` instead.
- `tests/dropins/`: a test-only drop-in, built in and as modules (plus
  wrong-API, wrong-app and no-describe modules that must be refused), which
  gains a consumer of each integration point as it lands; and
  `u-studio-video-editor-testdropin`, the real `main.cpp` and window with
  the test drop-in built in (never installed), linked and exported as a
  module-mode editor, so the module build loads into it too
  (`USTUDIO_DROPIN_PATH`).

> REVIEW: VE Core, 2026-09-25: the host and loader live in a new `src/dropins/` layer (core + engine + GModule, no GTK) because both the editor and the render tool load drop-ins; doc 02's table has it. The top-level `meson.build` now enters `drop-ins/` between `src/`'s libraries and `src/app` / `src/render`, so the programs can link built-ins while the program paths stay put.

> REVIEW: VE Core, 2026-09-25: a module links nothing from the app; it resolves symbols against the running program. Everything it reaches through `DropInHost` goes through the vtable, but a module that calls core or engine code directly (effects will) needs the programs built with `export_dynamic` (on whenever any drop-in is a module) and the core/engine/dropins libraries linked whole, so symbols the app itself never uses exist. That `link_whole` lands with the first real module.

### Integration points

These are the only changes to `src/` that the drop-ins need, plus the
`DropInHost` object that `registerDropIn()` receives and that exposes
IP3–IP6. They are written as general extension points, not as
effects-specific code. A new one is added to this table in the same change
that needs it.

| IP | Where | What it is | Needed from |
|---|---|---|---|
| IP1 | `core/model` | Model data and mutators: `Easing` replacing `Keyframe::Interp`; `Effect::mix`, `Effect::mask`; `Sequence::effects`; `Transition::recipe` + `params`; `AdjustmentBlock` and its lane; `Look` in the bin; `Clip::sourceParams` (titles); `EffectParamChanged` event; `Effect::owner` (which drop-in applies it, doc 17); the matching `check()` rules. Mutators stay on `Model` (doc 14); commands live in `drop-ins/effects/core/`. | FX1 |
| IP2 | `core/xml` | Writer and reader handle the IP1 fields directly (`<filter>` elements, `ustudio:*` effect and field properties); format version 5 (4 is taken by the render-graph save, doc 09). Kept in `src/` so project data never depends on a drop-in being built. | FX1 |
| IP3 | `engine/engine_extension`, `engine/engine_sync` | An `EngineExtension` interface; a drop-in registers a factory with `DropInHost::addEngineExtension()` and every `EngineSync` (the engine thread's, each render's) creates its own instances: `beginBuild()`, `decorateCut()` (every cut of a clip, including dissolve tail and head cuts, with the segment's offset for the keyframe rule; filters go on with `attachToCut()`), `decoratePlaylist()`, `decorateTractor()`, `makeTransitionSegment()` (recipe-driven sub-tractor, FX3), `makeProducer()` (per-clip producers, titles), `compositor()` (replace the default `composite` track compositor; effects uses `frei0r.cairoblend`), and `applyInPlace(ParamChange)` returning true when a parameter change was applied to live filters without a rebuild. With no extension registered, EngineSync behaves exactly as today. | FX1 (FX3 for `makeTransitionSegment`) |
| IP4 | `engine/factory_policy` | Extra plugin search paths and module directories contributed before `Mlt::Factory::init()`: the curated `FREI0R_PATH` (and `OFX_PLUGIN_PATH`), and titles' `libmltustudio.so`. Because it runs before init, each drop-in also exposes `contributeFactoryPaths()`, listed in `drop_ins.h` next to `registerDropIn()`. | FX1 |
| IP5 | `app/shell_host.h`, `app/shell_hosts.cpp` | `ShellHost`, which the editor window implements and hands to each drop-in's `ShellExtension` (`DropInHost::addShellExtension()`) once its UI is built: an **inspector host** (`addInspectorPage()`: a sidebar on the right, created by the first page); a **selection signal** (`selectionChanged` plus `currentSelection()`: clips, track, transition, adjustment block); **action contributions** (`addActions()`: `ActionSpec` lists with their own target, listed in Help; taken names refused, taken shortcuts dropped); **hint contributions** (`addHints()`, `setTooltip()`: Help's Controls tab lists them under the drop-in's category); a **preview overlay host** (`addPreviewOverlay()` plus `previewMapping()`, frame ↔ widget coordinates); a **timeline overlay/lane provider** (`TimelineOverlayProvider`: paint, `pressed()` hit-test and `laneHeight()`, for curve lanes, the FX lane and the transition shelf); an **import handler registry** (`addImportHandler()`: file extension → handler, so titles can own `.ustitle` without touching the import code); **`assetChangedOnDisk()`** (a drop-in watching its own files reports one changed: the asset's fingerprint is updated and the engine rebuilds, without an undo step; titles T1, `DROPIN_API_VERSION` 6). Also `model()`, `execute()` (through the undo stack), `projectChanged`, `currentFrame()`, `seek()` and `playheadMoved` (FX2: previous/next keyframe, animated values following the playhead; API 9), `showInspectorPage()` (FX2: E opens the Browser; API 10), `showStatus()`. With nothing registered the window is pixel-identical to before. | FX2 (timeline provider: FX4; import handlers: titles T1) |
| IP6 | `render/`, `dropins/render_subcommand.h` | `u-studio-render` dispatches subcommands registered by drop-ins (`DropInHost::addRenderSubcommand()`: a name, a one-line summary for `--help`, and `run(args, out)` returning the exit status); effects registers `--probe-effect`. With none registered the tool is the M6 placeholder it was; an unknown option exits 2. | FX1 |
| — | build | Top-level `meson.build` adds `subdir('drop-ins')`; `meson_options.txt` gains one `dropin_<name>` option per drop-in (ADR-014) and `titles`; `drop_ins.h` is generated. | FX1 |

Target size of the wiring in `app_window.cpp`: instantiate the module,
place its widgets in the hosts, register it. Under about 50 lines per
module; anything bigger is a missing host in IP5.

> REVIEW: VE Core, 2026-09-25: as built the per-module wiring in `app_window.cpp` is zero lines. A drop-in registers a `ShellExtension` from `registerDropIn()`, and the window calls it with itself as the `ShellHost` once its UI exists, so the module places its own widgets through the hosts (the test drop-in's whole shell layer is `tests/dropins/app/test_shell.cpp`, about 50 lines in `extendShell()`). `setTooltip()` is on the host too, so a module reaches the shell only through the vtable, as with `DropInHost`; the core and timeline code a module calls resolve against the program, which a module-mode editor links whole (step 0's note).

> REVIEW: VE Core, 2026-09-25: the inspector floats over the content (`AdwOverlaySplitView` collapsed, hidden until toggled; the toggle is a round button in the content's top-right corner). Docked side by side it needs the content's minimum width (about 885 px) plus its own, wider than the default 1100 px window, and the header bar has no room for another button (one more widened the whole window). Docking when there's room takes an `AdwBreakpoint`, and a window with breakpoints drops its content-based minimum size: that's for FX2 to decide with real pages, not part of adding the host. `AdwViewSwitcher` rather than `AdwInlineViewSwitcher`, so the libadwaita floor stays where it was.

> REVIEW: VE Core, 2026-09-25: the selection has no single mutation point in the shell (clicks, keys, undo pruning), but every change redraws the timeline; so after each timeline snapshot the window compares the selection with the last one posted and, when it differs, emits `selectionChanged` from an idle (never inside the draw). Only when a drop-in has a shell extension. Transitions and adjustment blocks aren't selectable in the shell yet; their fields stay empty until FX4's gestures fill them. Import handlers take their files before the media import and the rest follow them on the track; their extensions join the Import dialog's Media filter and get a filter of their own.

The timeline provider in IP5 is ideally part of M3's timeline widget (doc
06, ADR-008). If M3 closes without it, FX4 adds it as its own integration
commit.

**IP1 as built (2026-09-25):** `types.h` has `Easing` (MLT's
`mlt_keyframe_type`, value for value, pinned by `static_assert`s in
`engine_sync.cpp`), `KeyframedValue`, `EffectMask`, `Effect::mix`, `mask`,
`owner`, `Clip::sourceParams`, `Transition::recipe` and `params`,
`AdjustmentBlock`, `Sequence::effects` and `adjustmentBlocks`, and
`Project::looks`. `Model` has `EffectTarget` (clip, track, sequence,
adjustment block) and mutators for each: add, remove, move and enable an
effect; `setEffectParam` and `setEffectMix` (event `EffectParamChanged`);
masks, adjustment blocks, looks, source params, transition recipes. The
test drop-in's `core/` has the commands and IP1's property test (random
effect and clip commands, undo all and redo all equal, `check()` and
`snapshot() == project()` after every step).

> REVIEW: VE Core, 2026-09-25: keyframes are required to be sorted, not to lie inside their owner's length. A trim would otherwise have to destroy keyframes that extending the clip again should bring back, and a split's right half keeps the left part's keyframes (shifted, so negative) so interpolation into its start doesn't change. The engine (IP3) clamps when it writes animation strings.

> REVIEW: VE Core, 2026-09-25: `Transition::service` stays beside `recipe`: `recipe` empty (every existing project) means the plain dissolve played with `service`, which is what the engine and format 4 already use; FX3's recipes resolve to services in the engine. Smaller than replacing the field and migrating every reader.

> REVIEW: VE Core, 2026-09-25: effect ids are unique project-wide (`check()`), so `Model::splitClip` gives the right half's effects new ids (kept across redo by `SplitClip`) and shifts their keyframes by the split offset.

> REVIEW: VE Core, 2026-09-25: IP3 registers factories, not instances (`registerEngineExtension()`), because the live graph and each render build on different threads; per-graph instances keep their filter maps without locks. `applyInPlace` takes a `ParamChange` (the effect as it is now plus the changed parameter names) instead of a model event: the engine sees snapshots, not events (ADR-016), so `EngineSync::setProject()` diffs the two snapshots, and only when effect values or mixes are all that changed does it offer them in place; any change no extension claims rebuilds. `beginBuild()` was added so an extension can drop the previous graph's filters.

> REVIEW: VE Core, 2026-09-25: a filter on a clip's cut must carry the cut's in/out (`engine::attachToCut()`): the cut's frames carry their source position and `mlt_filter_get_position()` subtracts the filter's `in` (MLT 7.40, `mlt_filter.c`), so a filter left at in 0 on a cut of source frames 100–194 animated from source frame 0. The writer puts the same in/out on the native `<filter>` of a clip's entry.

> REVIEW: VE Core, 2026-09-25: IP6's names are the drop-in's to choose but checked by the host: lowercase letters, digits and `-`, not `help`, first registration wins (a clash is refused with a warning, as for drop-in names). The subcommand writes its result to the stream it's given (stdout in the tool), so the editor's probe job reads one JSON line from the child; the test drop-in's `--<name>-probe <service> [frames]` is that shape in miniature.

**IP4 as built (2026-09-25):** `engine::FactoryPaths` (in
`factory_policy.h`; `dropins::FactoryPaths` names it, and
`DROPIN_API_VERSION` went to 2 with the move) reaches
`FactoryPolicy(const FactoryPaths &)`: before `Mlt::Factory::init()` it
sets `FREI0R_PATH` and `OFX_PLUGIN_PATH` to exactly the contributed lists
(untouched when there are none), and links every `.so` in the contributed
module directories into the curated module directory through the same
denylist as the system's, after them, so a system module keeps its name.
The warm-up, log-level and decoder-limit order after init is unchanged.
`engine-factory-paths` checks a stand-in module is linked and registered,
a Qt-named one denied, a system name kept, and no Qt mapped;
`engine-factory-policy` is unchanged and passes.

### Not purely additive

Three changes can't be pure drop-ins and are reviewed as such:

- **IP1** changes shared model types, so it lands once, early, with its
  own property tests (random effect commands, undo all, equal).
- **IP2** bumps the project format; old files still load (doc 09's
  migration rule).
- **Adjustment blocks** (FX4) change the tractor's shape (lower tracks move
  into a sub-tractor). If `decorateTractor()` proves insufficient in the
  FX0 spike, FX4 adds a graph-level hook to IP3 rather than editing
  `rebuildAll()` ad hoc.

### Gating

| Situation | Behaviour |
|---|---|
| Built with `-Ddropin_effects=disabled` (the default until FX2's gate passes) | No effects engine or UI; IP3–IP6 are no-ops; projects containing effects load, save and round-trip unchanged (IP1 and IP2 are always built) and play without the effects, with a one-line status notice |
| Built with effects, `frei0r-plugins` missing | frei0r entries hidden, compositing falls back to `composite` (ADR-011) |
| Built with effects, a plugin quarantined | Hidden unless "Show unstable effects" |

Both configurations run the full test suite: a `just` recipe per
configuration is added with IP1. The effects tests also run on their own,
without the app.

### Sequencing around the M3 wrap-up

1. **Now, inside `drop-ins/effects/` only.** FX0 spikes (standalone
   repros), descriptors, the registry and probe logic (testable against
   `Mlt::Repository` without EngineSync), overlay and recipe data, the luma
   generator. Until the build integration point lands, the folder builds
   on its own (`meson setup` in `drop-ins/effects/`, or a scratch
   top-level option). Nothing outside `drop-ins/` changes.
2. **After the post-M3 audit.** Land IP1 and IP2, then IP4, IP3, IP6 and
   IP5, one reviewed commit each, each a no-op with the option off. These go
   before or at the very start of M4 so M4's bin and import work builds on
   them rather than colliding with them.
3. **Per phase gate.** Wire the module to its integration points behind
   the option; flip the default to on once FX2's acceptance criteria pass.

## Model changes (core)

The model already has `Effect { id, service, displayName, enabled, params }`
on clips and tracks, and `Param { name, value, keyframes }`. Changes:

| Change | Purpose |
|---|---|
| `Keyframe::Interp` → `Easing`, mirroring MLT's `mlt_keyframe_type` (discrete, linear, three smooth variants, and the sinusoidal, quadratic, cubic, quartic, quintic, exponential, circular, back, elastic, bounce families with in, out and in-out each) | MLT 7.40 interpolates all of these natively; we just pick the operator |
| `Effect::mix` (0–1, keyframable) | universal wet/dry on every effect |
| `Effect::mask` (optional shape + feather + invert, keyframable) | apply any effect through a region |
| `Sequence::effects` | master effects on the final output |
| `AdjustmentBlock { id, lane, start, length, effects, fadeIn, fadeOut }` on a new FX lane | effects applied to everything beneath for a time range |
| `Look { id, name, effects }` in the project bin (and user library on disk) | saved effect stacks |
| `Transition::recipe` + `params` (replacing the bare `service` string) | wipes, motion and blend transitions |

**Clip transform is not in this table: it is core (ADR-018, M4 F).**
`Clip::transform` (bounds mode, position, size, rotation, crop, flip, each
a `KeyframedValue`) is edited, saved and played without this drop-in, and
its preview handles are core shell. What FX adds is keyframing those same
fields, not a transform effect of its own.

New invariants for `Model::check()`: effect ids unique and below `nextId`;
keyframes sorted, inside the owner's length; adjustment blocks
non-overlapping per lane; every `Transition::recipe` resolvable (unknown
recipe = warning and dissolve fallback, never a load failure).

### Commands

All user edits are commands (doc 04). Slider drags merge.

| Command | Merge | Notes |
|---|---|---|
| `AddEffect(target, descriptor, index)` | no | target = clip, track, sequence, adjustment block |
| `RemoveEffect`, `MoveEffect` (reorder), `SetEffectEnabled` | no | |
| `SetParam(effect, param, value)` | yes, same effect + param | constant value |
| `SetKeyframe`, `RemoveKeyframe`, `MoveKeyframe`, `SetKeyframeEasing` | `SetKeyframe` yes while dragging | |
| `SetEffectMix`, `SetEffectMask` | yes | |
| `ApplyLook(targets, look)` | no | composite; one undo step for many clips |
| `PasteEffects(targets, effects, mode)` | no | mode = append or replace |
| `AddAdjustmentBlock`, `ResizeAdjustmentBlock`, `MoveAdjustmentBlock`, `RemoveAdjustmentBlock` | resize/move while dragging | |
| `SetTransitionRecipe(transition, recipe, params)` | no | |

## Engine mapping (EngineSync)

Where filters attach:

| Model owner | MLT attachment |
|---|---|
| Clip effect | `attach()` on every cut that plays that clip: its exclusive segment **and** its tail or head cut inside a dissolve sub-tractor. Without the second, an effect vanishes for the length of every dissolve. |
| Track effect | `attach()` on the track playlist (as `volume` already is) |
| Adjustment block | Tracks beneath the FX lane go into a sub-tractor; block effects attach to it with the block's in/out. Verify in FX0; fallback is master-level only. |
| Sequence (master) effect | `attach()` on the top-level tractor |

**Keyframe offset rule.** Keyframe positions are stored relative to the
clip start. When a clip is split into several cuts (exclusive segment plus
dissolve tail/head), each cut's filter gets the animation string shifted by
that cut's offset into the clip, so a fade or move stays continuous across
a dissolve. FX0 verifies that MLT positions a cut-attached filter's
animation relative to the cut's own start.

**Animation strings.** Each easing maps to MLT's animation-string operator
for that `mlt_keyframe_type`. The operator table is taken from MLT's own
`mlt_animation.c` and pinned by a test that round-trips every easing through
`Mlt::Animation`, not typed from memory.

**Mix and masks.** Implemented once for every effect, using MLT's own
`mask_start` / `mask_apply` pair: `mask_start` snapshots the frame, the
effect runs, `mask_apply` composites the result back over the snapshot with
`transition=affine` at `mix` opacity, optionally limited to a `shape` mask.
The Qt default of `mask_apply` is overridden every time.

**Blend modes.** The core editor always composites tracks with
`composite`. When the effects drop-in is loaded and frei0r is present, it
replaces the track compositor with `frei0r.cairoblend` through IP3's
`compositor()` hook, and a per-clip blend mode is the `cairoblend_mode`
filter's `mode` parameter. Values
(normal, multiply, screen, overlay, ...) are read from metadata at runtime.

**Parameter changes must not rebuild.** Today every model event rebuilds the
tractor and restarts the audio consumer (ADR-005 plus the hot-swap removal).
At 30 slider events a second that would restart the audio device 30 times
a second. A new `EffectParamChanged` event takes a fast path: EngineSync
keeps a map from `EffectId` to the live `Mlt::Filter` objects it attached
and sets the property in place under the service lock, then asks the
consumer to refresh when paused. Structural changes (add, remove, reorder,
enable) still rebuild. This is the single most important engine item in
the plan.

**Thread safety.** Preview runs the consumer with `real_time=1` (one
worker), so plugins in `not_thread_safe.txt` are safe there. Export may
later use more threads; the render CLI must consult the same list (MLT
does this for its own threading, verify in FX0).

## Persistence

Format version 5 (doc 09). Effects are written as MLT `<filter>` elements
inside the owning `<entry>`, `<playlist>` or `<tractor>`, with
`ustudio:effect_id`, `ustudio:mix`, `ustudio:mask` and keyframes in MLT's
native animation syntax, so `melt` renders effects without the editor
(ADR-004). Referenced files (LUTs, luma maps) are relativised like media
paths. Generated luma maps are referenced by a `ustudio:luma` name and
resolved from the data directory, so projects stay portable.

**IP2 as built (format 5, 2026-09-25):** `core/xml/effect_io.{h,cpp}` writes
each effect as a `<filter>`: native properties (`mlt_service`, each
parameter by name, a keyframed one as an animation string from
`core/model/animation.h`, `disable` when off) on every cut that plays the
clip, dissolve tails and heads included, and the full model record in
`ustudio:*` properties (typed values, keyframes with easing, mix, mask,
owner) on the record entry, record playlist or sequence tractor, which the
reader rebuilds from alone. Adjustment blocks and looks are never-played
playlists; source parameters and recipes are `ustudio:*` properties.
Verified: every field round-trips exactly (and saves byte-identical); a
saved animated `brightness` plays animated through MLT's own xml producer;
the format-4 fixture still loads. A project whose effects' drop-in isn't
loaded opens with a one-line notice and plays without them.

> REVIEW: VE Core, 2026-09-25: a cut's native keyframes are the owner's shifted by the cut's offset, and a keyframe outside the cut is replaced by one on its edge carrying the value interpolated linearly there (exact for linear and discrete, close for the others), because MLT reads negative positions as counted from the end. IP3 can reuse `keyframesForCut()` or compute the eased value itself.

> REVIEW: VE Core, 2026-09-25: mix and masks are stored in the model record only; the native `mask_start` / `mask_apply` pair around a filter lands with FX1, when the engine applies them, so the file and the editor's playback don't disagree in between.

Dissolves already play in `melt`: format 4 writes the same render graph
the editor plays, dissolve sub-tractors included. Adjustment blocks still
need a graph-level writer path, an FX3 deliverable.

## Transitions

Dissolves already exist (AddTransition, the sub-tractor, drag and
right-click creation). The generalisation keeps that engine shape and swaps
what runs inside the sub-tractor.

| Recipe family | MLT mechanism | Examples |
|---|---|---|
| Dissolve | `luma` without `resource` + `mix` | dissolve, dip to black, dip to white, dip to brand colour |
| Wipe | `luma` with `resource` (a luma map), `softness`, `reverse` | linear, radial, clock, iris, diamond, barn door, blinds, checker, and brand shapes |
| Motion | `affine` transition with keyframed `rect` on the outgoing and incoming cuts | push, slide, zoom through, spin, whip (with `frei0r` motion blur where present) |
| Blend | frei0r mixer2 plugins | additive flash, screen burn, multiply fade |
| Audio | `mix` with `start=-1` (crossfade) or `volume` keyframes with easing | linear, equal power, cut |

A recipe is a small JSON file in `drop-ins/effects/data/transitions/`:

```json
{
  "id": "wipe.radial",
  "name": "Radial Wipe",
  "video": { "service": "luma", "luma": "radial", "params": { "softness": 0.1 } },
  "audio": { "curve": "equal-power" },
  "exposed": ["softness", "reverse"]
}
```

Brand and user recipes are just more files; no code change.

**Luma library.** This install has no luma images, and copying kdenlive's
is off the table. A small build-time generator (`drop-ins/effects/data/lumas/generate.py`,
or a C++ tool if Python in the build is unwelcome) writes 16-bit PGM maps:
linear in 8 directions, radial, clock, iris, diamond, barn door, blinds,
checker, noise dissolve, and Unicorn Tears shapes (horn, star, sparkle). Every
map is procedural, so the set is ours, scales to any resolution, and adds no
binary media to the repo (CLAUDE.md).

**Edge transitions.** Fades on a single clip edge (no neighbour) use the
existing `FadeSpec` and become recipes too: fade from black, from
transparent, and scale-in.

## UX

Principles:

1. **See it before you commit.** Hovering an effect or transition shows it
   on the real footage, at the playhead, before anything is applied.
2. **The preview is a control surface.** Anything with a position, size,
   colour or region is edited by touching the picture.
3. **Every number is draggable.** Scrub any value horizontally; type to
   set; Shift for fine, Ctrl for coarse.
4. **One gesture, one undo.** Drags and slider runs merge.
5. **No modal dialogs** for effect work. Everything lives in the Rack and
   the Browser.

### Effect Rack (inspector)

Recommended answer to open question 6 in doc 13: a right sidebar, collapsible
with a header-bar toggle, because the preview is 16:9 and the timeline needs
the vertical space. The Rack shows the stack of whatever is selected (clip,
several clips, track header, adjustment block or the sequence):

- Each effect is a card, like a pedalboard: a bypass switch, name, cost
  badge, a **Mix** knob, a **Mask** chip, and an expander with parameters.
- Drag a card to reorder. Drag it onto another clip to copy it there.
- With several clips selected, the Rack shows the effects they share;
  changing a value applies to all of them as one undo step.
- A parameter with keyframes shows a small curve glyph and
  previous/next-keyframe arrows.

### Effect Browser

Opened with `E` or from the Rack's "Add" button. A search field with fuzzy
matching over names, tags and categories, then a grid:

- Each tile is the **current frame rendered through that effect**, produced
  lazily by a thumbnail-style worker with its own producer (never the live
  tractor), cached by (asset, frame, effect).
- **Live audition:** arrow keys or hover move a temporary effect onto the
  selected clip in the preview; Enter applies, Escape or moving away reverts.
  Audition renders the paused frame on the worker, so it never rebuilds the
  live tractor.
- Sections: Featured, Brand Looks, Recent, then categories. Quarantined
  effects appear only with "Show unstable effects".
- Cost badge on every tile: light, medium or heavy, from the health probe's
  measured milliseconds per frame.

### Applying effects

- Drag from the Browser onto a clip, a track header, the FX lane, or
  **onto the preview itself**, which targets the topmost visible clip under
  the pointer.
- `Ctrl+Shift+C` / `Ctrl+Shift+V` copy and paste a whole stack; the paste
  popover offers Append or Replace.
- **Looks:** save the selected stack as a Look from the Rack's menu. Looks
  show in the Browser and apply like any effect. The app ships brand Looks
  built from `lut3d` plus glow and grain, using `.cube` files we author.

### Direct manipulation on the preview

- `Point` and `Rect` parameters (frei0r positions, `affine` rect, mask
  shapes) draw handles over the preview, as the core's clip transform
  handles do (M4 F2, ADR-018). Dragging writes a keyframe when the
  parameter is animated, otherwise sets the value.
- `Color` parameters get an eyedropper that samples the displayed frame.
- Masks are drawn right on the preview: rectangle, ellipse, or free polygon,
  with a feather handle.

### Keyframes that feel musical

- **Pin:** a pin button next to any parameter records its current value at
  the playhead. The first pin turns animation on for that parameter.
- **Feel chips:** each keyframe's easing is chosen from a row of named chips
  with a tiny curve drawing: Linear, Smooth, Ease in, Ease out, Snap, Bounce,
  Elastic, Hold. Each maps to one MLT easing; an "All curves" popover exposes
  the full set.
- **Touch-record** (the DAW automation pattern, rare in video editors): arm
  one or more parameters, play, and move the controls; keyframes are written
  as you perform and thinned afterwards so the curve stays editable.
- **Curve lanes** under a clip in the timeline (FX4, needs M3's timeline
  widget): one lane per animated parameter,
  with draggable keyframes and easing handles. Press `C` on a clip to expand them (`K` is already shuttle stop).

### Compare

- Hold `\` to bypass every effect on the selection; release to restore.
- While paused, a split-compare slider on the preview shows before and
  after side by side. The "before" half comes from the worker (the frame
  without effects), composited in GTK, so the live graph is untouched.

### FX lane (adjustment blocks)

A thin lane above the video tracks. Draw a block across a time range and
drop effects on it; everything beneath is affected for that range. Block
edges have fade handles that ramp the block's Mix, so a grade or blur eases
in and out without keyframing anything.

### Transitions

- Existing gestures stay: drag a clip edge past a touching neighbour, or
  right-click the seam and Add Transition.
- `T` adds the default transition at the cut nearest the playhead on the
  active track.
- After a transition is created, a **seam shelf** appears on it: a row of
  small animated previews rendered from the actual two clips at low
  resolution. Click one to swap recipe. Scrolling over the seam cycles
  recipes; the preview follows live.
- Transition parameters show in the Rack when the seam is selected.

### Keyboard summary

| Key | Action |
|---|---|
| `E` | Effect Browser |
| `T` | Default transition at nearest cut |
| `\` (hold) | Bypass effects on selection |
| `C` | Toggle curve lanes on selected clip (FX4) |
| `Ctrl+Shift+C` / `V` | Copy / paste effect stack |
| `P` | Pin current parameter value at playhead |

All single-key shortcuts must be suppressed while a text field has focus;
the 2026-09-23 audit (A1) found that GTK application accelerators run in
the capture phase before a focused entry.

## Testing

- **Engine tests** (no media files, per CLAUDE.md): `color:`, `noise:`,
  `tone:` sources; attach every *featured* effect, render 10 frames through
  the `null` consumer, assert no crash and non-degenerate output.
- **Preview equals export:** frame hashes from the playback path and from
  `u-studio-render` must match for a synthetic project with keyframed
  transform, a dissolve with effects on both sides, a mask and an adjustment
  block.
- **Easing round-trip:** every `Easing` value through the animation string
  and back through `Mlt::Animation`.
- **Keyframe offset:** an animated clip across a dissolve renders a
  continuous ramp (no jump at the dissolve boundaries).
- **Health probe:** a deliberately crashing test plugin (a tiny frei0r
  `.so` built in `tests/`) is quarantined, and the editor keeps running.
- **No Qt:** factory-policy test extended to every loaded plugin.
- **Model:** property test (random effect commands, undo all, equal) and
  XML round-trip for every new field.

## Phases

Each phase lists the integration points (IP, see "Drop-in structure") it
needs. Module-internal work for FX0 and FX1 can start now; integration
points land after the post-M3 audit. Only FX4 depends on M3's timeline
widget, through IP5's timeline provider.

### FX0 — Spikes and packaging (about 1 week)

Integration points: none (standalone repros only). Add one spike item:
whether `decorateTractor()` is enough for adjustment blocks.


Each item ends with a written finding in this doc or the
[implementation notes](../../developer/notes/README.md), per CLAUDE.md's empirical-knowledge rule.

- Install `frei0r-plugins` on the dev machine (owner approval needed for
  `dnf`); count the services; `ldd` every plugin for Qt.
- frei0r parameter addressing (index or name) and animation of frei0r
  parameters through MLT keyframe strings.
- Animation position semantics for a filter attached to a cut with a
  non-zero `in` (the keyframe offset rule).
- `mask_start` / `mask_apply` with `transition=affine` for universal Mix.
- `luma` with a generated 16-bit PGM `resource`.
- `frei0r.cairoblend` as track compositor and `cairoblend_mode` values.
- Adjustment blocks: filter attached to a sub-tractor of lower tracks with
  in/out.
- In-place property set on a live filter while playing (the no-rebuild path).
- Whether `<filter>` inside `<entry>` round-trips through MLT's `xml`
  producer.

Acceptance: every item has a recorded yes/no with the repro kept in
`tests/engine` or the scratchpad notes; ADR-011 updated with anything that
changes the plan.

> REVIEW: VE Core, 2026-09-27: FX0 spikes run; every item is answered in
> [the effects notes](../../developer/notes/effects.md). All yes, with two
> plan changes: `decorateTractor()` covers an FX lane only on top of the
> stack (a mid-stack lane needs a sub-tractor hook), and the drop-in's
> curated list must leave out MLT's `not_thread_safe.txt` plugins. frei0r
> was already installed (2.5.6), so no `dnf` was needed; no Qt in any
> plugin. ADR-011 needs no change.

### FX1 — Engine and model (about 2 weeks)

Integration points: IP1, IP2, IP3 (without `makeTransitionSegment`),
IP4, IP6.


`EffectRegistry`, descriptor cache, overlays loader, `FactoryPolicy`
plugin curation, health and cost probe (`--probe-effect`), model and
command changes, EngineSync attachment (clip, dissolve cuts, track, master),
keyframe offset rule, Mix and mask, the no-rebuild parameter path, format v4.

Acceptance:

- [x] Every installed frei0r service is either usable or quarantined with a
      reason, and the probe never crashes the editor.
- [x] Preview-equals-export hash test passes.
- [x] Dragging a parameter slider does not restart the consumer (log shows
      no `restart #` lines during the drag).
- [x] `melt` renders a saved project with clip, track and master effects.

**FX1 as built (VE Effects, 2026-09-28):** `drop-ins/effects/`
([README](../../../drop-ins/effects/README.md)).

- The filters an effect becomes live in `src/core/model/effect_native.h`
  (`core::nativeFilters()`), used by the project writer and the engine
  extension alike, so the file `melt` plays and the editor's graph can't
  drift. It also closes IP2's open item: the writer now emits the mix.
- `EffectRegistry` normalises every MLT filter but `movit.*` (599 on the dev
  machine, 545 shown) and applies `data/overlays/*.json`. Overlays can also
  give a default MLT doesn't (brightness's `level`) and mark a plugin
  `unstable`.
- IP4 curation, the extension (clip cuts including dissolve tails and heads,
  tracks, master; the mix; in place for values), `--probe-effect` and
  `--effects-registry` (IP6), and the editor's health scan (IP5, a shell
  extension with no UI).
- Acceptance, how it was checked:
  - **frei0r usable or quarantined.** All 102 frei0r services probed
    through the render tool: 101 ok, and `pixs0r` still running at the
    20-second deadline, so it's quarantined as timed out. `defish0r` had been
    black at a NaN default, now treated as none. `3dflippo` probes ok but is
    quarantined by its overlay (it corrupts memory at other read sizes). `effects-scan` shows a plugin
    that crashes and one that hangs quarantined while the host keeps
    running.
  - **Preview equals export.** `effects-engine` compares frames from the
    live graph, one built the way `renderProject()` builds its own, and the
    saved file through MLT's `xml` producer, across a dissolve with clip,
    track and master effects and a mixed frei0r effect. Export's H.264
    output is lossy, so the graphs are compared, not the file.
  - **No restart.** 30 slider steps through `SetParam` gave 30 in-place
    applies, no `rebuilt`, one undo step. FX2 checks the same in the app
    with a real drag.
  - **`melt`.** Stock `melt-7` renders the saved project (`xml:` prefix:
    melt picks a loader by extension).

> REVIEW: VE Effects, 2026-09-28: FX0's plan change (b) is dropped. MLT's frei0r module already serialises the `not_thread_safe.txt` plugins (one shared instance, the service lock held across `f0r_update`), so they're offered and flagged, not left out; only MLT's blacklist, Qt-linking plugins and quarantined ones stay out of `FREI0R_PATH` ([effects notes](../../developer/notes/effects.md)).

> REVIEW: VE Effects, 2026-09-28: "Mix and masks" is implemented for the mix only. A constant mix is `frei0r.cairoblend`'s opacity inside `mask_apply` (`affine` without frei0r), because `cairoblend` costs 6 ms a 1080p frame against 14 for `affine`. A keyframed mix can't animate a transition there: on a cut in a playlist the transition reads the timeline position. So it's a `brightness` filter with an animated `alpha` between the pair. Masks stay in the model only, for FX2 with the mask UI (FX0 row 4).

> REVIEW: VE Effects, 2026-09-28: the editor doesn't read MLT metadata in its own process (not safe beside a running graph). The registry comes from `u-studio-render --effects-registry` and is cached, keyed by the plugin set, MLT's version and the overlays. The curation is the drop-in's `contributeFactoryPaths()` (IP4), not new code in `FactoryPolicy`. There's no `Point` kind yet, because no installed family reports one; keyframes animate `Scalar` parameters only (`core::Keyframe` is a number).

### FX2 — Rack, Browser and keyframes in the inspector (about 2 weeks)

Integration points: IP5 (inspector host, selection signal, action
contributions, preview overlay host). Flips `dropin_effects` to `builtin` by default
when its gate passes.


Effect Rack, Effect Browser with frame thumbnails and live audition,
generic parameter widgets, drag and drop onto clips, tracks and preview,
multi-select editing, copy/paste stacks, Looks, pins and feel chips,
bypass-hold and paused split compare, cost badges.

Acceptance:

- [ ] Any frei0r effect can be found, auditioned, applied, keyframed and
      undone without touching a dialog.
- [ ] Audition never rebuilds the live tractor.
- [ ] Brand Looks ship and apply in one drag.

### FX3 — Transitions library (about 2 weeks)

Integration points: IP3 `makeTransitionSegment()`; IP1's
`Transition::recipe` fields.


Recipe loader, generated luma library, wipe, motion, blend and audio-curve
recipes, seam shelf with live previews, `T` shortcut, blend-mode track
compositing, and the writer emitting dissolve and wipe sub-tractors so
`melt` plays them.

Acceptance:

- [ ] At least 20 recipes ship, all generated or authored in-repo.
- [ ] Swapping a recipe is one undo step and survives save and reload.
- [ ] `melt` renders a saved project's transitions like the editor does.

### FX4 — Timeline and preview manipulation (about 2–3 weeks, needs M3)

Integration points: IP5 timeline overlay/lane provider; possibly a
graph-level IP3 hook for adjustment blocks (see "Not purely additive").


Curve lanes, touch-record, FX lane with adjustment blocks and fade handles,
on-preview handles for `Point`/`Rect`/mask parameters, eyedropper.

Acceptance:

- [ ] Touch-recording a slider during playback produces an editable curve
      with fewer keyframes than frames.
- [ ] An adjustment block affects exactly the tracks beneath it for exactly
      its range, in preview and export.

### FX5 — Optional families (about 1–2 weeks, any time after FX2)

Integration points: none new (IP4 for `OFX_PLUGIN_PATH`).


LADSPA and VST2 audio effects in the Rack (with the same health probe),
OpenFX behind an experimental preference, LUT library management (import
`.cube` files into the project).

## Decisions needed from the owner

All decided by the owner on 2026-09-24:

1. `frei0r-plugins`: installed on the dev machine (2.5.6); a dependency of
   the effects drop-in only (ADR-011 as narrowed by ADR-014), added to its
   Flatpak extension in M7.
2. Audio plugin pack: **recommend LSP** (Linux Studio Plugins), detected at
   runtime, never required.
3. OpenFX: **explore in FX5**, behind the same health probe as frei0r;
   dropped if it proves unstable.
4. The **right-sidebar Rack** is confirmed (closes doc 13, question 6).
5. FX and titles tracks: **the FX0 and T0 spikes first, then alternate**
   milestones between the two tracks.
