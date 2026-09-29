# ADR-022: The curated MLT module directory is an allowlist

**Status:** Accepted (VE Strategist for the owner, 2026-09-28). Amends
ADR-007.

## Context

ADR-007 keeps Qt out of the process by building a private directory of
links to MLT's modules and initialising MLT from it. It links every module
except a denylist (`qt6`, `glaxnimate-qt6`). On the dev machine that still
loaded 25 modules, of which the editor uses 11. The others (decklink,
jackrack, ladspa, opencv, oldfilm, openfx, rnnoise, sox, vidstab, and so on)
were linked because nobody had excluded them.

A module does more than offer services: it runs code at `Factory::init()`.
VE Effects found that MLT's openfx module dlopens every `.ofx` bundle in
`/usr/OFX/Plugins` and `/usr/local/OFX/Plugins` at init, and
`OFX_PLUGIN_PATH` can only add folders to that list. So any OpenFX plugin
installed system-wide was loaded into the editor. That is ADR-007's failure
mode reached by another route. A denylist can only close such holes after
someone finds them.

## Decision

The curated directory links only the modules that are listed.

- **The editor's own list:** `core`, `plus`, `normalize`, `avformat`,
  `xml`, `sdl2`, `rtaudio`, `gdk`, `resample`, `xine`, and `movit` (the GPU
  pipeline, ADR-019). This is CLAUDE.md's required list;
  `engine::editorModules()` holds it in code.
- **Drop-ins add their own** through `FactoryPaths::allowModules` (IP4), by
  module name (`frei0r` for `libmltfrei0r.so`). The effects drop-in lists
  what its services come from (frei0r, sox, jackrack, ladspa, oldfilm,
  plusgpl, kdenlive, vidstab, rubberband, rnnoise, opencv), and openfx only
  behind its experimental preference after scanning every bundle for Qt.
  A drop-in's own module directories (`mltModuleDirs`) link as before.
- **The Qt denylist still wins:** a module matching `qt6` never links,
  even when listed.
- **Stale directories are swept:** a curated directory's name carries the
  owning process's id (`ustudio-mlt-modules-<pid>-<random>`). Each start
  removes ones whose process no longer runs, and old-form ones (no pid) more
  than a day old; FactoryPolicy's own cleanup can't run after SIGKILL or a
  crash (VE Effects counted 563 left in `/run/user/1000`).

## Consequences

- An MLT module we don't list can't run code in the editor, whatever is
  installed on the system. A test checks that no unlisted module is linked
  or mapped.
- A new MLT service the editor needs from a module not on the list means
  adding the module to `editorModules()` and CLAUDE.md in the same change.
  The required-services test fails first if it's missing.
- The Flatpak can drop the modules no one lists (VE Installers), and gets
  smaller.
- Drop-ins must list their modules. A service from an unlisted module
  simply isn't registered: the effects drop-in's registry and health scan
  report it as missing.
