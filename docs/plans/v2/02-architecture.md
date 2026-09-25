# 02 — Architecture

## Layer map

```
┌──────────────────────────────────────────────────────────────────────────┐
│  app/   (executable: u-studio-video-editor)                               │
│  GTK4 + libadwaita. Windows, panels, custom widgets, actions, dialogs.     │
│  Depends on: ui-kit, engine, core.                                        │
├──────────────────────────────────────────────────────────────────────────┤
│  engine/  (static lib: libustudio-engine)                                 │
│  The ONLY code that includes <mlt++/Mlt.h>. EngineSync (model→MLT),       │
│  PlaybackController (consumer), MediaProbe, ThumbnailService,             │
│  WaveformService, Renderer, FactoryPolicy.                                │
│  Depends on: core, mlt++, glib (for GMainContext dispatch only).          │
├──────────────────────────────────────────────────────────────────────────┤
│  core/  (static lib: libustudio-core)                                     │
│  Project model, Commands, UndoStack, Document, serialisation, ids,        │
│  time math, Signal. Pure C++23 + libxml2. NO GTK, NO MLT, NO GLib.        │
├──────────────────────────────────────────────────────────────────────────┤
│  render/  (executable: u-studio-render)                                   │
│  Headless CLI: project file → avformat consumer, JSON progress on stdout.  │
│  Depends on: engine, core. NO GTK.                                        │
└──────────────────────────────────────────────────────────────────────────┘
tests/  doctest. core tests run everywhere; engine tests need MLT but no
        display and no media files; app tests are smoke-only (see doc 11).
```

Dependency rule, enforced by meson (each layer is a separate `static_library`
with `include_directories` limited to what it may see):

