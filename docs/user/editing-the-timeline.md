# Editing on the timeline

[Docs home](../README.md) › [User guide](README.md) › Editing on the timeline

Every edit can be undone (`Ctrl+Z`) and redone (`Ctrl+Shift+Z`).

## Tracks

- **Add** a track with **Add track** in the header bar. **Reorder** a track
  by dragging its handle.
- **Click a row** to make it the active track: imports and splits land
  there. `S`/`D` or `Up`/`Down` move the active track up and down.
- **Video:** higher tracks draw over lower ones. **Audio:** all tracks mix
  together, including audio-only tracks.
- **Right-click empty track space** for:
  - a track volume slider
  - **Lock Track**: its clips can't be moved, trimmed, split or removed
    until you unlock it. Locked tracks are tinted.
  - **Hide Track** (video): the picture is off in preview and render, and
    the tracks under it show through.
  - **Mute Track**
  - **Edit Track Name**
  - **Remove Track**
- **Double-click a track's name strip** to rename it. A track's name also
  appears on each of its clips.

## Moving around

| Do this | Result |
|---|---|
| `Ctrl` + mouse wheel, `+`/`-`, or pinch | Zoom (the frame under the pointer stays put) |
| `0` | Fit the whole project |
| `Shift` + wheel, touchpad swipe, scrollbar | Scroll sideways |
| Click or drag on empty timeline space | Move the playhead (scrubs while you drag) |
| `A` / `F` | Previous / next cut or marker on any track |
| `Shift+A` / `Shift+F` | Previous / next cut on the active track |
| `Home` / `End` | Start / end |

During playback the view follows the playhead a page at a time.

## Clips

| Do this | Result |
|---|---|
| `X` or the scissors button | Split the active track's clip at the playhead |
| Drag a clip's body | Move it, on the same track or another one |
| Drag near a clip's edge | Trim it shorter or longer |
| `Delete` | Lift the selected clips, leaving a gap |
| `Shift+Delete` | Ripple delete: remove the clips and close the gap |
| Right-click a gap | Close it: later clips move earlier to fill it |
| `Ctrl` + drag a clip | Copy it |
| `Alt` + drag an edge | Ripple trim: later clips on the track follow |
| `Shift` + drag an edge | Slip: same place and length, different part of the source |
| `Shift+S` / `Shift+D` | Move the selected clip to the nearest free track above / below |
| `,` / `.` | Nudge the selection one frame (`Shift` for ten) |

- **Snapping:** moves and trims snap to clip edges on every track, markers,
  the playhead and the start. A magenta line shows the snap. Hold `Ctrl` or
  turn it off in Settings to place freely.
- **Refused edits** draw red while you drag, and nothing changes when you
  let go. For example, a clip can't overlap another or land on a locked
  track.
- **Ripple mode** (`R`, or the Ripple button): moving a clip closes the gap
  it leaves and pushes later clips along where it lands.

## Selecting

| Do this | Result |
|---|---|
| Click | Select one clip |
| `Shift` + click | Add a clip |
| `Ctrl` + click | Toggle a clip |
| `Shift` + drag on empty space | Box-select |
| `Ctrl+A` / `Escape` | Select all / clear |
| `Tab` / `Shift+Tab` | Select the next / previous clip on the active track and jump to it |

Dragging any selected clip moves the whole selection as one undo step.

## Markers

- `M` adds a marker at the playhead, and `Shift+M` removes it.
- Markers show on the ruler. `A`/`F` jump to them and edges snap to them.
- Markers can't be named yet.

## Dissolves

A dissolve cross-fades picture and sound between two touching clips on the
same track. It shows as a hatched area.

- **Add one:** drag a clip's edge past the neighbour it touches (the drag
  distance sets the length), or right-click where the two clips meet and
  choose **Add Transition** (about half a second).
- **Resize:** drag either edge of the hatched area.
- **Remove:** right-click it and choose **Remove Transition**.
- A dissolve uses the extra footage each clip has beyond its cut, so
  nothing else on the track moves.
- Moving, trimming or splitting a clip into a dissolve removes that
  dissolve.
- With the Effects add-on, **T** adds a dissolve at the cut nearest the
  playhead, and a dissolve can be a dip, a flash, a slide, a push or a
  wipe instead
  ([Transition styles](effects.md#transition-styles)).

## Naming clips

Double-click a clip, or right-click it and choose **Add Name**, to give it
a name. Enter confirms and Escape cancels. Hover over a clip to see its
name, timecodes, length and source file, plus a thumbnail of the frame
under the pointer.

See also: [Audio](audio.md) · [Keyboard shortcuts](keyboard-shortcuts.md) ·
[v2 doc 06: Timeline UI](../plans/v2/06-timeline-ui.md)
