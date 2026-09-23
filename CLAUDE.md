---
applyTo: "**"
---

# u Studio Video Editor — Agent & coding standards

This file governs all AI-assisted development on `u-studio-video-editor`.
Read it in full before writing or reviewing any code.

**`docs/plans/v2/` is the architecture record.** Its ADRs
(`docs/plans/v2/adr/`) are the contract: a change that contradicts an ADR
either supersedes it with a new ADR or does not land. The numbered docs are
guidance and may drift; the milestone acceptance criteria in
`docs/plans/v2/12-roadmap-and-milestones.md` define "done". Decisions already
taken on the open questions are recorded in
`docs/plans/v2/13-risks-and-open-questions.md` — do not re-litigate them.

---

## Project identity

**u-studio-video-editor** is a from-scratch, GNOME-native, multi-track video
editor for Linux: GTK4 + libadwaita for the UI, MLT (via `mlt++`) for
playback and rendering, C++ built with meson/ninja. Licence
GPL-3.0-or-later.

It is **not** a port of kdenlive. `~/Repos/kdenlive` is a current checkout
kept as a *reference for MLT usage patterns* (consumer setup, track/transition
wiring, playlist quirks). Learn from it; never copy code from it. The point of
this project is a different, smaller architecture.

**No Qt, no KDE Frameworks, anywhere in the process.** This is a runtime
property, not a link-line property: `Mlt::Factory::init()` with no directory
argument dlopens MLT's Qt6 modules (measured: 32 Qt libraries mapped).
`ldd | grep -i qt` passing proves nothing. The v2 fix is a curated module
directory (ADR-007); until it lands, do not add anything that depends on a
`qt6` MLT service (`qtblend`, `qtext`, `qimage`, `glaxnimate`), and never
add Qt to `meson.build`.

