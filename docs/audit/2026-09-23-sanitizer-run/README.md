# Sanitizer run artifacts (2026-09-23)

Scripts used for [the sanitizer report](../2026-09-23-sanitizer-report.md),
kept so the run can be repeated after fixes. Nothing here is built by meson.

| File | Purpose |
|---|---|
| `mkmedia.cpp` | Renders a 10 s 1080p30 H.264/AAC test clip (`colour` + `tone`, burnt-in `timer`) with MLT |
| `mkproject.cpp` | Builds a 3-track project with the core commands (two clips with a dissolve, a still on V2, an audio clip) and saves it as an untitled autosave |
| `session.sh` | Runs inside `dbus-run-session`; starts the AT-SPI bus and registry by hand (bus activation of both is refused there), then the driver |
| `drive.py` | Launches the app, clicks Recover via AT-SPI, drives transport and edits through the window's exported `org.gtk.Actions`, then closes the window |
| `rebuildrepro.cpp` | Standalone repro for finding S1: RSS growth over 300 tractor rebuilds with `Tractor::field()` leaked versus deleted |

Build the helpers against MLT (`pkg-config mlt++-7`) and, for
`mkproject.cpp`, the core sources plus `libxml-2.0`. The report's "How to
rerun" section has the exact commands and environment.
