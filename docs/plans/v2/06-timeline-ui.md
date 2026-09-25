# 06 — Timeline UI

A custom `GtkWidget` subclass (`UsTimelineView`, GObject via `G_DEFINE_TYPE`)
that draws with the `snapshot` vfunc, implements `GtkScrollable`, and owns
no edit logic. Interaction lives in `TimelineController` (plain C++), which
turns gestures into commands (doc 04). ADR-008 explains why a custom widget
rather than a `GtkListView` of clip widgets.

## Layout of the timeline area

```
┌────────────┬──────────────────────────────────────────────────┐
│ (corner)   │ Ruler  (UsTimelineRuler; shares hadjustment)      │
├────────────┼──────────────────────────────────────────────────┤
│ Track      │ UsTimelineView                                    │
│ headers    │   ┌ V2 ───────────────────────────────────────┐   │
│ (UsTrack   │   │ [clip][clip]      [clip]                  │   │
│  Headers;  │   ├ V1 ───────────────────────────────────────┤   │
│  shares    │   │        [clip                       ]      │   │
│  vadj.)    │   ├ A1 ───────────────────────────────────────┤   │
│            │   │ [~~~~waveform~~~~][~~~~~]                 │   │
│            │   └───────────────────────────────────────────┘   │
│            │                      │ playhead                    │
└────────────┴──────────────────────────────────────────────────┘
          GtkScrolledWindow (h + v scrollbars) around UsTimelineView
```

Three widgets share two `GtkAdjustment`s. Headers and ruler are ordinary
widgets (a `GtkListBox` of header rows; a small custom widget for the ruler).

## Viewport model

```cpp
struct Viewport {
    double pxPerFrame;       // zoom; clamp [0.005, 200]
    FrameIndex scrollFrame;  // leftmost visible frame (from hadjustment)
    double scrollY;          // from vadjustment
    int trackHeight(TrackId) const;   // per-track, user-resizable; default 64 video / 40 audio
    double xForFrame(FrameIndex f) const { return (f - scrollFrame) * pxPerFrame; }
    FrameIndex frameForX(double x) const;   // floor; never negative
};
```

> REVIEW: Claude (2026-09-24): built as `app/timeline/viewport.{h,cpp}`,
> with scroll kept in pixels (`scrollX`) rather than a frame index so it
> scrolls smoothly, and a fit mode that follows the sequence until the user
> zooms. Zoom keys are `+`/`=`/`-`/`0` without Ctrl (the owner's single-key
> style); pinch is not wired yet.

- Zoom keeps the frame under the cursor fixed (Ctrl+wheel, Ctrl+±, pinch).
- "Fit" zoom = sequence length + 10% into the width.
- The scrollable extent is `max(sequence length × 1.25, visible width)` so the
  user can drop past the end.
- Rendering is resolution-independent: at low zoom clips shorter than 2 px
  draw as a 1 px tick; thumbnails/waveforms/labels drop out below width
  thresholds (label < 40 px, thumbnails < 24 px height).

## Drawing (snapshot)

Per frame of the widget, in order:

1. Track lanes: alternating `ink_800/ink_850` bands; locked tracks hatched;
   hidden/muted tinted.
2. Clips: rounded rect (`radius-sm`, 6 px, not the brand's 14 px which is too
   chunky at 40 px height), fill from track kind, 1 px border; selected gets a
   2 px `brand_cyan` ring and a soft glow (`gsk_blur` node on a copy of the
   border path, only for the selected few, never for all clips).
   - Video clips: a strip of thumbnails at fixed intervals (one per
     `thumbnailIntervalPx = 96`), from `RenderCache`; placeholder gradient
     until ready.
   - Audio (and audio part of video clips, bottom 40%): waveform from cached
     peaks, drawn as a `GskPath` fill, `brand_violet` at 70% alpha.
   - Label: clip name, `Space Grotesk` 11 px, clipped.
   - Fade handles at the corners when the clip is ≥ 60 px wide.
   - Transition region: overlapping diagonal hatch between the two clips.