Platform: Fedora (owner's machine, Wayland, PipeWire) first; any modern
GNOME desktop second; Flatpak is the reference shipped artifact from M7.
Windows/macOS are not targets.

The owner's use case: 1080p/4K livestream and promo editing for the Unicorn
Tears brand. The design system at
`~/projects/unicorn-tears/claude-design-system` is the visual source of
truth (see "Design system" below).

---

## Repository layout

Today (v1, single executable):

```
meson.build               Project definition, dependencies (GTK4, libadwaita, MLT, GLib)
src/main.cpp              AdwApplication entry point; loads CSS; creates the window
src/engine/               The ONLY code that includes <mlt++/Mlt.h>
  mlt_engine.{h,cpp}      Profile, per-track playlists in a Tractor, pull-loop playback,
                          edit primitives (move/trim/split/lift/close-gap), save/load, render
  waveform_cache.{h,cpp}  Background audio-peak extraction with its own throwaway producers
src/ui/                   GTK4/libadwaita shell, built imperatively (no .ui files)
  app_window.{h,cpp}      Header bar, preview, multi-row timeline, transport, context menus
  style_css.h             Unicorn Tears tokens mapped onto libadwaita named colours
src/util/log.{h,cpp}      Thread-safe logger → stderr + logs/<app>-<timestamp>.log
docs/plans/v2/            v2 architecture, model, roadmap, ADRs (see top of this file)
builddir/                 meson build output — gitignored, per-worktree
logs/                     runtime logs — gitignored
```

Planned (v2, from milestone M0 — see `docs/plans/v2/02-architecture.md`):
`src/core/` (pure C++ model + commands, no GTK/MLT), `src/engine/` (MLT
only), `src/app/` (GTK only), `src/render/` (headless CLI), `tests/`,
`data/`. Until M0 lands, keep working in the current layout; M0 is one
announced, mechanical PR that moves files without changing logic.

---

## Language & build standards

- **C++23** is the decided standard (doc 13, Q1); `meson.build` still says
  `cpp_std=c++17` and is bumped in M0. Until then, write code that compiles
  under both (no `std::expected`, no `std::print` yet).
- Toolchain: GCC 16 on the dev machine; keep it buildable with GCC ≥ 13.
  `ccache` is on the PATH — rebuilds are cheap, don't skip them.
- `warning_level=2` today, `3` plus `-Wshadow -Wconversion -Wold-style-cast
  -Wnon-virtual-dtor` and `werror` in CI from M0. New code must be
  warning-clean at the current level.
- Build, run, verify:

  ```sh
  meson setup builddir                       # once per worktree
  meson compile -C builddir
  USTUDIO_LOG_LEVEL=debug ./builddir/src/u-studio-video-editor
  ```

  When `tests/` exists (M0): `meson test -C builddir --print-errorlogs`
  before every commit.
- Dependencies are: GTK4 ≥ 4.10, libadwaita, GLib/GIO/GObject, MLT 7
  (`mlt-framework-7`, `mlt++-7`), and today `libpulse-simple` (removed in
  M2). v2 adds `libxml2` (already an MLT transitive dep) and a vendored
  doctest header. **Anything else needs an ADR** and the owner's sign-off.

---

## Code style

Match the existing files; a `.clang-format` arrives in M0 and becomes the
authority.

- 4-space indent, braces on their own line for functions/classes, same line
  for control flow (as in `app_window.cpp`). 120-column soft limit.
- `PascalCase` types, `camelCase` functions and variables, `m_` members,
  `k` constants (`kAudioRate`), `snake_case.{h,cpp}` files, `#pragma once`.
- Namespaces: anonymous namespace for file-local helpers; `ustudio::core/
  engine/app/render` from M0. No `using namespace` in headers.
- Ownership: `std::unique_ptr` by default; `std::shared_ptr` only for
  producers shared between cuts and for lifetime tokens; raw pointers are
  non-owning and never outlive the current main-loop iteration (GObject
  pointers held via an RAII wrapper from M0).
- No exceptions across GTK callback boundaries. Trampolines (`static`
  callbacks forwarding to a member function) are the only place that casts
  `gpointer`; keep them grouped at the bottom of the file under the banner
  comment, as now.
- Logging goes through `Log::{debug,info,warn,error}` (`src/util/log.h`).
  No `printf`/`std::cout`/`g_print` for diagnostics. Prefix messages with a
  bracketed subsystem (`"[engine] …"`, `"[timeline] …"`) so logs grep by
  layer.
- Comments explain *why* and record empirical MLT findings (see next
  section). Don't restate what the code says.

---

## Architecture boundaries (enforced by review now, by meson from M0)

| Layer | May include | May not include |
|---|---|---|
| `src/engine/` | `mlt++`, GLib (dispatch only) | GTK, libadwaita |
| `src/ui/` (→ `src/app/`) | GTK, libadwaita, GIO, engine headers | any `<mlt…>` header, `<pulse/…>` |
| `src/util/` (→ `src/core/`) | std only | GTK, GLib, MLT |

- **Model → engine → screen, never backwards.** In v1 the timeline still
  reads clip state from `MltEngine::clips()`; that is the known debt v2's M1
  removes (ADR-003). Do not add *new* code paths that derive UI state from
  MLT objects — route new state through plain C++ structs the UI owns.
- **Every user-visible edit will be a Command** (doc 04). Until M1, keep
  edit primitives in `MltEngine` small, single-purpose, and validated before
  mutation (as `moveClip`/`trimClipStart` do: check the whole destination
  span, then touch the playlist).

### Threading rules

- GTK widgets are touched from the main thread only. Cross from a worker via
  `g_idle_add`/`g_main_context_invoke` (v1) or the `MainThreadDispatcher`
  (M2). If you are holding a `GtkWidget*` on a worker thread, that is the bug.
- `MltEngine::m_mltMutex` guards the live tractor. Hold it for one operation,
  never across a blocking call you don't control, never from the main thread
  for anything that can wait on a decode. Things that must not contend with
  it (waveforms, render) open their own throwaway `Mlt::Profile`/`Producer`
  — keep that pattern.
- MLT producers are not shared across threads. A worker owns its own
  producer; the live tractor belongs to the engine thread + main thread
  under the mutex.
- Never destroy an MLT service a running consumer/pull loop can still reach.
  Stop first, then tear down; `Mlt::Factory::close()` is last, once.

### MLT empirical-knowledge rule

MLT's documentation is thin and several return values are unreliable. The
team's practice is the rule:

- **Don't trust a return code MLT documents as unreliable** (`split_at`,
  `resize_clip`, `insert_at`). Verify by inspecting state afterwards.
