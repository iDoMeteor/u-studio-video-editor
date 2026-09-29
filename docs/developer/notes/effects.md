# Effects

[Docs home](../../README.md) › [Developer docs](../README.md) › [Implementation notes](README.md) › Effects

What the effects drop-in ([doc 15](../../plans/v2/15-effects-and-transitions.md),
[`drop-ins/effects/`](../../../drop-ins/effects/README.md)) relies on about MLT
7.40 and frei0r-plugins 2.5.6 (Fedora 44). Every row was a standalone repro
first.

## FX0 spikes (2026-09-27, VE Core, 320×180)

| # | Question | Answer |
|---|---|---|
| 1 | What frei0r brings, and whether any of it pulls in Qt | 158 plugin files; MLT registers **102 `frei0r.*` filters and 49 transitions**. `ldd` finds **no Qt** in any of them. MLT's frei0r module ships `blacklist.txt` (`perspective`) and `not_thread_safe.txt` (`baltan`, the `bigsh0t_*` 360° set, `colorhalftone`, `delay0r`, `delaygrab`, `distort0r`, `ising0r`, `medians`, `nervous`, `partik0l`, `plasma`, `tehRoxx0r`, `vertigo`): the drop-in's curated list leaves those out, or runs them only in a single-threaded render. |
| 2 | Parameter addressing: index or name | **Both.** `frei0r.brightness` with `"0"=0.8` and with `"Brightness"=0.8` gave the same result (grey 128 → 204). Prefer the index: it's what MLT's own presets and kdenlive write, and names can change between plugin versions. |
| 3 | Animating a frei0r parameter with an MLT keyframe string | **Yes.** `"0"="0=0.2;29=0.8"` on a filter with in/out 0–59: frame 0 → 51, frame 29 → 204. The keyframe offset rule for a cut with a non-zero `in` is IP3's (`engine::attachToCut()`, [engine sync](engine-sync.md#filter-in)). |
| 4 | `mask_start` / `mask_apply` with `transition=affine` (universal Mix / masked effects) | **Yes, Qt-free.** `mask_start filter=frei0r.brightness filter.0=0.9` then `mask_apply transition=affine` applies the effect (128 → 230). The default `transition` is `qtblend`, which ADR-007 denies, so `transition=affine` must always be set. A shaped (partial) mask wasn't tested here; that's FX2's with the mask UI. |
| 5 | `luma` with a generated 16-bit PGM `resource` | **Yes.** A 64×36 P5 gradient (maxval 65535) as `luma`'s `resource` wipes left to right: halfway, the left edge is already the B clip and the right edge still A. So wipe shapes can be generated, not shipped as images. |
| 6 | `frei0r.cairoblend` as a track compositor, and its blend modes | **Yes.** Red under 50% blue (`color:0x0000ff80`) gives 127/0/127 in `normal`. Parameter `"1"` takes mode names: `multiply` 127/0/0, `screen` 255/0/127, `overlay` 255/0/0, `add` 255/0/127. A transition needs its in/out set, or it covers frame 0 only. (MLT colours: 8-digit `#` is `#aarrggbb`; use `0xrrggbbaa` for a clear alpha.) |
| 7 | Adjustment blocks: a filter on a sub-tractor of lower tracks, with in/out | **Yes.** A filter attached to a tractor with in/out 20–39 applies there only (frames 10, 30, 50: 128, 230, 128). |
| 8 | Is `decorateTractor()` enough for adjustment blocks? | **Only for a lane on top.** A filter on the whole tractor affects every track, so a top FX lane works with `decorateTractor()` alone. A lane in the middle of the stack needs the tracks beneath it grouped into a sub-tractor that the filter attaches to: a structural hook, not a decoration. |
| 9 | In-place property set on a live filter while playing | **Yes**, done in IP3 (`EngineExtension::applyInPlace()`) and used by clip transforms (ADR-018): no rebuild, no consumer restart. |
| 10 | `<filter>` inside `<entry>` through MLT's `xml` producer | **Yes**, done in IP3 (the writer puts the cut's in/out on it; `tests/dropins/test_dropin_engine`). |

FX0 row 1's "leave the `not_thread_safe.txt` plugins out" turned out
unnecessary: see "Not-thread-safe frei0r plugins" below.

## FX1 findings (2026-09-28, VE Effects)

