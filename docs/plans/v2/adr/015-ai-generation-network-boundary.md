# ADR-015: AI generation runs in a separate, networked helper; the editor stays network-free

**Status:** Proposed (2026-09-24)

## Context
The owner wants AI image and video generation (doc 18) as a drop-in.
CLAUDE.md says the app makes no network requests; AI services are network
services. Flatpak grants network access per app, not per extension, so
network code inside the editor would require network access for the whole
editor.

## Decision
- All network code lives in `u-studio-generate`, a separate executable in
  `drop-ins/ai-generation/helper/`, shipped as its own Flatpak app
  (`com.ustudio.Generate`) with network access. The editor and its Flatpak
  keep no network access and link no network library.
- The editor side of the drop-in only launches the helper with context and
  imports finished files handed back through an exported GApplication
  action.
- libsoup 3, libsecret and json-glib are allowed in the helper only.
- Network traffic happens only on an explicit user action, with a
  confirmation of what leaves the machine; keys live only in the keyring.
- When accepted, CLAUDE.md's security section changes from "the app makes
  no network requests" to "the editor makes no network requests; the only
  network code is the AI generation helper (ADR-015)".

## Consequences
- The no-network property of the editor stays literal and testable (`ldd`,
  Flatpak manifest).
- Two apps to package for users who opt in.
- A small IPC surface (command-line context in, a GApplication action
  out) instead of in-process calls.
