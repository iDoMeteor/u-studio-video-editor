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
- **Throughput.** Three half-size transformed tracks play every frame on
  the GPU path at Full and Half, with half the CPU; the CPU path doesn't
  (figures under "Evidence").
- **Colour.** On a BT.709-tagged source the GPU path matches the source
  within 1–2 levels. The CPU path was off by up to about 20 (red 253 →
  231). The cause was the black background: MLT's colour producer tagged
  it BT.601 and `composite` kept the tag. VE Core fixed that in 7c7f9fa by
  re-tagging the background with the profile's colourspace. Geometry is
  identical.
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
  device fails. **On the CPU path hardware decode doesn't pay:** it saves
  16–22% CPU but loses 5–10% of frames, because every decoded frame is
  downloaded from the GPU to be composited. On the GPU path it nearly
  halves CPU use at full frame rate (see "Evidence").
- **Risk:** kdenlive ships with movit hard-disabled ("Disable movit until
  it's stable"). We use a narrow set of services (`rect`, `overlay`,
  `mirror`, `flip`, `luma`, `mix`, and the loader's normalisers `crop`,
  `resample`, `resize`, `convert`), and the soak in G3's acceptance must prove that set.

## Decision

**1. GPU acceleration is a second engine path, chosen per process
lifetime, with the CPU path always available.** Settings › Performance
gains **GPU acceleration** (Automatic / Off; Automatic is the default and
means "on where the probe passes") and **Hardware video decoding**
(Automatic / Off; Automatic means "on while the GPU pipeline is": the
measurements show it costs frames on the CPU path; VE Strategist,
2026-09-27). Both land in G2. The CPU path stays the reference and the fallback; no
feature may exist only on the GPU path.

