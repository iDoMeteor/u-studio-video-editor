# 18 — AI generation drop-in

**Status:** proposal, 2026-09-24. Owner idea (2026-09-23) and direction
(2026-09-24, "we'll do the ai client as a drop in"): generate images and
video with AI services from inside the editor. Pick a service, fill in its
prompt and settings, send, and the result is saved and inserted into the
project automatically.

This is a drop-in ([doc 15, "Drop-in structure"](15-effects-and-transitions.md);
[doc 17](17-drop-in-catalogue-and-distribution.md)), opt-in, and the only
part of the product that talks to the network. How that coexists with
CLAUDE.md's "the app makes no network requests" is decided in
[ADR-015](adr/015-ai-generation-network-boundary.md): **the editor stays
network-free; a separate helper process owned by this drop-in does the
network work.**

## What the user does

1. **Generate…** (header-bar button and an action in the registry) opens
   the generation window.
2. Choose a **service**: hosted image, hosted video, or a local server
   running on the same machine.
3. Fill in the **form for that service**. Every service is different, so
   the form is built from the service's own description: prompt, negative
   or system prompt where supported, seed image(s), aspect ratio (defaulting
   to the sequence's), duration and frame rate for video, quality or model,
   random seed, number of variations.
4. **Seed images** come from the media bin, from the **current frame at the
   playhead** (passed in by the editor), or from a file.
5. **Send.** The job appears in a queue with status and progress, and can
   be cancelled. Several jobs can run at once.
6. When a result arrives it is **saved into the project's `generated/`
   folder**, added to the bin, and, if "insert at playhead" was ticked,
   placed on the active track: one undo step in the editor. Variations land
   side by side in the bin to pick from.

Extras that make it pleasant:

- **Presets**: save a service plus prompt and settings under a name
  ("Brand neon background, 16:9"); ship a few Unicorn Tears presets.
- **Re-run**: every generated asset keeps how it was made (below), so
  "Generate again", "Vary" and "Edit prompt and regenerate" are one click
  from the bin.
- A small badge on generated assets in the media browser.

## Architecture

```
drop-ins/ai-generation/
  meson.build  README.md  register.{h,cpp}
  editor/     Editor side, no network code: the Generate action, launching the
              helper with context, receiving results, AddAsset + InsertClip,
              the bin badge and "Generate again" actions.        (GTK)
  helper/     u-studio-generate: the generation window, adapters, job queue,
              downloads, keyring access.        (GTK, libsoup, libsecret, json-glib)
  adapters/   One description per service: form schema + request/response
              mapping. Code only where a service needs it (auth, polling).
  data/       presets, service icons (symbolic)
  tests/      adapter tests against a local mock server; editor-side tests
```

**Why a separate process** (ADR-015):

- The editor process never links a network library, so "the editor makes no
  network requests" stays literally true and testable.
- Under Flatpak, network access is a per-app permission; extensions can't
  add it. The helper ships as its own small Flatpak app,
  `com.ustudio.Generate`, with network access; the editor's Flatpak keeps
  none.
- Crashes, hangs and slow downloads in the helper can't take the editor
  down.
- API keys live only in the helper's process and the system keyring.

**How the two talk.** The same pattern as the titles app (doc 16):

- The editor launches the helper with context on the command line: the
  project's `generated/` folder, a PNG of the current frame, the sequence's
  size and frame rate, and the active track.
- The helper hands each finished file back by activating the editor's
  exported GApplication action (e.g. `app.insert-generated`) with the file
  path and whether to insert at the playhead. The editor probes and imports
  it like any other file; the helper never touches the project file.

Integration points: IP5 (action contribution, import handler, bin badge
through the media browser host), IP1 only if provenance is later shown in
the model (not needed for v1: provenance is a sidecar file).

## Services and adapters

- **Adapter description** (`adapters/<service>.json`): display name, kind
  (image, video), endpoint, authentication style, the form schema (fields,
  types, ranges, defaults, which fields accept images), and the mapping from
  form fields to the request body and from the response to result files.
  The helper builds the form from the schema, like the effect Rack builds
  parameter widgets from descriptors.
- **Adapter code** only where needed: request signing, asynchronous jobs
  (submit, then poll a status URL until done, as video services typically
  work), multi-part uploads for seed images.
- **Local servers are first-class**: adapters for locally run generators
  (for example ComfyUI or AUTOMATIC1111-compatible servers on
  `localhost`) cost nothing, keep footage private, and are the first
  adapter built, since they are also the easiest to test.
- **No provider details are assumed in this plan.** Every hosted adapter is
  written and verified against that provider's current API documentation
  when it is built, the same rule CLAUDE.md applies to MLT property names.
  Which hosted services come first is an owner decision.

## Safety, privacy and cost

- **Explicit action only.** Nothing is sent without the user pressing Send.
  No background calls, no telemetry, no update checks.
- **What leaves the machine is shown first.** Before the first send to a
  remote service (and whenever seed images are included), a confirmation
  lists the host, the prompt text and thumbnails of every image being
  uploaded. "Don't ask again for this service" is per service and
  revocable in Settings.
- **Keys** are stored in the GNOME keyring through libsecret, per service.
  They never appear in project files, settings files, logs or provenance
  records. Log lines redact authorization headers.
- **Costs**: where a service reports price or credits, the form shows the
  estimate before sending and the queue shows what each job cost; a session
  total is visible. No automatic retries that could multiply charges;
  "Retry" is a user action.
- **Files**: downloads stream to a temporary file in `generated/` and are
  renamed into place only when complete and non-empty; names include
  service and timestamp; nothing is ever overwritten. Cancelling removes the
  temporary file.
- **Provenance**: next to every result, a `<file>.generation.json` records
  service, model, prompt, negative/system prompt, settings, seed, the
  seed-image file names, request id and time. No keys.
- **Untrusted responses**: results are treated like any imported media
  (project files and media are untrusted input, CLAUDE.md); nothing from a
  response is executed or shell-interpolated.
- The user remains responsible for each service's terms; the helper links
  to them from its service list.

## Dependencies

libsoup 3 (HTTP), libsecret (keyring) and json-glib (JSON), all GNOME
platform libraries, linked **only** into the helper. They need owner
sign-off through ADR-015.

## Phases

### G0 — Spike (about 1 week)

A throwaway helper with one local adapter and one hosted image adapter:
authentication, a synchronous image request, an asynchronous job with
polling, streaming download, keyring round-trip, the GApplication action
back into the editor, and the Flatpak permission model (editor without
network, helper with).

### G1 — Helper and adapter framework (about 2 weeks)

The generation window, schema-driven forms, the job queue with cancel,
keyring storage, the privacy confirmation, provenance files, atomic
downloads, the local adapter and the first hosted image adapter.

Acceptance:

- [ ] The editor binary links no network library (`ldd`), and its Flatpak
      manifest has no network permission.
- [ ] No API key is ever written outside the keyring (test scans project,
      settings, logs and provenance after a run against a mock server).
- [ ] Cancelling mid-download leaves no partial file.

### G2 — Editor integration (about 1 week)

The Generate action and header-bar button, launching with context
(current-frame PNG, sequence format), receiving results, import plus
insert at playhead as one undo step, the bin badge, Generate again / Vary.

Acceptance:

- [ ] A generated image appears in the bin and, when asked, on the active
      track at the playhead; undo removes both.
- [ ] With the drop-in disabled, the editor has no Generate button and
      projects containing generated media behave like any other project.

### G3 — Video services (about 1–2 weeks)

Hosted video adapters with long-running jobs, job resume after a helper
restart (job ids persisted), frame-rate and size matching to the sequence
on import.

### G4 — Polish (about 1 week)

Presets and brand presets, variations picker, seed-from-frame shortcuts,
cost summary.

## Decisions needed from the owner

1. Which hosted image and video services to support first.
2. Confirm the separate helper process (ADR-015) rather than network code
   inside the editor.
3. Where results go by default: the project's `generated/` folder
   (recommended) or a user-chosen folder.
4. Whether the helper should also run standalone (from the app grid) for
   generating assets outside a project.
