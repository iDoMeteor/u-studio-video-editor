# 12 — Roadmap and milestones

Each milestone ends with acceptance criteria that a reviewer can check
without reading code. Estimates are effort for one engineer familiar with
GTK and C++, not calendar time; the team's parallelism will change the
calendar, not the order. Dependencies are strict: don't start a milestone
whose prerequisite hasn't passed its criteria.

```
M0 Foundation ─▶ M1 Model+Undo+Save ─▶ M2 Playback v2 ─▶ M3 Multi-track timeline
                                                   └───▶ M4 Bin & assets ──┘
                                             M3 + M4 ─▶ M5 Effects & compositing ─▶ M6 Export ─▶ M7 Polish & Flatpak
```

## M0 — Foundation (no behaviour change)

**Effort:** ~1 week. **Coordination:** the only milestone that moves existing
files. Do it as one PR in an announced window; rebase in-flight work once.

Deliverables:
- Meson restructure into `core/engine/app/render` + `tests` (doc 11); the
  existing `MltEngine` and `AppWindow` move under `engine/` and `app/`
  unchanged except includes; `util/log.*` moves to `core/`.
- `FactoryPolicy` with the curated module dir; `MltEngine` uses it.
- doctest vendored; `tests/engine/test_factory_policy` (no Qt in maps;
  required consumers present) and one trivial core test.
- `.clang-format`, `justfile`, CI workflow (build + tests + format).
- `data/`: desktop file, metainfo, icon placeholder, GResource with the CSS
  moved out of `style_css.h`.
- `docs/plans/v2` linked from the root README under "Roadmap".

Acceptance:
- [ ] `just build && just test` green on a clean Fedora 44 container.
- [ ] The app behaves exactly as before (import, play, split).
- [ ] Factory-policy test proves 0 Qt mappings.
- [ ] `grep -rn '<mlt' src/app` returns nothing (enforced by the build).

## M1 — Project model, commands, undo, save/load

**Effort:** ~2–3 weeks. **Depends on:** M0.

Deliverables:
- `core/model`, `core/commands`, `UndoStack`, `Document` (docs 03, 04).
- `EngineSync` v1: build tractor from model, rebuild-per-track; verifier.
- `AppWindow` switches to reading the model; existing single-track UI kept,
  now driven by commands (`InsertClip`, `SplitClip`, `RemoveClip`).
- XML writer/reader, atomic save, open, recent files, dirty flag, autosave
  + recovery (doc 09).
- Undo/redo actions with labels in an Edit menu.

Acceptance:
- [ ] Property test (random commands → undo all → equal) passes 10k iterations.
- [ ] `EngineSync::verify()` never fails across the property test.
- [ ] Save → quit → open restores the timeline identically (round-trip test).
- [ ] `melt saved.ustudio` (or `u-studio-render`) plays the saved file with
      no editor.
- [ ] Kill -9 during editing → next launch offers recovery with ≤ 2 min lost.

## M2 — Playback v2

**Effort:** ~2 weeks. **Depends on:** M1 (needs the tractor to come from
`EngineSync`).

Deliverables:
- `PlaybackController` over `sdl2_audio`/`rtaudio`/`null` (doc 05).
- `LatestFrameSlot`, dispatcher with lifetime token, single-copy texture path.
- Transport: play/pause, J/K/L, frame step, home/end, loop in/out, scrub
  on ruler, preview scale preference, volume.
- Remove `libpulse-simple`.

Acceptance:
- [ ] A/V sync: a generated clip with a 1 kHz beep on frame 0 of every
      second and a white flash on the same frames shows no perceptible offset
      (< 1 frame) at 1×; verified by eye and by the `null`-consumer position
      test.
- [ ] Pause shows the exact frame at the playhead (timecode matches the burnt
      -in `timer` filter of a test clip).
- [ ] 4K60 source plays at real time with frame dropping at preview scale
      0.5 on the dev machine; no unbounded memory growth over 10 min.
- [ ] Sanitiser run of the playback tests is clean; shutdown during playback
      is clean 100/100 runs.

## M3 — Multi-track timeline

**Effort:** ~4 weeks. **Depends on:** M2.

Deliverables:
- `UsTimelineView`, ruler, track headers, viewport, zoom/scroll (doc 06).
- Add/remove/rename/mute/hide/lock tracks.
- Select (click, shift, marquee), move, trim, ripple trim, slip, split,
  delete, ripple delete, copy, snapping, markers, in/out points.
- Thumbnail strips and waveforms from `RenderCache` (worker services from
  doc 07; the bin panel itself is M4, but the services land here).
