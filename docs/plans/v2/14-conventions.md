# 14 — Conventions

Short, because a `.clang-format` and the compiler enforce most of it.

## C++

- C++23, no exceptions across GTK callback boundaries (catch at the
  trampoline and log), no RTTI-dependent design. Exceptions are fine inside
  `core` for programming errors; expected failures return
  `std::expected<T, Error>`.
- Namespaces: `ustudio::core`, `ustudio::engine`, `ustudio::app`,
  `ustudio::render`. No `using namespace` in headers.
- Naming: `PascalCase` types, `camelCase` functions/variables, `m_` members,
  `k` constants, `UPPER` macros (rare), files `snake_case.{h,cpp}`.
- Ownership: `std::unique_ptr` by default; `std::shared_ptr` only for
  producers shared between cuts and for lifetime tokens. Raw pointers are
  non-owning and never stored across an event loop iteration, except
  GObject pointers held via `GObjectPtr`.
- Every mutation of the model is a mutator on `Model` called from a
  `Command`. Adding a mutator without a command is a review blocker.
- Signals: `core::Signal` is main-thread-only; connecting returns a
  `Connection` RAII handle that disconnects on destruction. Store it in the
  subscriber.
- Assertions: `USTUDIO_ASSERT(cond, "message")` → `g_error`-like abort in
  debug, log + return in release. Invariant checks use it.
- Logging: keep the team's `Log::{error,warn,info,debug}` API from
  `src/util/log.h` as the front door (it is thread-safe and dependency-free;
  it moves to `core/log.h` in M0). Two additions: a category prefix
  convention (`"[engine] ..."`, `"[sync] ..."`, `"[timeline] ..."`) so logs
  are greppable per layer, and the log file location moves from `./logs/`
  to `$XDG_STATE_HOME/ustudio/logs/`. Routing through `g_log_structured` is
  optional later; not a v2 requirement.

## Portability (ADR-017)

Windows 10/11 is a secondary launch target. The port is scheduled later, but
code written from now on must not make it harder.

- **OS-specific calls live in `src/platform/`**, behind std-only interfaces,
  one implementation file per OS (`*_linux.cpp`, later `*_windows.cpp`).
  That covers `/proc`, `unistd.h`, `fcntl.h`, `sys/*`, `dlfcn.h`, signals,
  symlinks, `dup2`/fd tricks, thread naming and allocator tuning.
  `src/platform/` includes std and OS headers only: no GTK, GLib or MLT.
- **Already portable, use directly:** `std::filesystem`, `std::thread`,
  GLib's `g_get_user_*_dir()`, `GSubprocess`, GModule, GIO's
  default-app launching, GSettings.
- **Paths:** `std::filesystem::path` throughout. No hard-coded `/tmp`, `~`
  or `/`-joined absolute paths. Tests use the scratch helper or
  `temp_directory_path()`.
- **Names:** shared-library and executable suffixes come from `platform::`
  (`.so`/`.dll`, `""`/`.exe`).
- **Child processes:** `GSubprocess`, cancelled through a `platform::`
  helper.
- **Linux-only features** (document portal, snap scrub, `mallopt`) are fine
  behind `src/platform/` or an `#ifdef` with a working no-op elsewhere.
- **Text and file names:** write UTF-8 with `\n`. Never assume ASCII file
  names.
- **"When passing by":** when a change touches a file on the list below,
  move that file's OS-specific code into `src/platform/` as a separate
  small commit, and strike it from the list. No big-bang refactor. The
  first such commit created `src/platform/` and its meson boundary check
  (`tools/platform_boundary_check.sh`, whose allowlist is this list).

### Migration list (survey 2026-09-25)

