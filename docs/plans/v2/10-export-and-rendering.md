# 10 — Export and rendering

## Out of process (ADR-009)

Rendering runs in a child process, `u-studio-render`, launched by the editor
with `GSubprocess`. Reasons: an avformat/encoder crash cannot take the editor
down; the editor stays responsive and editable; the same binary is a
scriptable CLI and the proxy generator; it's testable headless in CI.

The editor does **not** export via its live tractor. It saves a snapshot of
the project to a temporary file (the same serialiser as Save, doc 09) and
hands that path to the child. This means "what you render is what you saved",
a property we can test.

## CLI

```
u-studio-render <project.ustudio> --out <file> [--preset <name>] [--range in:out]
                [--profile <mlt profile or WxH@fps>] [--proxy <asset-id>] [--set k=v]...
                [--progress json|none]
```

- Loads the project with `Mlt::Producer(profile, "xml", path)` (MLT's own
  reader, because the file is valid MLT XML; this also exercises the "file is
  independently renderable" guarantee on every export).
- Builds an `avformat` consumer; applies the preset (`consumer.set("properties",
  presetName)` using MLT's shipped presets under
  `/usr/share/mlt-7/presets/consumer/avformat/`, or our own `.properties`
  files in `data/render-presets/`), then `--set` overrides.
- Listens to `consumer-frame-show`; prints one JSON line per second:
  `{"frame":1234,"total":7500,"fps":48.2,"eta":130}`; final line
  `{"done":true,"file":"…"}` or `{"error":"…"}`. Exit code 0/1.
- `SIGTERM` = cancel: `consumer.stop()`, delete partial output, exit 130.
- `--proxy` renders one asset's proxy per doc 07 and ignores the timeline.

## Presets

Curated, user-facing, each a `.properties` file (MLT preset format):

| Name | Container/codecs | Use |
|------|------------------|-----|
| `MP4 H.264 (default)` | mp4 / libx264 crf=20 / aac 192k | general, YouTube |
| `MP4 H.264 High Quality` | crf=16, `preset=slow` | archive-ish |
| `WebM VP9` | webm / libvpx-vp9 / opus | web |
| `Matroska ProRes` | mkv / prores_ks profile 3 / pcm_s16le | intermediate |
| `Audio only (FLAC / AAC)` | | |
| `GIF` | | fun; short ranges |
| `Image sequence (PNG)` | | |

Hardware encoders (`h264_vaapi`, `h264_nvenc`) as presets that are shown only
if `avformat` reports the encoder (`Mlt::Properties` from
`repository->metadata(consumer_type, "avformat")` → `vcodec` list, or a probe
render of 1 frame at first use). Never default to them.

## Export dialog (app)

`AdwDialog` with: preset dropdown (+ advanced expander showing the resulting
properties, editable), output file (`GtkFileDialog` save), range
(whole / in-out), resolution/fps (defaults to sequence; scaling changes only
the consumer's `width/height`, not the profile, so compositing geometry is
preserved), "add to queue" vs "render now".

## Render queue

A `RenderQueue` (app) holds jobs; one runs at a time by default (setting).
A bottom "Render" panel lists jobs with progress bars, cancel, "show in
Files" on completion, and a log expander with the child's stderr. Jobs
survive project close but not app quit (v2.0). Desktop notification on
completion when the window is not focused (`GNotification`).

## Correctness checks

- `tests/render/`: render a synthetic 2-second project (`color:` + `tone`
  producers) with each preset to `/tmp`, then probe the output with a fresh
  `Mlt::Producer` and assert frame count, size, fps, and the presence of the
  audio stream. Runs in CI (no display needed).
- Frame-count invariant: `rendered frames == out - in + 1`. Any off-by-one is
  a bug in the range handling, not tolerance.
