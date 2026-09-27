# ADR-019: GPU acceleration: movit compositing on our own GL context, hardware decode, CPU fallback

**Status:** Accepted (owner, 2026-09-27, through the VE Strategist: "we
should def support it at the very least, have a config toggle to turn it
off"). Amends doc 15 (movit was a non-goal) and risk R6 in doc 13 (movit
"out of scope"). Neither earlier call was evidence-based; the owner has
no objection to movit.

## Context

M4's last open box is 1080p30 in real time with three transformed tracks.
The CPU can't do it: one 1080p cut composited onto black costs 4 ms a
frame, and 20–23 ms with the `affine` transform filter, whatever the
picture's size or rotation (the full-frame canvas; MT4, doc 19,
`docs/developer/notes/engine-sync.md`). Three transformed tracks play at
about 22 frames/s at Half and 16 at Full. The owner's machine has an Intel
Iris Xe (Alder Lake-P GT2).

MLT 7.40 ships a GPU pipeline, the `movit` module (movit 1.7). kdenlive and
Shotcut get its GL context from Qt (`qglsl`), which ADR-007 bans.
`libmltmovit.so` itself links no Qt: it pulls libmovit, libepoxy, fftw3,
and libX11/GLX (for its deprecated `xgl` consumer only). It is **already
loaded** today, because `factory_policy`'s curated directory only denies
`qt6` and `glaxnimate`.

Standalone repros (VE GPU, 2026-09-27, MLT 7.40, Mesa, Iris Xe) established:

- **A Qt-free context works.** A surfaceless EGL 1.5 display
  (`EGL_PLATFORM_SURFACELESS_MESA`) with a desktop GL 3.0 context, made
  current on a worker thread; `glsl.manager` + `fire_event("init glsl")`
  then reports `glsl_supported=1`. No window, no X11 or Wayland
  connection.
