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

## Measurements

Three half-size 1080p30 H.264 cuts in quadrants over black, played through
`sdl2_audio` at `real_time=1`, `rgba` (frames shown per second). The load
average was about 40 from other work, so the CPU row is pessimistic; the
quiet-machine rerun goes in ADR-019's Evidence section.

| Path | Full | Half |
|---|---|---|
| CPU: `affine` + `composite` | 10.9 | 17.2 |
| GPU: `movit.rect` + `movit.overlay` | 29.8 | 29.8 |

Colour, BT.709-tagged limited-range source: the GPU path is within 1–2
levels of the source. The CPU path, with or without `affine`, is off by up
to about 20 (red 253 → 231; cyan's red 0 → 22). The offset is in MLT's CPU
YUV→RGBA step, and VE Core has it as a possible existing bug. An
*untagged* source differs more on both paths, because each assumes its own
matrix.
