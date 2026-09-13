# 01 — Goals, scope, principles

## What v2 is

A **usable multi-track editor** for the common short-form workflow: import a
handful of clips, arrange them on a few video and audio tracks, trim, split,
add a transform/opacity/volume effect and a dissolve, add a text title, and
export an H.264/AAC MP4. With undo, save/load, and no crashes under ordinary
use. GNOME-native, GTK4 + libadwaita, MLT underneath, no Qt/KDE in the process.

Target user: the studio's own livestream/promo editing (the Unicorn Tears
brand context in `~/projects/unicorn-tears/claude-design-system`), which means
1080p and 4K sources, mixed frame rates (25/30/60), screen recordings, and
music tracks. Not a broadcast NLE.

## Goals (in priority order)

1. **Correctness of the edit.** What the timeline shows is what renders. The
   model is the truth; the engine is verified against it.
2. **Never lose work.** Undo everything, autosave, crash recovery.
3. **Honest playback.** A/V in sync, frame-accurate scrubbing, real-time
   preview that drops frames instead of slowing down.
4. **Testable without a display or media files.** Core logic runs in CI with
   synthetic MLT producers (`color:`, `noise`, `tone`).
5. **Stays small.** Every subsystem has one owner module. Prefer MLT features
   over reimplementation. Target ≤ 25k lines of C++ for the full v2 scope.

## Non-goals for v2

- Color management/HDR pipelines, scopes.
- Motion tracking, stabilisation UI, speech-to-text, AI features.
- Nested sequences (the model allows several sequences; the UI shows one).
- Plugin/extension API.
- Collaborative or cloud anything.
- Feature parity with kdenlive. It is the reference for MLT usage, not the
  target.

## Principles

- **Model → Engine → Screen, never backwards.** UI mutates the model through
  commands. The engine subscribes to model changes. The UI subscribes to model
  and engine events. No code path reads timeline state from MLT to display it
  (position/playback state are the one exception and come through a defined
  `PlaybackController` API).
- **One thread owns each thing.** The model and UI are main-thread-only. MLT
  services are touched from the main thread under the tractor lock; MLT's own
  consumer threads pull frames. Workers own their own `Mlt::Producer`
  instances and never share them.
- **Everything mutating is a Command.** If it isn't undoable, it isn't an
  edit. Preferences and view state (zoom, scroll) are not edits.
- **Debug builds verify, release builds trust.** Invariant checks on the model
  and a model-vs-engine reconciliation check run after every command in
  debug builds and in tests.
- **Runtime policy over link policy.** "No Qt" is enforced by what MLT loads
  at runtime and checked by a test, not by `ldd`.
- **Boring dependencies.** GTK4, libadwaita, GLib/GIO, MLT/mlt++, libxml2
  (already an MLT dep). Vendored: doctest single header. Nothing else without
  an ADR.
