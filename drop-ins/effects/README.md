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
keyframes in the inspector) is in progress: the Rack's first slice is in. Effects in a project play in the
preview, export and stock `melt`; every effect MLT offers is described,
health-checked in its own process and quarantined if it crashes or hangs.
There's no UI yet: the Rack and Browser are FX2, and the build option stays
`disabled` by default until FX2's gate passes.

## Layout

| Folder | What | May use |
|---|---|---|
| `core/` | `EffectDescriptor` and normalisation (every family's parameters to one set of kinds), curated overlays, the effect commands (add, remove, reorder, bypass, set parameter, set mix; gestures merge), probe results and `effect-health.json`, a small JSON reader and writer | std, `src/core` (never GTK, GLib or MLT: checked by the build) |
| `engine/` | `EffectRegistry` (every MLT filter, from `Mlt::Repository` metadata), frei0r discovery and curation (IP4), `EffectsExtension` (IP3), `u-studio-render --probe-effect` and `--effects-registry` (IP6) | `src/engine`, mlt++, GLib |
| `app/` | The Effect Rack (an inspector page, IP5: the stack of the selected clip, its track or the sequence; bypass, reorder, remove, Mix, one control per parameter; **E** opens the Add search), the catalog it reads, and the editor's background health scan (the registry and one probe child per effect) | GTK, GIO, `engine/` headers without MLT; never MLT (checked by the build) |
| `data/overlays/` | Curated names, categories, featured flags, defaults MLT doesn't give, hidden plumbing, known-unstable plugins, one JSON file per family | — |
| `register.cpp` | The drop-in's describe function and its integration points | the drop-in host API |
| `tests/` | `effects-core`, `effects-engine`, `effects-scan`; `frei0r/broken_plugin.cpp` builds two frei0r plugins broken on purpose (one crashes, one hangs) | doctest |

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