| File | Linux-only code | Likely `platform::` helper |
|---|---|---|
| `src/core/xml/writer.cpp` | `fcntl.h`, `unistd.h` (fsync for atomic save) | `platform::syncFile()` |
| ~~`src/app/autosave.cpp`~~ | ~~`/proc/<pid>/stat` start time for process liveness~~ | done: `platform::processExists()`, `platform::processStartTime()` |
| `src/app/main.cpp` | `g_unix_signal_add` for SIGTERM and SIGINT | `platform::installQuitHandlers()` (console and session-end handlers on Windows) |
| `src/app/settings.cpp`, `src/app/snap_env.cpp` | `/proc/self/exe` | `platform::executablePath()` |
| `src/app/portal_path.cpp` | document-portal xattrs | stays Linux-only; a no-op elsewhere |
| ~~`src/app/app_window.cpp`~~ | ~~`unistd.h`~~ | done: `platform::currentProcessId()` (autosave's owner pid) |
| `src/engine/factory_policy.cpp` | a symlink farm for the curated module directory, `.so` filtering, `mallopt` | `platform::linkOrCopy()`, `platform::sharedLibrarySuffix()`, `platform::tuneAllocator()` |
| `src/engine/engine_sync.cpp` | `dup2` to silence MLT's stdout encoder list | `platform::ScopedStdoutSilence` |
| `src/engine/engine.cpp` | `pthread_setname_np` | `platform::setThreadName()` |
| `src/dropins/registry.cpp` | `libustudio-dropin-*.so` names | `platform::sharedLibrarySuffix()` |

## GTK / GObject

- `GObjectPtr<T>`: RAII holder (`ref_sink` on construction from floating,
  `unref` on destruction, `get()`, `release()`). Use it for anything we
  keep; don't for widgets owned by their parent.
- Custom widgets are real GObject subclasses (`G_DEFINE_TYPE`) with a
  private C++ struct in `priv`, constructed/destroyed in `init`/`dispose`.
  The C++ logic (`TimelineController`, `Viewport`) lives in plain classes the
  widget owns, so it's testable without GTK.
- Callbacks: static trampolines forward to member functions (as v1 does);
  the trampoline is the only place that casts `gpointer`. Group them at the
  bottom of the file with a banner, as `app_window.cpp`'s trampolines section does.
- Actions: every user command is a `GAction` (`app.` or `win.` prefix) with
  an accelerator registered in one table (`app/action_registry.cpp`), so menus,
  shortcuts, and the shortcuts window agree.
- Tooltips and Help text: every control's tooltip comes from
  `app/ui_hints.cpp` via `setTooltip(widget, "<area>.<control>")`; the hint
  names its action and the shortcut is looked up, never typed into the text.
  The Help dialog's Controls tab lists the same table. Tooltips that show
  data (a file path, a clip's timecodes) are set directly.
- GTK/GLib/MLT C-cast warning noise is silenced via `include_type: 'system'`
  on those `dependency()` calls (root `meson.build`), not a pragma wrapper
  around the includes — see doc 11 for why the pragma approach doesn't
  actually cover call-site macro use.
- CSS classes, not per-widget style overrides. Named colours from
  `style.css`; cairo/snapshot colours from generated `tokens.h`.
- Threads never touch widgets. If you're holding a `GtkWidget*` on a worker
  thread, that's the bug.

## MLT

- Only `engine/` includes MLT headers. `engine/mlt_util.h` provides
  `ScopedServiceLock`, `getInt/getDouble/getString` helpers with defaults,
  and `setAnimation`.
- Property names are string constants in one header (`engine/mlt_props.h`),
  not scattered literals.
- Every `Mlt::Consumer` is stopped before the services it reads are
  destroyed. Every `Mlt::Frame` obtained in a callback is consumed inside the
  callback.
- Don't trust return codes that MLT documents as unreliable (`split_at`,
  `insert_at`). Verify by inspecting state; the verifier is the backstop.

## Git

- Small PRs against `main`; one milestone = several PRs. No PR moves files
  *and* changes logic.
- Commit messages: imperative subject ≤ 72 chars, body explains why. End
  with the attribution lines the tooling adds.
- CI must be green; `werror` is on in CI.

## Docs

- Every ADR is ≤ 1 page: Context, Decision, Consequences, Status.
- When a doc in this set becomes wrong, fix it in the same PR as the code.
