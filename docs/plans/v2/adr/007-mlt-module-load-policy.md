# ADR-007: Curated MLT module directory

**Status:** Accepted (implemented in M0)

## Context
`Mlt::Factory::init()` with no argument loads every module in
`/usr/lib64/mlt-7/`, including `libmltqt6.so` and
`libmltglaxnimate-qt6.so`, which dlopen Qt6 into the process. Measured on
2026-09-12: 32 Qt library mappings after init, 0 with a curated directory
containing symlinks to every module except `*qt6*`. All required consumers,
transitions, and profiles remain available.

## Decision
`engine::FactoryPolicy` creates (or refreshes) a directory
`ustudio-mlt-modules/` of symlinks to the system module directory,
excluding a denylist (`qt6`, `glaxnimate-qt6`), and calls
`Mlt::Factory::init(thatDir)`. It's placed under `$XDG_RUNTIME_DIR` when
set, or the system temp directory otherwise — CI containers and other
headless environments have no logind session and so no
`XDG_RUNTIME_DIR`, and that must not silently degrade to unsafe default
init (found in M0: both CI jobs exercising this test failed exactly this
way before the temp-dir fallback was added). The denylist is a constant in
code, overridable by `USTUDIO_MLT_DENYLIST` for debugging. A test asserts
no `libQt` mapping after init and that the required service names exist.
The Flatpak build compiles MLT without the Qt modules, making the policy
structural there.

## Consequences
- The "no Qt/KDE in the stack" claim becomes true and continuously tested,
  including in CI (no `XDG_RUNTIME_DIR` there).
- Depends on `mlt_factory_init(directory)` semantics (stable since MLT 6).
- Modules that need data files (`MLT_DATA`) keep working because data lookup
  is independent of the module directory (verified).
- If neither `XDG_RUNTIME_DIR` nor the system temp directory is usable,
  fall back to default init and log a warning; the test would catch this
  in CI but a user machine would run with Qt loaded rather than fail to
  start.

> REVIEW: VE Core, 2026-09-28: the default denylist also names `openfx`.
> At `Factory::init()` MLT's openfx module dlopens every `.ofx` bundle in
> `/usr/OFX/Plugins` and `/usr/local/OFX/Plugins`, and `OFX_PLUGIN_PATH`
> only adds to those (`src/modules/openfx/factory.c`, MLT 7.40). So any
> OpenFX plugin installed system-wide, Qt-linked or not, was loaded into the
> editor, which is this ADR's failure mode by another route. VE Effects
> found it. The effects drop-in may bring openfx back only behind its
> experimental preference, after scanning every bundle for a Qt dependency
> (FX5). A module allowlist that replaces the denylist is planned as an
> ADR that amends this one.
