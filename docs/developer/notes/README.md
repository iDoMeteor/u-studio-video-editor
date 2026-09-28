# Implementation notes

[Docs home](../../README.md) › [Developer docs](../README.md) › Implementation notes

MLT's documentation is thin and several of its return values can't be
trusted. So every surprising MLT, GTK or GLib behaviour this project relies
on was confirmed with a standalone repro first, and the finding is written
down here. Read the note for the area you're touching before you change it.
If you find something new, add it here, or in the engine comment where it
applies ([CLAUDE.md](../../../CLAUDE.md), "MLT empirical-knowledge rule").

These notes used to be the second half of the root `README.md`. They moved
here unchanged on 2026-09-26.

| Note | Covers |
|---|---|
| [Playback engine](playback-engine.md) | The engine thread, consumer preroll, preview scale, SDL signal handlers, pause/seek/refresh, one consumer per process, multi-track audio `mix` |
| [Engine sync](engine-sync.md) | mlt++ wrapper ownership, chunked playlists, the `mix` calloc and mmap threshold, rebuild-per-track, stream switches on masters, dissolve sub-tractors, transition strip rules, per-command batching, invalid producers, filter `in`, `composite` + `affine` transforms |
| [Media caches and probing](media-caches.md) | Waveform profile/fps and peak cap, thumbnail batching, the avformat decoder cap, the `dv_pal` default profile, lazy `meta.media.*`, audio detection |
| [Render](render.md) | Cancelling, avformat consumer properties, H.264 encoder choice, snapshot rendering, atomic `.part` output, progress events |
| [Project files](project-files.md) | MLT XML with `ustudio:` properties, the format v4 render/record playlists, untrusted-input checks, autosave recovery rules |
| [Settings and GSettings](settings.md) | Missing-schema fallback, testing with the memory backend |
| [GPU](gpu.md) | Our own EGL context for movit, `glsl.manager` as a sticky process-wide switch, `loader-nogl`, per-producer `\?hwaccel=`, movit's service set and gaps, measured throughput and colour |
| [Keyframes and easing](animation.md) | The one keyframe model shared by effects and titles, `easedValue()`, how MLT interpolates each easing, cut-edge keyframes |
| [Effects](effects.md) | M5 FX0 spikes: frei0r services and Qt check, parameter addressing and animation, `mask_start`/`mask_apply`, 16-bit `luma` wipes, `cairoblend` modes, adjustment blocks. FX1: the mix's transition and why a keyframed mix is an alpha filter, not-thread-safe plugins, plugins that crash, hang or report NaN defaults, frei0r search order |
| [Titles](titles.md) | T0 spikes: a custom MLT producer module with YAML metadata, Pango per thread, 4K cost, alpha conversion, `xml` round trip, app fonts, GApplication actions from another process. T1: hinting off for scale-true layout, font directories, detecting a substituted font, renderer cost, making the producer through `loader`, a self-contained module, boundless assets, watching atomic saves |
| [MLT upstream candidates](mlt-upstream.md) | MLT bugs we work around, with our workaround and repro, to report upstream later |
| [App shell](app-shell.md) | Shortcuts vs text entry, `GtkRecentInfo`, `g_file_trash()`, the playhead overlay, ruler height |

Related:

- [Architecture](../architecture.md): where each of these pieces sits.
- [Audits](../../audit/README.md): the bug and sanitizer audits many of
  these findings came from.
- [v2 design docs](../../plans/v2/README.md), in particular
  [doc 05](../../plans/v2/05-playback-engine.md) (playback) and
  [doc 19](../../plans/v2/19-concurrency.md) (concurrency).
