# GPU notes

[Docs home](../../README.md) › [Developer docs](../README.md) › [Implementation notes](README.md) › GPU

Findings behind [ADR-019](../../plans/v2/adr/019-gpu-acceleration.md) (GPU
acceleration). Each was confirmed with a standalone repro against MLT 7.40,
movit 1.7.1 and Mesa on an Intel Iris Xe (VE GPU, 2026-09-27).

## A Qt-free GL context for movit

- MLT's `movit` module (`libmltmovit.so`) links no Qt. It pulls libmovit,
  libepoxy, fftw3 and libX11/GLX; the last two serve only its deprecated
  `xgl` consumer. The curated module directory (ADR-007) already includes
  it, because `factory_policy` denies only `qt6` and `glaxnimate`.
- movit needs a current GL context, nothing more. A surfaceless EGL display
  (`eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, …)`),
  `eglBindAPI(EGL_OPENGL_API)`, a 3.0 context with `EGL_NO_CONFIG_KHR`, and
  `eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)` are enough.
  Then `Mlt::Filter(profile, "glsl.manager")`, `fire_event("init glsl")`,
  and `get_int("glsl_supported")` is 1. libepoxy dispatches through the
  current EGL context; no window or display-server connection is involved.
- Under a real consumer (`sdl2_audio`, `real_time=1`), make the context
  current in a `consumer-thread-started` listener. It runs on MLT's render
  thread, and `eglBindAPI` must be called there too, since it is
  per-thread. Release it in `consumer-thread-stopped`. Overriding thread
  creation (`consumer-thread-create`, as kdenlive does for Qt's context)
  isn't needed. Fire `init glsl` once, from the first render thread.
- Scope every `Mlt::Consumer`/`Tractor` so it is destroyed before
  `Mlt::Factory::close()`. A consumer destroyed after it segfaults in
  `~Consumer`, and with buffered stdout the crash also swallows the
  program's output.

## Our context in the editor (`platform::GlContext`)

- libEGL is `dlopen`ed (`libEGL.so.1`; `USTUDIO_EGL_LIBRARY` overrides it,
  which the no-EGL test uses), so the build links nothing new.
  `eglGetPlatformDisplayEXT`, `eglQueryDevicesEXT` and `glGetString` come
  through `eglGetProcAddress` (libglvnd dispatches `glGetString` to the
  current context's vendor).
- Define `EGL_NO_X11` (and the older `MESA_EGL_NO_X11_HEADERS`) before
  `<EGL/egl.h>`: otherwise `eglplatform.h` includes Xlib, whose `None`,
  `Bool` and `Status` macros break ordinary C++.
- The display is initialised once per process and never terminated; other
  contexts may still use it, and drivers tear down badly at exit.
- Contexts use `EGL_NO_CONFIG_KHR` and no surface; a second context shares
  objects with the first (`eglCreateContext`'s share argument) and can be
  current on another thread at the same time.
- The probe (`engine::probeGpu()`, `u-studio-render --gpu-probe`) renders
  `color:#2080c0` placed by `movit.rect` in the top-left quadrant of a
  320×180 frame over black, and checks both areas within 3 levels. It then
  fires `close glsl` while the context is current, and clears the global
  `glslManager`, which returns the process to the CPU chain.

## `glsl.manager` is a process-wide, sticky switch

Before a manager exists, the default loader attaches `color_transform
deinterlace fieldorder crop swscale resize swresample resample
avcolor_space audioconvert` to a producer. Once one exists, *every*
producer loaded afterwards gets `movit.crop movit.resample movit.resize`
and `movit.convert` instead. `loader.ini` lists each movit normaliser
first, and a movit filter can only be created while a manager exists;
`loader-nogl` skips any `movit.` name (`attach_normalizers()` in MLT's
`producer_loader.c`). That includes producers opened on worker threads
that have no GL context.

| How the producer is opened, manager alive | Normalisers |
|---|---|
| `Mlt::Producer(profile, path)` (default loader) | movit chain |
| `Mlt::Producer(profile, "loader-nogl", path)` | CPU chain only |
| `Mlt::Producer(profile, "loader-nogl:" + path)` | CPU chain **and** movit chain (the default loader wraps it) |
| `Mlt::Producer(profile, "avformat", path)` | none |

So a CPU graph must never use the default loader while a GPU session can
be alive in the process: 0.60.0-beta.1's in-process export, a CPU graph
built on a pool thread while the preview played on the GPU, got movit's
normalisers on a thread with no GL context and aborted in libepoxy
("Couldn't find current GLX or EGL context"). `openProducer()` now gives
`loader-nogl` to every CPU graph and worker, and the default loader only
to a GPU graph (`test_gpu_pipeline`'s export case). Two gaps remain:
MLT's own `luma` transition opens a wipe's `resource` through the default
loader when it first renders, and so does any service that loads a file
by itself. Plain dissolves have no resource.

Destroying the manager filter does not switch back: the global property
`glslManager` still holds it. Clearing it does:
`mlt_properties_set_data(mlt_global_properties(), "glslManager", nullptr, 0, nullptr, nullptr)`
(kdenlive's `disableGPUAccel()` does the same). Found first by VE Installers
in the Flatpak sandbox.

## Hardware decode per producer

`producer_avformat` reads `hwaccel` from the resource's query string
(`parse_url()`). For a plain file with no format prefix the delimiter is an
**escaped** `\?`, so a literal `?` in a filename stays part of the name:

- `path\?hwaccel=vaapi` engages VAAPI. This works through the default
  loader and through `avformat`, and the resource property keeps the
  suffix.
- `path?hwaccel=vaapi` (unescaped) opens nothing through the loader, and
  through `avformat` it decodes in software.
- Setting a `hwaccel` property after construction does nothing: the codec
  is opened in the constructor. `avformat-novalidate` doesn't change that.
- The device isn't taken from the query string. It comes from
  `MLT_AVFORMAT_HWACCEL_DEVICE`, else `/dev/dri/renderD128`.
  `MLT_AVFORMAT_HWACCEL` switches every producer in the process.
- To see whether VAAPI is really in use, check the process's own
  `/proc/self/fdinfo/*` `drm-engine-video:` counter. MLT logs its hwaccel
  messages below the warning level `factory_policy` sets.

On the Iris Xe, one VAAPI H.264 1080p producer pulled serially costs
6.8 ms a frame against 4.0 in software (decode plus download). Under the
real playback graph it saves CPU but doesn't raise frames/s, because
compositing is the bottleneck.

## movit's service set and gaps

- Placement is `movit.rect` (`rect`, `distort`, `fill`, `halign`,
  `valign`). **No movit service rotates.** A CPU `affine` filter on a cut
  inside a GPU graph works: MLT converts around it, the picture matches the
  CPU path, and the cost is the CPU filter's.
- `movit.mirror` is horizontal and `movit.flip` vertical. They flip the
  whole frame the chain holds when they run, and a cut's `movit.rect`
  placement is applied before them (the loader's `movit.resize` reads
  `resize.rect` below every cut filter), so an unrotated flipped clip
  mirrors its rect to land where the CPU puts it (`gpuTransformFilters()`).
  In a rotated clip's CPU island they run on the fitted source ahead of
  `affine`, as the CPU `mirror` filter does. The CPU `mirror` filter itself
  must not go into a GPU graph: it asks for yuv422, and a rotated clip from
  a source that isn't frame-sized (1344x768 in 1080p) came out sheared
  (0.79, demo PiP; `test_gpu_pipeline` covers rotation, crop and flips
  together).
- `movit.crop` has no parameters; it is the loader's normaliser and
  applies the frame's crop properties.
- Compositing is the `movit.overlay` transition (Porter-Duff and SVG blend
  modes, `compositing`), with `movit.luma` and `movit.mix` for dissolves.
- kdenlive ships with movit hard-disabled ("Disable movit until it's
  stable", `src/mainwindow.cpp`); treat stability as unproven until our
  soak passes.

## The GPU graph (EngineSync, G3)

- **movit blends in linear light, always.** `mlt_movit_input.cpp` gives
  every RGBA input (titles, an RGBA background colour, a CPU island's
  output) `GAMMA_REC_709` whatever the frame says; only YUV inputs follow
  the frame's `color_trc`. So "declare everything linear" can't make it
  blend in gamma space as `composite` does. 50% `#20c040` over `#c02020`:
  CPU 112,113,49 (the coded values averaged), GPU 137,137,50 (light
  averaged; `test_gpu_pipeline` checks both against their formulas).
  Opaque pictures match within 2 levels.
- **Dissolves use `movit.luma_mix`,** which squares the progress so a
  dissolve in linear light looks even (`transition_movit_luma.cpp`);
  `movit.mix` takes the progress as it is. The midpoint therefore differs
  from the CPU's `luma`; the frames either side of the dissolve match.
- **A plain Fit of another aspect is placed by `movit.rect`,** not left to
  the compositor: `movit.overlay` doesn't centre a picture by itself, and
  the rect costs nothing on the GPU (`EngineSync::compositorFits()` is
  CPU-only).
- **The CPU `affine` filter's canvas producer must be a worker producer**
  (`loader-nogl`). The filter reads it on the CPU; opened through the
  loader while a manager exists, it would hand the filter a movit frame.
  Off the GL thread that's a crash, not just a wrong picture: the effects
  drop-in's frame renderer (Transitions tiles, Browser previews) aborted
  inside `filter_affine.c` rendering a push while the GPU pipeline was on
  (smoke, 2026-09-29; `effects-gpu` repro). Every `affine` filter a worker
  builds gets a `loader-nogl` `colour:0` as its `producer`.
- **The core `crop` filter works unchanged under movit:** it sets the
  frame's crop properties, and the loader's `movit.crop` normaliser applies
  them (`test_gpu_pipeline`'s crop case matches the CPU).
- **Alpha pairing is skipped on GPU graphs** (VE Core's 4:2:2 fringe fix):
  movit gets the source's alpha straight and blends RGBA itself.
- **The consumer's render thread** takes the context in a
  `consumer-thread-started` listener registered before `start()`, and
  releases it in `consumer-thread-stopped`, which fires inside `stop()` on
  that thread before it's joined. Switching pipelines must stop the
  consumer before the listeners go and the session ends
  (`Engine::Thread::disableGpu()`); otherwise a thread could exit with the
  context still current on it.
- **Teardown order:** consumer, then the graph (EngineSync), then the
  session, whose destructor fires `close glsl` with the context current
  and clears the global switch.
- The GPU graph starts at about 2.5x the CPU graph's RSS (about 910 MB
  against 380 at 1080p, four tracks; movit's resource pool and chains).
- **MLT leaks an `MltInput` per input per frame on the GPU path**
  (MLT 7.40, found by ASan in `engine-gpu-engine`). `convert_image()` in
  `filter_movit_convert.cpp` makes one for every input of every frame and
  stores it with `GlslManager::set_input()`, which is
  `set_frame_specific_data()` on the frame with no destructor. Only
  `build_movit_chain()`, which runs when a chain is (re)built, takes it
  over. With the chain reused, as on every ordinary frame, it leaks: about
  1.2 KB an input a frame (the `MltInput`, its movit `FlatInput` and that
  effect's uniform vectors), ~10 MB a minute of 30 fps playback with five
  inputs, most of the GPU soak's growth. It can't be freed from outside:
  the type is private to the module, and its destructor doesn't free its
  `FlatInput`. The actual cause, found while writing the patch: MLT does
  delete the parked `MltInput` (`dispose_movit_effects()`, and the two
  error paths in `convert_image()`), but `~MltInput()` leaves its
  `movit::Input` to a chain that never took it. The fix
  (`packaging/flatpak/patches/mlt-movit-convert-input-leak.patch`, 14
  lines) deletes both there; a parked input has no texture yet, so no GL
  context is needed. Measured: RSS +3.63 KB a frame → 0.00 (three inputs,
  1,200 frames), and `engine-gpu-engine` under ASan with no `create_input`
  suppression reports nothing. `lsan.supp` keeps the suppression for
  distro MLT builds. How it was built and checked, to redo on an MLT
  bump: copy MLT's `src/modules/movit`, compile it against the installed
  MLT headers and movit's (the Flatpak build tree has them), and point a
  build tree's `mlt-framework-7.pc` `moduledir` at a directory holding
  the patched module.
- The CPU path's own growth in the same soak (about 20 MB a minute with
  three transformed tracks) is VE Core's open item.

## The loader needs MLT's `deinterlace` (the `xine` module)

The loader's deinterlace normaliser is `deinterlace`, from MLT's `xine`
module; without it (`loader.ini`: `deinterlace=deinterlace,avdeinterlace`)
the loader falls back to avformat's `avdeinterlace`, which hands on every
producer's frame as BT.601 limited-range YUV 4:2:2. On the GPU pipeline
movit then gets YUV where it would have had the colour producer's RGBA,
and `#2080c0` comes out 44,128,191: the Flatpak's first test build had
`MOD_XINE=OFF` and its `--gpu-probe` failed with exactly that (G5,
2026-09-27; reproduced natively with `USTUDIO_MLT_DENYLIST=qt6:glaxnimate-qt6:xine`).
It also costs a CPU conversion per frame. `engine-factory-policy` now
requires the `deinterlace` filter. The module has no dependencies of its
own.

## One master producer, one input per frame (0.69.x crash)

MLT's movit keys a chain's inputs by producer: `build_movit_chain()` and
`set_movit_parameters()` look them up as `chain->inputs[producer]`, where
`producer` is the frame's cut parent (`filter_movit_convert.cpp`). If one
master feeds two inputs of the same frame, both land on one `MltInput`, and
when they differ in size or format, movit uploads one's pixels with the
other's dimensions. That happens with a copy of a clip on another track, or
a dissolve between two clips of the same file. VE Demos hit it in 0.67.1:
V3's rotated, flipped picture-in-picture was a copy of a V1 clip, and V2
had 1344×768 clips. At Half, the rotated CPU island arrives as RGBA at the
profile's size while V1's cut is YUV at the source's, and the paused
refresh aborted in movit's `create_fbo` (`status == GL_FRAMEBUFFER_COMPLETE`).
Our repro segfaulted instead, in `glTexSubImage2D` under
`YCbCrInput::set_gl_state`, in about one run in six (Fedora's movit has no
asserts).

On the GPU pipeline each track now has its own masters, alternating
between neighbouring clips so a dissolve's two sides differ
(`EngineSync::masterLane()`). `test_gpu_pipeline` checks that structurally
(no master shared between tracks or across a same-file dissolve) and renders
the tour's four-track graph frame by frame at Full and Half. The cost is up
to two decoders per asset per track instead of one per asset.

## Frames rendered on the consumer's own thread (0.71.x crash)

I had assumed the consumer's render thread renders every frame, so only it
needed a GL context. It doesn't. At `real_time=1`, MLT's read-ahead thread
(`consumer_read_ahead_thread()`, `mlt_consumer.c`) skips `get_image()` for a
frame it judges late (`skip_next`) and passes the frame on unrendered
(`rendered` unset). sdl2_audio (`consumer_sdl2_audio.c`) then fires
`consumer-frame-show` without checking that flag in three places:
- the paused refresh: `consumer_thread()` shows each frame it pulls at
  speed 0 directly (line 630);
- the stop path: `video_thread()` "spits out all the frames" still queued
  (lines 531, 537), and `consumer_thread()` shows its last frame (line 664).
  A stop happens on every rebuild, since `setTractor()` restarts the
  consumer.

Our handler (`PlaybackController::handleFrameShow()`) calls `get_image()`,
which then runs the whole movit graph on sdl2's thread. With no context
current there, every framebuffer is incomplete, and movit asserts in
`create_fbo` or `EffectChain::render`. It needs a late frame, so it was
intermittent and got likelier under load: while recording, while other
sessions used the GPU, and around rebuilds. `engine-gpu-engine` hit it
one run in two to four.

Measured with frame-show instrumented (`rendered`, the image data,
`_speed`, whether `shutdown()` was stopping the consumer) over `gpu_stress`:
- 2 minutes: 164 frames arrived rendered, and 4 unrendered, all during a
  stop.
- 2.5 minutes under 4 parallel x264 encodes: 349 rendered, 1 unrendered
  during a stop, and 1 unrendered at speed 0 while not stopping (the paused
  refresh). The two crash reports came right after a pause.

An earlier version of this note blamed `mlt_consumer.c`'s "forcing next
frame". That branch is in `worker_get_frame()`, which runs only for
`|real_time| > 1`; VE Bugs pointed it out (2026-09-28).

Fix: the session has a second context in the same share group, which
`handleFrameShow()` makes current around `get_image()` on the GPU pipeline
(`PlaybackController::setFrameShowHooks()`, `GpuSession::frameShowEnter()`).
The render thread and the consumer thread can now both render; MLT's movit
serialises a chain per service (`lock_service`), and movit's resource pool
is thread-safe.
- Evidence: `engine-gpu-engine` failed 1 of 2 before the fix and passes
  20 of 20 after (`--repeat 20`). `gpu_stress` (random play, pause, seek,
  scale, proxies and dissolve edits through Engine) aborted within about
  30 s on seeds 1 and 2 before, and runs 3 minutes clean on each after;
  meson runs it for 45 s (`engine-gpu-stress`).
- On the CPU pipeline, rendering there is what MLT intends and needs no
  thread-bound state.
- Exports are unaffected: at `real_time=-1` the consumer waits for the
  render thread to render each frame.

Since 0.75.1, PlaybackController drops the frames shown while the consumer stops (`m_stopping`, checked before any GL context is made current), so only the paused refresh can still render on sdl2's thread.

## Wipes on the GPU pipeline (FX3, 2026-09-28)

A wipe (a luma transition with a gradient map) on the GPU pipeline should
be MLT's CPU `luma` inside the GPU graph, not `movit.luma_mix`.

- **Repro:** 1080p30, a 20-frame wipe between two H.264 clips (solid
  `#2040c0` and `#20c040`, BT.709-tagged), map `wipe16.pgm`: 640×360, P5,
  maxval 65535, a left-to-right ramp (big-endian 16-bit samples),
  softness 0.1. The graph is EngineSync's dissolve shape: black on track 0,
  a sub-tractor holding the tail and head cuts joined by the transition
  (in/out 0–19), composited by `composite` (CPU) or `movit.overlay` (GPU).
  Each of the 20 frames is pulled as RGBA, and the wipe edge is the first x
  on row 540 whose green falls below 128.
- **CPU `luma` inside the GPU graph:** the edge tracks the all-CPU wipe
  within ~9 px on every frame, so progress is linear. The map keeps 16 bits
  (`transition_luma.c` reads `.pgm` itself with `mlt_luma_map_from_pgm()`),
  there's no banding, and preview equals export (exports take the preview's
  pipeline). It costs ~140–155 ms a 1080p frame pulled one at a time (two
  downloads, the CPU blend, one upload), against ~60 all-CPU, so a wipe drops
  frames at Full preview while it plays.
- **`movit.luma_mix`:** the edge stays at x=0 for frames 0–3, then catches
  up (edge 53 at frame 4 against the CPU's 362, 1880 at frame 19): it
  squares the progress (`transition_movit_luma.cpp`: `mix = pow(mix, 2.0)`)
  to look even in linear light. It also opens the map through a producer
  (`mlt_factory_producer(profile, nullptr, resource)`, line 149), so the
  gradient reaches movit at 8 bits. Matching the CPU would take an MLT patch
  to both.
- Keep wipe maps `.pgm`: for other formats the CPU `luma` opens the map
  through the default loader (`transition_luma.c`, line 822), which gives
  movit normalisers while a GPU session lives (the loader section above).
- A standalone repro's CPU baseline needs VE Core's background re-tag
  (`attachProfileColorspace()`), or its colours come out BT.601-shifted and
  look like a GPU error.

Motion transitions (slide, push) need no GPU variant: MLT's `affine` transition in the dissolve sub-tractor (slide), plus an `affine` filter on the tail cut (push), play as CPU islands inside the GPU graph exactly as on the CPU. Repro at 1080p30, a 25-frame transition between H.264 clips, the sub-tractor composited over black by `composite` or `movit.overlay`: the incoming clip's edge on row 540 is at the same x on both pipelines at every sampled frame (1680, 1440, … 0: linear, 80 px a frame), with no gap between the pictures. A 1080p frame pulled on its own costs about 40 ms for a slide on the GPU pipeline (64 on the CPU) and 66 for a push (67): the island is cheap here because the compositing around it runs on the GPU, unlike a wipe's two downloads and an upload. Not checked: the outgoing picture's own movement during a push (solid colours can't show it); the CPU recipe's own repro covers it.

## Exports (G4)

- `renderProject()` checks `GpuSession::current()` once, at the start: with
  the preview on the GPU, the export shares the session and renders on the
  GPU pipeline with a context of its own from `GpuSession::sharedContext()`
  (same share group: movit's `ResourcePool` hands textures between the
  two), current on the consumer's render thread via
  `consumer-thread-started`/`-stopped`.
- One render thread (`real_time=-1`) on the GPU: parallel render threads
  would each need a context, and movit's chains are locked per service
  anyway. The encoder keeps its thread budget.
- The session is shared by reference and ends with its last holder; its
  destructor holds the registry lock, so a new session can't start while a
  dying one still owns the global manager. Never let the last reference go
  on the main thread: making our context current there would replace
  GTK's.
- A CPU export while the preview is on the GPU is a CPU graph with
  `loader-nogl` producers (see the loader section above).

## Measurements

The quiet-window figures (2026-09-27) are in ADR-019's "Evidence" section.
In brief:
- Three half-size 1080p tracks play every frame on the GPU path at Full
  and Half, using about half the CPU of the CPU path, which shows 20.7 and
  25.6 frames/s.
- Hardware decode costs frames on the CPU path but nearly halves CPU use on
  the GPU path.

Colour, BT.709-tagged limited-range source: the GPU path is within 1–2
levels of the source. The CPU path was off by up to about 20 (red 253 →
231) until 7c7f9fa. The cause was MLT's `colour` producer, which tagged the
black background BT.601; `composite` kept the tag, and the RGBA conversion
used it. An *untagged* source differs more on both paths, because each
assumes its own matrix.
