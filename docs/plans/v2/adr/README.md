# Architecture Decision Records

[Docs home](../../../README.md) › [v2 planning docs](../README.md) › ADRs

One page each. Status is one of Proposed / Accepted / Superseded by ADR-nnn.
All were **Proposed** as of 2026-09-12 pending team review; each file
carries its own current status (ADR-011 to ADR-019 are Accepted).

| ADR | Decision |
|-----|----------|
| [001](001-mlt-gtk4-stack.md) | MLT as the engine, GTK4 + libadwaita as the toolkit, C++23 (records the existing v1 choice) |
| [002](002-consumer-based-playback.md) | Playback via an MLT consumer (`sdl2_audio`), not a hand-rolled pull loop |
| [003](003-model-is-the-source-of-truth.md) | A pure-C++ project model is the source of truth; MLT is a projection |
| [004](004-mlt-xml-project-format.md) | Project files are MLT XML with `ustudio:` properties, written by our serialiser |
| [005](005-rebuild-per-track-sync.md) | Engine sync rebuilds a whole track playlist per change, verified in debug |
| [006](006-compositing-without-qt-or-frei0r.md) | *Superseded by 011.* Track compositing with `composite` + `affine`; frei0r optional; Qt modules never |
| [007](007-mlt-module-load-policy.md) | MLT factory initialised from a curated module directory to keep Qt out of the process |
| [008](008-custom-timeline-widget.md) | Timeline is one custom GtkWidget using snapshot, not a widget per clip |
| [009](009-out-of-process-rendering.md) | Export runs in a child process (`u-studio-render`) from a saved project snapshot |
| [010](010-doctest-vendored.md) | doctest, vendored, as the test framework |
| [011](011-frei0r-required-and-effect-families.md) | frei0r is a required runtime dependency; effects come from frei0r, MLT, libavfilter and audio plugin hosts, health-checked out of process |
| [012](012-titles-mlt-module.md) | Titles render through our own MLT module (Pango + Cairo) and are authored in a separate `u-studio-titles` app |
| [013](013-effects-and-titles-as-drop-in-modules.md) | Effects and titles are drop-in modules behind named integration points, optional at build time; their project data never is |
| [014](014-drop-in-loading-and-distribution.md) | Drop-ins build built-in or as loadable modules; core ships with effects and titles, everything else is an opt-in package |
| [015](015-ai-generation-network-boundary.md) | AI generation runs in a separate networked helper; the editor stays network-free |
| [016](016-concurrency-model.md) | The main thread does UI and commands only; an engine thread, a worker pool and child processes do the rest, on immutable model snapshots |
| [017](017-windows-secondary-target.md) | Windows 10/11 is a secondary launch target; OS-specific code lives behind `src/platform/`, new code stays portable, existing Linux-only sites migrate when touched |
| [018](018-clip-transform-is-core.md) | Clip transform (move, scale, rotate, crop, flip; OBS-like bounds) is core model, engine and shell; the effects drop-in may add keyframing on top |
| [019](019-gpu-acceleration.md) | GPU acceleration: movit compositing on our own EGL context and per-producer hardware decode, behind a setting that probes and falls back to the CPU path |
| [020](020-template-packages-and-sharing.md) | Template packages are local `.zip`/`.tar.gz` archives validated before install; sharing runs in a separate networked helper against a service with signed-URL downloads |
| [021](021-lottie-layers-via-thorvg.md) | Lottie animations are a titles layer drawn by ThorVG in the titles drop-in; our validator refuses expressions and external assets first; the Flatpak builds ThorVG without expressions or file access |
| [022](022-curated-mlt-modules-allowlist.md) | The curated MLT module directory links only listed modules (the editor's, and what drop-ins name); Qt stays denied; stale directories are swept at start |