- **Don't guess service or property names.** Check the module's YAML
  metadata (`Mlt::Repository::metadata()`, or the `.yml` files under
  `/usr/share/mlt-7/`) — the `avformat` consumer takes `vcodec`/`ab`/`vb`,
  not ffmpeg CLI flags. Say so in a comment when a name was verified that way.
- **Reproduce before relying.** Anything surprising (cut producers report
  `resource="<producer>"`, `get_frame()` auto-advances, `mix` needs
  `start=1 sum=1`, XML round-trip yields a non-tractor) was confirmed with a
  standalone repro before being built on. Keep doing that, and write the
  finding into the README's implementation-notes sections or the engine
  comment where it applies, so the next agent doesn't rediscover it.
- Only `sdl2_audio`, `rtaudio`, `null`, `avformat`, `xml`, and the core
  transitions (`composite`, `affine`, `luma`, `mix`) are assumed present
  (verified on this machine, doc 00). `frei0r` is **required** as of
  ADR-011 (2026-09-23), but `frei0r-plugins` is not yet installed on every
  machine: code must still detect it at runtime and degrade (hide frei0r
  effects, fall back to `composite`) rather than crash, until packaging
  makes it structural. Effects and titles plans: `docs/plans/v2/15-*`,
  `16-*`.

---

## Design system

`src/ui/style_css.h` maps the Unicorn Tears tokens (ink scale, magenta /
cyan / violet, semantic colours) onto libadwaita named colours so the whole
shell reskins from one place.

- Add CSS classes, not per-widget style overrides. New colours go in as
  tokens next to the existing `@define-color` lines.
- Glow is a *selection/focus* treatment only; this app is looked at for
  hours. No glowing static chrome, no animated decoration in the editor.
- Cairo/snapshot-drawn items (clips, playhead) currently duplicate hex
  values in `app_window.cpp`; if you add one, keep it adjacent to the
  existing `kClip*` constants with a comment naming the token. M3 generates
  both from one source.
- Fonts (Space Grotesk, JetBrains Mono, Anton) are not bundled and may not be
  installed; every rule must keep its generic fallback.
- Icons: symbolic GNOME icon names (`media-playback-start-symbolic`), never
  raster art in the chrome.

---

## Testing discipline

There is no test suite yet; M0 adds doctest (`tests/core`, `tests/engine`).
The rules apply from the moment it exists, and the verification habit
applies now:

- Before claiming a change works, **build it and exercise it**: run the app
  for UI changes, or a standalone repro for engine changes (the render and
  playlist primitives were each validated that way before wiring in). Report
  what you ran and what you saw.
- Behaviour changes ship with a test (or, pre-M0, with a written repro in
  the commit body) in the same change when practical.
- If any test fails, report the exact test names and output and stop
  claiming success until it is resolved or the owner explicitly defers it.
- **Never implement production code solely to make a pre-existing failing
  test pass.** If you find red tests you didn't write and don't fully
  understand, stop and ask — they may encode a planned API.
- **No binary media in the repo.** Tests generate their inputs with MLT
  generators (`color:`, `noise:`, `tone:`). The owner's real footage is never
  committed, never moved, never overwritten. Renders during verification go
  to a path you name under `/tmp` or the scratchpad, not next to source
  media.
- Sanitiser builds (`-Db_sanitize=address,undefined`) are the standard for
  anything touching threads or MLT lifetime.

---

## Isolated agent worktrees (one checkout per agent)

Several agents and the owner work on this repo at the same time; files under
`src/` changed underneath a review session within minutes on 2026-09-12. A
shared working tree means one agent's build, hook, or stash sees another's
half-written files. So each agent session works in its own git worktree:

- **Use Claude Code's worktree isolation** (`EnterWorktree`, or
  `isolation: "worktree"` when spawning subagents). It creates a worktree
  under `.claude/worktrees/<name>` (gitignored) on its own branch. Manual
  equivalent: `git worktree add ../u-studio-video-editor.worktrees/<name>
  -b agent/<name>`.
- **The main checkout (`~/Repos/u-studio-video-editor`) stays on `main` and
  only ever `git pull --ff-only`s.** It is where the owner runs the app and
  where `builddir/` for the live binary lives. An agent must not edit,
  stage, stash, or commit files there.
- Each worktree needs its own `meson setup builddir` (gitignored; ccache
  makes the first compile cheap). Don't point a worktree at another tree's
  `builddir`.
