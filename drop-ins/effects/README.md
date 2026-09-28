# Effects drop-in

[Docs home](../../docs/README.md) › [Developer docs](../../docs/developer/README.md) › Effects drop-in

Effects on clips, tracks and the whole sequence from frei0r, FFmpeg's
libavfilter and MLT's own filters, with keyframes, a wet/dry mix and undo.
The plan is [doc 15](../../docs/plans/v2/15-effects-and-transitions.md); the
decisions are [ADR-011](../../docs/plans/v2/adr/011-frei0r-required-and-effect-families.md)
(frei0r, narrowed by [ADR-014](../../docs/plans/v2/adr/014-drop-in-loading-and-distribution.md):
a dependency of this drop-in only, never of the editor) and
[ADR-013](../../docs/plans/v2/adr/013-effects-and-titles-as-drop-in-modules.md)
(one self-contained folder; nothing in `src/` includes from here).

Status: FX1 (engine and model) is done; FX2 (the Rack, the Browser,
keyframes in the inspector) is in progress: the Rack, keyframes in it (pins, previous/next, feels; **P** pins the value
last changed) the Browser (**E**: tiles of the frame through each effect,
audition on the preview, Enter, a click or a drag onto the picture adds),
several clips at once, copy and paste (**Ctrl+Shift+C**, **Ctrl+Shift+V**)
Looks (brand Looks ship in `data/looks/brand.json`; save your own from
the Rack's menu), and compare (hold **\\** for the picture without the
clip's effects; the Rack's Compare button for a before/after split you
drag) are in. Effects in a project play in the
preview, export and stock `melt`; every effect MLT offers is described,
health-checked in its own process and quarantined if it crashes or hangs.
There's no UI yet: the Rack and Browser are FX2, and the build option stays
`disabled` by default until FX2's gate passes.

## Layout

| Folder | What | May use |
|---|---|---|
| `core/` | `EffectDescriptor` and normalisation (every family's parameters to one set of kinds), curated overlays, the effect commands (add, remove, reorder, bypass, set parameter, set mix; gestures merge), probe results and `effect-health.json`, a small JSON reader and writer | std, `src/core` (never GTK, GLib or MLT: checked by the build) |
| `engine/` | `EffectRegistry` (every MLT filter, from `Mlt::Repository` metadata), frei0r discovery and curation (IP4), `EffectsExtension` (IP3), `u-studio-render --probe-effect` and `--effects-registry` (IP6), and `FrameRenderer` (a clip's frame through a stack, on a thread of its own with throwaway producers: the Browser's tiles and audition, never the live graph) | `src/engine`, mlt++, GLib |
| `app/` | The Effect Rack, the Browser and the Transitions page (inspector pages, IP5; the page also outlines its transition on the timeline and claims a double-click on one), the catalog they read (registry and health), and the editor's background health scan (the registry and one probe child per effect, featured first) | GTK, GIO, `engine/` headers without MLT; never MLT (checked by the build) |
| `data/overlays/` | Curated names, categories, featured flags, defaults MLT doesn't give, hidden plumbing, known-unstable plugins, one JSON file per family | — |
| `data/transitions/` | Transition recipes (dissolves, dips, flashes, slides, pushes, 20 wipes): each a name and the `Transition::params` it resolves into (`src/core/model/transition_native.h` says what they mean; the wipe maps are generated there) | — |
| `data/looks/` | Brand Looks (`brand.json`): stacks of filters with settings, no LUT files (a LUT library is FX5's) | — |
| `register.cpp` | The drop-in's describe function and its integration points | the drop-in host API |
| `tests/` | `effects-core`, `effects-engine`, `effects-scan`; `frei0r/broken_plugin.cpp` builds two frei0r plugins broken on purpose (one crashes, one hangs) | doctest |

A transition recipe is data too: `core/transitions.h` loads the recipes and
changes a transition's with one undo step (`SetTransitionRecipe`);
`core::nativeTransition()` (`src/core/model/transition_native.h`) turns
them into MLT services for the editor's graph and the project writer alike.

The filters an effect becomes are decided in one place in `src/`:
`core::nativeFilters()` (`src/core/model/effect_native.h`), which the project
writer and this drop-in's engine extension both use, so a saved project
plays in `melt` exactly as the editor plays it.

## How an effect plays

| Model owner | MLT |
|---|---|
| A clip | Its filters on every cut that plays the clip, including its tail or head inside a dissolve, with in/out set to the cut's and keyframes shifted to it (`engine::attachToCut()`, `core::keyframesForCut()`) |
| A track | Its filters on the track's playlist |
| The sequence | Its filters on the whole tractor |

At a constant full mix an effect is its own filter. Any other mix wraps it
in `mask_start` (which runs the effect) and `mask_apply` (which composites
the result back over the untouched frame through `frei0r.cairoblend`, or
`affine` without frei0r). A keyframed mix adds a `brightness` filter
between them whose `alpha` carries the keyframes: an animated transition
inside `mask_apply` can't animate on a cut in a playlist
([effects notes](../../docs/developer/notes/effects.md)).

A change to parameter values or the mix that keeps the same filters is set
on the live filters without a rebuild, so dragging a slider never restarts
playback. Adding, removing, reordering or bypassing an effect rebuilds.

With the GPU pipeline on (ADR-019), effects are CPU filters inside the movit
graph: MLT downloads the frame before a run of CPU filters on one cut and
uploads it after, so a run costs one transfer. Blend modes (FX3) will need
the GPU engine's help: until then they're for the CPU pipeline only.

## Plugins and health

- **Which frei0r plugins load.** `contributeFactoryPaths()` finds the
  plugins MLT would load (its own search order) and sets `FREI0R_PATH` to
  the system directories, unless one must be left out: it names a Qt
  library (ADR-007) or the health scan quarantined it. Then `FREI0R_PATH`
  is a private directory of links to the rest.
- **A package's own frei0r.** When the drop-in is packaged apart from an
  MLT without frei0r (the Flatpak extension, ADR-014), the package installs
  MLT's frei0r module to `<prefix>/<libdir>/u-studio/mlt` and the plugins to
  `<prefix>/<libdir>/frei0r-1`. When those folders exist the module joins
  FactoryPolicy's curated directory (through its denylist; a system frei0r
  module keeps its name) and the plugins come first in the search order.
  The frei0r module's data (`blacklist.txt`, `not_thread_safe.txt`,
  `resolution_scale.yml`, ...) is read from `MLT_DATA/frei0r/`, which the
  core package ships.
