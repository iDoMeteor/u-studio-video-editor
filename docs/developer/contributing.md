# Contributing

[Docs home](../README.md) › [Developer docs](README.md) › Contributing

[CLAUDE.md](../../CLAUDE.md) holds the full rules for everyone, human or
agent. This page is the short version.

## Workflow

1. **Work in your own git worktree**, never in the shared main checkout:
   `git worktree add ../u-studio-video-editor.worktrees/<name> -b <branch>`.
   Branch names are `feature/…`, `fix/…`, `docs/…`, or `agent/…` for agent
   seats.
2. Set up and build in that worktree (`just setup && just build`). Each
   worktree has its own `builddir`.
3. Make a small, focused change. Don't refactor working code on the side.
4. `just test` must pass. Thread and MLT-lifetime changes also need the
   sanitizer tiers ([Testing](testing.md)).
5. Commit, then land it with `git push origin HEAD:main`. If that's
   rejected, merge `origin/main`, rebuild, retest and push again. **Never
   rebase, cherry-pick or force-push.**
6. Remove your worktree and delete the merged branch afterwards.

## Code

- C++23, GCC ≥ 13, warning-clean under the project's flags.
- `.clang-format` is the authority on style (`just fmt`).
- `PascalCase` types, `camelCase` functions, `m_` members, `k` constants,
  `snake_case` files.
- Log through `Log::debug/info/warn/error` with a `[subsystem]` prefix,
  never `printf`.
- Comments explain *why*, and record MLT findings.
- **MLT: verify, don't guess.** Check property and service names against
  the module `.yml` metadata, don't trust MLT return codes documented as
  unreliable, and confirm surprises with a standalone repro. Write the
  finding into the [implementation notes](notes/README.md).
- No new dependencies without an ADR. Never add Qt, KDE Frameworks,
  GStreamer, gtkmm, Boost or CMake.
- Keep new code portable to Windows: OS calls go in `src/platform/`
  ([ADR-017](../plans/v2/adr/017-windows-secondary-target.md)).

## Commits and versions

- Imperative subject of 72 characters or fewer, a blank line, then bullet
  points on *why* and what you verified.
- `version:` in `meson.build` is the one source of truth (SemVer, pre-1.0).
  Bump MINOR for a user-facing feature and PATCH for a fix, in the same
  commit. Add a one-line entry at the top of
  [`CHANGELOG.md`](../../CHANGELOG.md).
- Only a releasable build (a beta or a release) gets release notes in
  `data/com.ustudio.VideoEditor.metainfo.xml`. Those notes appear in
  Help › Release notes.

## Documentation

- When you land a feature or remove a limitation, update the
  [README](../../README.md) and the matching [user guide](../user/README.md)
  page in the same change.
- When code makes a [v2 design doc](../plans/v2/README.md) wrong, fix the
  doc in the same change. To disagree with a doc, add a `> REVIEW:`
  blockquote in place.
- Only entry docs sit at the repo root (`README.md`, `CLAUDE.md`,
  `CHANGELOG.md`). Everything else goes under `docs/` and is linked from
  [the docs index](../README.md), so you can reach it by browsing from the
  README.
- Headings in sentence case, tables for reference data, and fenced code
  blocks with a language tag.
