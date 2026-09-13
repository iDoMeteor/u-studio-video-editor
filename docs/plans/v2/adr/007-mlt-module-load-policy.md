# ADR-007: Curated MLT module directory

**Status:** Proposed

## Context
`Mlt::Factory::init()` with no argument loads every module in
`/usr/lib64/mlt-7/`, including `libmltqt6.so` and
`libmltglaxnimate-qt6.so`, which dlopen Qt6 into the process. Measured on
2026-09-12: 32 Qt library mappings after init, 0 with a curated directory
containing symlinks to every module except `*qt6*`. All required consumers,
transitions, and profiles remain available.

## Decision
`engine::FactoryPolicy` creates (or refreshes) a per-user directory
`$XDG_RUNTIME_DIR/ustudio-mlt-modules/` of symlinks to the system module
directory, excluding a denylist (`qt6`, `glaxnimate-qt6`), and calls
`Mlt::Factory::init(thatDir)`. The denylist is a constant in code, overridable
by `USTUDIO_MLT_DENYLIST` for debugging. A test asserts no `libQt` mapping
after init and that the required service names exist. The Flatpak build
compiles MLT without the Qt modules, making the policy structural there.

## Consequences
- The "no Qt/KDE in the stack" claim becomes true and continuously tested.
- Depends on `mlt_factory_init(directory)` semantics (stable since MLT 6).
- Modules that need data files (`MLT_DATA`) keep working because data lookup
  is independent of the module directory (verified).
- If the runtime dir can't be created, fall back to default init and log a
  warning; the test would catch this in CI but a user machine would run
  with Qt loaded rather than fail to start.
