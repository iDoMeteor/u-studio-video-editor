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

## Reusing ai-animated-video

The owner's `~/Repos/ai-animated-video` project already talks to current
image and video services, with models, prices and API behaviour kept up to
date. Surveyed read-only on 2026-09-24 (no keys or `config.env` read):

| What exists there | Where | How we use it |
|---|---|---|
| Provider scripts for OpenAI GPT-image, xAI Grok image and video, Ideogram, Stability (SD3.5 Large), Kling and Runway. Each is a CLI (`-f <prompt file>`, provider flags, `-I <seed image>` for image-to-video, output path), with its endpoint, auth, polling, error codes and verification date documented in its header | `*-retrieve-*.sh`, `gpt-image-retrieve.sh` | **Run them as the helper's generation backend** in G0–G1 instead of reimplementing each API |
| Model catalogue: exact model ids, labels, prices in usable units, status, sunset dates and replacements, verified against each provider's docs (`VERIFIED_ON = "2026-09-23"`) | `frontend/lib/model-catalog.ts` | **Model pickers and cost estimates** in the helper's forms |
| Settings schema: every tweakable with type, default, help text, options and secret flag | `frontend/lib/config-fields.ts` | **The per-service form schema** (doc 18's adapter description, largely already written) |
| Capability and pricing overview across providers | `frontend/lib/model-atlas.ts` | the service picker's comparison view |
| Prompt "art director" (text → scene prompt via GPT or Grok), narration (TTS) and transcription scripts | `get-image-prompt.sh`, `openai-stt-batch.sh`, `transcribe-openai.py` | later extras: "Improve my prompt", narration as an audio asset, cloud transcription for captions (all network, so helper-only) |

How the reuse works:

- **Call the scripts in place; don't copy them.** Every provider script
  sources that repo's `functions.sh` and calls `load_config`, and its
  AGENTS.md forbids renaming or relocating scripts. The helper runs them
  from an ai-animated-video checkout or installed copy whose location is a
  setting, with a scratch working directory per job.
- **Keys never go into a file we write in the project.** `load_config`
  accepts a `CONFIG_FILE` override. The helper writes a per-job config with
  mode 0600 under `$XDG_RUNTIME_DIR`, filled from the keyring, and deletes
  it when the job ends; or passes keys through the environment if G0 shows
  the scripts honour that. Which one is G0's first question.
- **Catalogue data is synced, not hand-copied.** A small export step in
  ai-animated-video (it already has the TypeScript toolchain) writes
  `model-catalog.json` with its `VERIFIED_ON` date; the drop-in keeps a
  copy in `drop-ins/ai-generation/data/` and shows that date in the UI.
  ai-animated-video stays the single place prices and models are
  maintained.
- **Their runtime needs** are `bash`, `curl`, `jq`, `base64` and `file` for
  every provider script, plus ImageMagick for one and ffmpeg/ffprobe for
  another. All are normal Fedora packages; the helper's Flatpak would bundle
  them.
- **Later, if needed:** once the helper is proven, any adapter can be
  reimplemented natively (libsoup) using the script and its header notes as
  the executable specification. Doing it up front would redo verified work.

Owner coordination: this makes ai-animated-video an upstream of the
drop-in. Changes it would need (the catalogue export, a stable exit-code
and output contract for the scripts, possibly env-based keys) are made
there, under that repo's own rules.

## Services and adapters

- **Adapter description** (`adapters/<service>.json`): display name, kind
  (image, video), endpoint, authentication style, the form schema (fields,
  types, ranges, defaults, which fields accept images), and the mapping from
  form fields to the request body and from the response to result files.
  The helper builds the form from the schema, like the effect Rack builds
  parameter widgets from descriptors.
- **Adapter code** only where needed. For the services ai-animated-video
  covers, the adapter runs its provider script (see "Reusing
  ai-animated-video") and parses its result; request signing, polling and
  uploads are already handled there. Native code is only for services it
  doesn't cover.
- **Local servers are first-class**: adapters for locally run generators
  (for example ComfyUI or AUTOMATIC1111-compatible servers on
  `localhost`) cost nothing, keep footage private, and are the first
  adapter built, since they are also the easiest to test.
- **No provider details are assumed in this plan.** The first hosted
  services are the ones ai-animated-video already supports and keeps
  verified (OpenAI GPT-image, xAI Grok image and video, Ideogram,
  Stability, Kling, Runway). Any other service is verified against its
  provider's current API documentation when its adapter is built.

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

libsecret (keyring) and json-glib (JSON), GNOME platform libraries, linked
**only** into the helper; plus, at runtime, an ai-animated-video checkout
and its tools (`bash`, `curl`, `jq`, `base64`, `file`, ImageMagick,
ffmpeg). libsoup 3 is needed only if an adapter is later written natively.
All need owner sign-off through ADR-015.

## Phases

### G0 — Spike (about 1 week)

A throwaway helper that runs ai-animated-video's scripts: one hosted image
job (GPT-image or Grok) and one video job with seed image (Kling or
Runway); how keys reach the scripts without a file in the project
(`CONFIG_FILE` pointing at a 0600 file in `$XDG_RUNTIME_DIR`, or the
environment); the scripts' exit codes and output as a result contract;
reading `model-catalog.json`; cancelling a running job; the GApplication
action back into the editor; and the Flatpak permission model (editor
without network, helper with). Plus one local-server adapter, since that
path needs native code.

### G1 — Helper and adapter framework (about 2 weeks)

The generation window, schema-driven forms, the job queue with cancel,
keyring storage, the privacy confirmation, provenance files, atomic
downloads, the local adapter, and the image services ai-animated-video
supports, with model pickers and price estimates from its synced
catalogue.

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

The video services ai-animated-video supports (Grok video, Kling, Runway),
with long-running jobs, resume after a helper restart where the scripts
allow it, and frame-rate and size matching to the sequence on import.

### G4 — Polish (about 1 week)

Presets and brand presets, variations picker, seed-from-frame shortcuts,
cost summary.

## Decisions needed from the owner

1. Confirm reusing ai-animated-video as the upstream for provider scripts
   and the model catalogue, and which of its services to enable first.
2. Confirm the separate helper process (ADR-015) rather than network code
   inside the editor.
3. Where results go by default: the project's `generated/` folder
   (recommended) or a user-chosen folder.
4. Whether the helper should also run standalone (from the app grid) for
   generating assets outside a project.
