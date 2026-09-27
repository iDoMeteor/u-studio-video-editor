# Flatpak packaging

`com.ustudio.VideoEditor.yml` builds the beta-tester Flatpak: the app on the
GNOME 51 runtime, with its own MLT 7.40.0, FFmpeg 8.1.3 and x264. It's a
single-file bundle, not a Flathub submission.

## Building

```sh
just flatpak
```

- Needs `flatpak-builder`, the Flathub remote, and the GNOME 51 runtime and
  SDK as a `--user` install (`--install-deps-from=flathub` fetches them).
- The first build takes about 15 minutes (FFmpeg and MLT from source).
  Later builds reuse `build-flatpak/state` and take a few minutes.
- The bundle lands in `build-flatpak/` (gitignored scratch). Then
  `just dist` copies it to `~/projects/_software-dist/u-stu-video-editor/`
  with a `<name>.sha256` sidecar.
- `just dist` never overwrites or removes anything there: a repeated name
  becomes `-2`, `-3`, and so on (owner's rule). `USTUDIO_DIST_DIR` moves the
  destination.
- **VS Code snap terminals:** they export `XDG_DATA_HOME` into
  `~/snap/code/…`, which silently moves every `flatpak --user` operation
  into a second, private installation there. Run `just flatpak` from a
  normal terminal, or with `env -u XDG_DATA_HOME`.

## What's in it, and why

| Module | Source | Why it's built here |
|---|---|---|
| x264 | git `b35605ac` (stable; same as Fedora 44) | H.264 export; no Flathub runtime has it |
| FFmpeg 8.1.3 | release tarball, sha256-pinned | `--enable-gpl --enable-libx264`, dav1d, VA-API; no avdevice, no network |
| MLT 7.40.0 | release tarball, sha256-pinned | Distros such as Mint 22 ship 7.22; the engine relies on 7.40 |
| u Studio | this repo (`dir` source) | `-Dbuildtype=release`, editor plus `u-studio-render` |

- **MLT modules.** Only the modules the app requests are built: core,
  plus (the `affine` clip transform), normalize (`volume`), avformat, xml,
  sdl2, rtaudio, gdk (`pixbuf` stills) and resample.
  - The Qt6 and glaxnimate modules are explicitly off (ADR-007), and so is
    frei0r (a future drop-in, ADR-014).
  - To recheck the list, match the service strings in `src/` against the
    `identifier:` lines of MLT's module `.yml` files.
- **SDL2** comes from the runtime (sdl2-compat on SDL3), so audio goes to
  PulseAudio/PipeWire.
- **Licence.** The app itself is MIT (`LICENSE`), but linking x264 makes
  the bundle as a whole GPL. flatpak-builder installs
  each module's licence files under `/app/share/licenses/`.
- **Permissions.** Wayland with X11 fallback, DRI, PulseAudio, and file
  access to home, `/media`, `/run/media` and `/mnt`. There's no network
  access; the editor makes no requests.
- **Fonts.** Space Grotesk, JetBrains Mono and Anton aren't bundled yet
  (doc 11 plans that for M7); the CSS fallbacks apply.

## For testers (Linux Mint 22 and other distros)

```sh
flatpak install --user ./u-studio-video-editor-<version>.flatpak
flatpak run com.ustudio.VideoEditor
```

Double-clicking the file in Mint's Software Manager works too.

- The bundle names Flathub as its runtime source, so the first install
  also downloads the GNOME 51 runtime (about 450 MB) if it isn't there
  already.
- Logs are under
  `~/.var/app/com.ustudio.VideoEditor/.local/state/ustudio/logs/`.
  Help › About has Open Log Folder and Copy Diagnostics for bug reports.
- Settings, autosaves and proxies live under
  `~/.var/app/com.ustudio.VideoEditor/`.
- To uninstall: `flatpak uninstall --user com.ustudio.VideoEditor`.