- Black backing track; audio `mix` and video `composite` between tracks so
  stacked clips actually show.

Acceptance:
- [ ] Every gesture in the doc 06 table works with keyboard and mouse and is
      one undo step.
- [ ] Illegal drops are shown red and refused; no model invariant violation
      is reachable via UI (fuzz with `xdotool`-scripted random drags for 10
      min under the debug verifier).
- [ ] Snapshot ≤ 4 ms with 10 tracks × 500 clips on screen.
- [ ] Timeline stays responsive while thumbnails and waveforms generate.

## M4 — Media bin and assets

**Effort:** ~2 weeks. **Depends on:** M1; parallel with M3.

Deliverables: bin panel, multi-import, drag to timeline, probing with
validation, missing-media placeholders and relink dialog, still images and
image sequences, proxies (generate/toggle) via `u-studio-render --proxy`.

Acceptance:
- [ ] Importing a folder of 200 mixed files does not block the UI; failures
      are listed, not silent.
- [ ] Move a media file away, reopen project → red placeholders, playback
      continues, relink restores everything with no other model change.
- [ ] Proxy on/off changes nothing in the model except `proxyPath`.

## M5 — Effects, keyframes, transitions

**Effort:** ~4 weeks. **Depends on:** M3, M4.

Deliverables: catalogue + generic effect panel, Transform with on-preview
handles, Opacity, Crop, colour basics, Blur, Volume, fades, Text title,
keyframe lane, dissolve/dip-to-black transitions, track volume, split audio.

Acceptance:
- [ ] Every catalogue entry passes the metadata self-check on this machine.
- [ ] Keyframed transform renders identically in preview and in
      `u-studio-render` output (frame diff on a synthetic project).
- [ ] Dissolve between two clips survives move/trim of either clip and
      undo/redo (verifier + XML round-trip).

## M6 — Export

**Effort:** ~2 weeks. **Depends on:** M5 (needs the whole graph to be
serialisable), but the CLI can start after M1.

Deliverables: `u-studio-render` CLI, presets, export dialog, render queue
panel, notifications, hardware encoder detection.

Acceptance:
- [ ] `tests/render` passes for every preset.
- [ ] Killing the editor mid-render leaves the render running to completion
      (child is independent); cancelling from the UI removes the partial file.
- [ ] Rendered frame count equals the range length for all fixtures.

## M7 — Polish, kdenlive import, Flatpak

**Effort:** ~3 weeks. **Depends on:** M6.

Deliverables: kdenlive best-effort importer with warning report; Flatpak
manifest with MLT built without Qt modules and fonts bundled; preferences
dialog; About; keyboard shortcuts window; accessibility pass; screenshots
in metainfo; first tagged release `2.0.0`.

Acceptance:
- [ ] `flatpak-builder` produces a bundle in CI; the app runs from it with
      audio on a stock Fedora Workstation and GNOME OS.
- [ ] Three real `.kdenlive` projects from `~/Repos/kdenlive/tests` (or the
      team's own) import with a warning list and play.
- [ ] `appstreamcli validate` passes.

## Post-2.0 candidates (not planned)

Speed ramps (`timewarp`), stabilisation, nested sequences in UI, wipes,
audio mixer panel with meters, `frei0r` effect exposure, GL/dmabuf preview
upload, drop-frame timecode, OpenTimelineIO export.

Real-time capture, as a new source alongside file import (owner request,
2026-09-23): desktop audio + video capture, and separately, microphone
narration capture. Undesigned — this is a materially different subsystem
from anything v2 currently scopes, not a media-bin extension:
- A live MLT producer pulling frames continuously, not a file opened once
  (the whole engine today assumes a `Producer` with a known, fixed
  length — `Sequence::length()`, `Asset::info.lengthInSequenceFrames`,
  every clip's `in`/`out` bound checks). Whether captured video/audio
  lands as a normal `Asset` once recording stops, or needs its own model
  concept, is open.
- Needs its own ADR (CLAUDE.md: any new dependency or runtime capability
  needs one) once scoped: which MLT/PipeWire producer service actually
  captures a desktop video source and a "monitor" (loopback) audio
  source on this stack — verified against installed module metadata,
  never guessed — plus how a live capture interacts with the
  single-consumer-thread playback model (doc 05) and the "one thread
  owns each thing" principle (doc 01) while a capture and a preview
  might need to run at once.
- Two distinct capture inputs (desktop audio+video vs. mic-only
  narration) likely means two different service/pipeline shapes, not
  one flag.