**2. The GL context is ours, created behind `src/platform/`** (ADR-017):
`platform::GlContext` (create in a share group, make current on the
calling thread, release, destroy; std-only interface). Linux: EGL,
surfaceless Mesa platform first, then the first EGL device
(`EGL_EXT_platform_device`, how NVIDIA's driver offers the same). Windows
(with the port): WGL on a hidden window. The engine makes it
current from `consumer-thread-started` and releases it from
`consumer-thread-stopped`; every GL-using thread (preview render thread,
export render thread) gets its own context in one share group. libEGL is
loaded at run time (`dlopen`, in `src/platform/`), so nothing new is
linked, and a machine without it simply has no GPU path. Only EGL's
headers are needed to build, and without them the platform layer builds
a stub that reports "built without EGL". Our code never includes movit
or GL headers; the platform layer includes only EGL's.

**3. The movit module stays in the curated directory always; the switch
is whether a `glsl.manager` exists.** None is created unless the setting
is Automatic *and* the probe passed. Turning GPU off (the setting, or a
fallback) clears the global `glslManager`, drops EngineSync's master
producers and rebuilds, so every producer is reopened on the CPU chain.

**4. One engine helper opens every producer:**
`engine::openProducer(profile, resource, use)` (`src/engine/producer_open.h`).
Only a GPU graph's producers (`GpuGraph`) use the default `loader`; a
worker's (thumbnails, waveforms, probes, `audio_sync`) and a CPU graph's
use the `loader-nogl` service, so a CPU graph stays CPU while a GPU session
lives elsewhere in the process (an export on a pool thread while the
preview plays on the GPU crashed in 0.60.0-beta.1, fixed in 0.61.1-beta.1).
A drop-in's producer (`EngineExtension::makeProducer()`) uses `Graph`, which
follows the graph being built on that thread. EngineSync's video masters
get `\?hwaccel=<api>` when hardware decode is on. The suffix is an
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
in a corner, has no rotation). Dissolves use `movit.luma_mix`. A plain Fit
is placed by `movit.rect` too (the overlay doesn't centre). Alpha pairing
(VE Core's 4:2:2 fringe fix) is skipped: movit blends straight RGBA.
**The GPU graph blends in linear light:** MLT's movit input linearises
every RGBA source whatever its tags, so soft edges, semi-transparent
pictures and dissolve midpoints come out brighter than on the CPU, which
blends the coded values; opaque pictures match within 2 levels (G3,
`tests/engine/test_gpu_pipeline`). Accepted as the GPU path's look, with
export on the preview's pipeline (point 7). **Decided** by the owner on
2026-09-27 (through the VE Strategist: "i'm fine w/both"), together with
rotated tracks staying CPU islands (about 22 fps for three at 1080p Full)
as an accepted exception to M4's real-time box.
Effects stay CPU (frei0r, avfilter) inside the GPU graph the same way.
A rotation-capable GPU transform would need a movit effect of our own,
which means linking GPL movit; out of scope.

**6. Probe out of process, fall back automatically** (`app::GpuAcceleration`).

- `u-studio-render --gpu-probe` creates the context, initialises
  `glsl.manager`, renders a generated two-track frame through the GPU graph
  and compares it with the CPU graph within a tolerance. It exits non-zero
  on any failure, and a GL driver crash takes down only the child.
- The result is cached with the app and MLT versions it was made with; a
  cached pass turns the pipeline on at startup, and the probe runs again
  in the background on every launch (a driver update isn't in the key),
  correcting both the cache and the pipeline.
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
own render thread. In process (G4, `renderProject()`): a live session
(`GpuSession::current()`) is shared by reference, the export's context is
created in its share group and made current on the avformat consumer's one
render thread (`real_time=-1` on the GPU), and the preview switching back
to the CPU mid-export leaves the session alive until the export ends. When
MT5 moves export into `u-studio-render` (ADR-009), the child has no session
of its own: the editor passes it the choice (a `--gpu` flag) and it starts
one. A test renders a generated project through both
paths and requires them to match within 2 levels per channel, the
GPU-vs-source difference measured (the CPU colour offset is fixed,
7c7f9fa). Hardware *encode* (VAAPI `h264_vaapi`/`hevc_vaapi`
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
| G1 | Hardware decode in the engine: `openProducer()` with Live/Worker, `EngineSync::setHardwareDecode()` and the `\?hwaccel=` projection, `platform::hardwareDecodeApi()`; nothing turns it on yet (export takes it with the pipeline choice in G4) | Live masters decode on VAAPI when asked, workers stay software (test); a failed device falls back; docs |
| G2 | `platform::GlContext` (Linux EGL), `engine::probeGpu()`, `u-studio-render --gpu-probe` | Probe passes here and fails cleanly without EGL (test); no graph or UI change yet |
| G3 | GPU graph in EngineSync behind the flag; context on the consumer's render thread; runtime fallback; the probe run from the editor with its cached result; the crash sentinel; Settings › Performance's two switches, hardware decode following the GPU pipeline | M4's box: three transformed 1080p tracks at Full play every frame; 10-minute soak holds; ASan+TSan full suites; preview matches CPU within tolerance |
| G4 | Export on the GPU path; preview/export equality test; `u-studio-render` after MT5 | Render of a generated project matches the preview path within tolerance. Met in process (2026-09-27): 50% green over red, preview 137,137,50, export 137,138,51 (the CPU: 112,113,49); a title over video on the GPU has no dark fringe (darkest red 250+) and differs from the CPU's by 1.0 on average. `u-studio-render` waits for MT5 |
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

Findings and repro details: `docs/developer/notes/gpu.md`. Figures from
the quiet window of 2026-09-27 (other teams held; `vmstat` mean idle
73.8% over 147 samples; generated 1080p30 media; Iris Xe, Mesa, MLT 7.40).

**The real engine, CPU path** (`playback_soak --transformed 3`: V1 plus
three transformed tracks; 60 s each). Frames shown per second, and the
process's CPU (100% = one core):

| Source | Scale | Software decode | VAAPI decode |
|---|---|---|---|
| H.264 | Half | 21.8 fps, 179% | 20.6 fps, 150% |
| H.264 | Full | 13.6 fps, 219% | 12.3 fps, 185% |
| HEVC | Half | 22.1 fps, 191% | 20.4 fps, 148% |
| HEVC | Full | 13.0 fps, 229% | 11.9 fps, 184% |

**GPU against CPU under the real consumer** (a standalone graph: black plus
half-size cuts in quadrants; `sdl2_audio`, `real_time=1`, `rgba`; 20 s
each):

| Tracks | Scale | CPU path | GPU path | GPU path + VAAPI |
|---|---|---|---|---|
| 3 | Full | 20.7 fps, 127% | 29.9 fps, 61% | 29.9 fps, 36% |
| 3 | Half | 25.6 fps, 102% | 29.9 fps, 53% | 29.9 fps, 31% |
| 1 | Full | 30.1 fps, 93% | 30.1 fps, 31% | |

**One frame pulled at a time** (MT4's method, ms per 1080p frame):

| Tracks | CPU, rgba | CPU, yuv422 | GPU, rgba | GPU, yuv422 |
|---|---|---|---|---|
| 1 | 32.1 | 25.8 | 10.1 | 16.0 |
| 3 | 95.6 | 85.7 | 19.9 | 23.4 |

The GPU path wants `rgba` out, which is what PlaybackController asks for;
`yuv422` adds a CPU conversion.

**G4, a title's anti-aliased edges in an H.264 export** (white over red,
mean red deficit at edge pixels): the CPU export 14 (VE Core's alpha
pairing), the GPU export 26, and a CPU export without the pairing 31. The
pairing trick has no GPU counterpart yet; 4:2:0's shared chroma darkens
sharp red edges by itself.

**G3, the real engine on the GPU pipeline** (`playback_soak --transformed 3
--scale full`, 1080p30 H.264, `vmstat` mean idle 76.6%, 2026-09-27):

| Run | Frames shown | Process CPU |
|---|---|---|
| GPU + VAAPI, unrotated, 10 min | 17,835 of 18,000 (99.1%; every frame in 57 of 60 ten-second windows), lag a steady 5 frames | 62% |
| GPU + VAAPI, each track rotated (CPU islands), 2 min | 22 fps | 210% |
| CPU, unrotated, 2 min | 10 fps | 197% |

RSS grew 908 → 1064 MB over the 10 minutes on the GPU, about 15 MB a
minute, and 382 → 423 MB over 2 minutes on the CPU. On the GPU most of it
is an MLT bug found by ASan: `movit.convert` leaks an `MltInput` (about
1.2 KB) for every input of every frame whenever it reuses a chain, about
10 MB a minute at 30 fps with five inputs (docs/developer/notes/gpu.md).
It can't be freed from outside the module; the fix is a small MLT patch,
`packaging/flatpak/patches/mlt-movit-convert-input-leak.patch` (RSS +3.63
KB a frame → 0.00 with three inputs; LeakSanitizer clean), carried in the
Flatpak's MLT build (G5) and drafted for upstream in
`docs/developer/notes/mlt-upstream.md`. The CPU growth
is VE Core's open item.
