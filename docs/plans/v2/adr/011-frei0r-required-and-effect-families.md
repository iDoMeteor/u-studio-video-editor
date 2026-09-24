# ADR-011: frei0r is required; effects come from mature plugin families

**Status:** Accepted for the frei0r requirement (owner directive,
2026-09-23: "full frei0r compatibility is mandatory"). The rest of the
family list is Proposed until the FX0 spikes in doc 15 report.
Supersedes [ADR-006](006-compositing-without-qt-or-frei0r.md).
**Partially superseded by [ADR-014](014-drop-in-loading-and-distribution.md)
(2026-09-24):** frei0r is required by the effects drop-in, not by the core
editor. "Full frei0r compatibility" still holds wherever the effects
drop-in is installed; the core editor never depends on frei0r and keeps
`composite` as its track compositor.

## Context
ADR-006 treated frei0r as optional because it was not installed, and kept
compositing on the core `composite` transition. The owner now wants a broad
effects and transitions feature set as soon as possible, built on existing
ecosystems rather than effects we write. On this machine MLT 7.40 already
hosts frei0r (0 plugins installed), libavfilter (333 filters), its own
native modules (91 filters), SoX, LADSPA, VST2 and an experimental OpenFX
host. LV2 is not compiled in. Measurements are in doc 15.

## Decision
- `frei0r-plugins` becomes a required runtime dependency. The Fedora
  package is a documented prerequisite; the Flatpak bundles it (M7).
  Every installed frei0r filter, mixer and generator is exposed through a
  generated UI, not a hand-picked subset.
- MLT native modules and libavfilter are first-class sources alongside
  frei0r. LADSPA and VST2 audio are supported with plugin packs optional.
  OpenFX is explored behind an experimental preference (doc 15, FX5).
  movit and every Qt service stay excluded.
- `FactoryPolicy` curates plugin search paths (`FREI0R_PATH`, and
  `OFX_PLUGIN_PATH` when enabled) the same way it curates MLT modules, so
  ADR-007's no-Qt guarantee covers plugins, and so quarantined plugins are
  never loaded.
- No native plugin is offered until an out-of-process health probe
  (`u-studio-render --probe-effect`) has rendered it without crashing.
- Track compositing moves to `frei0r.cairoblend`, with a per-clip blend
  mode through `cairoblend_mode`. Per-clip transform stays on `affine`.
- `mask_apply` is always given an explicit non-Qt `transition`; its default
  is `qtblend`.

## Consequences
- A stock Fedora machine needs `dnf install frei0r-plugins` before the
  effect set is complete. Until that is packaged, code must still detect a
  missing frei0r at runtime and degrade (hide frei0r entries, fall back to
  `composite`) rather than crash.
- Native plugins run in-process. The health probe, the curated search path
  and MLT's own `blacklist.txt` / `not_thread_safe.txt` are the mitigation;
  a plugin that passes the probe can still misbehave on real footage (doc 13,
  R11).
- CLAUDE.md's "frei0r is not installed; treat it as optional" line changes
  to point here.