- Branch names: `agent/<name>` for agent seats, `feature/<name>`,
  `fix/<name>`, `docs/<name>` for owner-directed work.
- **Landing:** commit on your branch, build and test there, then
  `git push origin HEAD:main` (a fast-forward). If rejected as
  non-fast-forward: `git fetch origin && git merge origin/main` (a merge
  commit is fine), rebuild, retest, push again. Never rebase, cherry-pick,
  or force. A pull request is the alternative when the owner asks for review.
- Never `git worktree remove`/`prune` another agent's tree and never pop,
  apply, or drop a stash you did not create. Report a stray worktree or stash
  to the owner instead.
- A session still running in the main checkout is exposed to the
  concurrent-edit hazard. Say so when it bites rather than retrying blind.

---

## Sub-agent usage

Sub-agents (the `Agent`/`Task` tool, parallel `Explore` agents, `Workflow`
orchestration) are for long-running or background work only: a multi-file
code review, an independent research question big enough to blow up the
main context window, a build/test run the session doesn't need to block on.

- Don't spawn a sub-agent for anything doable directly in a handful of tool
  calls — one `grep`, reading a known file, a small targeted edit. Do that
  work yourself; a sub-agent adds latency and token cost without buying
  parallelism there.
- Default to doing the work in the current session. Reach for a sub-agent
  only when the task is genuinely long-running/background, or when
  parallel, independent lookups would otherwise serialize in one context.
- When a task matches this bar, prefer running it in the background rather
  than blocking the current turn on it, and report back once it completes.

---

## Git conventions

- Commit messages: imperative subject ≤ 72 chars, blank line, body bullets
  explaining *why* and what was verified (match the existing history, e.g.
  `Add clip lift/close-gap editing, waveforms, and format-matched render`).
  End with the attribution trailer the tooling provides.
- Commit after each substantial, built-and-verified change; no monolithic
  commits, and no PR that both moves files and changes logic.
- Never commit `builddir/`, `logs/`, `.claude/`, media files, or renders.
  `.gitignore` already covers the first three; check before adding new
  output directories.
- Versioning: `version:` in `meson.build` is the single source of truth,
  SemVer 2.0.0, pre-1.0 (never bump MAJOR; user-facing feature bumps MINOR,
  fix bumps PATCH). Bump it in the same commit as the user-facing change
  and add a one-line, newest-first entry to `CHANGELOG.md` (create it on
  the first bump). Internal refactors, tests, docs: no bump.

### Git history safety (hard stop)

- Never intentionally detach `HEAD`, and never work in a detached state.
- Never run `git rebase`, `git cherry-pick`, or any force-push variant
  (`--force`, `--force-with-lease`, or equivalent).
- Never rewrite branch history; never check out a different branch in the
  shared main checkout.
- `--no-verify` is never an agent's call; it needs the owner's explicit word
  for that specific commit, and an approval given once does not carry over.
- If an operation would require any of the above, stop and report: the
  exact blocker, the affected branch, and the safe alternatives, then wait
  for explicit instruction.

### Git hook discipline

No hooks are configured yet (M0 adds `clang-format` and the tests to CI; a
pre-commit hook may follow). When hooks exist: warnings and errors are
immediate action items — fix, rerun, then push. Run hooks only inside your
own worktree.

---

## Documentation SOP

- `README.md` is the entry point and must stay honest: when you land a
  capability or remove a limitation listed under "Current capabilities" /
  "Not yet", update it in the same change. Its implementation-notes sections
  are where empirical MLT findings live until v2's `engine/` comments and
  tests absorb them.
- `docs/plans/v2/` is the design record. When code makes a v2 doc wrong,
  fix the doc in the same PR. Disagree in place with a `> REVIEW:`
  blockquote (name + one sentence) rather than forking a doc. Superseding an
  ADR means a new ADR plus marking the old one "Superseded by".
- Root-level markdown is reserved for entry docs (`README.md`, `CLAUDE.md`,
  `CHANGELOG.md`). Planning, audits, and debug notes go under `docs/`.
- Don't create new `.md` files for routine code-only changes; use comments
  and commit bodies. Documentation tasks are the exception.
- Headings in sentence case; tables for reference data; fenced code blocks
  with a language tag; backticks for paths, flags, functions, env vars.

