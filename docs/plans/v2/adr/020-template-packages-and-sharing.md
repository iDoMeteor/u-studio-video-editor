# ADR-020: Template packages are local archives; sharing runs in a separate networked helper against a signed-URL service

**Status:** Accepted (owner, 2026-09-27, through the VE Strategist: "a
load/publish template feature that can load/save local or remote
text/chiron template packages (zip or tar/gz files)", and "we'll use
signed urls for downloads so we don't just open up full public access to
them for robots & such"). Extends ADR-012 (titles) and follows the
network boundary of ADR-015.

## Context

Titles (doc 16) already plan templates: `.ustitle` files with fields, a
built-in gallery, and user templates (T4). The owner wants whole sets of
templates to move between machines and people:

- **locally**, as a `.zip` or `.tar.gz` file saved or opened by the user;
- **remotely**, from a shared catalogue that users browse, download from
  and publish to.

CLAUDE.md says the editor makes no network requests, and ADR-015 keeps it
that way for AI generation by putting all network code in a separate
helper with its own Flatpak app id. A Flatpak grants network access per
app, so network code in the editor or in `u-studio-titles` (which ships in
the editor's Flatpak) would give the whole editor network access.

A template package is untrusted input from strangers: archives can carry
path traversal, symlinks, decompression bombs and files we never want to
open.

## Decision

1. **Package format.** A template package is a `.zip` or `.tar.gz`
   holding a `pack.xml` manifest (libxml2, like `.ustitle`), the
   templates (`.ustitle`), their generated preview PNGs, and optional
   fonts and images (PNG and JPEG only). The manifest carries the pack id,
   version, title, author, licence, tags, and a SHA-256 per file. Doc 20
   has the details.
2. **Local load and save are in the titles drop-in, with no network.**
   Both `u-studio-titles` and the editor's New Title gallery can open a
   package (it installs into the user template library) and save
   templates as a package. Archive reading and writing use **libarchive**
   (BSD-2-Clause, MIT-compatible; in the GNOME runtime and on Windows),
   allowed in the titles drop-in only. It's never linked by `src/`.
3. **Every package is validated before anything is written:**
   - relative paths only, no `..`, no absolute paths, no symlinks,
     hardlinks or device files, and names normalised;
   - only the allowed file types, sniffed by content and not trusted by
     extension;
   - size, file-count and compression-ratio limits;
   - a manifest schema check and SHA-256s that match;
   - every `.ustitle` parsed with the titles reader.

   A failure rejects the whole package with a reason the user can read.
   Nothing in a package is ever executed.
4. **Remote access lives in `u-studio-share`**, a separate helper
   executable under `drop-ins/titles/share/`, shipped as its own Flatpak
   app (`com.ustudio.Share`) with network access. The editor and
   `u-studio-titles` keep no network access and link no network library.
   - The helper browses the catalogue, downloads a package to a local
     file and hands it over through an exported GApplication action, the
     same pattern as ADR-015.
   - For publishing, the titles app writes the package locally and
     launches the helper with its path.
   - libsoup 3 and json-glib are allowed in the helper only.
5. **Network traffic only on an explicit user action:** browse, download
   or publish. There are no background checks, no telemetry and no
   automatic updates. Publishing shows exactly what will be uploaded and
   under which account and licence. Sign-in tokens live in the keyring
   (libsecret, helper only).
6. **Downloads use short-lived signed URLs.** The catalogue's storage is
   never publicly readable. The service issues a download URL per request
   that expires within minutes, so robots can't crawl or hot-link the
   packages. The helper re-verifies every package (point 3) whatever the
   service says.
7. **The back end is a separate project.** Doc 21 loosely defines an AWS
   serverless service. Its code doesn't live in this repository, and the
   client never assumes more than doc 21's API.

## Consequences

- The editor's no-network property stays literal and testable (`ldd`, the
  Flatpak manifest). CLAUDE.md's security section names both helpers.
- A third app id to package (`com.ustudio.Share`); users who never share
  never install it. ADR-015's AI helper and this one might later merge
  into one networked helper; that would need a new ADR.
- libarchive becomes a dependency of the titles drop-in. VE Installers
  confirms it's in the Flatpak runtime and the Snap base before T4b lands.
- Running the service costs money and needs moderation. Doc 21 lists the
  owner decisions it needs before anything is deployed.