- **The scan.** Once the window exists, on a thread of its own (forking a
  child from the editor blocks for tens of milliseconds), the editor gets
  the registry from
  `u-studio-render --effects-registry` (cached in
  `$XDG_CACHE_HOME/ustudio/effects-registry.json`) and probes every offered
  effect with `u-studio-render --probe-effect <service>`, two at a time,
  frei0r first. A probe runs the effect on a 1080p source at its defaults,
  at every number's minimum and maximum, and read at half size. A child
  that crashes or passes its 20-second deadline is quarantined. Results go
  to `$XDG_CACHE_HOME/ustudio/effect-health.json` after each one, so an
  interrupted scan resumes. A new plugin set (or MLT, or overlays) starts
  over.
- **Quarantined effects** are never attached: a project that uses one plays
  without it, with a warning in the log.

## Building and testing

```sh
meson configure builddir -Ddropin_effects=builtin   # or module
meson compile -C builddir
meson test -C builddir effects-core effects-engine effects-scan
./builddir/src/render/u-studio-render --effects-registry
./builddir/src/render/u-studio-render --probe-effect frei0r.glow
```

As a module it links its own copy of `src/platform` (stateless): the
programs link core, engine and dropins whole but not platform, so a
platform function the program itself never calls would otherwise be
missing when the module loads.

The tests need `frei0r-plugins` installed; `effects-engine`'s `melt` check
needs `melt-7` and is skipped without it. Both `builtin` and `module` must
pass the whole suite (ADR-013): `just dropins-builtin` and
`just dropins-module`.

## The Browser's rendering

Tiles and the audition are rendered by `FrameRenderer` on its own thread,
from its own producer of the clip's media (kept open between tiles; the
`loader-nogl` service, so always the CPU chain), with the clip's effects
and the candidate attached to a fresh cut. Only effects the health scan has
passed run here, since this is the editor's process: the rest show
"checking…". The audition is a picture over the preview (the preview
overlay host), so the live graph is never rebuilt. The renderer stops when
the window goes, before MLT's factory closes.

## Several clips, paste and Looks

- With several clips selected, the Rack shows the effects they share: the
  same service's first, second, ... occurrence among each clip's own
  effects, so a Look applied to clips that already differ lines up. A
  change applies to all of them as one undo step; reordering and keyframes
  are one clip's at a time.
- Paste puts the copied stack after, or instead of, this drop-in's
  effects on every selected clip (other drop-ins' effects stay).
- A Look is a stack; applying one adds it after the clip's own effects.
  A Browser tile carries an effect or a Look (`Catalog::effectsFor()`),
  applied with Enter or a click, or dragged onto the picture (the topmost
  clip at the playhead) or onto the Rack.
