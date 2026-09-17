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
  bottom of the file with a banner, as `app_window.cpp:277` does.
- Actions: every user command is a `GAction` (`app.` or `win.` prefix) with
  an accelerator registered in one table (`app/actions.cpp`), so menus,
  shortcuts, and the shortcuts window agree.
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
