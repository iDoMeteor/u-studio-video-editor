# Architecture overview

[Docs home](../README.md) › [Developer docs](README.md) › Architecture

A short tour of the code. For the full design, read
[v2 doc 02: Architecture](../plans/v2/02-architecture.md). The
[ADRs](../plans/v2/adr/README.md) are the binding contract.

## Layers

```text
core  ──▶  engine  ──▶  app
  │           │
  └──────▶ render (u-studio-render CLI)
```

| Layer | What it is | May include | May not include |
|---|---|---|---|
| `src/core/` | Project model, commands, undo stack, XML files, logging, thread pool. Pure C++23. | std, libxml2 | GTK, GLib, MLT |
| `src/engine/` | The **only** code that touches MLT: graph building, playback, caches, render, proxies | core, `mlt++`, GLib (dispatch only) | GTK, libadwaita |
| `src/app/` | The GTK4/libadwaita window, built in code (no `.ui` files) | core, engine headers, GTK, libadwaita, GIO | any `<mlt…>` header (checked by the build) |
| `src/render/` | `u-studio-render`: proxies and drop-in subcommands today; the full headless renderer is milestone M6 | core, engine | GTK |
| `src/platform/` | Every OS-specific call, one file per OS ([ADR-017](../plans/v2/adr/017-windows-secondary-target.md)) | std, OS headers | GTK, GLib, MLT |
| `src/dropins/` | The drop-in registry and host API ([ADR-013](../plans/v2/adr/013-effects-and-titles-as-drop-in-modules.md), [ADR-014](../plans/v2/adr/014-drop-in-loading-and-distribution.md)) | core | — |
| `drop-ins/titles/` | The titles drop-in ([doc 16](../plans/v2/16-titles-tool.md), [ADR-012](../plans/v2/adr/012-titles-mlt-module.md)): `core/` (the `.ustitle` model, elastic timing, fields), `render/` (Pango/Cairo renderer), `mltmodule/` (the `ustudio_title` MLT producer), `engine/` and `editor/` (its integration points); see its [README](../../drop-ins/titles/README.md) | `core/`: std, libxml2, `src/core`; `render/`: Pango, Cairo, fontconfig; `mltmodule/`: MLT's C API; `engine/`: `src/engine`; `editor/`: `src/app/shell_host.h`, GIO | `core/`: GTK, GLib, MLT (checked by the build); `render/`, `mltmodule/`: GTK |

Three rules hold everything together:

1. **The model is the source of truth**
   ([ADR-003](../plans/v2/adr/003-model-is-the-source-of-truth.md)). Model
   → engine → screen, never backwards. MLT is a projection of
   `core::Model` and the UI never reads state from MLT objects.
2. **Every user-visible edit is a `core::Command`** run through the
   `UndoStack` ([doc 04](../plans/v2/04-commands-and-undo.md)). `apply()`
   validates first and leaves the model untouched when it fails.
3. **The main thread never touches MLT**
   ([ADR-016](../plans/v2/adr/016-concurrency-model.md),
   [doc 19](../plans/v2/19-concurrency.md)). The window publishes
   immutable model snapshots to `engine::Engine`, which owns one engine
   thread for graph builds and playback. Probing, loading, saving,
   waveforms and thumbnails run as pool jobs, each with its own throwaway
   MLT producer.

## Key modules

