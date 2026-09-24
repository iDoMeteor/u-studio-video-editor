# ADR-014: Drop-ins build built-in or as loadable modules; core ships with opt-in extras

**Status:** Proposed (2026-09-24, owner: "ship core & have opt-in plugs").
Extends [ADR-013](013-effects-and-titles-as-drop-in-modules.md).

## Context
ADR-013 made effects and titles drop-ins that register through a
generated, compile-time list. The owner now wants a core editor plus
optional drop-ins users install separately (doc 17), which needs runtime
loading.

## Decision
- Each drop-in builds in one of three modes via `-Ddropin_<name>=`
  `disabled | builtin | module`. The boolean `effects` / `titles` options
  in docs 15 and 16 become these.
- A module is `libustudio-dropin-<name>.so` exporting one C function,
  `ustudio_drop_in_describe()`, with name, version, drop-in API version and
  its entry points. The API version is an integer in `src/`, bumped with
  any change to `DropInHost` or an integration point. Modules must match it
  and the app release exactly; mismatches are refused with a message.
- Modules load only from trusted locations (`$libdir/u-studio/drop-ins/`,
  the Flatpak extension mount). `USTUDIO_DROPIN_PATH` is a development-only
  override that logs a warning.
- Settings has a Drop-ins page: enable or disable each installed drop-in
  (applied on restart), and pointers to where to get missing ones. The app
  never downloads or installs anything itself.
- Packaging (owner decision, 2026-09-24): the core package is the editor
  and the render tool only. **Every** drop-in, effects and titles
  included, is its own opt-in package (a Flatpak extension of
  `com.ustudio.VideoEditor.DropIn`, or an RPM subpackage), carrying its own
  runtime dependencies. So frei0r ships with the effects drop-in, not with
  core, and this narrows ADR-011: frei0r is required by the effects drop-in,
  not by the editor. Development and test builds may still build effects
  and titles `builtin`.

## Consequences
- Each drop-in's tests run in both builtin and module mode.
- Loading native code in-process is a trust decision; restricting load
  locations and releasing drop-ins with the app keeps it one we control.
- Exact version matching means users update drop-ins with the app, which
  Flatpak and RPM both do naturally.
