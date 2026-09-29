# MLT upstream candidates

[Docs home](../../README.md) › [Developer docs](../README.md) › [Implementation notes](README.md) › MLT upstream candidates

MLT behaviour we work around that looks like a bug upstream. Each entry
names the source, what we do instead, and where the repro lives. The
standalone repros filed upstream are in
[`tools/upstream-repros/mlt/`](../../../tools/upstream-repros/README.md),
one folder per report.

| Behaviour | Source | Our workaround | Repro |
|---|---|---|---|
| `composite` blends 4:2:2 YUV with each pixel's own alpha, so the two pixels of a pair mix U and V by different amounts: anti-aliased alpha edges get a dark, coloured fringe (white at alpha 5 over red: (60,63,5), not (255,5,5)). | `transition_composite.c`, `composite_line_yuv()` | `attachAlphaPairing()` evens out each pair with different alphas before the compositor ([Engine sync](engine-sync.md)) | `tests/engine/test_colour.cpp`, titles' `test_engine.cpp` |
| `composite`'s `halign`/`valign` are right only for frames read at the profile's size: `get_image` aligns `item.w` (profile pixels) against the B image's width (requested pixels). | `transition_composite.c`, second `alignment_calculate()` call | Centre only when frames are read at profile size (`EngineSync::FrameReads`) | [Engine sync](engine-sync.md) |
| `producer_colour` tags its YUV BT.601 whatever the profile, and `composite` keeps the A frame's tag. | `producer_colour.c` | Re-tag the black master; other colours as RGBA ([Engine sync](engine-sync.md)) | `tests/engine/test_colour.cpp` |
| In MLT XML a `resource` of `color:…` without `mlt_service` loads as black, whatever the colour. | `producer_xml.c` resource handling | Write `mlt_service=color` and a bare `0xRRGGBBAA` | [Engine sync](engine-sync.md) |
| `pixbuf` (stills, image sequences) ignores `video_index=-1`, which turns off an avformat producer's picture. | `producer_pixbuf.c` | Mark such frames `test_image` so the compositor skips them (`attachHideVideo()`) | `tests/engine/test_colour.cpp` |
| movit's `movit.convert` leaks a `movit::Input` (about 1.2 KB) per input per frame whenever it reuses a chain: `~MltInput()` leaves the input to a chain that never took it. ~10 MB a minute of GPU playback. | `filter_movit_convert.cpp`, `dispose_movit_effects()` and two error paths in `convert_image()` | The Flatpak's MLT carries a fix, `packaging/flatpak/patches/mlt-movit-convert-input-leak.patch` (report draft below); distro builds leak | [GPU](gpu.md), `tests/engine/test_gpu_engine` under ASan |
| `sdl2_audio` can hand `on_consumer_frame_show` a freed frame right after a consumer restart (a SEGV in `mlt_frame_get_position`, seen once under ASan). | `consumer_sdl2_audio.c`, `mlt_consumer.c` | None yet | [Playback engine](playback-engine.md) |
| A `colour` producer with the plus `affine` filter, encoded as QuickTime Animation (`qtrle`, `argb`) by the `avformat` consumer, died with SIGFPE in a `core` filter's `get_image` on the consumer's read-ahead thread; the same graph encoded to PNG, ProRes or VP9 didn't. Not reached by the editor's graphs (they render H.264). Not narrowed down further. | a `core` filter under `transition_affine`'s `get_image` | Tests encode qtrle from a PNG still instead | `tests/engine/test_colour.cpp` (`makeAlphaVideo`) |
| The `avfilter` filter leaks a properties object (with its strings) each time it sets up a filter graph: `init_image_filtergraph()` closes `p` only on its failure path, and the success path returns first. Once per graph (the first frame, then a format or size change), not per frame. | `filter_avfilter.c:431`, `:711` | None needed at that rate; LSan suppression `leak:filter_get_image` (`tests/sanitizers/lsan.supp`) | The effects drop-in's `test_engine` LUT case under `just asan` |
| The `avformat` consumer drops alpha when encoding to a yuva `pix_fmt` unless `mlt_image_format=rgba` is set: it requests yuv422 for anything but `rgba`/`argb`/`bgra`. | `consumer_avformat.c` | Set `mlt_image_format=rgba` for alpha exports | [Engine sync](engine-sync.md) |

## Report draft: movit.convert leaks a movit::Input per input per frame

For the MLT issue tracker (not filed yet), with the patch attached
(`packaging/flatpak/patches/mlt-movit-convert-input-leak.patch`, which
applies with `patch -p1` to MLT 7.40.0).

> **movit: `MltInput`'s `movit::Input` leaks on every frame that reuses a chain**
>
> MLT 7.40.0, movit 1.7.1. `convert_image()` in
> `src/modules/movit/filter_movit_convert.cpp` creates an `MltInput` for
> each input of each frame (`create_input()` news a `FlatInput` or
> `YCbCrInput`) and parks it on the frame. When the chain for the frame's
> fingerprint already exists, `finalize_movit_chain()` calls
> `dispose_movit_effects()`, which deletes the parked `MltInput`, but
> `~MltInput()` doesn't delete its `movit::Input`, which only an
> `EffectChain` would own. So each frame played through a reused chain
> leaks one movit input per MLT input: about 1.2 KB each. The two error
> paths in `convert_image()` that delete a parked `MltInput` leak the
> same way.
>
> Repro: any GPU graph pulled frame after frame, for example black plus
> two `movit.rect`'d colour producers composited with `movit.overlay`,
> 1,200 frames as RGBA from a thread with a GL context current. RSS grows
> 3.63 KB a frame, and LeakSanitizer reports allocations under
> `MltInput::useFlatInput()`. With the patch: no growth and no report.
>
> The fix deletes the `movit::Input` along with the `MltInput` wherever an
> input that never reached a chain is deleted. Such an input has no
> texture yet (inputs create theirs in `set_gl_state()`), so this needs
> no GL context.
