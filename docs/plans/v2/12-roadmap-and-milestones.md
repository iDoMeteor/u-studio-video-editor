# 12 — Roadmap and milestones

Each milestone ends with acceptance criteria that a reviewer can check
without reading code. Estimates are effort for one engineer familiar with
GTK and C++, not calendar time; the team's parallelism will change the
calendar, not the order. Dependencies are strict: don't start a milestone
whose prerequisite hasn't passed its criteria.

```
M0 Foundation ─▶ M1 Model+Undo+Save ─▶ M2 Playback v2 ─▶ M3 Multi-track timeline ──┐
                                                   └───▶ M4 Bin & assets ──────────┤
                                                                                   ▼
   FX track (M5):  FX0 spikes ─▶ FX1 engine ─▶ FX2 Rack+Browser ─▶ FX3 transitions ─▶ FX4 lanes+handles ─┐
                   (FX0–FX3 start now; FX4 needs M3)                                                     ├─▶ M6 Export ─▶ M7 Polish & Flatpak
   Titles track:   T0 spikes ─▶ T1 format+renderer+producer ─▶ T2 titles app ─▶ T3 animation ─▶ T4 templates ┘
                   (independent of M3/M4; T4 shares the Rack with FX2)
```

**2026-09-23 re-plan (owner request: effects, transitions and titles as
soon as possible).** M5 is no longer a single block after M3 and M4. It
becomes the FX track in [doc 15](15-effects-and-transitions.md), whose
first four phases depend only on work that has already landed (model,
commands, EngineSync, playback). A new titles track
([doc 16](16-titles-tool.md)) runs in parallel and is independent of the
timeline widget. Both tracks need an owner; with one engineer, do FX0 and
T0 first (they are short and de-risk everything after them), then
alternate.

**2026-09-24: effects and titles are drop-in modules**
([ADR-013](adr/013-effects-and-titles-as-drop-in-modules.md)). The order
from here:

1. Wrap up everything through M3 (in progress).
2. Audit (bugs plus sanitizers) before starting M4.
2a. **Concurrency, MT0–MT3** ([doc 19](19-concurrency.md), ADR-016,
   owner direction 2026-09-24): worker pool and model snapshots; import,
   probing, save and load off the main thread; an engine thread owning
   graph building and playback; parallel caches. Before the integration
   points, because IP3's hooks and M4's import work are built on it. MT4
   (playback throughput) and MT5 (parallel, out-of-process export) can run
   alongside M4.
3. Land the integration points IP1–IP6 (doc 15, "Drop-in structure") as
   small reviewed commits, each a no-op with the drop-in build options
   `disabled`, so M4's bin and import work builds on them. The drop-in
   loader and the Settings "Drop-ins" page (ADR-014) land alongside IP5.
4. M4, with the FX and titles tracks continuing in parallel and wiring into
   those integration points phase by phase.
5. Optional drop-ins from the catalogue
   ([doc 17](17-drop-in-catalogue-and-distribution.md)) as opportunity
   allows: **audio polish first, then keying**; then stabilise,
   auto-captions, motion tracking, speed ramps, audio visualiser and AI
   generation ([doc 18](18-ai-generation.md)), each starting with its spike
   and, where listed, its dependency ADR.

Drop-in work inside `drop-ins/effects/` and `drop-ins/titles/` (including
the FX0 and T0 spikes) may start at any time, because it changes nothing
outside its own folder.

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
- [x] `just build && just test` green on a clean Fedora 44 container.
      (2026-09-24, `51b150c`: fresh `fedora:44`, `xvfb-run -a just test`, 14/14.)
- [ ] The app behaves exactly as before (import, play, split).
      (In daily use; owner to confirm.)
- [x] Factory-policy test proves 0 Qt mappings. (`engine-factory-policy`.)
- [x] `grep -rn '<mlt' src/app` returns nothing (enforced by the build). (`app-boundary-check`.)

## M1 — Project model, commands, undo, save/load

**Effort:** ~2–3 weeks. **Depends on:** M0.

Deliverables:
- `core/model`, `core/commands`, `UndoStack`, `Document` (docs 03, 04).
- `EngineSync` v1: build tractor from model, rebuild-per-track; verifier.
- `AppWindow` switches to reading the model; existing single-track UI kept,
  now driven by commands (`InsertClip`, `SplitClip`, `RemoveClip`).
