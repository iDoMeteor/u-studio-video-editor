# 11 — Build, test, CI, packaging

## Meson layout

```
meson.build                 project(); options; deps; subdir('src'); subdir('data'); subdir('tests')
meson_options.txt           tests (feature), render_tool (bool), werror handled by -Dwerror
src/meson.build             subdir core/engine/app/render in that order
src/core/meson.build        libustudio_core = static_library(... , dependencies: [libxml2_dep])
src/engine/meson.build      libustudio_engine = static_library(..., dependencies: [mltpp_dep, mlt_dep, glib_dep], link_with: core)
src/app/meson.build         executable('u-studio-video-editor', ..., dependencies: [gtk4, adwaita, gio], link_with: [core, engine]) + gresource
src/render/meson.build      executable('u-studio-render', ..., link_with: [core, engine])
tests/meson.build           doctest dependency from subprojects; one executable per layer; test() entries
subprojects/doctest/       vendored doctest.h plus its meson.build (ADR-010), so offline builds work
data/meson.build            desktop file, metainfo, icons, gschema compile, install
```

Standard: **C++23** (`cpp_std=c++23`; GCC ≥ 13 or Clang ≥ 17). We use
`std::expected`, `std::jthread`, designated initialisers, ranges. Fedora 44
ships GCC 16; the Flatpak GNOME 49 SDK ships GCC 15. Ubuntu 24.04's GCC 13
compiles it (no `std::print` use; keep it out).

Warnings: `warning_level=3`, `-Wshadow -Wconversion -Wold-style-cast
-Wnon-virtual-dtor`, `werror=true` in CI only. GTK/GLib/MLT's C headers trip
`-Wold-style-cast` inside macros (`GTK_WIDGET`, `G_CALLBACK`,
`G_DEFINE_AUTOPTR_CLEANUP_FUNC`, ...) at every call site, not just where the
header is included, so a `GTK_INCLUDE_BEGIN/END` pragma pair around the
`#include` (as originally planned here) only silences the subset of these
that happen to expand during header parsing itself — verified empirically
during M0 to leave most call-site warnings in place. Instead every
third-party `dependency()` in the root `meson.build` is declared with
`include_type: 'system'`, which makes meson pass `-isystem` instead of `-I`;
GCC exempts warnings whose diagnostic traces back to a system header
(including macros defined there) regardless of where in our code they're
expanded. Confirmed clean under `meson setup -Dwerror=true`.

Dependency isolation is enforced in `src/app/meson.build` with a custom
target that greps the app sources for `mlt` and `pulse` includes and fails.
Cheap, and it stops the boundary eroding by accident.

## Tests

Framework: **doctest** (single header, MIT, vendored; ADR-010). Chosen over
GTest/Catch2 because nothing is installed here and doctest needs no build
step.

| Suite | Needs | What it covers |
|-------|-------|----------------|
| `tests/core` | nothing | model invariants; every command's apply/revert equality; composite command atomicity; undo stack merge/limit/clean-point; XML round-trip (`write(read(x)) == x` on fixtures); migrations; time/timecode math; id allocator. **Property test:** random sequences of valid commands on a random model, undo all, assert model equality; 10k iterations nightly, 200 in PR CI. |
| `tests/engine` | MLT installed, no display, no media files | `FactoryPolicy`: after init, `/proc/self/maps` contains no `libQt`; required services present. `EngineSync`: build from model with `color:`/`noise:`/`tone:` producers, run `verify()` after every command of the same random-command generator; sub-tractor dissolves; black track length. `PlaybackController` with `null` consumer: play → positions advance monotonically; pause → refresh shows exact frame; seek while paused; loop range; shutdown order under ASan. `MediaProbe` on generated files (`color:` rendered to a tiny MP4 in the test setup via avformat consumer). |
| `tests/render` | MLT | each preset renders a synthetic project; output probed (doc 10). |
| `tests/app` | GTK; `GDK_BACKEND` needs a display | Smoke only: construct the window offscreen under `xvfb-run` or `weston --backend=headless` in CI; open a fixture project; assert no critical GLib warnings (`G_DEBUG=fatal-criticals`). UI logic that matters (`TimelineController` state machine, `Viewport` math, snapping) is plain C++ and tested in `tests/core`-style with fake input events, no GTK. |

Sanitisers: a CI job builds with `-Db_sanitize=address,undefined` and runs
`tests/core` and `tests/engine`. MLT has some known leaks at `Factory::close`;
use an `LSAN_OPTIONS=suppressions=tests/sanitizers/lsan.supp` file rather than
disabling leak checks.

Test data: **no binary media in the repo.** Every media-needing test
generates its input with MLT generators at setup. Project fixtures are XML.

## CI (GitHub Actions; adapt if the repo lands elsewhere)

```
jobs:
  build-test:     container: fedora:44
                  dnf install gcc-c++ meson ninja-build gtk4-devel libadwaita-devel mlt-devel libxml2-devel xorg-x11-server-Xvfb
                  meson setup b -Dwerror=true -Dbuildtype=debugoptimized
                  meson compile -C b
                  xvfb-run -a meson test -C b --print-errorlogs
  sanitizers:     same, -Db_sanitize=address,undefined, core+engine suites only
  format:         clang-format --dry-run --Werror $(git ls-files '*.cpp' '*.h')
  no-qt-policy:   run b/tests/engine/test_factory_policy  (also part of build-test; listed separately so a failure is loud)
  flatpak:        (from M7) flatpak-builder with the manifest; artifact the bundle
```

A `justfile` (or `Makefile`) at the root wraps `setup`, `build`, `test`,
`run`, `fmt`, `flatpak` so local and CI invocations are identical.

## Packaging

- `data/com.ustudio.VideoEditor.desktop`, `.metainfo.xml` (AppStream, with
  screenshots later), scalable + symbolic icons, `gschema.xml`. Installed by
  meson; `meson install` works into a prefix.
- GResource: CSS, effect catalogue JSON, render presets, any `.ui` files.
  Loaded at startup; no runtime file lookups for our own assets.
- **Flatpak** (M7): `com.ustudio.VideoEditor.json` on `org.gnome.Platform//49`.
  MLT is built as a module in the manifest with `-DMOD_QT6=OFF
  -DMOD_GLAXNIMATE_QT6=OFF` and the modules we need on. This makes the "no
  Qt" policy structural in the shipped artifact; `FactoryPolicy`'s curated
  dir remains for distro/system builds. Fonts (Space Grotesk, JetBrains Mono,
  Anton) are bundled as modules from their upstream releases (OFL) and
  registered with Fontconfig via the Flatpak's font dirs. SDL2 (for
  `sdl2_audio`) and ffmpeg come from the runtime/extension
  (`org.freedesktop.Platform.ffmpeg-full`).
- Version from `meson.project_version()` into a generated `config.h`; the
  About dialog reads it.

## Local dev loop

```
just setup      # meson setup builddir -Dbuildtype=debug -Dtests=enabled
just build
just test       # meson test -C builddir
just run        # ./builddir/src/app/u-studio-video-editor
just check-qt   # runs the factory-policy test only
```

Debug builds enable `Model::check()` after every command and
`EngineSync::verify()` after every engine event (behind `USTUDIO_VERIFY=0` to
turn off when profiling).

> REVIEW: Claude (2026-09-24): not built yet. Today `verify()` runs in tests only (`engine-sync`
> runs it after each of 500 commands and their undos), `check()` runs before save,
> and `USTUDIO_VERIFY` doesn't exist.
