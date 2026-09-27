# Effects (M5 FX0 spikes)

[Docs home](../../README.md) › [Developer docs](../README.md) › [Implementation notes](README.md) › Effects

The FX0 spikes of [doc 15](../../plans/v2/15-effects-and-transitions.md),
run 2026-09-27 against MLT 7.40 and frei0r-plugins 2.5.6 (Fedora 44), each a
standalone repro at 320×180. Nothing here is wired into `src/`; the effects
drop-in (ADR-011, ADR-014) is where it will be used.

| # | Question | Answer |
|---|---|---|
| 1 | What frei0r brings, and whether any of it pulls in Qt | 158 plugin files; MLT registers **102 `frei0r.*` filters and 49 transitions**. `ldd` finds **no Qt** in any of them. MLT's frei0r module ships `blacklist.txt` (`perspective`) and `not_thread_safe.txt` (`baltan`, the `bigsh0t_*` 360° set, `colorhalftone`, `delay0r`, `delaygrab`, `distort0r`, `ising0r`, `medians`, `nervous`, `partik0l`, `plasma`, `tehRoxx0r`, `vertigo`): the drop-in's curated list leaves those out, or runs them only in a single-threaded render. |
| 2 | Parameter addressing: index or name | **Both.** `frei0r.brightness` with `"0"=0.8` and with `"Brightness"=0.8` gave the same result (grey 128 → 204). Prefer the index: it's what MLT's own presets and kdenlive write, and names can change between plugin versions. |
| 3 | Animating a frei0r parameter with an MLT keyframe string | **Yes.** `"0"="0=0.2;29=0.8"` on a filter with in/out 0–59: frame 0 → 51, frame 29 → 204. The keyframe offset rule for a cut with a non-zero `in` is IP3's (`engine::attachToCut()`, [engine sync](engine-sync.md#filter-in)). |
| 4 | `mask_start` / `mask_apply` with `transition=affine` (universal Mix / masked effects) | **Yes, Qt-free.** `mask_start filter=frei0r.brightness filter.0=0.9` then `mask_apply transition=affine` applies the effect (128 → 230). The default `transition` is `qtblend`, which ADR-007 denies, so `transition=affine` must always be set. A shaped (partial) mask wasn't tested here; that's FX2's with the mask UI. |
| 5 | `luma` with a generated 16-bit PGM `resource` | **Yes.** A 64×36 P5 gradient (maxval 65535) as `luma`'s `resource` wipes left to right: halfway, the left edge is already the B clip and the right edge still A. So wipe shapes can be generated, not shipped as images. |
| 6 | `frei0r.cairoblend` as a track compositor, and its blend modes | **Yes.** Red under 50% blue (`color:0x0000ff80`) gives 127/0/127 in `normal`. Parameter `"1"` takes mode names: `multiply` 127/0/0, `screen` 255/0/127, `overlay` 255/0/0, `add` 255/0/127. A transition needs its in/out set, or it covers frame 0 only. (MLT colours: 8-digit `#` is `#aarrggbb`; use `0xrrggbbaa` for a clear alpha.) |
| 7 | Adjustment blocks: a filter on a sub-tractor of lower tracks, with in/out | **Yes.** A filter attached to a tractor with in/out 20–39 applies there only (frames 10, 30, 50: 128, 230, 128). |
| 8 | Is `decorateTractor()` enough for adjustment blocks? | **Only for a lane on top.** A filter on the whole tractor affects every track, so a top FX lane works with `decorateTractor()` alone. A lane in the middle of the stack needs the tracks beneath it grouped into a sub-tractor that the filter attaches to: a structural hook, not a decoration. |
| 9 | In-place property set on a live filter while playing | **Yes**, done in IP3 (`EngineExtension::applyInPlace()`) and used by clip transforms (ADR-018): no rebuild, no consumer restart. |
| 10 | `<filter>` inside `<entry>` through MLT's `xml` producer | **Yes**, done in IP3 (the writer puts the cut's in/out on it; `tests/dropins/test_dropin_engine`). |