---

## Agent autonomy — tool execution

No permission needed for:

- Read-only shell (grep, find, ls, cat, diff, `git status/log/diff/show`,
  `pkg-config`, `ldd`, `ffprobe`, `nm`, `objdump`).
- `meson setup`, `meson compile`, `meson test`, `ninja`, `clang-format
  --dry-run`, compiling a standalone repro in the scratchpad against MLT.
- Launching the app from **your own worktree's** `builddir` to verify a
  change. It opens a window on the owner's desktop: keep it short, close it
  or kill the process you started, and never leave instances running.
- Reading the reference checkouts (`~/Repos/kdenlive`, the design system).

Permission **is required** before:

- Anything destructive or irreversible: `rm -rf` outside `builddir/` and
  the scratchpad, `git reset --hard`, `git clean`, `git branch -D`,
  worktree removal, overwriting or moving the owner's media or project files.
- Installing or removing system packages (`dnf`), or changing `meson.build`
  dependencies.
- Pushing to any branch other than your own `agent/<name>` branch or the
  fast-forward landing on `main` described above; creating or modifying
  GitHub repos, releases, or workflows.
- Killing processes you did not start (the owner may have the live editor
  open from the main checkout).

---

## What the agent should NOT do

- Don't refactor working code unless the task asks for it; targeted,
  minimal diffs. In particular, don't "modernise" the pull-loop playback,
  the `GKeyFile` project format, or the fixed `atsc_1080p_30` profile
  ad hoc — each is replaced by a planned milestone (M2, M1/ADR-004, M1)
  with its own design; a partial rewrite in between costs more than it saves.
- Don't add `.ui`/GResource files for the shell yet; imperative construction
  is deliberate for v1. (v2 allows `.ui` for static dialogs only.)
- Don't add error handling for situations that cannot occur, and don't
  wrap MLT calls in try/catch — `mlt++` does not throw.
- Don't introduce new dependencies, third-party headers, or build systems
  (CMake, autotools) without an ADR and the owner's sign-off. No Qt, KDE
  Frameworks, GStreamer, or direct SDL/PulseAudio use outside what MLT's
  consumer does internally.
- Don't guess MLT property names, profile names, codec strings, or GTK4 API
  shapes from memory — verify against installed metadata/headers, and say
  when you couldn't.
- Don't add sleeps or timers to "fix" A/V timing; playback pacing is a
  known problem with a designed solution (ADR-002). Document the symptom
  and reference the plan instead.
- Don't leave debug logging at `info` level; use `debug`.
- Don't run or edit anything in the sibling `*-stable` or `*.bak-*`
  repositories under `~/Repos`.

---

## Preferred libraries

| Purpose | Library | Notes |
|---|---|---|
| UI toolkit | GTK 4.22 + libadwaita 1.9 | C API from C++; RAII wrapper for refs from M0 |
| Media engine | MLT 7.40 via `mlt++` | The only media engine. Only `src/engine/` includes it |
| Main loop / IO / settings | GLib, GIO, GSettings | `g_idle_add` today, `MainThreadDispatcher` from M2 |
| Drawing | GSK snapshot (v2), cairo (v1 timeline) | Custom widgets, not a widget per clip (ADR-008) |
| Project files | `GKeyFile` (v1) → MLT XML + libxml2 (v2, ADR-004) | |
| Audio output | `libpulse-simple` (v1) → MLT `sdl2_audio` consumer (M2, ADR-002) | |
| Tests | doctest, vendored (ADR-010) | |
| Formatting | clang-format (M0) | |

Do **not** use: Qt, KDE Frameworks, GStreamer/GES, gtkmm (the codebase uses
the C API deliberately), Boost, CMake, or any GUI framework other than GTK4.

---

## Security & data safety

- The app makes no network requests. Keep it that way; nothing in the
  editor phones home, checks versions, or fetches fonts.
- Project files and imported media are **untrusted input**: they name
  arbitrary resource paths that MLT will open. Never execute, `system()`, or
  shell-interpolate anything read from a project file or media metadata.
- Never overwrite a user's source media. Renders and proxies are written to
  an explicitly chosen output path, atomically (temp file + rename).
- Logs go to `logs/` (later `$XDG_STATE_HOME`); never log full file
  contents or environment dumps.
