# Bug audit follow-up, 2026-09-22

Read-only audit of everything that landed between `ba2d011` (the base of
the 2026-09-20 audit) and `0abaf3e` (v0.10.0): 17 commits, about 4,900
added lines across core, engine, app and tests. Every changed line under
`src/` was read; tests were checked only for coverage of the findings.
Nothing was changed. Severity ordering and the "verified" / "by
inspection" labels follow the first audit. The three repros ran against the
installed MLT 7.40 from the scratchpad; the owner's `builddir` is
`buildtype=debug` with asserts on, which decides crash-vs-UB for T1.

## Summary

| ID | Severity | Area | One line |
|---|---|---|---|
| T1 | High | core/app | No edit command knows about transitions: deleting, splitting, moving or trimming a linked clip leaves a dangling or inconsistent transition, which crashes the next right-click or drag on that row and makes the saved project unloadable |
| T2 | Medium | core/engine | A clip can be the `b` of one dissolve and the `a` of another with combined length beyond its own; the engine then lays the track out wrong |
| E1 | Medium | engine | Pause still lands about a prefetch-buffer late (reported 2026-09-20, unchanged) |
| E2 | Medium-low | engine | Thumbnails are decoded through MLT's default 720x576 4:3 profile: letterboxed, non-square pixels (verified) |
| A1 | Medium-low | app | New Project / Open after a Recover keeps the pending cleanup path, so a later Save of a different project deletes the recovered autosave |
| C1 | Low | core | `extendAssetLength` is never reverted, so undo of an import or trim leaves the model different from before |
| E3 | Low | engine | `hasAudio` is still "not a still image", though the probe now decodes a frame and could ask |
| A2 | Low | app | Reload and New Project discard unsaved edits with one click and no confirmation |
| A3 | Low | app | Autosave owner check treats a reused pid as "alive" |
| A4 | Low | app | `refreshMediaBrowser()` rebuilds the whole grid once per finished thumbnail |

## T1. Transitions are not maintained by any other edit (High, by inspection)

`AddTransition` and `RemoveTransition` are the only commands that look at
`Sequence::transitions`. Everything else mutates linked clips as if the
transition did not exist:

- `Model::removeClip` ([model.cpp:310](../../src/core/model/model.cpp#L310))
  and `Model::removeTrack` ([model.cpp:228](../../src/core/model/model.cpp#L228))
  leave a `Transition` whose `a` or `b` no longer exists.
- `Model::splitClip` ([model.cpp:356](../../src/core/model/model.cpp#L356))
  keeps the left half's id. Splitting `a` before the overlap leaves the
  transition pointing at the left half while the new right half is the
  clip that actually overlaps `b`. Splitting inside the overlap shortens
  `b` below the transition length.
- `MoveClip`, `ResizeClip` and `SplitAudio` ([primitives.cpp:243](../../src/core/commands/primitives.cpp#L243),
  [298](../../src/core/commands/primitives.cpp#L298)) have no transition
  checks either. Trimming `b`'s end shorter than the overlap, or moving
  `b` elsewhere, leaves both clips with their extended handles and a
  stale record.

Consequences, in the order a user meets them:

1. **Crash on the next right-click or drag in that row.**
   `onTimelineRightClicked` ([app_window.cpp:1245](../../src/app/app_window.cpp#L1245))
   and `onTrackDragBegin` ([app_window.cpp:1623](../../src/app/app_window.cpp#L1623))
   call `m_model.clip(t.a)` / `clip(t.b)` for every transition on the
   row with no `hasClip` guard. Only the draw path (line 2087) guards.
   With a dangling id that is an `assert` abort in the owner's debug
   build, and undefined behaviour (an end-iterator dereference) in a
   release build. Steps: two touching clips, Add Transition, right-click
   the first clip, Delete Clip, then right-click anywhere on that row.
2. **The project saves but will not reopen.** `saveProject` does not run
   `check()`, and `loadProject` now refuses any file that fails it
   ([reader.cpp:524](../../src/core/xml/reader.cpp#L524)). A dangling
   transition fails invariant 6, a split `a` fails the overlap rule
   (previous clip is no longer `t.a`), and a shortened `b` fails
   "longer than the shorter clip". The user sees "invalid project" on
   their own file, with Reload offering no way out.
3. **The engine lays the track out wrong.** `planTrackSegments`
   ([engine_sync.cpp:216](../../src/engine/engine_sync.cpp#L216)) only
   matches a transition when `t.a` and `t.b` are the two adjacent clips.
   After a split of `a`, the new right half is appended at full length
   and `b` starts before the cursor, so no blank is inserted and every
   later entry on the track shifts by the overlap length. There is no
   dissolve, and `verify()` (not run by the app) would report it.

Undo heals cases 1 and 3 because `RemoveClip::revert` restores the clip,
but nothing heals a saved file. There is no test covering any edit on a
transition-linked clip (`tests/core/test_commands.cpp` has none).

Fix, in order of preference: make the clip commands transition-aware by
composing with `RemoveTransition` first (delete, split, move, trim on a
linked clip removes the transition, shrinking both clips back, then
performs the edit), and have `Model::removeTrack` drop that track's
transitions. As a stop-gap: guard the two app loops with `hasClip`, run
`check()` before `saveProject` and refuse with a message, and add a
`RemoveClip`+`AddTransition` round-trip test.

## T2. Stacked dissolves on one clip (Medium, by inspection)

`AddTransition::apply` ([primitives.cpp:441](../../src/core/commands/primitives.cpp#L441))
validates one transition against the two clips but not against the
transitions those clips already have. A middle clip of length 20 can take
a 15-frame dissolve from the previous clip and a 15-frame dissolve into
the next; each passes `length <= min(clip lengths)`, and `check()` passes
too. In `planTrackSegments` the middle clip's exclusive span becomes
negative and is omitted (line 259), but the outgoing transition segment
starts at `clip.end() - 15`, which is before the cursor, so it is appended
late and the track drifts by the difference. Fix: in `AddTransition`,
require `incomingLength + newLength <= a.length()` on `a` and the
mirror on `b`, and add the same rule to `check()`.

## E1. Pause overshoot (Medium, verified 2026-09-20, unchanged)

`pause()` ([playback_controller.cpp:195](../../src/engine/playback_controller.cpp#L195))
records the last displayed frame, purges, and sets `refresh`, but never
seeks the tractor back to that frame. The producer's position is ahead by
the prefetch depth (`buffer=25` plus the audio buffer), so the frame that
shows after a pause is about 30 frames past the one the user saw, and
`setTractor()` compounds it on every edit. The 2026-09-20 repro measured
52 to 83. Kdenlive's pattern is purge, then `seek(displayedPosition)`,
then refresh. Note the new `set("refresh", 0)` in `play()` (line 188)
works because MLT fires `property-changed` on every set, which is what
wakes the consumer thread. The value written is irrelevant, so the
comment's "latched" explanation is not what is happening, but the fix is
correct.

## E2. Thumbnails decode at 720x576 4:3 (Medium-low, verified)

`ThumbnailCache::workerMain` ([thumbnail_cache.cpp:81](../../src/engine/thumbnail_cache.cpp#L81))
opens the file under `Mlt::Profile()`, which is MLT's default `dv_pal`
(720x576, sample aspect 16:15, display 4:3). The loader's normalising
filters scale and pad the frame to the profile, so `get_image` returns
720x576 with black bars. Repro on a rendered 1920x1080 red clip:

```
default profile: 720x576 sar 16/15 dar 4/3
get_image -> 720x576  meta.media=1920x1080
row 2   centre pixel (0,0,0)      <- bar
row 144 centre pixel (255,24,0)
row 573 centre pixel (0,0,0)      <- bar
```

The nearest-neighbour downsample then keeps the bars and the 16:15 pixel
stretch, so every 16:9 thumbnail is a squashed 4:3 tile. Fix: after the
first `get_frame`, read `meta.media.width/height` and set the profile's
width, height and square sample aspect before decoding the thumbnail
frame, or open under the sequence profile as the waveform job now does.

## A1. Pending autosave cleanup outlives the recovered project (Medium-low, by inspection)

`m_pendingAutosaveCleanupPath` is set on Recover
([app_window.cpp:2555](../../src/app/app_window.cpp#L2555)) and consumed
by the next successful Save. `onNewProjectClicked`
([app_window.cpp:799](../../src/app/app_window.cpp#L799)),
`onOpenProjectFinished` (line 742) and `onReloadProjectClicked` (line
775) do not clear it. Recover, then New Project (or Open), then Save that
other project: the recovered autosave, still the only copy of that work,
is deleted. Fix: clear both pending paths whenever `m_model` is replaced
by anything other than a Recover.

## C1. `extendAssetLength` has no inverse (Low, by inspection)

`InsertClip::apply` and `ResizeClip::apply` call
`Model::extendAssetLength` ([model.cpp:197](../../src/core/model/model.cpp#L197))
for boundless assets, but their `revert()` does not restore the previous
length. After import-then-undo, or extend-then-undo, the asset's recorded
length stays extended, which breaks the "revert restores the model
bit-for-bit" contract in `command.h` and makes the writer emit a longer
producer `out`. Harmless to playback. Fix: capture the old length in
`apply()` and restore it in `revert()`.

## E3. `hasAudio` still guessed (Low, by inspection)

`probeMedia` ([engine_sync.cpp:141](../../src/engine/engine_sync.cpp#L141))
now decodes a frame to read fps and size but still sets `hasAudio` to
"not a still image". A video with no audio stream gets waveform jobs and
a "Split Audio" menu item that produces a silent clip. After the
`get_frame`, `producer.get_int("audio_index")` is -1 for no audio
(avformat sets it once probed); verify against the module YAML before
relying on it.

## A2. Reload and New Project have no confirmation (Low)

Both header-bar buttons sit next to Open and Save and replace the model
immediately. Autosave keyed on the old path usually leaves a recoverable
copy, but a project that was never saved loses everything since its last
autosave. A one-line "Discard unsaved changes?" alert when `!isClean()`
would match the recovery dialog's pattern.

## A3. Autosave owner check and pid reuse (Low)

`ownerAlive` ([autosave.cpp:57](../../src/app/autosave.cpp#L57)) treats
any live process with the recorded pid as the owner. After a crash, an
unrelated process can hold that pid for a long time, and the autosave is
then never offered. Recording the process start time alongside the pid
(from `/proc/<pid>/stat` field 22) and comparing both closes it.

## A4. Media browser rebuild per thumbnail (Low)

`onThumbnailReady` calls `refreshMediaBrowser()`
([app_window.cpp:2145](../../src/app/app_window.cpp#L2145)), which
destroys and recreates every row, once per finished thumbnail, whether or
not the panel is visible. With N assets that is N full rebuilds on
import of a project. Updating only the finished row's `GtkPicture`, or
skipping when the panel is hidden, is enough.

## Checked and found sound

- The 2026-09-20 fixes: profile lifetime in `reset()`, `Signal`
  non-copyable with hand-written `Model` copy/move, `EngineSync`
  disconnect in its destructor, `InsertClip`/`ResizeClip` rejecting a
  negative `in`, `mkdtemp` module directory with a separate scan error
  code, loop wrap gated on playing forward, waveform fps keyed cache and
  striding, undo merge blocked at the clean point, `markDirty()`, the
  A3 gap start, A4 audio-track handling in `InsertClip`/`MoveClip`,
  A6 render `.part` plus rename and asset-path refusal, A7 focusable
  flags, single-instance re-activation.
- `AddTransition`/`RemoveTransition` are exact inverses and the resize
  drag composes them correctly, including the fallback to plain
  `RemoveTransition` at zero length.
- `buildTransitionSubTractor` cut ranges match `planTrackSegments`, and
  the missing-media black placeholder keeps clip timing intact.
- Reader self-heal of `next_id`, the fps guard, and the JSON bounds
  fixes.

## Not done

- No build or test run (read-only audit; the only `builddir` is the
  owner's). Repros were standalone programs in the scratchpad.
- The double-click path was not exercised: the drag gesture still claims
  presses that land on a clip, so whether `GtkGestureClick` delivers
  `n_press == 2` afterwards depends on GTK's denial semantics. The owner's
  own report in commit `a1d9a4e` suggests it does, so it is not listed as
  a finding.