| Module | Role | Read more |
|---|---|---|
| `core/model/` | `Project`/`Sequence`/`Track`/`Clip`/`Asset` value types, validating mutators, `Model::changed`, `check()`; transforms, retiming, track segments | [doc 03](../plans/v2/03-project-model.md) |
| `core/commands/` | One command per edit, `CompositeCommand`, `Transaction`, `UndoStack`; timeline edits (ripple, slip, move many) in `timeline_edits.h` | [doc 04](../plans/v2/04-commands-and-undo.md) |
| `core/xml/` | `.ustudio` reader and writer, backups, release notes | [ADR-004](../plans/v2/adr/004-mlt-xml-project-format.md), [project file notes](notes/project-files.md) |
| `core/audio/align` | Audio cross-correlation for Sync Tracks (Audio) | — |
| `core/concurrency/` | The worker `ThreadPool` | [doc 19](../plans/v2/19-concurrency.md) |
| `core/log.h` | Thread-safe logger and `ScopedTimer` | [Building › Logging](building.md#logging) |
| `engine/factory_policy` | Curated MLT module directory so Qt never loads; `Factory::init`/`close`; malloc and decoder-cap tuning | [ADR-007](../plans/v2/adr/007-mlt-module-load-policy.md) |
| `engine/engine` | The engine thread and its main-thread façade | [doc 19](../plans/v2/19-concurrency.md), [playback notes](notes/playback-engine.md) |
| `engine/engine_sync` | Builds the `Mlt::Tractor` from a model snapshot, one track playlist at a time; `verify()`; media probing; render | [ADR-005](../plans/v2/adr/005-rebuild-per-track-sync.md), [engine sync notes](notes/engine-sync.md) |
| `engine/playback_controller` | Playback through an MLT consumer (`sdl2_audio` → `rtaudio` → `null`) | [ADR-002](../plans/v2/adr/002-consumer-based-playback.md), [doc 05](../plans/v2/05-playback-engine.md) |
| `engine/dispatcher` | `MainThreadDispatcher`: posts closures onto the GLib main thread | [doc 19](../plans/v2/19-concurrency.md) |
| `engine/waveform_cache`, `engine/thumbnail_cache` | Background peak and thumbnail extraction | [media cache notes](notes/media-caches.md) |
| `engine/proxy`, `engine/audio_sync` | Proxy rendering, audio sync matching | [doc 07](../plans/v2/07-media-bin-and-assets.md) |
| `app/app_window` | Header bar, preview, timeline, transport, media browser, dialogs | [app shell notes](notes/app-shell.md) |
| `app/timeline/` | `Viewport` (zoom/scroll maths), `TimelineController` (gestures, tested without GTK), `UsTimelineView` and its GSK renderer | [doc 06](../plans/v2/06-timeline-ui.md), [ADR-008](../plans/v2/adr/008-custom-timeline-widget.md) |
| `app/action_registry` | Every window action and its default shortcut, in one table | [Keyboard shortcuts](../user/keyboard-shortcuts.md) |
| `app/ui_hints` | Tooltip and Help text for every control; drop-ins add their own through `ShellHost` | — |
| `app/*_queue` | Import, save, render and proxy queues off the main thread | [doc 19](../plans/v2/19-concurrency.md) |
| `app/autosave` | Autosave and crash recovery | [doc 09](../plans/v2/09-persistence-and-formats.md) |
| `app/settings` | GSettings wrapper with a missing-schema fallback | [settings notes](notes/settings.md) |
| `app/style/style.css` | Unicorn Tears design tokens on libadwaita named colours; `tools/gen_tokens.py` turns them into `tokens.h` for the drawn timeline | [CLAUDE.md](../../CLAUDE.md), "Design system" |

## Where things live on disk

| What | Path (native) |
|---|---|
| Logs | `$XDG_STATE_HOME/ustudio/logs/` |
| Unfinished renders | `$XDG_STATE_HOME/ustudio/pending-renders/` |
| Render profiles | `$XDG_CONFIG_HOME/ustudio/render-profiles.ini` |
| Settings | GSettings schema `com.ustudio.VideoEditor` (`data/com.ustudio.VideoEditor.gschema.xml`) |
| Save backups | `.ustudio-backups/` beside the project file, newest 5 kept |
| Proxies | The user cache directory |

Under Flatpak, all of these sit inside `~/.var/app/com.ustudio.VideoEditor/`.

## Why it's built this way

- [v2 doc 00: v1 review](../plans/v2/00-v1-review.md) explains what the
  first single-track version got wrong.
- [v2 doc 01: Goals, scope, principles](../plans/v2/01-goals-scope-principles.md)
  is the one-page definition of v2.
- This project is **not** a port of kdenlive. It uses the same MLT engine
  behind a new, much smaller GTK4 UI.
