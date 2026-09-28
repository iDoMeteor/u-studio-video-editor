# MLT upstream candidates

[Docs home](../../README.md) › [Developer docs](../README.md) › [Implementation notes](README.md) › MLT upstream candidates

MLT behaviour we work around that looks like a bug upstream. Each entry
names the source, what we do instead, and where the repro lives. Nothing
here has been filed; reports are drafted from this list later (VE Core's
queue), one repro each against the MLT version in use (7.40).

| Behaviour | Source | Our workaround | Repro |
|---|---|---|---|
| `composite` blends 4:2:2 YUV with each pixel's own alpha, so the two pixels of a pair mix U and V by different amounts: anti-aliased alpha edges get a dark, coloured fringe (white at alpha 5 over red: (60,63,5), not (255,5,5)). | `transition_composite.c`, `composite_line_yuv()` | `attachAlphaPairing()` evens out each pair with different alphas before the compositor ([Engine sync](engine-sync.md)) | `tests/engine/test_colour.cpp`, titles' `test_engine.cpp` |
| `composite`'s `halign`/`valign` are right only for frames read at the profile's size: `get_image` aligns `item.w` (profile pixels) against the B image's width (requested pixels). | `transition_composite.c`, second `alignment_calculate()` call | Centre only when frames are read at profile size (`EngineSync::FrameReads`) | [Engine sync](engine-sync.md) |
| `producer_colour` tags its YUV BT.601 whatever the profile, and `composite` keeps the A frame's tag. | `producer_colour.c` | Re-tag the black master; other colours as RGBA ([Engine sync](engine-sync.md)) | `tests/engine/test_colour.cpp` |
| In MLT XML a `resource` of `color:…` without `mlt_service` loads as black, whatever the colour. | `producer_xml.c` resource handling | Write `mlt_service=color` and a bare `0xRRGGBBAA` | [Engine sync](engine-sync.md) |
| `pixbuf` (stills, image sequences) ignores `video_index=-1`, which turns off an avformat producer's picture. | `producer_pixbuf.c` | Mark such frames `test_image` so the compositor skips them (`attachHideVideo()`) | `tests/engine/test_colour.cpp` |
| `sdl2_audio` can hand `on_consumer_frame_show` a freed frame right after a consumer restart (a SEGV in `mlt_frame_get_position`, seen once under ASan). | `consumer_sdl2_audio.c`, `mlt_consumer.c` | None yet | [Playback engine](playback-engine.md) |
| A `colour` producer with the plus `affine` filter, encoded as QuickTime Animation (`qtrle`, `argb`) by the `avformat` consumer, died with SIGFPE in a `core` filter's `get_image` on the consumer's read-ahead thread; the same graph encoded to PNG, ProRes or VP9 didn't. Not reached by the editor's graphs (they render H.264). Not narrowed down further. | a `core` filter under `transition_affine`'s `get_image` | Tests encode qtrle from a PNG still instead | `tests/engine/test_colour.cpp` (`makeAlphaVideo`) |
| The `avformat` consumer drops alpha when encoding to a yuva `pix_fmt` unless `mlt_image_format=rgba` is set: it requests yuv422 for anything but `rgba`/`argb`/`bgra`. | `consumer_avformat.c` | Set `mlt_image_format=rgba` for alpha exports | [Engine sync](engine-sync.md) |