3. Gap markers: none (gaps are just empty).
4. Snap indicator: 1 px `brand_magenta` line while dragging and snapped.
5. Drag preview overlay: translucent copy of the dragged clips at the
   candidate position; red tint if the candidate is invalid.
6. Loop range, markers (ruler mostly), in/out points.
7. Playhead: 1 px `brand_magenta` line + the ruler's triangle. Redrawn via a
   *separate* overlay child widget (`GtkOverlay`) so a position change during
   playback does not invalidate the whole timeline; only the overlay redraws.

Colours come from `style/tokens.h`, generated from the same values as
`style.css` by a script (`tools/gen_tokens.py`) so they never drift again
(the review noted `app_window.cpp:10-12` duplicating CSS hex by hand).

> REVIEW: Claude (2026-09-24): built as described (the generated header is
> `tokens.h` in the build tree). Also as built: clips under 4 px draw as a
> tick without an outline, waveforms need 24 px, and thumbnails tile edge
> to edge at the source's aspect rather than every 96 px, which left gaps.
> The drop-in hook is `TimelineOverlayProvider` (paint over the tracks);
> extra lane height waits for per-track row heights.

## Interaction (TimelineController)

> REVIEW: Claude (2026-09-24): built as `app/timeline/timeline_controller.
> {h,cpp}` with `selection.*` and `row_layout.h`. It takes a context per
> event (model, viewport, playhead) and returns outcomes (commands to try in
> order, seek, status, rename) that the window applies, so every gesture is
> tested against a real Model (`tests/app/test_timeline_controller.cpp`).
> Still drawn by the window's cairo area until `UsTimelineView` lands.

A state machine driven by `GtkGestureClick`, `GtkGestureDrag`,
`GtkEventControllerMotion`, `GtkEventControllerScroll`,
`GtkEventControllerKey`, and `GtkDropTarget` (drops from the bin).

```
Idle
 ├─ press on clip body        → SelectOrDrag  (click = select; shift = toggle; drag > 4px = Moving)
 ├─ press on clip edge (±6px) → Trimming(side)
 ├─ press on fade handle      → FadeDragging
 ├─ press on transition       → TransitionResizing
 ├─ press on empty            → RubberBand (marquee select) or, with Alt, playhead scrub
 │    (REVIEW: Claude, 2026-09-24: as built, a plain drag on empty space
 │     scrubs, as the owner already used it, and Shift+drag is the marquee)
 ├─ press on ruler            → Scrubbing (seek per motion, coalesced)
 └─ external drag enters      → InsertingFromBin
Moving/Trimming/…  ─ motion → compute candidate; snap; dry-run validate; redraw overlay
                   ─ release → execute one command (or nothing if invalid) → Idle
                   ─ Escape → cancel → Idle
```

Context menu (`GtkGestureClick` on the secondary button; carried over from
v1, which already ships clip/gap/track right-click menus):

| Right-click target | Menu items |
|---|---|
| Clip | Delete (lift — leaves a gap, nothing else moves); **Split Audio** (doc 13 Q2 — pulls the clip's audio out to a new linked clip on the nearest audio track, creating one if needed; only shown when the clip has audio and isn't already audio-only); Add/Edit Clip Name; Remove Clip Name |
| Gap (blank span) | Close Gap (ripples later content on that track earlier to fill it; dissolves between the moved clips stay) |
| Where two clips touch | Add Transition |
| Dissolve | Remove Transition |
| Empty track space | Track volume; Lock/Unlock Track; Hide/Show Track (video tracks); Mute/Unmute Track; Edit Track Name; Remove Track |

Every item's tooltip and its Help entry come from `app/ui_hints.cpp`.

Snapping (toggle with `S` key state or magnet button): candidate edges snap
to clip edges on all tracks, playhead, markers, in/out, and sequence start,
within `snapThresholdPx = 8`. Implemented as a sorted vector of snap frames
rebuilt on model change; a binary search per motion event.

Modifiers, matching GNOME/kdenlive habits where they agree:

