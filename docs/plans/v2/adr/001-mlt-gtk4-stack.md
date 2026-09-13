# ADR-001: MLT engine, GTK4 + libadwaita toolkit, C++23

**Status:** Proposed (records the v1 decision, adds the language standard)

## Context
v1 chose MLT for playback/rendering and GTK4/libadwaita for a GNOME-native
UI, explicitly not porting kdenlive. MLT 7.40 is installed with the modules
we need. The alternative engine (GStreamer + GES) is more GNOME-aligned but
GES's editing model is less mature for multi-track compositing with effects,
and the team already has MLT working.

## Decision
Keep MLT (via mlt++) as the only media engine and GTK4 + libadwaita as the
only toolkit. Code is C++23 (GCC ≥ 13 / Clang ≥ 17). No Qt, no KDE
Frameworks, no GStreamer in the process.

## Consequences
- MLT's quirks (unreliable return codes, thread-safety rules) are ours to
  wrap, in one layer (`engine/`).
- We inherit MLT's XML as a natural project format (ADR-004) and its
  consumers for playback (ADR-002) and export (ADR-009).
- The "no Qt" goal needs a runtime policy, not just a link check (ADR-007).
