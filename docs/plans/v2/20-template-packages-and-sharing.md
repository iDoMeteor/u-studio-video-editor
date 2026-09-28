# 20 — Template packages and sharing

[Docs home](../../README.md) › [v2 planning docs](README.md) › Template packages and sharing

**Status:** proposal, 2026-09-27. Owner request the same day: load and
save text and chiron (lower-third, ticker, bug) template packages as
`.zip` or `.tar.gz`, locally or from a shared catalogue users can
publish to, with signed-URL downloads. The decision record is
[ADR-020](adr/020-template-packages-and-sharing.md); the back-end
requirements are in [doc 21](21-template-sharing-backend.md). Builds on
templates in [doc 16](16-titles-tool.md) (T4).

## What users get

- **Save as Package…**: in the titles app's gallery, pick templates from
  My Templates, add a title, description, tags and licence, and save a
  `.zip` or `.tar.gz`.
- **Open Package…**: in the titles app, or the editor's New Title gallery.
  It shows what's inside (previews, fonts, licence, author) before
  installing into My Templates, under a section named after the pack.
  Installing a newer version of the same pack replaces it after asking;
  an older version is refused unless the user confirms.
- **Remove Pack** from the gallery.
- **Browse Shared Templates…**: opens the `u-studio-share` helper (it
  offers to install it if it's missing). The user searches, previews,
  downloads, and the pack arrives in the gallery as if it were opened
  from disk.
- **Publish…**: from a pack in My Templates. It shows what will be
  uploaded, asks the user to sign in, and hands the file to the helper,
  which uploads it. The pack becomes public only after the service has
  checked it (doc 21).

## Package format

An archive (`.zip`, or `.tar.gz` / `.tgz`) with this layout:

```text
pack.xml                 manifest (required)
templates/<name>.ustitle one or more templates
previews/<name>.png      one generated preview per template (required)
fonts/<file>.otf|.ttf    optional; each needs a licence entry
images/<file>.png|.jpg   optional; referenced by templates
LICENSE.txt              optional; the pack's licence text
```

`pack.xml` (libxml2, versioned like `.ustitle`):

| Field | Notes |
|---|---|
| `format` | `1` for now; unknown majors are refused |
| `id` | reverse-DNS or `<publisher>/<slug>`, stable across versions |
| `version` | SemVer; used for replace and upgrade |
| `title`, `description`, `tags` | shown in the gallery and the catalogue |
| `author`, `publisher` | publisher is set by the service on publish |
| `licence` | an SPDX id (for example `CC-BY-4.0`, `CC0-1.0`, `MIT`) |
| `min-app-version` | the oldest titles drop-in that can render it |
| `files` | every file with its SHA-256 and size |
| `fonts` | per font: family, file, licence (only licences that allow redistribution, such as `OFL-1.1`) |

## Validation (client side, before anything is written)

The helper and both apps run the same validator (in `drop-ins/titles/core`,
with libarchive in `drop-ins/titles/package`):

- **Paths:** relative only; no `..`, no absolute paths, no drive letters;
  NFC-normalised; no duplicate names that differ only by case (Windows).
- **Entry types:** regular files and directories only. Symlinks,
  hardlinks, devices and FIFOs reject the pack.
- **Content types:** sniffed from magic bytes, not trusted from the
  extension. `.ustitle` and `pack.xml` must parse; images must decode;
  fonts must load in FreeType.
- **Limits (initial):** 25 MB compressed, 100 MB uncompressed, 500
  entries, per-entry compression ratio under 100:1, 40 templates, 10
  fonts.
- **Integrity:** every file matches the manifest's SHA-256 and size, and
  there are no files the manifest doesn't list.
- **References:** templates may only reference fonts and images inside
  the pack or the system's fonts. No absolute paths and no URLs.
- Extraction goes into a temporary directory, then an atomic rename into
  the user template library; a failure leaves nothing behind.

Nothing in a pack is executed. `.ustitle` has no scripting, and dynamic
fields are evaluated by our own code.

## Where things live

| Piece | Location | Network |
|---|---|---|
| Manifest, validator, library install/remove | `drop-ins/titles/core`, `drop-ins/titles/package` | none |
| Open/Save Package UI | `u-studio-titles` and the editor's New Title gallery (IP5) | none |
| Browse, download, sign-in, publish | `u-studio-share` helper (`drop-ins/titles/share/`, own Flatpak app `com.ustudio.Share`) | yes, on explicit action only |
| Catalogue service | separate project, [doc 21](21-template-sharing-backend.md) | n/a |

The hand-off follows ADR-015:
- the helper downloads to its own cache, verifies, then activates
  `app.install-template-pack(path)` on the titles app (or the editor);
- to publish, the app launches the helper with the pack's path.

## Phases (titles track)

- **T4b — Local packages (about 1 week, right after T4).**
  - Deliverables: the format, validator, Open/Save Package and Remove
    Pack, a libarchive dependency confined to the titles drop-in, and a
    fuzz-style validator test corpus (traversal, symlink, bomb, bad hash,
    unknown file, oversized).
  - Acceptance:
    - [x] A pack saved on one machine opens on another and renders
          identically. (`titles-packs-app`: packed, opened into another
          library, byte-identical frames; live on Xvfb, 0.68.0.)
    - [x] Every malicious sample is rejected with a readable reason and
          writes nothing. (`titles-pack`: 20 samples in memory, and real
          archives with a traversal, a symlink and a gzip bomb.)
    - [x] `.zip` and `.tar.gz` of the same pack install identically.
          (`titles-pack`, also a .zip made with the `zip` tool.)
- **T7 — Sharing helper (about 2 weeks, after T4b; needs the service in
  doc 21 or its mock).**
  - Deliverables: `u-studio-share` with browse/search, preview, download
    (through signed URLs), sign-in (system browser, OAuth PKCE) and
    publish; the hand-off actions; a local mock of doc 21's API for tests.
  - Acceptance:
    - [ ] The editor and titles Flatpak still have no network permission
          and link no network library.
    - [ ] A download is verified again client-side, even when the service
          says it's clean.
    - [ ] Nothing reaches the network without a user action (checked with
          the mock's request log).

## Open questions (owner)

1. Catalogue name and domain (with the `djunicorntears.com` decision).
2. Moderation: publish instantly after automated checks, or hold for
   review? (Recommended: automated checks, then review for a new
   publisher's first pack.)
3. Allowed licences for published packs. (Recommended: CC0, CC-BY-4.0,
   MIT, OFL for fonts.)
4. Whether anonymous users can download, or only signed-in ones.
   (Recommended: anonymous, rate-limited, signed URLs only.)