| Finding | Detail | Where it's used |
|---|---|---|
| Mix transition: `frei0r.cairoblend` | Inside `mask_apply`, `cairoblend`'s `"0"` (opacity) at 0.5 over brightness level 2 on grey 128 gave 192, for about 6 ms a 1080p frame over the plain effect. `affine`'s `rect` with a fifth value (`… 50%`) gave 191 for 14 ms. `composite` ignored its `geometry` opacity there (255) and cost 32 ms. | `core::nativeFilters()`; `affine` is the fallback without frei0r |
| A transition inside `mask_apply` can't animate on a cut in a playlist | `mask_apply` runs its transition when the image is fetched, and by then the playlist has set the frame's position to the timeline's; the transition subtracts the cut's source `in` and reads a negative position. `transition.0 = "0=0;80=1"` animated on a bare cut (128 → 64 → 0) and stayed at the first value in a playlist (128 throughout). A **filter's** animation position is fixed when the frame is processed, so a `brightness` filter with `level=1` and an animated `alpha` between `mask_start` and `mask_apply` animates in both (128 → 64 → 0), with the transition opaque. | A keyframed mix is `mask_start`, `brightness alpha=…`, `mask_apply`; a constant one is the transition's opacity |
| The filters' in/out must still be the cut's | Without them, neither the transition nor the alpha filter animates on a cut (`engine::attachToCut()` sets them; the writer does too). | Every cut filter |
| Not-thread-safe frei0r plugins | MLT's frei0r module already serialises them: one shared instance, the service lock held across `f0r_update`, no slicing (`frei0r_helper.c`, lines 144, 176 and 325). They're safe with frame threads, just slower. So they're offered, flagged `notThreadSafe`, not left out. | `EffectRegistry` |
| `brightness` works on YUV luma unless `rgb_only=1` | Level 0.5 on grey 128 reads 54 with the default, 64 with `rgb_only=1` (`filter_brightness.yml`). | Tests set `rgb_only` |
| frei0r's `3dflippo` writes out of bounds | It crashes whenever a frame is read at a size other than the profile's: 64×36 up to 960×540 in a 1080p profile, never at 1920×1080 or in a matching profile. Heap corruption doesn't always crash (the probe's noise source ran on). Marked `unstable` in the overlay, so it's quarantined from the start. | `data/overlays/frei0r.json` |
| frei0r's `defish0r` reports a NaN default | Its "Non-Linear scale" (`"9"`) default is `nan`; set, it blacks out the picture. The health probe caught it. A non-finite default counts as none. | `normaliseParam()` |
| frei0r's `pixs0r` runs for minutes at its maximums | Pixel sort at 1080p with every number at its maximum didn't finish in 2 minutes. The render tool catches SIGTERM for a clean cancel and a probe inside a plugin never checks, so the scan stops a child with SIGKILL at its deadline. | `HealthScan` |
| Services are registered as data | `mlt_repository_transitions()`'s entries hold data, not strings: `get("frei0r.cairoblend")` is null even when it exists. Ask `property_exists()`. | `EffectsExtension` |
| MLT's frei0r search order | `factory.c` walks FREI0R_PATH from its **last** directory and keeps the first registration of a name, so a later directory wins. It dlopens every plugin at init to read its info, then again when a filter is created. | `findFrei0rPlugins()` |
| Stock `melt` and `.ustudio` | `melt project.ustudio` fails to load it; `melt xml:project.ustudio` plays it (the loader picks by extension). | The melt test |
| Spawning from the editor costs 17–27 ms | `g_subprocess_launcher_spawnv()` from the editor's process blocked the main loop 17–27 ms per child (a 60 s run: about 40 stalls of 40–50 ms), and parsing the 1 MB registry JSON about 200 ms. The scan runs on its own thread around its own `GMainContext`; afterwards a 25 s run showed no stall from it. | `HealthScan` |
| Posting from a worker thread | `g_main_context_invoke_full()` runs the closure inline on the calling thread whenever nothing owns the main context, and GLib's lock is invisible to TSan (a data race on every hand-off in the frame renderer's first version). Both effects threads now post through `engine::MainThreadDispatcher` (`g_idle_add_full` plus TSan release/acquire annotations, `src/engine/dispatcher.cpp`). | `FrameRenderer`, `HealthScan` |
| Metadata cost | All 628 filters' metadata: about 0.2 s. Reading it beside a running graph isn't safe, so the editor gets the registry from `u-studio-render --effects-registry` (0.38 s in a child) and caches it. | `HealthScan` |

