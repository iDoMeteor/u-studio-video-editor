# Changelog

All notable user-facing changes to this project are documented here.
Format: newest first, one line per change. Internal refactors, tests, and
docs-only changes are not listed (CLAUDE.md).

## 0.28.0

- Edits no longer freeze the window while playback catches up: rebuilding
  the playback timeline and restarting playback run on their own thread.

## 0.27.1

- Edits on large projects rebuild playback much faster: 5,000 clips on
  one track went from about 20 s per edit to 0.35 s. Projects with many
  dissolves no longer grow to tens of gigabytes of memory after a few
  edits.

## 0.27.0

- Settings has a "Worker threads" option for the background work (import,
  load, save): Automatic by default, applies after a restart.

## 0.26.0

- Opening, reloading and recovering a project read the file in the
  background; opening another project while one is still loading replaces
  it, and an edit made meanwhile brings back the "Discard unsaved
  changes?" question.

## 0.25.0

- Saving and autosaving write in the background, so a big project no longer
  freezes the window while saving; closing or quitting waits for a save
  that's still writing.
- Fixed: undoing past the last save and then making a new edit could show
  the project as saved when it wasn't.

## 0.24.0

- Importing several files probes them in parallel off the main thread, shows
  "Importing 3 of 12…", keeps them in the order picked, and reports and
  skips a file it can't open; Import, timeline drops and media-browser
  drops all work this way.

## 0.23.2

- Release builds log at Info by default instead of Debug (debug builds are
  unchanged); `USTUDIO_LOG_LEVEL` still overrides either way.

## 0.23.1

- Fixed: Shift+Delete crashed after removing a media file whose clips were
  selected.
- Fixed: quitting while a render ran crashed and left a `.part` file; the
  app now asks, stops the render and cleans up.
- Fixed: playback could freeze for good when a consumer was stopped just
  after it started.
- Fixed: a ripple or plain move that put a clip back where it was deleted
  its dissolves; a dissolve could no longer be made as long as a whole clip.
- Zooming no longer leaves thumbnails decoding for levels already left.

## 0.23.0

- Ripple mode (R, or the Ripple button): moving a clip closes the gap it
  leaves and pushes later clips along where it lands.
- Keyboard: Tab / Shift+Tab select the next / previous clip on the active
  track, Up / Down change track, comma / period nudge the selection a frame
  (Shift: ten).
- Pinch to zoom the timeline.

## 0.22.0

- The timeline draws with GTK's scene graph (a custom widget) and stays
  fast with thousands of clips on screen; video clips show a strip of
  thumbnails; a move or copy that would be refused shows red while you
  drag.

## 0.21.1

- Fixed: a head trim could push a clip past the clip it dissolves into,
  leaving them overlapping; a ripple trim on a clip's head left its
  outgoing dissolve partner behind; and redo of an edit made on a copied
  clip failed (the copy came back with a different identity).

## 0.21.0

- Ripple trim (Alt+drag an edge), slip (Shift+drag an edge), copy
  (Ctrl+drag), ripple delete (Shift+Delete), and markers (M adds one at
  the playhead, Shift+M removes it).

## 0.20.0

- Select several clips (Shift+click, Ctrl+click, Shift+drag a box, Ctrl+A;
  Escape clears) and drag them together as one undoable move.

## 0.19.0

- Dragged clips and edges snap to clip edges on every track, markers and
  the playhead, never to their own old position, with a magenta line where
  they snap.
- Trimming the end of a clip works for every clip. It used to go wrong for
  any clip not cut from the start of its file (refused, wrong length, or a
  dissolve instead of a trim).
- Delete removes every selected clip as one undo step.

## 0.18.0

- Zoom the timeline with Ctrl+wheel or +/-, fit it with 0, and scroll it
  sideways (Shift+wheel, touchpad, scrollbar) or vertically when tracks
  don't fit. The view follows the playhead during playback.

## 0.17.2

- The preview scale setting now takes effect: Half and Quarter (and Auto
  on 4K projects) render playback at the smaller size, so large media
  plays much more smoothly. The saved default preview scale is also
  applied at startup.

## 0.17.1

- The app always uses its dark theme; under a light system theme, Help
  and Settings showed white lists.

## 0.17.0

- Hide or mute a track from its right-click menu; the row's name strip
  shows "Hidden"/"Muted".
- Help has a new Controls tab explaining every button, menu item and
  timeline gesture, and every control has a tooltip that names its current
  shortcut.

## 0.16.5

- Rendering works on stock Fedora, which has no libx264: it falls back to
  OpenH264. Before, the MP4 had no video.