| Gesture | Result |
|---------|--------|
| drag clip | move (overwrite drop refused if it would overlap; shown red) |
| Ctrl+drag | copy |
| Alt+drag on edge | ripple trim |
| Shift+drag on edge | slip (moves in/out together, clip stays) |

> REVIEW: Claude (2026-09-24): built as in this table, with two
> refinements. Ctrl on a clip means copy only when it drags; a Ctrl click
> toggles the selection. Shift on a clip's body adds it to the selection
> (and drags the group); only Shift on an edge slips. The "Ripple mode"
> toggle is not built.
> Update, same day: Ripple mode is built (`R` or the transport's Ripple
> button; `core::RippleMove`), and a ripple drop inside a clip goes to its
> nearer edge, since a ripple insert happens at a cut. Pinch zoom is wired
> (GtkGestureZoom).
| drag with `Ripple` mode on | ripple move (later clips follow) |
| drag between tracks | moves track; audio-only clips can only land on audio tracks |

Keyboard (as `GAction`s with accelerators so they show in the menu). The
shipped bindings are the owner's and are the source of truth
(`app/action_registry.cpp`); rows marked M3 are still to come:

| Key | Action |
|-----|--------|
| Space | play/pause |
| J / K / L | reverse / pause / forward; repeated J/L step speed |
| ← / → | −1 / +1 frame; Ctrl = 10 frames, Alt = 1 minute |
| Home / End | sequence start/end |
| A / F | previous / next cut on any track, or marker |
| Shift+A / Shift+F | previous / next cut on the active track |
| S / D | active track up / down |
| (menu) Sync Tracks (Audio) | two clips selected: the right-clicked one stays, the other moves to line its sound up with it (±5 s) |
| Shift+S / Shift+D | the selected clip up / down to the nearest free track of its kind; with nothing selected, active track to the top / bottom |
| X | split the active track's clip at the playhead |
| Delete / Shift+Delete | delete / ripple delete |
| I / O | set loop in / out |
| Ctrl+Z / Ctrl+Shift+Z | undo / redo |
| + / − / 0 | zoom in / out / fit (M3) |
| M / Shift+M | add marker / remove the marker at the playhead |
| Ctrl+A / Escape | select all / clear selection |
| Tab / Shift+Tab | select next / previous clip on the active track |
| ↑ / ↓ | active track up / down (also S / D) |
| , / . (Shift: ×10) | nudge the selection one frame |
| R | Ripple mode on/off |
| Ctrl+G / Ctrl+Shift+G | group / ungroup (M3.5) |

## Selection model

`Selection` (app layer): `std::set<ClipId>` + optional `TransitionId` +
current track. (Built as `app/timeline/selection.h`, clips only so far;
dragging a selected clip moves the whole set via `core::MoveClips`.) Emitted as a signal; the effects panel and actions follow it.
Selection is cleared when a selected clip is removed. Linked audio/video from
the same asset move together by default (v2.0 treats a video clip with
`audioEnabled` as one clip on the video track carrying audio; "Split
Audio" (shipped) creates a separate audio-track clip).

## Performance budget

- Snapshot for 10 tracks × 500 visible clips in < 4 ms (measured with
  `GTK_DEBUG=…` frame timing).
- Thumbnail/waveform requests are keyed by (asset, frame, size) and
  (asset, resolution) and issued lazily from snapshot via `RenderCache`, which
  answers from memory or enqueues to the worker and redraws on completion.
  Never block snapshot.
- Model change → view relayout is O(changed tracks), not full.

## Accessibility and HiDPI

- All drawing in logical pixels; GTK handles the scale factor.
- Keyboard-only operation must be complete: selection navigation with
  Tab/Shift+Tab across clips on the current track, ↑/↓ change track.
- The timeline reports `GTK_ACCESSIBLE_ROLE_LIST` with clips as
  `LIST_ITEM` children via `gtk_accessible_update_*` (lightweight; no
  child widgets). Full AT-SPI is a v2.x polish item.
