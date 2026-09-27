# Packaging and releases

[Docs home](../README.md) › [Developer docs](README.md) › Packaging

## Flatpak

The beta Flatpak is built from `packaging/flatpak/com.ustudio.VideoEditor.yml`
with `just flatpak`. [`packaging/flatpak/README.md`](../../packaging/flatpak/README.md)
has the full details: what's bundled and why (MLT 7.40, FFmpeg with x264,
only the MLT modules the app uses, no Qt), permissions, and build tips.

```sh
just flatpak                                   # bundle lands in build-flatpak/
just dist build-flatpak/<bundle>.flatpak       # copy with a .sha256 sidecar
```

`just dist` never overwrites anything. A repeated name gets `-2`, `-3`, and
so on. `USTUDIO_DIST_DIR` changes the destination.

Testers install the published bundle as described in
[Installing](../user/installing.md).

x264, FFmpeg, Eigen, movit and MLT are module files under
`packaging/flatpak/modules/`, shared by the local manifest and the
Flathub one. movit and its build-time Eigen are there for GPU compositing
([ADR-019](../plans/v2/adr/019-gpu-acceleration.md)); FFTW, libepoxy
and GL come from the runtime. All but x264 carry `x-checker-data`, so
Flathub's update bot opens a PR when upstream releases. x264 has no release tags, so its commit is bumped by hand. An
MLT or FFmpeg major bump needs the smoke test and the engine suites
before it lands.

## Package checks

Two checks guard every package, Flatpak or Snap:

- **Nothing from a test run or the build machine ships.**
  `tools/check_bundle_clean.py <tree>` fails on:
  - project files, media, raw audio or logs;
  - images outside the icon and metadata directories;
  - autosave, dconf or `.var` directories;
  - the build machine's home path, or the smoke test's folder names.

  `just flatpak` runs it between the build and the export, and the Snap
  runs it in `override-prime`. So a dirty tree never becomes a package.