- Images import as stills even where the system image loaders can't run.

## 0.16.4

- Closing a gap no longer removes the dissolves between the clips it moves.

## 0.16.3

- Autosave now also runs while you edit continuously, so a crash loses at
  most 2 minutes of work. Before, it waited for 2 minutes with no edits.

## 0.16.2

- Saved projects now play exactly as in the editor when opened by other
  MLT tools (melt): dissolves, track volume and split audio included.
  Previously everything after a dissolve played late and the dissolve was
  a hard cut. Older project files still open normally.

## 0.16.1

- Fixed Split Audio doubling the sound: the video half of a split clip kept
  playing its audio underneath the new audio clip.

## 0.16.0

- Full transport controls next to the play button: go to start, shuttle
  reverse, step back, stop, step forward, shuttle forward, go to end.
- X splits the active track's clip at the playhead.

## 0.15.5

- Stepping frame by frame quickly (holding an arrow key, or clicking the
  step button repeatedly) no longer drops steps.
- Fixed a rare flash of the first frame in the preview right after an edit
  while paused (a regression from 0.15.3's playback change).

## 0.15.4

- Fixed Ctrl+S (and Save As, Render, Import) failing with "failed to write
  /run/user/…/doc/…" when the file picker handed back a sandbox portal
  path: the app now saves to the file's real location.

## 0.15.3

- The app now quits cleanly on logout, shutdown, `kill`, or Ctrl+C in a
  terminal, saving an autosave first if there are unsaved changes.
  Previously it ignored these until forcibly killed.

## 0.15.2

- Fixed a crash on quit, and a possible crash when refreshing the recent
  projects menu, caused by freeing timestamps the recent-files list still
  owned.

## 0.15.1

- Fixed memory growing with every edit: each edit leaked the previous
  timeline's transitions (roughly 100–330 KB per edit), plus a smaller
  per-edit leak in the black background track.

## 0.15.0

- Added a Help dialog (header bar `?` button) with Keyboard Shortcuts and
  About tabs, and a Settings dialog (gear button) with General and
  Playback tabs — autosave delay, recent-projects list size, default
  preview scale, and maximum shuttle speed are now user-configurable and
  persisted via GSettings, instead of hardcoded.

## 0.14.7

- Pausing now stops on the frame that was on screen, instead of jumping
  about a second ahead.

## 0.14.6

- A timecode ruler now runs along the top of the timeline, with ticks
  that adapt to the current zoom level.

## 0.14.5

- Click and drag on empty timeline space to scrub, following the pointer
  continuously instead of only seeking once you release.

## 0.14.4

- Moving or trimming a clip now snaps to nearby clip edges and the
  playhead when within about 8 pixels.

## 0.14.3

- Rendering now shows a live percentage in the status bar instead of
  only a start and finish message.

## 0.14.2

- Import (Ctrl+I) supports selecting multiple files at once.
- Import and Open Project file pickers now filter to media files and
  `.ustudio` projects respectively, instead of showing every file.
- Drag files in from the file manager onto the timeline to import and
  place them, or onto the media browser to just add them to the project.
- Double-click a media browser row to insert it at the playhead on the
  active track.

## 0.14.1

- The window title now shows the current project's name (or "Untitled
  Project") instead of just "u Studio Video Editor" for every project.
- A "Recent projects" button next to Open lists the last 10 projects
  opened or saved, for one-click reopening.

## 0.14.0

- Space bar toggles play/pause.
- Delete removes the selected clip (leaving a gap, same as right-click →
  Delete Clip).
- Ctrl+S saves in place once the project has a file, without opening a
  dialog; Ctrl+Shift+S always opens the Save As dialog. Ctrl+O/N/I open
  a project, start a new one, and import media.

## 0.13.6

- The timeline playhead now updates on its own lightweight overlay
  instead of redrawing every clip, label, and waveform on the timeline
  30 times a second during playback.

## 0.13.5

- Opening a different project now asks first if the current one has
  unsaved changes, matching Reload and New Project — previously it went
  straight to the file picker with no warning.

## 0.13.4

- "Delete File…" in the media browser is now "Move File to Trash…" — it
  moves the file to your desktop's Trash instead of permanently deleting
  it, so it's still recoverable afterward.

## 0.13.3

- Closing the window with unsaved changes now asks first (Save, Discard,
  or Cancel) instead of quitting immediately and losing them. Choosing
  Discard (or any other path to quitting while still dirty) still leaves
  a final autosave behind as a recovery point.

## 0.13.2

- Renaming a track or clip inline no longer triggers transport shortcuts
  (previous/next cut, shuttle, step, loop in/out…) while you're typing —
  bare letters and arrow keys used to be swallowed by those instead of
  going into the name field.

## 0.13.1

- Trimming, moving, or splitting a clip that has a dissolve transition on
  it no longer refuses the edit, or silently drops the transition, when
  the edit doesn't actually touch the transition's own overlap — only
  edits that reach into the overlap strip it now.
- Splitting a clip whose far edge has a dissolve now keeps the
  transition, moved onto whichever half of the split still owns that
  edge, instead of always stripping it.
- A clip drag that ends back exactly where it started no longer strips
  the clip's dissolve transition.
- Deleting a media asset from the media browser no longer leaves a
  dangling dissolve transition on a clip it removed, or on a surviving
  clip that shared the dissolve with a removed one.

## 0.13.0

- Timeline playhead: a vertical cyan line at the current frame, spanning
  every track, updating live during playback and scrubbing — not just
  the seek bar below the preview.

## 0.12.8

- Importing several media files at once no longer rebuilds the entire
  (usually hidden) media browser panel once per finished thumbnail; it
  now catches up in one rebuild whenever the panel is next opened.

## 0.12.7

- Fixed autosave recovery being able to treat a crashed session's owner
  as "still alive" (and so never offer its autosave) if an unrelated
  process later reused the same pid -- the process's own start time is
  now cross-checked too, not just the pid.

## 0.12.6

- Reload and New Project now confirm ("Discard unsaved changes?") before
  replacing the current project if it has unsaved edits, matching Save's
  own guard elsewhere -- both used to replace the model on a single
  click with no way back.

## 0.12.5

- Fixed a video with no audio track getting a waveform decode job and a
  "Split Audio" menu item that produced an empty, silent clip; whether
  media actually has audio is now checked instead of guessed.

## 0.12.4

- Fixed undo of importing or resizing a still image/logo overlay leaving
  its recorded source length extended instead of restoring it, an
  internal inconsistency between the model and what undo is supposed to
  guarantee (harmless to playback, but wrong on inspection or in a saved
  project's `out` point).

## 0.12.3

- Fixed a data-loss bug: recovering an unsaved autosave, then starting a
  New Project (or Open/Reload) without saving the recovered work first,
  then later saving that different project, deleted the recovered
  autosave -- the only copy of the original unsaved work.

## 0.12.2

- Fixed media browser thumbnails for real (non-still) video: they were
  decoded through MLT's default 4:3-ish profile, so every 16:9 clip's
  thumbnail showed black letterbox bars and squashed pixels instead of
  its real shape.

## 0.12.1

- Fixed crashes and corrupted saved projects from editing a clip linked
  by a dissolve transition: deleting, moving, resizing, or splitting a
  linked clip now cleanly removes the dissolve first instead of leaving
  a dangling or mis-sized transition record. Also fixed a crash triggered
  by this fix during testing: some edits restarted the real audio device
  twice in immediate succession, which can segfault deep in the system
  audio stack; those edits are now batched into a single restart.
- Fixed a project being able to save with a corrupted transition that
  would then fail to reopen at all: Save now refuses (with a status
  message) if the project fails its own internal consistency check.
- Fixed adding a dissolve transition that combines with an existing one
  on the same clip to exceed the clip's own length, which laid out the
  rest of the track incorrectly.

## 0.12.0

- Media browser: right-click a row for "Remove from Project" (drops the
  asset and every clip cut from it) or "Delete File…" (confirms, then
  also deletes the file from disk). Drag a row onto the timeline to
  insert a full-length clip at the drop's exact track/frame.

## 0.11.0

- Keyboard shortcuts: `Ctrl+Left`/`Ctrl+Right` jump the playhead 10
  frames; `Alt+Left`/`Alt+Right` jump a minute, clamping to the
  timeline's start/end when less than a full minute remains in that
  direction.

## 0.10.1

- Fixed project recovery on launch picking an arbitrary orphaned autosave
  instead of the most recent one when several qualified at once, which
  could silently surface an older, thinner autosave over a richer one
  from the same crashed/unsaved session. The recovery dialog now also
  loops through every independently orphaned autosave in one launch
  instead of stopping after the first.

## 0.10.0

- Keyboard shortcuts: `A`/`F` jump the playhead to the previous/next cut
  on the active track (a clip's start or end, or the timeline's own
  start/end); `S`/`D` move the active track up/down.

## 0.9.1

- Fixed a crash: a project referencing media that's since been moved,
  renamed, or deleted would segfault the whole app the moment playback
  or a redraw reached that clip, instead of failing gracefully. That
  clip now plays as black and a status message names the missing file.

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
