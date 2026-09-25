# ADR-017: Windows is a secondary launch target; OS-specific code lives behind `src/platform/`

**Status:** Accepted (owner, 2026-09-25: "we will be adding windows as a
secondary launch target, so all code from here should keep that in mind and
do related side tasks when passing by"). Amends CLAUDE.md's platform line,
which had said "Windows/macOS are not targets".

## Context
Every building block the editor uses also runs on Windows:
- GTK4 and libadwaita (packaged in MSYS2; several GNOME apps ship Windows
  builds);
- MLT and FFmpeg (Shotcut and kdenlive ship on Windows through MLT);
- the `sdl2_audio` consumer;
- meson, C++23 and doctest.

Most of the code is plain C++, GLib or MLT and ports unchanged. A survey on
2026-09-25 found about 19 files with Linux-only code:
- `/proc` reads (the executable's path, process liveness for autosave);
- symlinks into the curated MLT module directory (ADR-007);
- `.so` module names and `$libdir` paths (drop-ins, ADR-014);
- `g_unix_signal_add` for quit on SIGTERM;
- document-portal xattrs, the snap-environment scrub, glibc `mallopt`;
- fd-level stdout redirection.

Porting now would slow M4 onwards. Leaving these calls scattered would make a
later port expensive.

## Decision
- **Targets:**
  - Primary: Fedora and other modern GNOME desktops on Linux, shipped as a
    Flatpak (M7).
  - Secondary: Windows 10/11 x64, built with MSYS2's UCRT64 toolchain
    against the same libraries.
  - macOS remains not a target.
- **The port itself** (Windows build, installer, a Windows CI job) is
  scheduled work, not part of M4. It's planned after the feature set
  settles, around M7. Linux stays the reference: a feature may land on
  Linux first, but never in a way that blocks Windows.
- **`src/platform/`** is a small library holding every OS-specific call,
  behind std-only interfaces with one implementation file per OS
  (`*_linux.cpp`, and later `*_windows.cpp`). It may include std and OS
  headers. Like `core`, it may not include GTK, GLib or MLT. GLib-level
  portability (`GSubprocess`, `g_get_user_*_dir`, GModule, GIO launching)
  is already cross-platform and stays in its layer.
- **Rules for new code, from now on:**
  1. No `/proc`, `unistd.h`, `fcntl.h`, `sys/*`, `dlfcn.h`, signals, symlinks,
     or other POSIX/Linux-only calls outside `src/platform/`. Add a function
     there instead.
  2. Paths: `std::filesystem::path` and GLib's user-dir functions. Never
     hard-code `/`-joined absolute paths, `/tmp` or `~`. Tests use a temp
     directory from `std::filesystem::temp_directory_path()` or the test's
     scratch helper.
  3. Shared-library names and executable suffixes come from `src/platform/`
     (`.so` vs `.dll`, `""` vs `".exe"`).
  4. Child processes go through `GSubprocess`; cancellation goes through a
     `platform::` helper (SIGTERM on Linux, a job object or
     `TerminateProcess` on Windows).
  5. Linux-only features (the document portal, the snap scrub, `mallopt`)
     are allowed when they sit behind `src/platform/` or an `#ifdef` with a
     no-op elsewhere, and the app works without them.
  6. Text files are written as UTF-8 with `\n`. File names are handled as
     UTF-8 or `std::filesystem::path`, never assumed ASCII.
- **Migration "when passing by":** existing Linux-only sites move behind
  `src/platform/` when a change touches the file anyway, as a separate small
  commit next to the feature commit. No big-bang refactor.
- **Review:** a new POSIX/Linux-only call outside `src/platform/` is a
  review blocker, like an MLT include in `src/app/`. A meson check (grep, as
  for the app boundary) enforces it once `src/platform/` exists. Until then,
  existing sites are listed in doc 14's migration list.

## Consequences
- The port becomes a focused job: an estimated 2–3 weeks for the build,
  platform implementations and installer, instead of an audit of the whole
  tree.
- Some small indirection cost now, one function call per platform
  touchpoint.
- Tests must not assume Linux paths or tools. Test media keeps being
  generated with MLT, which is portable.
- Packaging on Windows bundles GTK, MLT, FFmpeg (with x264) and the
  curated MLT modules. ADR-007's no-Qt policy is simpler there, because we
  control exactly which module DLLs ship.
- Drop-in modules (ADR-014) become `.dll` files on Windows, loaded from an
  install-relative directory. The exact-version rule is unchanged.
