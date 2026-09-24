# 13 — Risks and open questions

## Risks

| # | Risk | Likelihood | Impact | Mitigation |
|---|------|-----------|--------|------------|
| R1 | **MLT thread-safety while editing during playback.** Rebuilding a playlist under `Service::lock()` while the consumer's threads hold frames referencing old cuts. Kdenlive does this and it works, but there are historical crash bugs around it. | Medium | Crash | Keep the *master* producers alive across rebuilds (only cuts are recreated); never destroy a master while the consumer runs (deferred deletion list drained on pause/stop); ASan test loop of "edit while playing" in `tests/engine`. |
| R2 | **`sdl2_audio` via `sdl2-compat`/SDL3 under PipeWire** may misbehave (device selection, latency, resampling). | Medium | No audio / glitches | `rtaudio` fallback is present; `null` last resort keeps the app usable; test on a clean Fedora Workstation in M2. |
| R3 | **GTK texture upload cost** for 1080p+ RGBA at 60 fps (memcpy + GPU upload per frame) on weak iGPUs. | Medium | Dropped frames in preview | `scale=0.5` during playback (default Auto); later `GdkGLTextureBuilder`/dmabuf path; measure in M2 with `GDK_DEBUG=frames`. |
| R4 | **Curated module dir** breaks if a future MLT changes `mlt_factory_init(dir)` semantics or module file names. | Low | Qt sneaks back / missing modules | The policy test fails loudly in CI; Flatpak builds MLT without Qt so the shipped artifact doesn't depend on the trick. |
| R5 | **Rebuild-per-track cost** with very long timelines (10k+ clips on a track) during scrubbing edits. | Low for target users | Laggy edits | Verifier-backed incremental path is the planned optimisation; batch events coalesce rebuilds within a transaction. |
| R6 | **`composite` transition quality/perf** vs. `frei0r.cairoblend`/`qtblend` (e.g. sub-pixel positioning, blend modes). | Medium | Visual quality | Prefer `frei0r.cairoblend` when present; `movit` GPU path is out of scope; document known limitations in the effect panel. |
| R7 | **Mixed frame-rate sources** resampled by MLT (30→25 stutter) surprise users. | High | Perceived quality | Choose the sequence profile from the first imported clip by default; warn on mismatched imports; profile change support (rebuild) is planned (`SequenceProfileChanged` is declared, but no command emits it yet). |
| R8 | **Team velocity hit from M0 restructure** while feature work is in flight. | Medium | Merge pain | One announced window, one PR, mechanical moves only, no logic changes; `git mv` to preserve history. |
| R9 | **libxml2 API churn** (2.13+ deprecations). | Low | Build warnings | Wrap in one `core/xml/xml_util.h`; only use the tree API. |
| R10 | **Fonts not bundled on bare-metal builds** so the brand look differs from Flatpak. | Certain | Cosmetic | CSS fallbacks already exist; document `dnf` packages; Flatpak is the reference artifact. |
| R11 | **A native effect plugin (frei0r, LADSPA, VST2, OpenFX) crashes or hangs in-process** on real footage or unusual parameters (ADR-011). | Medium | Crash / data loss | Out-of-process health probe before any plugin is offered; curated `FREI0R_PATH`; MLT's `blacklist.txt` and `not_thread_safe.txt`; autosave; quarantine list the user can edit. |
| R12 | **frei0r packaging drift**: plugin renames and parameter changes between frei0r releases break saved projects; a subpackage (e.g. `-opencv`) pulls Qt. | Medium | Broken effects / Qt in process | Respect MLT's `aliases.yaml` and `param_name_map.yaml`; store the plugin version in the project; `ldd` check and factory-policy test over every plugin. |
| R13 | **Pango threading inside an MLT producer** (doc 16): shared font maps across consumer/render threads. | Medium | Crash in titles | One `PangoCairoFontMap` per producer; T0 stress test under the render CLI. |
| R14 | **Every parameter change rebuilds the tractor and restarts audio** (ADR-005 plus hot-swap removal), making sliders unusable. | Certain without the fix | Unusable effect UI | Doc 15's in-place parameter path is an FX1 acceptance criterion. |
| R15 | **Three tracks at once** (M3, FX, titles) split a small team. | High | Slippage | FX0 and T0 are short spikes; each track has its own gate; M3 is not blocked by either. |

## Open questions (decide by M1 start)

Resolved 2026-09-17 (team decision, questions reviewed one by one):

1. **C++23 vs C++20.** ✅ **Decided: C++23.** Docs assume 23 for
   `std::expected`. If any target compiler is < GCC 13, drop to 20 and
   vendor a small `Expected` — not a concern for now (dev machine is GCC
   16.1.1).
2. **Linked audio/video clips.** ✅ **Decided: single clip carries its own
   audio** (v2.0 docs' default), not kdenlive-style linked clips on two
   tracks. Simpler, covers the common case; "split audio" makes it explicit
   when needed.
3. **Track order in the model** (index 0 = top) vs MLT order. ✅ **Decided:
   index 0 = top, visual order in the model**, with reversal against MLT's
   native order happening once, inside `EngineSync`.
4. **Repository hosting/CI.** ✅ **Settled by reality, not a decision**: the
   repo lives on GitHub (`iDoMeteor/u-studio-video-editor`), so doc 11's
   GitHub Actions assumption applies as written — no translation needed.
5. **Does the app need multiple windows/documents in v2.0?** ✅ **Decided:
   single document, multi-window off** for v2.0 (per the docs'
   recommendation). The architecture still supports multi-window later (one
   `DocumentSession` each); just don't reintroduce the leaked-singleton
   pattern from the current v1 `AppWindow`.
6. **Effect panel placement**: right sidebar (docs) vs. below the preview.
   **Still open — deferred to M5 with a mockup**, as the doc originally
   specified. Not decided now. Doc 15 (2026-09-23) recommends a
   collapsible right sidebar (the Effect Rack) and lists it under
   "Decisions needed from the owner".
7. **Timecode**: NDF only in v2.0. ✅ **Decided: NDF only, DF explicitly out
   of scope** for v2.0.
8. **Proxy default**: auto-generate for sources > 1080p? ✅ **Decided: ask
   once per project on first 4K+ import** (not auto-generate silently, not
   manual-only).
