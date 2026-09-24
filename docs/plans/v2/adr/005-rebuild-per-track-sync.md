# ADR-005: Engine sync rebuilds a whole track per change

**Status:** Proposed

> REVIEW: Claude (2026-09-24): the code goes further than this ADR: every change rebuilds the
> whole tractor, not one track; there is no tractor lock (the consumer is
> stopped, the tractor swapped and the consumer restarted); and `verify()` runs
> in tests only. It should be amended, or superseded, to match.

## Context
Keeping an `Mlt::Playlist` in sync with the model can be done incrementally
(insert_at/remove/resize/move, handling blanks) or by rebuilding. Incremental
is what kdenlive does and is the source of a long tail of subtle bugs
(blank merging, off-by-one on resize, mix sub-tractors). Rebuilding a
playlist of cuts is cheap: cuts don't reopen files.

## Decision
On any model event that touches a track, `EngineSync` clears that track's
playlist under the tractor lock and re-appends blanks and cuts (and dissolve
sub-tractors) from the model. Transactions coalesce to one rebuild per track
per batch. A verifier compares playlist entries to the model in debug builds
and tests. Incremental updates may be added later behind the same interface
only if profiling shows the rebuild on the critical path.

## Consequences
- The sync layer is small and always consistent by construction.
- Cost is O(clips on the track) per edit, sub-millisecond for typical
  tracks; a known ceiling for pathological timelines (risk R5).
- Master producers must outlive rebuilds; only cuts are recreated. Filters
  are re-attached from the model on every rebuild (cheap).