- **The installed package works.** `tools/packaging-smoke/run.sh
  <flatpak|snap> <outdir> [--cleanup]` drives the installed app over
  AT-SPI on a private Xvfb display. The steps are:
  1. Import H.264 video, a PNG and a JPEG.
  2. Transform a picture on the preview (MLT's `affine`).
  3. Play, and check the 440 Hz test tone through SDL's disk driver.
  4. Split, undo, redo, save; reopen the saved project.
  5. Render with the High quality profile, and ffprobe the output.
  6. Make a 4K proxy.
  7. Copy Diagnostics.

  Along the way it checks that no Qt library is mapped. It prints
  PASS/FAIL per check and exits non-zero on any failure. Flatpak runs are
  isolated from the user's own app data (a throwaway `HOME`); `--cleanup`
  removes it. Run it before publishing any package.

## Flathub submission

Status: prepared, not submitted. Flathub accepts **stable releases only**,
and new apps can't go to flathub-beta, so the submission waits for a
release the owner declares stable. `tools/flathub_prep.py <tag>` then
writes the submission files to `build-flathub/<app-id>/`. That's the
manifest (the app pinned to the tag's commit), the module files, and
`flathub.json`, which is x86_64 only until an aarch64 build is tested.

### Checklist

| Requirement | State |
|---|---|
| App ID on a domain the owner controls, or `io.github.<user>.<repo>` | **Open.** `com.ustudio.*` can't be verified; the options are below |
| Domain verification: token at `https://<domain>/.well-known/org.flathub.VerifiedApps.txt` | Needs the chosen ID; the owner uploads the token |
| Stable release, `type="stable"` `<release>` entry, tag pushed | Waits for the first stable release |
| Builds offline from pinned sources (sha256 or commit) | Done: every source is pinned; `just flatpak` builds with no network |
| No binaries in the submission | Done |
| Licence files per module in `share/licenses/$FLATPAK_ID` | Done: flatpak-builder installs them, the app's MIT `LICENSE` included |
| Metainfo `project_license` matches the source | Done: `MIT` (the app). The GPL parts are bundled dependencies with their own licence files |
| `flatpak-builder-lint` (manifest, repo, appstream) | Not run yet: needs `org.flatpak.Builder` (owner question) |
| Metainfo: `<developer id=…><name>` | **Open:** needs the developer name and ID |
| Metainfo: screenshots at a tag or commit URL, window only, ≤ 1000×700, captions without full stops | **Open:** who makes them, and where they're hosted |
| Metainfo: branding colours | Done: `#FC3CBA` light, `#A04BFA` dark |
| Metainfo: OARS rating | Done: `oars-1.1`, no content |
| Name ≤ 20 characters, not lowercase-first; summary ≤ 35 characters, no toolkit names | Name decided: "U Stu Video Editor" (18; owner, 2026-09-27; VE Core renames the app). **Open:** the summary still names GTK4/libadwaita; owner question |
| Icon: SVG or PNG ≥ 256 px, no baked shadow | Done: the owner's SVG. Flathub also warns about icons that fill the whole canvas; this one nearly does |
| `x-checker-data` for external sources | Done for FFmpeg and MLT; x264 is manual |
| Static permissions justified | Justification below |

### App ID options

| ID | Needs | Notes |
|---|---|---|
| `com.unicornviz.UStu` | Token on unicornviz.com | The domain already hosts the downloads |
| `com.djunicorntears.UStu` | Token on djunicorntears.com | The current homepage |
| `io.github.idometeor.UStu` | Nothing extra | Ties the ID to the GitHub account and repo name |

Renaming touches these:
- the app ID in `main.cpp`;
- the GSettings schema ID and path;
- the desktop file, icon and metainfo file names, and the metainfo `<id>`;
- the D-Bus name that `drive.py` and the Actions calls use;
- `~/.var/app/<id>/`.

Only beta testers have the old ID, so a rename before any public release
costs them one reinstall. Their settings reset. Old projects still open,
because projects store media paths, not the app ID. Flatpak can
redirect the old desktop file with `rename-desktop-file`, but that isn't
needed before a public release.

### Permissions justification

The Flathub reviewers will ask about `--filesystem=home`, `/media`,
`/run/media` and `/mnt`. The answer:

> u Studio is a video editor. A project references footage, audio and
> pictures by path, often hundreds of files across home folders and
> external drives. It reopens them on every load, relinks moved media by
> searching folders, and writes renders and proxies next to or apart
> from them. Portal document paths (`/run/user/…/doc/`) aren't stable
> across sessions or machines, so projects saved with them break. We
> request home and removable-media locations rather than `host`: that's
> narrower than the video editors already on Flathub (Kdenlive,
> Shotcut, Pitivi, OpenShot and VidCutter all use `--filesystem=host`).
> There's no network permission, and the app makes no requests.

The other permissions are the standard set for a GTK video editor:
`--socket=wayland`, `--socket=fallback-x11`, `--share=ipc`, `--device=dri`
(GPU for decoding and the preview) and `--socket=pulseaudio` (playback).

### Submitting

1. Fork `flathub/flathub` on GitHub, with "Copy the master branch only"
   **unchecked**. Clone it with `--branch=new-pr`.
2. Branch from `new-pr` and add the files from `build-flathub/<app-id>/`.
3. Open a PR against the **`new-pr`** base branch, titled
   `Add <app-id>`.
4. Comment `bot, build` to run a test build. Answer the review.
5. On acceptance Flathub creates `flathub/<app-id>` and invites the
   owner as maintainer: GitHub 2FA must be on, and the invite accepted
   within a week.
6. Later releases are PRs to `flathub/<app-id>`, never a new submission.
   The x-checker bot opens PRs for dependency updates.

The owner opens the PR from their account; we prepare the files.

## Snap

Status: draft `snap/snapcraft.yaml`, not built yet. snapcraft and LXD
aren't installed (owner question), and the snap name isn't registered.

- `core24` with the `gnome` extension (GNOME 46: GTK 4.14 and
  libadwaita 1.5, the newest the app's symbols need), strict
  confinement.
- Plugs: `home` (not hidden folders), `removable-media`, `audio-playback`,
  plus the extension's `desktop`, `wayland`, `x11` and `opengl`.
- x264, FFmpeg and MLT are parts, built as in the Flatpak with the same
  pinned sources and MLT modules.
- **Not yet in the draft: movit (ADR-019).** The Snap needs Eigen (build
  only) and movit 1.7.2 parts, and `-DMOD_MOVIT=ON`, matching the Flatpak.
  core24's archive has `libfftw3-dev`, `libepoxy-dev` and Eigen, and the
  gnome extension's `gpu-2404` interface supplies Mesa. The parts go in
  when the Snap is first built, with the GL side done in VE GPU's stage G5.
- The app bakes MLT's module directory in at build time. Snap layouts bind
  the snap's copies to `/usr/lib/x86_64-linux-gnu/mlt-7` and
  `/usr/share/mlt-7`, so FactoryPolicy's curated directory (ADR-007) works
  unchanged. The draft is amd64 only.
- `override-prime` runs the clean check. The smoke test takes the `snap`
  runner.

For the Snap Store, the owner needs these:
- a Snapcraft (Ubuntu One) account;
- the snap name registered (`snapcraft register <name>`);
- the publisher name.

Strict confinement needs no manual review; classic would.

## Drop-in builds

Effects and titles are drop-ins
([ADR-013](../plans/v2/adr/013-effects-and-titles-as-drop-in-modules.md),
[ADR-014](../plans/v2/adr/014-drop-in-loading-and-distribution.md)). The
meson options `dropin_effects` and `dropin_titles` take `disabled` (the
default for now), `builtin` or `module`. `just dropins-builtin` and
`just dropins-module` build and test both configurations, with every
drop-in whose folder is in the tree (only `drop-ins/titles/` so far). The planned
catalogue is in [v2 doc 17](../plans/v2/17-drop-in-catalogue-and-distribution.md).

## Releases

- Everyday version bumps go in [`CHANGELOG.md`](../../CHANGELOG.md).
- A releasable build (a tagged beta or release) also gets a `<release>`
  entry in `data/com.ustudio.VideoEditor.metainfo.xml`, written for
  testers: what's new, what to try, known issues. A release meant for
  Flathub needs `type="stable"` (the default), not `development`.
- The first stable release, `2.0.0`, is milestone M7
  ([roadmap](../plans/v2/12-roadmap-and-milestones.md)).

See also [v2 doc 11: Build, test, CI, packaging](../plans/v2/11-build-test-ci-packaging.md).
GitHub Actions CI is parked (manual dispatch only) until the owner turns
it back on.
