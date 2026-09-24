# ADR-013: Effects and titles are drop-in modules behind named integration points

**Status:** Proposed (2026-09-24, owner direction: "implement our effects
& text work as drop-ins")

## Context
The effects and transitions work (doc 15) and the titles tool (doc 16) are
large, run in parallel with the M3 wrap-up and M4, and would otherwise
spread through the busiest shared files: `app_window.cpp` (about 4,400
lines), `engine_sync.cpp`, the XML writer and reader, and the action
registry. Several agents and the owner edit those files at the same time.

## Decision
- Every drop-in lives in its own self-contained folder under a top-level
  `drop-ins/` directory (`drop-ins/effects/`, `drop-ins/titles/`), holding
  its code for each layer, data, tests, build file and README (doc 15,
  "Drop-in structure"; doc 16, same heading).
- Dependencies point one way: a drop-in may include from `src/`; nothing
  in `src/` includes from `drop-ins/`. The only link back is a generated
  list of the enabled drop-ins' registration functions. Drop-ins don't
  depend on each other.
- They reach the rest of the program only through a fixed list of
  integration points, IP1 to IP6. Adding one means adding it to that list
  in doc 15 in the same change.
- Project **data** is never optional: the model fields and XML
  persistence for effects and titles (IP1, IP2) live in `src/`, so a build
  without either drop-in still loads and saves every project losslessly.
- A drop-in's folder is built only when its meson option (`effects`,
  `titles`) is on. With it off, every integration point is a no-op and the
  app behaves exactly as today; deleting the folder removes the feature.
- Integration points land as small, separately reviewed commits after the
  post-M3 audit, not mixed into module work.

## Consequences
- Module work can proceed now in worktrees without touching files the
  M3/M4 work is changing.
- Integration costs a handful of reviewed commits instead of a long-lived
  branch that conflicts with everything.
- A few pieces cannot be purely additive: the model additions (Model
  mutators must live on `Model`, doc 14), the format version bump, and
  adjustment blocks, which change the tractor graph's shape (FX4). Those are
  called out as such in doc 15.
- Both build configurations (modules off, modules on) must pass the full
  test suite.