- **It works under our consumer.** Under `sdl2_audio` at `real_time=1` and
  `mlt_image_format=rgba` (PlaybackController's settings), making the
  context current from `consumer-thread-started` on MLT's own render
  thread is enough; no thread override (`consumer-thread-create`) is
  needed. This answers doc 15's objection ("GPU context on the consumer
  thread conflicts with doc 05").
- **Throughput** (three half-size cuts in quadrants over black, 1080p30
  generated H.264, frames shown per second; measured under a load average
  of about 40 from other work, so the CPU figures are low):

  | Path | Full | Half |
  |---|---|---|
  | CPU (`affine` + `composite`) | 10.9 | 17.2 |
  | GPU (`movit.rect` + `movit.overlay`) | 29.8 (every frame) | 29.8 (every frame) |

  Quiet-machine figures: see "Evidence" below.
- **Colour.** On a BT.709-tagged source the GPU path matches the source
  within 1–2 levels. The CPU path is off by up to about 20 (red 253 → 231,
  cyan's red 0 → 22) with or without `affine`, so the offset is in MLT's
  CPU YUV→RGBA step, not in anything this ADR adds (routed to VE Core as a
  possible existing bug). Geometry is identical.
- **No movit service rotates.** `movit.rect` places and scales only. A CPU
  `affine` filter on a cut inside the GPU graph works (MLT uploads and
  downloads around it) and renders the same rotated picture as the CPU
  path, at the CPU filter's cost.
- **`glsl.manager` is a process-wide, sticky switch.** Once one exists,
  every producer the default loader opens gets `movit.crop movit.resample
  movit.resize … movit.convert` normalisers instead of `crop swscale
  resize …`, including the throwaway producers our workers open.
  Destroying the filter does not undo it; clearing the global
  `glslManager` property does. Only naming `loader-nogl` as the *service*
  (`Mlt::Producer(profile, "loader-nogl", path)`) keeps a producer on the
  CPU chain; the resource prefix `loader-nogl:path` gets *both* chains.
  (Found by VE Installers in the Flatpak sandbox; corrected by repro.)
- **Hardware decode can be per producer.** `avformat` reads `hwaccel` from
  the resource's query string, whose delimiter for a plain file is an
  escaped `\?`: `path\?hwaccel=vaapi` engages VAAPI through the default
  loader or `avformat` directly (the device comes from
  `MLT_AVFORMAT_HWACCEL_DEVICE`, else `/dev/dri/renderD128`). Setting a
  `hwaccel` property after construction does nothing; the codec is
  already open. `MLT_AVFORMAT_HWACCEL` would switch every producer in the
  process, workers included. MLT falls back to software decode when the
  device fails. On this machine VAAPI saves CPU (user time 92 → 73 s over
  90 s at Half) but doesn't raise frames/s: decode was never the
  bottleneck.
- **Risk:** kdenlive ships with movit hard-disabled ("Disable movit until
  it's stable"). We use a narrow set of services (`rect`, `overlay`,
  `mirror`, `flip`, `luma`, `mix`, and the loader's normalisers `crop`,
  `resample`, `resize`, `convert`), and the soak in G3's acceptance must prove that set.

## Decision

**1. GPU acceleration is a second engine path, chosen per process
lifetime, with the CPU path always available.** Settings › Performance
gains **GPU acceleration** (Automatic / Off; Automatic is the default and
means "on where the probe passes") and **Hardware video decoding**
(Automatic / Off). The CPU path stays the reference and the fallback; no
feature may exist only on the GPU path.

**2. The GL context is ours, created behind `src/platform/`** (ADR-017):
`platform::GlContext` (create in a share group, make current on the
calling thread, release, destroy; std-only interface). Linux: EGL,
surfaceless Mesa platform first, then a GBM device on the first render
node. Windows (with the port): WGL on a hidden window. The engine makes it
current from `consumer-thread-started` and releases it from
`consumer-thread-stopped`; every GL-using thread (preview render thread,
export render thread) gets its own context in one share group. Linking
EGL (`dependency('egl')`, libglvnd, already in every GTK install) is
approved by this ADR; our code never includes movit or GL headers other
than EGL/WGL in `src/platform/`.

**3. The movit module stays in the curated directory always; the switch
is whether a `glsl.manager` exists.** None is created unless the setting
is Automatic *and* the probe passed. Turning GPU off (the setting, or a
fallback) clears the global `glslManager`, drops EngineSync's master
producers and rebuilds, so every producer is reopened on the CPU chain.

**4. One engine helper opens every producer:**
`engine::openProducer(profile, resource, Chain::Live | Chain::Worker)`.
`Worker` (thumbnails, waveforms, probes, `audio_sync`, anything off the
live graph) always uses the `loader-nogl` service and never hardware
decode. `Live` (EngineSync's masters and the export graph) uses `loader`
and appends `\?hwaccel=<api>` when hardware decode is on. The suffix is an
engine-only projection like image sequences' `?begin=`: the writer never
saves it and `verify()` strips it. A test pins that no worker producer
carries a `movit.*` filter while a manager exists.

**5. The GPU graph** (EngineSync, behind the pipeline flag): track
compositing with `movit.overlay` onto track 0 (the same fan-in as
`composite`, ADR-018); a transform without rotation becomes a crop,
`movit.mirror` (horizontal)/`movit.flip` (vertical) and `movit.rect` on
the cut. `movit.crop` has no parameters of its own (it is the loader's
normaliser and applies the frame's crop properties), so how our crop
reaches it is G3's first repro; **a cut with a
non-zero rotation keeps the CPU `crop`/`mirror`/`affine` chain inside the
GPU graph** (correct, at the CPU cost; the owner's common case, a webcam
in a corner, has no rotation). Dissolves use `movit.luma`/`movit.mix`.
Effects stay CPU (frei0r, avfilter) inside the GPU graph the same way.
A rotation-capable GPU transform would need a movit effect of our own,
which means linking GPL movit; out of scope.

**6. Probe out of process, fall back automatically.**

- `u-studio-render --gpu-probe` creates the context, initialises
  `glsl.manager`, renders a generated two-track frame through the GPU graph
  and compares it with the CPU graph within a tolerance. It exits non-zero
  on any failure, and a GL driver crash takes down only the child.
- The result is cached, keyed by GL renderer string, Mesa/driver version,
  MLT version and app version. It is rerun when any of these change.
- At runtime:
  - any GL failure the engine can see (context loss, `glsl_supported=0`,
    a null image from a GPU frame) switches the process to the CPU path
    (point 3) and shows a toast;
  - a sentinel written while the GPU path is live and removed on clean
    exit turns Automatic into Off for the next launch if the app died with
    the GPU path active, and says so.
  - movit aborts on some internal errors, so this is the only fallback for
    those.

**7. Preview and export use the same pipeline.** An export takes the
pipeline the preview is using when it starts, with its own context on its
own render thread (in process today; in `u-studio-render` once MT5 moves
export out, ADR-009). A test renders a generated project through both
paths and requires them to match within a stated tolerance; until VE Core
settles the CPU colour offset, that tolerance records the known offset
rather than hiding it. Hardware *encode* (VAAPI `h264_vaapi`/`hevc_vaapi`
through the avformat consumer) belongs to M6's encoder detection, not
this ADR.

**8. Platforms.**

- Wayland and X11: the context is surfaceless, so it doesn't depend on the
  display server. Frames still reach GTK as RGBA memory textures; a
  zero-copy GL/dmabuf hand-off (risk R3) is a later milestone.
- Flatpak: the runtime's `org.freedesktop.Platform.GL` extension supplies
  Mesa's EGL, and `--device=dri` is needed. MLT is built with
  `MOD_MOVIT=ON` plus movit and a build-only Eigen, using the runtime's
  FFTW and epoxy (VE Installers' branch, about 10 MB).
- Windows: WGL behind the same interface when the port happens; MLT's
  Windows builds carry movit.
- movit and FFTW are GPL. They are bundled, never linked by our code; only
  MLT's module loads them, so the app stays MIT (CLAUDE.md licence note).

## Milestones (G, alongside M4 close-out and M5)

| Stage | Scope | Done when |
|---|---|---|
| G1 | Hardware decode: `openProducer()` with Live/Worker, the `\?hwaccel=` projection, the setting | Live graph decodes on VAAPI, workers stay software (test); a failed device falls back; docs |
| G2 | `platform::GlContext` (Linux EGL), `--gpu-probe`, cached result, sentinel, GPU setting shown with probe result | Probe passes here and fails cleanly with no GPU (`LIBGL_ALWAYS_SOFTWARE`/no render node); no graph change yet |
| G3 | GPU graph in EngineSync behind the flag; context on the consumer's render thread; runtime fallback | M4's box: three transformed 1080p tracks at Full play every frame; 10-minute soak holds; ASan+TSan full suites; preview matches CPU within tolerance |
| G4 | Export on the GPU path; preview/export equality test; `u-studio-render` after MT5 | Render of a generated project matches the preview path within tolerance |
| G5 | Flatpak (with VE Installers): GL extension, `--device=dri`, movit/FFTW modules; smoke test in the sandbox | Probe passes in the Flatpak on the owner's machine |

## Consequences

- M4's real-time box becomes reachable on the owner's machine without a
  cheaper CPU path. Machines without a working GL driver lose nothing.
- Two engine paths must stay equivalent. Every graph test runs on both
  where a GL context is available (CI: parked; locally: yes).
- A process that has ever created a `glsl.manager` must clear it to go
  back to CPU; the helper in point 4 keeps workers out of it either way.
- Rotated cuts don't get faster. If rotation turns out common in the
  owner's projects, a GPU rotation is a new ADR (it means a movit effect of
  our own, and GPL).
- Upstream movit maintenance and kdenlive's disabled GPU mode are
  standing risks; the toggle and fallbacks bound them to "slower", never
  "broken".

## Evidence

Repros live in the VE GPU scratchpad, and their findings go into
`docs/developer/notes/gpu.md` with each stage. Quiet-machine figures
(load < 4) are to be recorded here from the 2026-09-27 measurement
window.
