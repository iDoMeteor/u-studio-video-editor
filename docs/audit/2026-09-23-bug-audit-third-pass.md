# Bug audit, third pass, 2026-09-23

Read-only audit of everything between `0abaf3e` (the base of the
2026-09-22 follow-up) and `560a077` (13 commits, about 2,000 added lines):
the T1/T2 transition fixes, `RemoveAsset` with media-browser remove,
delete and drag-to-timeline, the A1 to A4 and C1, E2 and E3 fixes,
recovery ordering, new step shortcuts and the timeline playhead. Every
changed line under `src/` was read. Nothing in the repo was changed.
The final section lists low-effort enhancements, as requested.

Labels: **verified** means a standalone program compiled against the real
`src/core` sources (or GTK's own source, for A1) confirmed it; **by
inspection** means the code path was read end to end. The core repro
lives in the session scratchpad as `corerepro.cpp` and needs only
`g++ -std=c++23 -Isrc` plus `model.cpp` and `primitives.cpp`.

## Summary

| ID | Severity | Area | One line |
|---|---|---|---|
| A1 | High | app | Single-key shortcuts fire while typing a track or clip name, so letters like a, d, f, i, j, k, l, o and s never reach the entry and trigger playback or edits instead |
| C1 | High | core | `RemoveAsset` deletes clips without stripping their dissolves, recreating the dangling-transition state T1 fixed |
| C2 | Medium | core | Trimming the far edge of a dissolved clip is always refused |
| A2 | Medium | app | Closing the window with unsaved changes quits with no prompt and no final autosave |
| C3 | Low-medium | core/app | A clip drag that doesn't move the clip still runs `MoveClip`, silently removing its dissolve |
| C4 | Low | core | Edits on one end of a clip strip the dissolve on its other end too |
| E1 | Medium | engine | Pause still lands about a prefetch buffer late (carried over from 2026-09-20, unchanged) |
| A3 | Low | app | "Delete File" is a permanent unlink, not a move to Trash |
| A4 | Low | app | Open Project replaces unsaved work without the new discard prompt |
| A5 | Low | app | The timeline redraws completely on every displayed frame during playback |

## A1. Shortcuts swallow typing in the name editor (High, verified against GTK source)

`installActions` ([app_window.cpp:589](../../src/app/app_window.cpp#L589))
binds bare letters (`a`, `d`, `f`, `i`, `j`, `k`, `l`, `o`, `s`), plus
`Left`, `Right`, `Home`, `End` and their Ctrl/Alt variants, through
`gtk_application_set_accels_for_action`. GTK installs those on the window
in a shortcut controller with `GTK_PHASE_CAPTURE` and
`GTK_SHORTCUT_SCOPE_GLOBAL` (`gtk_window_set_application` in
`gtk/gtkwindow.c`, main branch). Capture runs from the window down before
the focused widget sees the key, and there is no editable-widget
exception. The inline name editor's `GtkEntry` sits under the timeline in
the propagation chain ([app_window.cpp:2580](../../src/app/app_window.cpp#L2580)),
so typing "Cheezus promo" into it:

- drops the `s` and `o` and moves the active track and loop-out instead;
- a typed `l` starts playback, `k` pauses it, `j` plays in reverse;
- `Left`, `Right`, `Home` and `End` move the playhead instead of the text
  cursor.

Not exercised by input injection (none available here), so the exact
symptom is inferred from GTK's documented dispatch order. Fix: disable the
transport actions while the editor is open (`g_simple_action_set_enabled`
on popup, re-enable in `onInlineNameEditClosed`), or move these shortcuts
to a `GtkShortcutController` on the timeline widget in the bubble phase,
so a focused entry gets first refusal.

## C1. `RemoveAsset` leaves dangling dissolves (High, verified)

`RemoveAsset::apply` ([primitives.cpp:71](../../src/core/commands/primitives.cpp#L71))
calls `Model::removeClip` directly for every clip that uses the asset. It
does not go through `stripTransitionsInvolvingClip`, which every other
clip-removing command now does. Repro output:

```
after AddTransition          transitions=1 check=ok
RemoveAsset(X) ok=1
after RemoveAsset(X)         transitions=1 check=transition 7 references a missing clip
```

User path: right-click an asset in the media browser, choose Remove from
Project (or Delete File), where one of its clips has a dissolve. The
app-level `hasClip` guards added last round stop the crash, but:

- the other clip keeps its dissolve extension, so it plays extra
  head or tail frames;
- Save refuses with "internal inconsistency ... please report it";
- autosave still writes the file, and recovery then fails to load it;
- with Delete File, the source file is already gone when the user finds
  out.

Undo does restore a consistent model, because the clip comes back
extended and the transition record was never removed. Fix: call
`stripTransitionsInvolvingClip` for each captured clip before capturing
it (same capture-after-strip rule as the other commands) and
`restoreTransitions` on revert. Add the repro above as a doctest.

## C2. Far-edge trims on dissolved clips are always refused (Medium, verified)

`ResizeClip::apply` ([primitives.cpp:462](../../src/core/commands/primitives.cpp#L462))
runs `isRangeFree` against the neighbour's current, dissolve-extended
span. The two clips of a dissolve overlap by design, so any resize whose
range still starts at the clip's current position overlaps the partner.
With A (0 to 110 after a 10-frame dissolve) and B (100 to 200):

```
trim B tail by 5 (far from the dissolve): ok=0
trim A head by 5 (far from the dissolve): ok=0
```

The user sees "Can't trim the clip that far — move the next clip out of
the way first", which is wrong on both counts. This existed since
dissolves landed; the T1 work did not introduce it. Fix: ignore the
dissolve partner in the range check (it may overlap by exactly the
transition length), or check against post-strip geometry the way
`MoveClip` now does with `transitionExtensionOf`.

## A2. No unsaved-changes prompt on close (Medium, by inspection)

There is no `close-request` handler anywhere in `src/app`. Closing the
window while the title shows the dirty mark quits immediately.
`prepareForShutdown` stops the consumer but does not autosave, and the
heartbeat only fires two minutes after the last edit. Up to two minutes of
work is lost silently, the same class of loss the new Reload and New
Project prompts were added to prevent. Fix: handle `close-request`,
reuse `confirmDiscardIfDirty` (with a Save option), and call
`performAutosave()` in `prepareForShutdown` when dirty.

## C3. A zero-distance clip drag strips its dissolve (Low-medium, verified)

`onTrackDragEnd` ([app_window.cpp:1863](../../src/app/app_window.cpp#L1863))
treats a drag as a move whenever either offset exceeds 3 pixels. A small
vertical wobble inside the same row produces `MoveClip` to the clip's own
track and position. `MoveClip::apply` accepts it and strips the dissolve:

```
no-op MoveClip(A): ok=1, transitions left=0
```

It is undoable, but nothing tells the user the dissolve vanished. Fix:
skip the command in `onTrackDragEnd` when the preview track and start
equal the original, and have `MoveClip::apply` refuse a move to the
current track and position.

## C4. Edits strip dissolves on the untouched end (Low, verified)

`stripTransitionsInvolvingClip` removes every transition on a clip,
incoming and outgoing. Splitting B at frame 180, eighty frames away from
its dissolve with A, removes that dissolve:

```
split B at 180: ok=1, transitions left=0
```

The same applies to `ResizeClip` once C2 is fixed. Documented as
intended in `primitives.h`, but it is a surprising loss for a split or
trim nowhere near the overlap. Fix: strip only the transition on the edge
being changed; for a split, keep the incoming one on the left half and
the outgoing one on the right half.

## E1. Pause overshoot (Medium, carried over)

`pause()` still purges and refreshes without seeking the tractor back to
the displayed frame, so it lands about 30 frames late and `setTractor()`
compounds that on every edit. Details and measurements are in the
2026-09-20 and 2026-09-22 documents.

## A3. "Delete File" is permanent (Low, by inspection)

`onDeleteAssetFileClicked` ends in `std::filesystem::remove`
([app_window.cpp:2477](../../src/app/app_window.cpp#L2477)). The dialog
says so, but on a desktop the expected behaviour is Move to Trash, and
CLAUDE.md treats the owner's footage as the thing never to lose.
`g_file_trash()` is a GIO call already in the dependency set.

## A4. Open Project skips the discard prompt (Low)

`onOpenProjectClicked` ([app_window.cpp:790](../../src/app/app_window.cpp#L790))
opens the file dialog and replaces the model on selection. The code
comment calls the dialog an implicit confirmation, but choosing a file to
open is not agreeing to discard the current edits. Wrapping it in
`confirmDiscardIfDirty` costs one line.

## A5. Full timeline redraw per frame (Low, by inspection)

`refreshTransport` ([app_window.cpp:2694](../../src/app/app_window.cpp#L2694))
now queues a full timeline redraw on every displayed frame so the
playhead moves. Each redraw builds Pango layouts for every label, and
`drawWaveform` takes the waveform cache mutex and builds a key string per
clip. That is about 30 full redraws a second on a 60-minute project.
Fine today; worth a playhead overlay widget, or redrawing only when the
playhead moves at least one pixel, before the timeline grows.

## Checked and found sound

- T1: strip-before-capture ordering in `RemoveClip`, `MoveClip`,
  `ResizeClip`, `SplitClip` and `RemoveTrack`, batching, and the split
  refusal path that restores the transition.
- T2: the chain-length rule in `AddTransition` and its mirror in
  `check()` match `planTrackSegments`.
- C1 (previous): asset-length restore only when nothing else has cut
  further.
- E2, E3 (previous): thumbnail profile reconfiguration and
  `audio_index` with the `property_exists` guard.
- A1 to A4 (previous): pending-cleanup clearing on New, Open, Reload;
  discard prompts; `/proc/<pid>/stat` field 22 parsing past the last `)`;
  hidden-panel skip.
- Recovery: newest-first selection and the offered-set loop.
- Save now refuses a model that fails `check()`.

## Not done

- No build or test run; the only `builddir` is the owner's.
- A1 was not exercised with real key input.

## Low-hanging enhancements

Each of these is small (roughly an hour or less), touches only
`src/app/app_window.cpp` unless noted, and needs no new dependency.

**Keyboard and saving**

1. **Space for play/pause.** There is no accelerator for toggling play;
   Space is what every editor uses. One `addAction` line, subject to the
   A1 fix.
2. **Ctrl+S saves in place.** Save always opens the file dialog. When
   `m_currentProjectPath` is set, save straight there and keep the dialog
   for Ctrl+Shift+S. Add Ctrl+O, Ctrl+N and Ctrl+I for Open, New and
   Import at the same time.
3. **Delete key removes the selected clip.** `m_selectedClip` already
   exists; route it to `RemoveClip`.
4. **Project name in the title bar.** `updateWindowTitle` shows only the
   app name. Use the file's base name, keeping the dirty mark.

**Import and media**

5. **Multi-file import.** Swap `gtk_file_dialog_open` for
   `gtk_file_dialog_open_multiple` and loop the existing import body.
6. **File dialog filters.** Add "Media" (video, audio, image MIME types)
   for Import and `*.ustudio` for Open, so a stray text file is never
   offered.
7. **Drop files from the file manager.** Add a `GDK_TYPE_FILE_LIST`
   `GtkDropTarget` on the timeline and media browser, reusing the import
   path. Drag-to-timeline for bin assets already exists next to it.
8. **Double-click a media-browser row to insert at the playhead** on the
   active track, reusing `onTimelineDrop`'s body.
9. **Trash instead of delete** (A3 above), with `g_file_trash`.

**Timeline**

10. **Snap to clip edges and the playhead** while moving or trimming. The
    cut boundaries already come from `cutBoundariesOnActiveTrack`; snap
    the preview frame when within a few pixels.
11. **Click-drag on empty timeline space to scrub**, instead of only
    click-to-seek. The drag gesture already falls through for empty
    space.
12. **Timecode ruler** along the top of the timeline, drawn with the
    existing `drawLabel` helper at whole-second or whole-minute ticks
    depending on scale.

**Feedback**

13. **Render progress.** Rendering reports only start and finish. Poll
    the render consumer's position against the tractor length from a
    `g_timeout_add` and show a percentage in the status bar.
14. **Log play and pause at debug level** in `PlaybackController`. The
    last playback investigation had logs with no record of whether Play
    was pressed.
15. **Recent projects** through `GtkRecentManager` (part of GTK), shown
    as a menu on the Open button.
