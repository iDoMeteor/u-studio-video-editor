# ADR-006: Compositing with `composite` + `affine`; frei0r optional; Qt never

**Status:** Superseded by [ADR-011](011-frei0r-required-and-effect-families.md) (2026-09-23)

## Context
Kdenlive composites with `qtblend` (Qt) or `frei0r.cairoblend` (frei0r). On
this machine frei0r is not installed and Qt modules are excluded by policy.
Available Qt-free transitions: `composite`, `affine`, `luma`, `mix`,
`matte`, `movit.*` (GPU pipeline, out of scope).

## Decision
Track blending uses the core `composite` transition (always active, full
frame, alpha-aware). Per-clip transform and opacity use the `affine` filter
with keyframed `transition.rect`. Audio uses `mix`. Dissolves use `luma`.
If `frei0r.cairoblend` is present at runtime it is offered as an optional
compositor via preference. Qt services (`qtblend`, `qtext`, `qimage`) are
never used; text uses `dynamictext`/`pango`.

## Consequences
- Works on a stock install with `mlt` only.
- `composite` is older and CPU-bound; quality is acceptable for 1080p
  preview and export; blend modes beyond normal are unavailable without
  frei0r (risk R6).
- Kdenlive projects using `qtblend` are imported with `composite` and a
  warning.
