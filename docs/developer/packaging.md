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

## Drop-in builds

Effects and titles are drop-ins
([ADR-013](../plans/v2/adr/013-effects-and-titles-as-drop-in-modules.md),
[ADR-014](../plans/v2/adr/014-drop-in-loading-and-distribution.md)). The
meson options `dropin_effects` and `dropin_titles` take `disabled` (the
default for now), `builtin` or `module`. `just dropins-builtin` and
`just dropins-module` build and test both configurations. The planned
catalogue is in [v2 doc 17](../plans/v2/17-drop-in-catalogue-and-distribution.md).

## Releases

- Everyday version bumps go in [`CHANGELOG.md`](../../CHANGELOG.md).
- A releasable build (a tagged beta or release) also gets a `<release>`
  entry in `data/com.ustudio.VideoEditor.metainfo.xml`, written for
  testers: what's new, what to try, known issues.
- The first stable release, `2.0.0`, is milestone M7
  ([roadmap](../plans/v2/12-roadmap-and-milestones.md)).

See also [v2 doc 11: Build, test, CI, packaging](../plans/v2/11-build-test-ci-packaging.md).
GitHub Actions CI is parked (manual dispatch only) until the owner turns
it back on.
