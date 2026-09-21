# Changelog

All notable user-facing changes to this project are documented here.
Format: newest first, one line per change. Internal refactors, tests, and
docs-only changes are not listed (CLAUDE.md).

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
