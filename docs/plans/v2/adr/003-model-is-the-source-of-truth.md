# ADR-003: The project model is the source of truth

**Status:** Proposed

## Context
v1 stores timeline state only in MLT objects and reads it back to draw. That
blocks undo, save/load, testing without media, and makes every UI query take
the decode mutex. Kdenlive's model owns MLT objects directly and keeps them in
sync inline, which works but couples the model to MLT everywhere.

## Decision
`core/` holds a pure-C++ model (no MLT, no GTK) that is the only truth for
what the user edited. All mutations are Commands on the model. `engine/`
subscribes to model events and projects the model into an `Mlt::Tractor`.
Nothing reads timeline structure back out of MLT for display. Debug builds
verify the projection against the model after every change.

## Consequences
- Undo, serialisation, and property-based testing become straightforward.
- There is a sync layer to maintain (`EngineSync`); its correctness is
  guarded by the verifier and the random-command tests.
- Playback position and playing state are the one piece of runtime state
  that lives in the engine, exposed through `PlaybackController`.
