# ADR-009: Export runs in a child process from a saved snapshot

**Status:** Proposed

## Context
Rendering in-process shares the editor's MLT factory, threads, and address
space with a running preview. Encoder or demuxer crashes take the editor
down, long renders freeze editing, and testing requires a GTK app. MLT
already supports rendering any project file headlessly.

## Decision
The editor saves a snapshot of the project to a temp file and launches
`u-studio-render` (our own small CLI built from `core` + `engine`) via
`GSubprocess`, parsing JSON progress lines from its stdout. The CLI loads
the file with MLT's `xml` producer and renders with `avformat` using preset
files. The same CLI generates proxies. Cancel is `SIGTERM`.

## Consequences
- What renders is exactly what was saved (ADR-004 makes this testable).
- Editing continues during export; crashes are isolated.
- Two binaries to install; the app must locate the CLI (same `bindir`, or
  `USTUDIO_RENDER_BIN` override).
- Render presets are plain MLT `.properties` files, shareable with `melt`.
