# Testing

[Docs home](../README.md) › [Developer docs](README.md) › Testing

All tests are [doctest](../plans/v2/adr/010-doctest-vendored.md) suites
under `tests/`, registered with meson and run with `just test` (or
`meson test -C builddir --print-errorlogs`). Run one suite by its meson
name, for example `meson test -C builddir engine-sync`.

## Layout

| Folder | Meson names | Needs |
|---|---|---|
| `tests/core/` | `core`, `core-undo-fuzz`, `core-thread-pool`, `core-model-release` | Nothing but the compiler: pure C++ |
| `tests/engine/` | `engine-*` (sync, playback controller, render, A/V sync, XML playback, caches, proxies, transform, mixed rates, …) | MLT, but no display and no media files |
| `tests/app/` | `app-*` (timeline controller and renderer, viewport, queues, settings, autosave, UI hints, …) | GTK for some; most test logic that was kept out of widgets |
| `tests/dropins/` | `dropins`, `dropin-*` | The drop-in build options (`just dropins-builtin`, `just dropins-module`) |
| `drop-ins/<name>/tests/` | `titles-*` | A drop-in's own tests, built only with its `-Ddropin_<name>` option on (ADR-013: the folder is self-contained) |
| `tests/sanitizers/` | — | LeakSanitizer suppressions for `just asan` |
| `tests/common/` | — | Shared helpers, such as the random command stream for property tests |

## Rules

The full rules are in [CLAUDE.md](../../CLAUDE.md), "Testing discipline".
The short version:

- **No binary media in the repo.** Tests generate their inputs with MLT
  generators (`color:`, `noise:`, `tone:`). Renders during a test go to a
  temporary path, never next to source media.
- **Behaviour changes ship with a test** in the same change when practical.
- **Build it and exercise it** before claiming a change works. Run the app
  for UI changes, or a standalone repro for engine changes.
- **A red test you didn't write** may encode a planned API. Ask before
  changing production code to make it pass.
- **Sanitizers** (`just asan`, `just tsan`) are required for thread and
  MLT-lifetime changes, in the tiers CLAUDE.md sets out. See
  [Building](building.md#sanitizers).

## The titles designer's smoke test

`tools/titles-smoke/run.sh <builddir> <outdir>` designs a lower third in
`u-studio-titles` from a blank canvas with the mouse and keyboard only
(doc 16, T2 acceptance), saves it through the Save dialog and checks the
saved title: three layers, the typed text, the bar's place and size, the
brand's fonts and gradient. It runs on a private Xvfb with its own D-Bus
session and AT-SPI bus, so nothing shows on the desktop; screenshots of
each step land in `<outdir>`. It needs a build with `-Ddropin_titles`, Xvfb,
python3 with `gi` (Atspi) and python-xlib, and ImageMagick's `import`.
Notes on driving GTK dialogs there are in
[the titles notes](notes/titles.md).

## Notable tests

- **Undo property test** (`core-undo-fuzz`, the same binary filtered to
  this one test: about 43 s alone, so it has its own 180 s timeout and
  `core` keeps 30 s): 10,000 random commands, undo them all,
  and the model must equal the start.
- **`EngineSync::verify()`** (`engine-sync`): after each of the first 500
  commands of that same stream, and each undo, the MLT graph must match
  the model.
- **A/V sync** (`engine-av-sync`): a generated beep and flash must line up
  in playback and render.
- **Project files play in `melt`** (`engine-xml-playback`): a saved
  project plays through MLT's `xml` producer frame for frame like the
  editor.
- **Timeline draw speed** (`app-timeline-render`): 10 tracks × 500 clips
  draw in under 4 ms.
- **GPU pipeline** (`engine-gpu-pipeline`, `engine-gpu-engine`,
  `engine-gpu-probe`, `app-gpu-acceleration`): the GPU graph against the
  CPU one scene by scene, playback through the real consumer on the GPU and
  back, the probe, and the editor's startup decisions with a fake render
  tool. The engine ones need a GL driver (EGL); without one they print a
  message and pass, so run them on a machine with a GPU before landing GPU
  work. `engine-gpu-probe-no-egl` checks the clean failure everywhere.
- **Playback soak**: `tests/engine/playback_soak.cpp` is a manual tool for
  long playback runs. `--gpu` plays on the GPU pipeline, `--hwdecode` adds
  VAAPI, `--no-rotation` leaves the transformed tracks unrotated
  (ADR-019). Judge memory by its "RSS growth after warm-up" line, not
  by the first report: the first minute is warm-up (see the
  [playback notes](notes/playback-engine.md)).

The acceptance criteria each test backs are listed per milestone in
[v2 doc 12](../plans/v2/12-roadmap-and-milestones.md).