- XML writer/reader, atomic save, open, recent files, dirty flag, autosave
  + recovery (doc 09).
- Undo/redo actions with labels in an Edit menu. (Shipped as header-bar
  buttons and Ctrl+Z / Ctrl+Shift+Z; there is no Edit menu.)

Acceptance:
- [x] Property test (random commands → undo all → equal) passes 10k iterations.
      (`core`, "UndoStack property"; stream shared via `tests/common/random_commands.h`.)
- [x] `EngineSync::verify()` never fails across the property test.
      (`engine-sync`: the first 500 commands of the same stream, verify() after
      every command and every undo. 500, not 10k, by the owner's decision
      (2026-09-24): each step rebuilds the whole tractor, so 10k took hours.)
- [x] Save → quit → open restores the timeline identically (round-trip test).
      (`core`: `test_xml.cpp`, and every edit in `test_timeline_edits.cpp`.)
- [x] `melt saved.ustudio` (or `u-studio-render`) plays the saved file with
      no editor. (`engine-xml-playback`: a saved project with a dissolve,
      track volume, muted clip and hidden/muted tracks matches the editor's
      own graph frame by frame, 90 frames, through MLT's xml producer.)
- [x] Kill -9 during editing → next launch offers recovery with ≤ 2 min lost.
      (0.16.3; live run 2026-09-24: edits every 30 s, kill -9 about 36 s after
      the last autosave, recovery offered, at most about 36 s lost. Rule tested
      in `app-autosave`.)

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
- [x] A/V sync: a generated clip with a 1 kHz beep on frame 0 of every
      second and a white flash on the same frames shows no perceptible offset
      (< 1 frame) at 1×; verified by eye and by the `null`-consumer position
      test. (Automated half: `engine-av-sync`, 2 samples offset. By eye:
      confirmed by the owner, 2026-09-24, with `make_sync_clip`'s clip.)
- [x] Pause shows the exact frame at the playhead (timecode matches the burnt
      -in `timer` filter of a test clip). (Frame index checked in
      `engine-playback-controller`; confirmed by eye by the owner, 2026-09-24.)
- [x] 4K60 source plays at real time with frame dropping at preview scale
      0.5 on the dev machine; no unbounded memory growth over 10 min.
      (`playback_soak`, 2026-09-24, after "Make preview scale work by
      shrinking the playback profile": 10 min of 4K60 H.264 in a
      3840×2160 @ 60 sequence at preview Half; the playhead held real time
      throughout, about 37 of 60 frames/s were shown (the rest dropped, as
      allowed), every shown frame reached the UI, and RSS levelled off at
      about 0.81 GB after 5 min; load 4–7, 67–73 °C. The earlier run's
      "450 behind" came from a 30 fps sequence, a preview scale that had no
      effect, and the soak tool's own polling loop; all three are fixed.
      Showing all 60 frames is a throughput goal for doc 19, MT4.)
- [x] Sanitiser run of the playback tests is clean; shutdown during playback
      is clean 100/100 runs. (`just asan`/`just tsan` 12/12 clean at 0.15.3,
      `docs/audit/2026-09-23-sanitizer-report.md`; the 100-run shutdown loop is
      in `engine-playback-controller`. Rerun at the post-M3 audit.)

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
- [x] Every gesture in the doc 06 table works with keyboard and mouse and is
      one undo step. (2026-09-24, 0.23.0: move, copy, ripple trim, slip,
      Ripple mode and moves between tracks, each one `core::Command`, driven
      by `TimelineController` and tested there and by the fuzz test; the
      keyboard table too, with Tab/nudge/Up/Down for keyboard-only
      editing. Trims and moves between tracks are mouse gestures; the
      keyboard can select, nudge, split, delete, ripple delete and mark.)
- [x] Illegal drops are shown red and refused; no model invariant violation
      is reachable via UI (fuzz with `xdotool`-scripted random drags for 10
      min under the debug verifier). (Red preview: `TimelineController::
      Preview::valid`. Fuzz: `app-timeline-fuzz`, 4 × 20,000 random
      gestures through the controller and undo stack with `Model::check()`
      after each, undo-all restoring the start; 800,000 more ad hoc. It found
      three real bugs, fixed in 0.21.1. Run without GTK, not by xdotool.)

      > REVIEW: Claude (2026-09-24): `xdotool` isn't installed here. AT-SPI's
      > `Atspi.generate_mouse_event()` against the app under Xvfb with
      > `GDK_BACKEND=x11` does drive real clicks (used to test the track menu).
- [x] Snapshot ≤ 4 ms with 10 tracks × 500 clips on screen. (`app-timeline-
      render`, optimised build, 2026-09-24, under load ~11: 0.6 ms with all
      5,000 fitted, about 2 ms zoomed in with labels and waveforms.)
- [x] Timeline stays responsive while thumbnails and waveforms generate.
      (The snapshot only asks the caches, which answer from memory or queue
      work for their own threads; the timeline strips have a worker of their
      own, newest requests first. `engine-thumbnail-cache`: 300 thumbnail and
      100 waveform requests while both workers decode, slowest call 0.8 ms.)

## Frame rate (post-M3, pre-M4)

What the owner means by "variable frame rate": clips at 24, 25, 30, 60 fps
and so on on one timeline, and a choosable output rate. Investigated
2026-09-25 (spike: generated media in the scratchpad, standalone repros
against MLT 7.40, `~/Repos/mlt` at v7.40.0 for the source).

- **FR0, mixed source rates (done, 0.41.0).** MLT maps every source into
  the sequence's rate by time: `producer_avformat.c:2854`,
  `req_position = position / fps * source_fps + 0.5`, the nearest source
  frame. `tests/engine/test_mixed_rates.cpp` puts lossless 23.976–60 fps
  sources in 30 and 24 fps projects and checks every frame of playback,
  seeks and render, the sound on every cut, thumbnails and save/load. A
  new, empty project takes its size and rate from its first video (doc 13
  R7); a later import at another rate notes that frames will repeat or be
  skipped.
- **FR1, output frame rate (done, 0.42.0).** A render profile's frame rate
  renders `core::retime()`'s copy of the project (every frame index scaled
  to the new rate, absolute positions rounded) on a profile at that rate.
  The avformat consumer's own `frame_rate_num` only relabels the stream;
  MLT's `consumer` producer wrapper didn't rescale length and gave NaN
  audio. Checked: 30 → 60, 60 → 30 and 24 → 29.97 by frame count,
  duration, picture per frame and beep on every cut.
- **FR2, change the sequence's rate (next, about 2 days).** A command with
  undo (sequence snapshot) reusing `retime()`; the engine already rebuilds
  on a new profile (`EngineSync::setProject()` → `rebuildOnNewProfile()`,
  stopping the consumer first). UI with a confirmation, since it rewrites
  every position. Full sanitizer suites.
- **FR3, VFR detection (optional, not scheduled; needs the owner's call).**
  Sources whose own frame rate varies (phones, screen recordings) are
  already handled correctly by the time mapping above. Detecting them
  (to warn, or to choose a rate) needs libavformat directly: MLT reports
  only the nominal `r_frame_rate` (60 for a file averaging 40), and frames
  carry no source pts. That is a new dependency: an ADR first.
- **CFR conform** belongs to M4's proxy plan, for editing responsiveness,
  not correctness: 60 s of 1080p VFR to CFR 30 took 16 s (x264 veryfast)
  or 26 s (medium) on the dev machine.
- **Untagged HD colour.** MLT treats an untagged HD source as BT.709;
  ffmpeg-based players and ffmpeg's own encoder default to BT.601, so an
  untagged 601 source renders with shifted hues (pure green 255 → 214).
  `force_colorspace=601` on the producer fixes it. Tagged sources (the
  owner's OBS footage: bt709, full range) are converted correctly; full
  range is mapped to limited and tagged, no crushed blacks.

Spike measurements (generated media; each frame's luma encodes its source
time, a beep marks each whole second):

| Question | Result |
|---|---|
| Probe of a VFR source (frames alternate 1/60 and 1/30 s, average 40 fps) | `meta.media.frame_rate_num/den` = 60/1 (nominal only); `get_length()` right (duration × profile fps) |
| Sequential read and seeks, 30 fps profile, over 60 s | worst error 16 ms (jitter file), 21 ms (29.97 with 0–4 ms jitter): within one source frame, no drift |
| Render, A/V over 60 s | beeps at 1.000 / 10.000 / 30.000 / 59.000 s; the picture there shows the matching source time within one output frame |
| VFR detection cost (ffprobe, most of it process start) | 150–220 ms; stated rates catch the jitter file (60 vs 40.01) but not mild phone jitter (29.970 vs 29.966); a 300-packet pts scan catches both |
| CFR conform, 60 s 1080p | 16.2 s (veryfast), 26.1 s (medium), 16 threads |
| 24 → 23.976 over 10 min, 103 clips | absolute rounding: worst cut 20.6 ms (half a frame is 20.9), no gaps, 14,386 frames = 600.016 s; rounding lengths or relabelling: 14,400 frames = 600.6 s, 0.6 s behind the audio |

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

## M5 — Effects, keyframes, transitions (the FX track)

**Effort:** ~9–11 weeks across phases. **Depends on:** M1 and M2 for FX0–FX3;
M3 for FX4. Detailed deliverables and per-phase acceptance criteria are in
[doc 15](15-effects-and-transitions.md); this section keeps only the
milestone-level gate.

Phases: FX0 spikes and packaging, FX1 engine and model, FX2 Rack, Browser
and inspector keyframes, FX3 transitions library, FX4 curve lanes, FX lane
and on-preview handles, FX5 optional plugin families.

Acceptance (milestone gate):
- [ ] Every installed frei0r service is usable or quarantined with a
      reason; none can crash the editor (ADR-011).
- [ ] Keyframed transform, masked effects, a dissolve with effects on both
      sides and an adjustment block render identically in preview and in
      `u-studio-render` output (frame hashes on a synthetic project).
- [ ] Dissolves and wipes survive move/trim of either clip and undo/redo
      (verifier + XML round-trip), and `melt` plays them.
- [ ] Dragging any parameter does not restart the playback consumer.

## Titles track (parallel to M3–M5)

**Effort:** ~8–10 weeks across phases. **Depends on:** M1 (done) and
`FactoryPolicy`. Details in [doc 16](16-titles-tool.md) and
[ADR-012](adr/012-titles-mlt-module.md).

Phases: T0 spikes, T1 format, renderer and `ustudio_title` producer, T2 the
`u-studio-titles` app, T3 animation (keyframes, text animators,
behaviours), T4 templates, fields in the editor and Bake title.

Acceptance (track gate):
- [ ] A title designed in `u-studio-titles` renders identically in the app,
      the editor preview and export.
- [ ] One template file drives many clips with different field values.
- [ ] No keystroke typed into any text field in either app triggers a
      shortcut.

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

Nested sequences in UI, audio mixer panel with meters, GL/dmabuf preview
upload, drop-frame timecode, OpenTimelineIO export, captions (titles T5),
Lottie layers (titles T6). Wipes and frei0r exposure moved into M5 (doc 15)
on 2026-09-23; speed ramps and stabilisation became optional drop-ins
(doc 17) on 2026-09-24.

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

Cross-track transitions (owner idea, 2026-09-25; to be considered later):
select two clips on different tracks that overlap or butt, right-click,
"Add transition". This is MLT's native composition model, as kdenlive
uses it: a transition planted between the two tracks for just that time
range, with `luma` for the picture and `mix` for the sound. That is
simpler for the engine than today's same-track dissolve sub-tractors.
- **Overlapping clips:** the transition covers the overlap. Its direction
  follows timing: an upper clip that starts later fades in; one that ends
  earlier fades out.
- **Butting clips: supported** (owner). There's no overlap, so it borrows
  handle footage beyond each clip's in/out point, as same-track dissolves
  do. That needs free space on each clip's track next to the cut.
- **Types:** every transition type the editor supports (owner), including
  M5's wipes (doc 15).
- **Open: moving or trimming either clip.** Strip the transition (T1, as
  same-track dissolves do), or carry it along while the clips still
  overlap. Undecided (owner, "not sure").
- **Touches:** the project model and `.ustudio` format (ADR-004),
  add/remove commands and undo, the T1 strip rules, timeline drawing,
  `EngineSync`, and tests.