## FX3 findings (2026-09-28, VE Effects)

- **`luma` reads our generated 16-bit PGM maps** (`core::writeLumaMap()`, P5,
  maxval 65535, big-endian) and scales a 640×360 map to the frame. The
  incoming clip (b track) appears where the map is darkest first: with the
  `left` map (0 at the left edge), red → blue at 1280×720 showed blue at
  x=100 from frame 6 of 25, at x=640 by frame 18, and red at x=1180 until
  the end. Repro: a two-colour tractor with `luma` `resource=left.pgm`,
  in/out 0–24, sampling the centre row (scratchpad `luma_repro.cpp`).
- **A transition recipe is pure data in `Transition::params`**
  (`core/model/transition_native.h`): `video.*`, `audio.*`, and `a.<n>.*` /
  `b.<n>.*` filters on the outgoing tail and incoming head cuts.
  `EngineSync::buildTransitionSubTractor()` and the writer's dissolve
  sub-tractor both expand it with `core::nativeTransition()`, so `melt`
  plays what the editor does (`engine-xml-playback`, "A saved wipe and dip
  …"). A `ramp:v0,v1,…` value becomes an animation spread over the
  transition, so it follows a change of length.
- **Project files can't name a service**: `Model::check()` refuses a
  transition whose params name anything outside
  `transitionServiceAllowed()`, or set anything MLT would open, on any of
  its services (the cut filters too) and at any depth: `resource`,
  `factory`, `background`, `luma`, `producer.*` (`affine`'s filter opens
  its `background` as a producer and passes `producer.*` and
  `transition.*` on, filter_affine.yml). The reader then refuses the file. A map is only ever a
  generated one, by name; an unknown name plays as the plain dissolve.
- **Where maps are written:** the editor's graph uses
  `<user cache>/ustudio/luma/v<N>/<name>.pgm` (`platform::
  userCacheDirectory()`); a saved project gets `ustudio-wipes/v<N>/<name>.pgm`
  beside it, named relative to the project, so `melt` finds it. `N` is
  `core::kLumaMapVersion`: an existing map is reused, so bump it whenever a
  map function changes, or old maps would stay in use forever. Writes are
  atomic (a uniquely named temp file, then a rename, the temp removed on
  failure), so an editor and a render child can make the same map at once.
- **On the GPU pipeline a wipe stays the CPU `luma`**, an island in the
  movit graph; only the plain dissolve (and a dip, whose `brightness`
  filters sit on the cuts) uses `movit.luma_mix`. VE GPU's repro
  (2026-09-28, 1080p, 20-frame wipe): `movit.luma_mix` squares the progress
  (the edge sits still for 4 frames, then catches up) and opens `resource`
  through a producer, so the map arrives at 8 bits (transition_movit_luma.cpp
  :149), while the CPU `luma` reads the `.pgm` itself at 16 bits
  (transition_luma.c:784) and tracks the all-CPU wipe within ~9 px on every
  frame. The cost is ~140–155 ms per 1080p frame during a wipe (~60 on the
  CPU pipeline), so a wipe drops frames at Full preview; the export takes
  the preview's pipeline and is exact. Keep maps `.pgm`: another image
  format goes through the default loader (transition_luma.c:822), the gap on
  a live GPU session. Tests: test_gpu_pipeline "a wipe plays as the CPU's,
  frame by frame" and "a dip to black is black in the middle".
- **Motion recipes** (slide, push) are MLT's `affine` transition with an
  animated `rect` ("X% Y% W% H%", every field a percentage, transition_
  affine.yml) as the video transition, and for a push an `affine` filter
  on the outgoing cut with `transition.rect` (filter_affine.yml passes
  `transition.*` on) moving it out. With the filter's in/out the cut's
  (attachToCut()), both move linearly over the transition: standalone
  repro, a 25-frame red/blue pair sampled at x=100/640/1180 (scratchpad
  `motion_repro.cpp`, `push_repro.cpp`), and engine-xml-playback's "A
  saved push …" (the edge frame by frame, preview equal to melt). A ramp
  of rects separates them with `|`, since a rect has spaces.

## FX4 findings (2026-09-28, VE Effects)

- **Masks** are `frei0r.alphaspot` between `mask_start` and `mask_apply`
  (`core::nativeFilters()`, only when cairoblend, and so frei0r, is loaded).
  alphaspot draws the shape into the effect's alpha; its parameter 0 is the
  shape (0 a rectangle; frei0r scales 0-1 onto four shapes, so 0.3 is the
  ellipse), 1 and 2 the centre, 3 and 4 the *half*-width and -height (all
  fractions of the frame), 5 the tilt (0.5 upright), 6 the soft edge, 7 and 8
  the alpha outside and inside. The mix goes in as the inside alpha (an
  animated mix animates it: a filter, so its keyframes hold on a cut, unlike
  a transition's), and `mask_apply`'s cairoblend composites at full opacity
  by that alpha. Inverting swaps 7 and 8. Repro: red through invert0r, a
  centred 0.2-half-size rectangle: cyan at the centre and at (870, 480) of
  1280x720, red in the corner; 0.3 turns (870, 480) red (an ellipse); an
  inside alpha of 0.5 gives 127,127,127 (scratchpad `fx4/mask_repro.cpp`).
  The drop-in's test_engine "A mask limits an effect to its shape" plays it
  through EngineSync and the saved file.
- A mask's values (shape, geometry, soft edge, invert) apply in place like
  parameter values (`EngineSync::applyInPlace()` blanks them); adding or
  removing one changes the filters and rebuilds.
- MLT's metadata gives some rect defaults in percent (`spot_remover`'s
  "0 0 10% 10%"); the descriptor reads them as pixels (10 x 10). Open.

## FX5 findings (2026-09-28, VE Effects)

- **MLT's plugin hosts, read from source (MLT 7.40):**
  - openfx (`src/modules/openfx/factory.c:314-352`) always scans
    `/usr/OFX/Plugins` and `/usr/local/OFX/Plugins` and `dlopen`s every
    `.ofx` at factory init; `OFX_PLUGIN_PATH` only adds folders. So the
    module itself must be denied unless OpenFX is wanted and every plugin
    in reach is Qt-free (VE Core's FactoryPolicy change).
  - jackrack's LADSPA and VST2 managers (`src/modules/jackrack/
    plugin_mgr.c:377`, `:990`) use `LADSPA_PATH` / `VST_PATH` *instead of*
    their built-in lists when set, and walk folders recursively opening
    every `.so`: curated like `FREI0R_PATH`.
- **The effects registry's modules** beyond the core list: frei0r (103
  services), sox (64), jackrack's libmltladspa (11), oldfilm (6), plusgpl
  (5), kdenlive (3), vid.stab (2), rubberband, rnnoise, opencv (1 each).
- **LUTs:** `avfilter.lut3d`'s `av.file` is a path avfilter opens itself, so
  it isn't resolved against the project (unlike a producer's `resource`):
  the model and the render graph keep absolute paths, and only the saved
  model record names a file inside the project's folder relatively (`f:`
  values, read back against where the project is now).

## Frame renderer memory

Found 2026-09-29 (VE Effects), after the 0.78.0 Flatpak's GPU playback
soak read +40 MB/min with the Effects add-on installed and +2.5 without.

- **It was one step, not a slope.** RSS sat flat, jumped about 60–120 MB
  once, and stayed flat; a least-squares line from 30 s on reads such a
  step as tens of MB a minute. The step was the drop-in's frame renderer
  (`engine/frame_renderer.cpp`) opening the selected 1080p clip: the
  hidden Browser re-rendered a tile each time the health scan checked an
  effect, and the worker kept the media open for good.
- **An open 1080p H.264 producer costs about 180 MB** in a worker: the
  avformat producer's `threads` defaults to 0, so FFmpeg starts a frame
  thread a core, each with its own buffers (`producer_avformat.c`,
  `thread_count = 0`). `threads=1` halves it (84 MB) but makes seeks two
  to four times slower, so the renderer keeps the default and closes the
  media after `FrameRenderer::kReleaseIdleMedia` (2 s) without a request.
- **Freed isn't returned.** Closing the producer gave back only about
  20 MB of RSS: glibc keeps the decoder threads' arenas for reuse.
  `platform::releaseFreeMemory()` (`malloc_trim(0)` on Linux) after the
  close brought it to +58 MB over the start (the rest is FFmpeg's and
  MLT's one-time state). Repro: a `FrameRenderer` rendering 20 frames of
  a 1080p30 clip, RSS from `/proc/self/status` before, after, and 3 s
  after.
- **Hidden pages don't render.** The Browser renders tiles only while its
  page is mapped, and catches up on `map`; the eyedropper, Compare and the
  audition render only on a user action.
- **Two crashes in the renderer, found with it (the demo tour, 2026-09-29).**
  A clip that doesn't open (a title whose template isn't picked yet) was
  kept, invalid, under its key; the next tile for it cut and seeked the
  invalid producer (`mlt_producer_seek` on a cut forwards to its parent).
  An unopened clip now stays unopened for its key. And the pages that own
  renderers are statics destroyed at `exit()`, after `main()` has closed
  the factory: a worker still holding media closed it through a function
  pointer into an unloaded module. The drop-in calls
  `FrameRenderer::stopAll()` on the application's `shutdown` signal.
- **Hardware-only FFmpeg filters** (`*_vulkan`, `*_opencl`, `*_cuda`,
  `*_vaapi`, `*_qsv`, `*_amf`, `hwupload`/`hwdownload`/`hwmap`,
  `libplacebo`; 50 on Fedora 44's FFmpeg) are left out of the registry
  (`isHardwareOnlyFilter()`): MLT hands them software frames, and
  `blackdetect_vulkan` crashed in its teardown without Vulkan (Xvfb).
- The soak that guards this: `tools/effects-smoke/run.sh <builddir> <outdir>
  soak_steps.sh` ([testing](../testing.md#the-effects-smoke-test)).

## Health scan children

- **A probe can outlive its editor.** VE Demos saw `u-studio-render
  --probe-effect deshake` still running after the editor quit, re-parented
  to the user's systemd (not pid 1) and ignoring SIGTERM (a probe inside a
  plugin never checks the render tool's cancel; 2026-09-29). The scan now
  stops on the application's `shutdown` signal (its thread kills its
  children and reaps them), not at `exit()`, and the probe subcommand calls
  `platform::exitWithParent()` (Linux: `PR_SET_PDEATHSIG` with SIGKILL, which
  fires when the *thread* that spawned the child ends: the scan's worker,
  which outlives its children). Repro: a probe under a throwaway parent,
  the parent SIGKILLed: the probe was gone within a second.

## Blend dissolves

Found 2026-09-29 (VE Effects), for the Blends recipes (`dissolves.json`).

- **A blend mode alone doesn't dissolve.** `frei0r.cairoblend` with its
  opacity (`"0"`) rising 0 to 1 in `add` or `screen` mode ends on
  `add(A, B)`, not on B, so the picture would jump at the end. The recipes
  fade the outgoing clip too: B's opacity `ramp:0,1,1`, A's `brightness`
  `ramp:1,1,0` (`rgb_only`). With A at black the light modes (`add`,
  `screen`, `lighten`) give exactly B; the dark ones (`multiply`,
  `darken`) would need A fading to white, so they aren't offered.
- **The service is registered, not core's.** `frei0r.cairoblend` is in no
  core list: the drop-in registers it (`core::registerTransitionService()`)
  when MLT's repository has it, and a build without it plays such a recipe
  as the plain dissolve (`nativeTransition()`), keeping the recipe.
- **MLT's `color:` generator reads differently through a transition.**
  `color:0x600000ff` reads 96 red as RGBA on its own but 87 through a
  plain `luma` dissolve and 88 through `cairoblend` (a 709/601 matrix
  mismatch: reds dim, greens brighten, 96 green read 113). PNG and H.264
  (BT.709-tagged) sources read the same on every path. Tests that compare
  colours across a transition's edge use generated FFV1 or H.264 clips,
  not `color:` directly (the drop-in's test_engine "Blend dissolve").
- The blend tiles show the real clips like the others; GPU: the transition
  is a CPU island in the movit graph, like the wipes (not yet measured by
  VE GPU).
- **A sound cut** (the Sound row's Cut, doc 15's third curve): `mix` with
  `start=1 sum=1` adds the incoming track unscaled, and a `volume` filter
  on each side (cut filter 9, clear of the styles' own) holds it at -100 dB
  on its half: `hold:0,-100` / `hold:-100,0`, core's discrete form of
  `ramp:` (`"0|=0;<length/2>|=-100"`). `volume`'s `level` is in dB and
  animated (`filter_volume.yml`). Test: engine-xml-playback "A transition's
  sound can cut at the middle" (two tones 240 degrees apart: a cut keeps one
  tone's level on both sides; an even crossfade halves it at the middle).

