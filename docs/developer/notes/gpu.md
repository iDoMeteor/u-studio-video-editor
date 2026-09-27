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
- `movit.mirror` is horizontal and `movit.flip` vertical.
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
  `FlatInput`. The fix belongs in MLT: give the parked input a destructor
  that deletes it and its input, and clear it where a chain takes it over.
  `lsan.supp` suppresses `create_input` until then.
- The CPU path's own growth in the same soak (about 20 MB a minute with
  three transformed tracks) is VE Core's open item.

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