| Layer | May include | May not include |
|-------|-------------|-----------------|
| core | std, libxml2, own headers | gtk, glib, mlt |
| engine | core, mlt++, glib | gtk, adwaita |
| app | core, engine, gtk4, adwaita, gio | mlt (the build's `app-boundary-check` target fails on any `#include` of an mlt or pulse header in `src/app`) |
| render | core, engine, gio (GSubprocess-free; it's the child) | gtk |
| dropins (`src/dropins`: the drop-in ABI, `DropInHost`, the module loader; ADR-014) | core, engine headers, glib/gmodule | gtk, adwaita |
| `drop-ins/<name>/core` *(docs 15, 16; the build's `dropin-boundary-check` enforces this and the app row)* | same as core, plus `src/core` headers | same as core |
| `drop-ins/<name>/engine` *(planned, doc 15)* | same as engine, plus `src/` core/engine headers | same as engine |
| `drop-ins/<name>/app`, `drop-ins/titles/editor` *(planned, docs 15, 16)* | same as app, plus `src/` headers | same as app |
| titlerender *(planned, doc 16)* | core, pango, pangocairo, cairo, fontconfig | gtk, mlt |
| mltmodule *(planned, doc 16)* | mlt framework C API, titlerender, core | gtk, mlt++ app code |
| titles app *(planned, doc 16)* | core, titlerender, gtk4, adwaita, gio | mlt |

Drop-ins live under a top-level `drop-ins/<name>/`. They reach `src/` only
through the integration points listed in doc 15, "Drop-in structure"
(ADR-013), and nothing in `src/` includes from `drop-ins/`.

## Target source layout

> REVIEW: Claude (2026-09-24): this is the target tree, not today's. M0 landed with a flatter
> layout (a single `app_window.*` plus `action_registry.*`, `ui_hints.*`,
> `autosave.*`, `settings.*`; commands in `primitives.*` and
> `timeline_edits.*`); CLAUDE.md's "Repository layout" describes the current tree.

```
src/
  core/
    ids.h                 typed ids (ClipId, TrackId, AssetId, EffectId), IdAllocator
    time.h                FrameIndex, Rational, Timecode formatting
    signal.h              tiny Signal<Args...> (connect/disconnect/emit), main-thread-only
    log.h/.cpp            moved from src/util/log.* unchanged in M0 (no deps; used by every layer)
    model/
      asset.h             Asset, MediaInfo
      effect.h            Effect, Param, Keyframe
      clip.h              Clip
      track.h             Track
      sequence.h          Sequence, Profile, Marker
      project.h           Project (bin + sequences + settings)
      model.h/.cpp        Model: owns Project, mutators, change events, invariants
    commands/
      command.h           Command interface, Transaction
      clip_commands.cpp   Insert/Remove/Move/Resize/Split/SetClipProperty
      track_commands.cpp  Add/Remove/Reorder/SetFlags
      effect_commands.cpp Add/Remove/Move/SetParam/SetKeyframe
      composite.cpp       RippleDelete, RippleTrim, InsertAt (built from primitives)
    undo_stack.h/.cpp
    document.h/.cpp       Document = Model + UndoStack + dirty flag + path
    xml/
      writer.cpp          Project → MLT XML (our serialiser)
      reader.cpp          MLT XML → Project (ours + kdenlive best-effort)
  engine/
    factory_policy.h/.cpp curated module dir + Factory::init/close (ADR-007)
    dispatcher.h/.cpp     MainThreadDispatcher (GMainContext invoke + lifetime token)
    engine_sync.h/.cpp    Model → Mlt::Tractor reconciler + debug verifier
    engine_extension.h/.cpp  drop-in hooks into the graph build (IP3, doc 15)
    playback.h/.cpp       PlaybackController over Mlt::Consumer
    media_probe.h/.cpp    worker: path → MediaInfo
    thumbnails.h/.cpp     worker: (asset, frame, size) → RGBA
    waveforms.h/.cpp      worker: asset → peak arrays, disk cache
    render_job.h/.cpp     in-process render (used by u-studio-render)
    mlt_util.h            RAII helpers around Mlt::Service lock, property get/set
  app/
    main.cpp
    application.h/.cpp    AdwApplication subclass; actions; open/new/recent
    window/
      main_window.*       AdwApplicationWindow; layout; menus; shortcuts
      preview_pane.*      GtkPicture + overlay controls
      transport_bar.*
      bin_panel.*         asset list/grid, import, drag source
      effects_panel.*     effect stack for selection
      shell_host.h, shell_hosts.*  drop-ins' UI hosts: inspector, actions, hints, overlays, import (IP5, doc 15)
      export_dialog.*
    timeline/
      timeline_view.*     custom GtkWidget (snapshot vfunc), viewport, hit-test
      timeline_controller.* interaction state machine (drag/trim/select/snap)
      track_headers.*
      ruler.*
      render_cache.*      thumbnail/waveform GdkTexture caches
    style/
      style.css           moved from style_css.h; in GResource
      tokens.h            the few colours cairo/snapshot code needs
  render/
    main.cpp              CLI
    render_cli.h/.cpp     argument dispatch, drop-ins' subcommands (IP6, doc 15)
data/
  com.ustudio.VideoEditor.desktop
  com.ustudio.VideoEditor.metainfo.xml
  icons/
  ustudio.gresource.xml
  render-presets/         our curated subset over MLT's presets
tests/
  core/  engine/  app/  fixtures/
subprojects/doctest/     vendored single header (wrap-file)
```

## Threading model

As of 0.28.0 ([ADR-016](adr/016-concurrency-model.md), [doc 19](19-concurrency.md) MT2),
`EngineSync` and `PlaybackController` live on a dedicated engine thread
behind `engine::Engine`. The main thread publishes immutable model
snapshots and never touches an MLT object.

Five kinds of threads, each with a fixed set of things it may touch.

| Thread | Owns | May touch | May not touch |
|--------|------|-----------|---------------|
| **GTK main** | Model, UndoStack, all widgets, `engine::Engine` (the facade and its state mirror) | the engine's command queue; `Model::snapshot()` | any MLT object; must never block > 16 ms (the stall monitor watches) |
| **Engine thread** (`engine::Engine`, one `std::jthread`) | EngineSync, the master producers, the tractor, PlaybackController | its own MLT graph; the dispatcher | the live Model (it gets snapshots), any widget |
| **MLT consumer threads** (created by `Mlt::Consumer::start`) | frame pulling, decoding, audio device | reads the tractor graph (MLT locks internally) | anything of ours except the `consumer-frame-show` handler, which only posts to the dispatcher |
| **Worker pool** (`std::jthread`s behind `engine::Workers`) | their own `Mlt::Producer`s, file IO, caches | the dispatcher | the shared tractor, the model, any widget |
| **Render child process** | everything in its own process | the project file, stdout | the editor's memory |

### MainThreadDispatcher

Two responsibilities:

1. **Post a closure to the main context**, via
   `g_idle_add_full(G_PRIORITY_DEFAULT, …)` — **not**
   `g_main_context_invoke_full()`, which this doc originally named. That
   function has a documented optimisation where, if nothing currently owns
   the target context, the *calling* thread acquires it and runs the
   function immediately, inline. For a poster on an MLT consumer thread
   that defeats the entire point (the closure must never run on that
   thread). Confirmed empirically (M2 1/N): a test that posted from a
   consumer thread with no GTK main loop running anywhere in the process
   crashed with heap corruption from unsynchronized concurrent access,
   100/100 runs; switching to `g_idle_add_full` (which always creates a
   genuine idle source and never runs inline on the poster's own thread)
   fixed it outright. `g_idle_add_full` still always targets the process's
   single default `GMainContext`, same as the plain `g_idle_add()` v1 used —
   the actual change from v1 is response (2) below, not the post mechanism.
2. **Guard lifetime.** Every subscriber holds a `LifetimeToken`
   (a `std::shared_ptr<void>`); posts capture a `std::weak_ptr` and are dropped
   silently if the token is dead by the time they run. This closes P4 from the
   review.

Plus one specialised channel, `LatestFrameSlot`: a single-slot mailbox for
video frames. The consumer thread `store()`s; the main thread `take()`s in
one queued idle. If the main loop falls behind, frames are overwritten, never
queued. Frame memory is handed to `GdkMemoryTexture` via
`g_bytes_new_with_free_func`, one copy total (out of MLT's frame buffer).

### Locking discipline for MLT

- All edits to the tractor go through `EngineSync`, which takes
  `Mlt::Service::lock()` on the tractor for the duration of one *model
  change event*, then releases.

> REVIEW: Claude (2026-09-24): not what shipped. `EngineSync::rebuildAll()` builds a new
> tractor with no lock (on the engine thread since 0.28.0), and
> `PlaybackController::setTractor()` stops the consumer, swaps the tractor in
> and restarts it, on the same thread (CLAUDE.md, "Threading rules").
- `PlaybackController` never touches the tractor structure; it only seeks and
  reads position.
- Workers never touch the shared tractor. They open their own producer for
  the asset (cheap: MLT caches avformat contexts per process is *not*
  guaranteed, so workers are rate-limited to N=2 concurrent decodes).

## Ownership and lifetime

- `Application` owns `std::vector<std::unique_ptr<DocumentSession>>`.
- `DocumentSession` owns one `Document` (core), one `EngineSync`, one
  `PlaybackController`, one `MainWindow`. Destroying the session destroys them
  in the order: window → playback (stop consumer, join) → engine sync →
  document. The consumer is stopped *before* the tractor is destroyed, always.
- `FactoryPolicy` is a process-wide singleton, initialised in `main()` before
  any window, closed after `g_application_run` returns.
- GTK objects: floating refs are sunk on insertion; every `g_object_ref` has a
  matching RAII holder (`GObjectPtr<T>`, see doc 14). No raw `g_object_unref`
  in app code outside the wrapper.

## Event flow for one edit

```
user drags clip
  → TimelineController computes MoveClip{clipId, newTrack, newPos} (preview, no command yet)
  → on release: Document::execute(std::make_unique<MoveClip>(…))
      → UndoStack::push → Command::apply(Model&)
          → Model::moveClip(...) validates, mutates, emits ClipMoved{clipId, oldTrack, newTrack}
              → EngineSync::onClipMoved: lock tractor; rebuild affected track playlists; unlock;
                                          [debug] verifyTrack(old), verifyTrack(new)
              → TimelineView::onModelChanged: invalidate layout, queue_draw
      → Document marks dirty; autosave timer armed
```

Drag preview during the gesture is rendered from a *transient overlay* in the
timeline view, not by mutating the model on every motion event. This keeps
undo granularity at one command per gesture and avoids hammering the engine.
