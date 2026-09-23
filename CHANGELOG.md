# Changelog

All notable user-facing changes to this project are documented here.
Format: newest first, one line per change. Internal refactors, tests, and
docs-only changes are not listed (CLAUDE.md).

## 0.9.0

- Collapsible media browser panel, to the left of the video preview:
  thumbnail, name, length, fps, and format for every imported asset.
  Toggle it from the new header-bar button next to "Add track". Import
  now also probes and records fps/dimensions (previously left at 0) and
  the file's format (from its extension).

## 0.8.0

- Dissolve transitions: drag either edge of an existing dissolve to grow
  or shrink it; right-click near where two touching clips meet for "Add
  Transition" (a default ~half-second dissolve) as an alternative to
  dragging one in. Fixed: double-clicking a track's name label could
  instead open the clip name editor if a clip happened to sit under it;
  a clip's own name now takes priority over its track's name in the
  corner badge once set (previously the track's name always won).
  Context menu: added "Edit Track Name"; renamed "Edit/Add/Remove Name"
  to "…Clip Name" to disambiguate from the new track one.

## 0.7.0

- Dissolve transitions between two adjacent clips on the same track.
  Drag a clip's edge past its exactly-touching neighbor (the same
  trim-drag gesture used for ordinary trims) to create a dissolve,
  shown as a diagonal-hatch overlay on the overlap; right-click the
  overlap for "Remove Transition". Creating one grows each clip using
  its own existing source-media handle, so nothing else on the track
  ever needs to move. Undoable, and saved/loaded with the project
  (project file format bumped to v3 for this — see the README).

## 0.6.0

- Tracks and clips can now be named. Double-click a track's name strip
  or a clip to edit its name inline; right-click a clip for "Add
  Name"/"Edit Name" and, once it has one, "Remove Name". Two clips cut
  from the same source can carry different names. A named track shows
  its name in the top-left corner of every clip on it. Hovering a clip
  shows a tooltip with its name, start/end timecodes, length (timecode
  and frame count), and source file.

## 0.5.2

- Fixed Play doing nothing after any seek (scrubbing, clicking the
  timeline, or even just the very first play after importing a clip):
  the playback engine left an internal "show one frame" flag set from
  the last pause/seek, which silently blocked continuous playback from
  ever resuming until the project was reloaded.

## 0.5.1

- Fixed a severe slowdown importing or editing a long clip (measured 18
  seconds on a real ~62-minute recording): computing its waveform used
  to decode every single video frame just for the audio peak display,
  saturating a CPU core decoding the same file the live preview was
  trying to play from at the same time. Waveform decoding is now capped
  to a bounded number of samples regardless of clip length (2.5 seconds
  for that same recording), with no visible loss of detail in the
  drawn waveform.

## 0.5.0

- New header-bar buttons next to Save/Open: Reload (re-opens the current
  project's file from disk) and New Project (resets to a fresh, empty,
  untitled project).
- Debug-level logging is now on by default (`USTUDIO_LOG_LEVEL=info` for
  the old, quieter behavior).

## 0.4.4

- Fixed a crash and "playback stopped working" after the app was
  launched a second time while already running (a second double-click,
  a second terminal invocation): re-launching used to silently open a
  second editing window with its own audio device instead of presenting
  the one already open, leaving two audio consumers fighting over the
  same output.
- Debug-level logging (`USTUDIO_LOG_LEVEL=debug`) now covers every
  user-facing status message and the playback engine's internal timing,
  for diagnosing both bugs and slow operations.

## 0.4.3

- The undo/redo buttons and the title bar's unsaved-changes mark now
  update after every edit (import, split, move, trim, delete, add/remove
  track, lock, volume) instead of staying stale until the first undo.
- Recovering unsaved work after a crash now correctly shows as unsaved,
  and its autosave is kept until a manual Save actually lands the
  recovered content somewhere durable, instead of being deleted right
  after recovery.
- "Close Gap" now closes the whole gap, not just the part after where you
  right-clicked.
- Importing or dragging a clip onto an audio track no longer lets its
  video show through wherever the video tracks above have a gap; dragging
  a video-only clip onto an audio track is refused instead of parking a
  dead clip there.
- A second running instance's own in-progress autosave is no longer
  offered (and possibly deleted) by another window's crash-recovery
  prompt.
- Save and Render both refuse to write over a file that's already one of
  the project's own media sources; a render that fails partway through no
  longer leaves a partial file at the name you asked for.
- Clicking the volume slider, a track's volume slider, or the preview-scale
  dropdown no longer swallows the Left/Right/Home/End playhead shortcuts.

## 0.4.2

- Fixed a still image/watermark stretched past its imported length
  playing shorter than the timeline showed, shifting every clip after it
  once the mismatch resolved itself later.
- Fixed the loop range affecting a seek or frame-step while paused, even
  well past the loop's own out point.
- Fixed the unsaved-changes indicator sometimes reporting "no changes"
  right after a save when a new edit immediately followed one already on
  the undo stack.
- Opening a corrupted or hand-edited project file now fails with a clear
  reason instead of silently loading invalid data.
- Fixed a rare crash opening a project while the previous one's engine
  state was still tearing down.

## 0.4.1

- Fixed playback silently failing to advance after an edit (import,
  split, move, trim, ...): an optimization meant to avoid closing and
  reopening the audio device on every edit was instead corrupting MLT's
  internal playback state. Every edit now does a clean restart of the
  playback consumer, which is slightly more expensive but actually
  correct.

## 0.4.0

- Track locking: right-click empty track space to lock/unlock a track.
  A locked track refuses insert/move/resize/split/remove on its clips
  (and as a move/Split Audio destination) until unlocked; locked rows
  get a subtle tint.
- Per-track volume: a slider in the same right-click menu sets each
  track's level independently (previously only a single, whole-project
  volume control existed).

## 0.3.0

- "Split Audio" on a clip's right-click menu: pulls its audio out to a
  new, independent clip on the nearest audio track with room (creating
  one if none has room). The two halves can then be moved and trimmed
  independently of each other.

## 0.2.0

- Importing a still image (PNG, JPEG, ...) now auto-detects it as such and
  defaults its length to span the rest of the current project from the
  insert point, instead of MLT's fixed ~8-minute default — drop one onto
  an empty top track for an instant full-timeline watermark/logo overlay.

## 0.1.0

- Playback now runs through a real MLT consumer (`sdl2_audio`, falling
  back to `rtaudio`, then `null`) instead of a hand-rolled pull loop —
  removes the ~150ms A/V offset and PulseAudio dependency of earlier
  builds.
- New transport controls: `J`/`K`/`L` shuttle (with speed ramping),
  frame-step (`Left`/`Right`), jump to start/end (`Home`/`End`), loop
  in/out (`I`/`O`), a volume slider, and a preview-scale preference
  (Auto/Full/Half/Quarter).
- Undo/redo for every edit, with labels and `Ctrl+Z`/`Ctrl+Shift+Z`.
- Project files are now MLT XML with `ustudio:` properties instead of a
  bespoke `GKeyFile` format — `.ustudio` files can be played directly by
  `melt` with no editor involved.
- Autosave and crash recovery: unsaved work is offered back on the next
  launch after a crash.
