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
| R7 | **Mixed frame-rate sources** resampled by MLT (30→25 stutter) surprise users. | High | Perceived quality | Choose the sequence profile from the first imported clip by default; warn on mismatched imports; profile change support (rebuild) exists. |
| R8 | **Team velocity hit from M0 restructure** while feature work is in flight. | Medium | Merge pain | One announced window, one PR, mechanical moves only, no logic changes; `git mv` to preserve history. |
| R9 | **libxml2 API churn** (2.13+ deprecations). | Low | Build warnings | Wrap in one `core/xml/xml_util.h`; only use the tree API. |
| R10 | **Fonts not bundled on bare-metal builds** so the brand look differs from Flatpak. | Certain | Cosmetic | CSS fallbacks already exist; document `dnf` packages; Flatpak is the reference artifact. |

## Open questions (decide by M1 start)

1. **C++23 vs C++20.** Docs assume 23 for `std::expected`. If any target
   compiler is < GCC 13, drop to 20 and vendor a small `Expected`. Default:
   23.
2. **Linked audio/video clips.** v2.0 models a video clip as carrying its
   own audio (one clip). Kdenlive models them as two linked clips on two
   tracks. Ours is simpler and covers the common case; "split audio" makes
   it explicit when needed. Confirm this matches the team's editing habits.
3. **Track order in the model** (index 0 = top) vs MLT order. Docs pick
   visual order in the model with reversal in `EngineSync`. Alternative is
   MLT order everywhere and reversal in the view. Either works; pick one and
   never mix.
4. **Repository hosting/CI.** Docs assume GitHub Actions. If the repo lives
   on GitLab or Codeberg, translate doc 11's CI section (the container steps
   are identical).
5. **Does the app need multiple windows/documents in v2.0?** The
   architecture supports it (one `DocumentSession` each); the UI can ship
   single-document. Recommendation: single document, multi-window off, but
   don't reintroduce the leaked-singleton pattern.
6. **Effect panel placement**: right sidebar (docs) vs. below the preview.
   Cosmetic; decide in M5 with a mockup (the `design` skill can produce one).
7. **Timecode**: NDF only in v2.0. Anyone editing 29.97 for broadcast will
   want DF. Confirm it's out of scope.
8. **Proxy default**: auto-generate for sources > 1080p? Recommendation: ask
   once per project on first 4K import.
