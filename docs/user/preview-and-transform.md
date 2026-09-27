# Preview and transform

[Docs home](../README.md) › [User guide](README.md) › Preview and transform

## Playback

| Do this | Result |
|---|---|
| `Space` or the play button | Play / pause |
| `J` / `K` / `L` | Shuttle back / stop / forward. Press `J` or `L` again for 2×, 4×, 8× |
| `Left` / `Right` | Step one frame |
| `Ctrl+Left` / `Ctrl+Right` | Step 10 frames |
| `Alt+Left` / `Alt+Right` | Step one minute |
| `Home` / `End` | Go to start / end |
| `I` / `O` | Set loop in / out at the playhead |
| Drag the seek bar, or empty timeline space | Scrub, also while playing |

The transport bar has buttons for each of these, and each button's tooltip
names its key. The cyan line on the timeline marks the current frame
across every track.

Pausing always shows the exact frame at the playhead, and picture and
sound stay in sync over long playback.

## Preview scale

The dropdown under the preview sets how large a picture the preview draws:
**Auto**, **Full**, **Half** or **Quarter**. Lower scales play large
footage more smoothly. Renders are always full size.

**Auto** uses Half for projects larger than 1080p, and for any project
where a clip has been moved, scaled or rotated. The dropdown shows what
Auto picked, for example "Auto (Half)". For 4K footage, also consider
[proxies](importing-media.md#proxies-smooth-editing-of-4k-and-phone-footage).

## Placing pictures (OBS-style)

Each picture fills the frame, centred, until you change it. To change it,
click the picture in the preview to select its clip:

| Do this | Result |
|---|---|
| Drag the picture | Move it |
| Drag a corner handle | Scale it (hold `Shift` to change the aspect ratio) |
| Drag an edge handle | Stretch it |
| Drag the round knob | Rotate it (hold `Shift` for 15° steps) |
| `Alt` + drag an edge or corner | Crop it |
| Arrow keys (after clicking the preview) | Nudge it |
| Hold `Ctrl` while dragging | Turn off snapping |

- Pictures snap to the frame's edges and centre, and to other pictures.
- **Right-click** the picture for Reset, Fit, Stretch, Centre, Flip and
  Rotate 90°.
- **Edit Transform** (`Ctrl+T`, or double-click the picture) opens a window
  beside the preview where you can type exact position, size, rotation and
  crop values.
- Each drag, and each Edit Transform session, is one undo step.
- Transforms are saved with the project, look exactly the same in the
  render, and stay with the clip when you trim, split, copy or ripple it.

Typical uses: a webcam in the corner, a screen recording beside a face, a
logo in a corner.

See also: [Rendering](rendering.md) ·
[ADR-018: Clip transform](../plans/v2/adr/018-clip-transform-is-core.md) ·
[v2 doc 05: Playback engine](../plans/v2/05-playback-engine.md)
