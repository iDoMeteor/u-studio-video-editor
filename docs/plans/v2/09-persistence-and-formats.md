# 09 — Persistence and formats

## Project file: MLT XML with an `ustudio:` namespace (ADR-004)

A `.ustudio` file is an MLT XML document (`<mlt>` root) that:

1. `melt` and `u-studio-render` can render **directly**, with no editor
   involved, because the tractor/playlist/filter/transition structure is the
   real MLT graph. This is the same strategy kdenlive uses (`.kdenlive` files
   are MLT XML with `kdenlive:` properties).
2. Carries everything the model needs to round-trip **losslessly** in
   `ustudio:*` properties, so loading never has to infer model structure from
   the MLT graph.

Written by **our serialiser** (`core/xml/writer.cpp`), not by MLT's `xml`
consumer, because the model is the truth and MLT's serialiser emits
implementation details (cached lengths, auto-attached normalisers) that
churn diffs and can't carry our ids cleanly. Read by our reader
(`core/xml/reader.cpp`, libxml2) which looks **only** at `ustudio:*`
properties and ids, then re-derives the graph. The MLT-facing structure is
regenerated on save; it is output, not input.

### Sketch

```xml
<?xml version="1.0"?>
<mlt LC_NUMERIC="C" version="7.40.0" producer="main_bin" root="/home/jj/Videos/promo">
  <profile description="ustudio" width="1920" height="1080" frame_rate_num="30000" frame_rate_den="1001" … />
  <!-- bin: one master producer per asset -->
  <producer id="asset3" in="0" out="7499">
    <property name="resource">clips/intro.mp4</property>       <!-- relative to root -->
    <property name="ustudio:asset_id">3</property>
    <property name="ustudio:folder">/b-roll</property>
    <property name="ustudio:fingerprint">10485760:1757600000123456789</property>
    <property name="ustudio:proxy">…</property>
  </producer>
  <playlist id="main_bin">          <!-- keeps producers referenced; kdenlive convention -->
    <entry producer="asset3" in="0" out="7499"/>
  </playlist>
  <!-- one playlist per track, top track LAST (MLT order) -->
  <playlist id="track_a1"> <property name="ustudio:track_id">11</property> … </playlist>
  <playlist id="track_v1">
    <property name="ustudio:track_id">10</property>
    <property name="ustudio:kind">video</property>
    <property name="ustudio:name">V1</property>
    <blank length="30"/>
    <entry producer="asset3" in="120" out="420">
      <property name="ustudio:clip_id">42</property>
      <filter id="fx7"><property name="mlt_service">affine</property>
        <property name="transition.rect">0=0 0 1920 1080 1;90~=100 50 1720 968 1</property>
        <property name="ustudio:effect_id">7</property></filter>
    </entry>
  </playlist>
  <tractor id="seq1" in="0" out="…">
    <property name="ustudio:sequence_id">1</property>
    <property name="ustudio:next_id">300</property>
    <property name="ustudio:markers">[{"id":5,"at":120,"text":"drop","color":2}]</property>
    <track producer="black"/><track producer="track_a1"/><track producer="track_v1"/>
    <transition …composite…/> <transition …mix…/>
  </tractor>
</mlt>
```

Paths are stored relative to the project file's directory when the asset is
under it, absolute otherwise; `root` is set so MLT resolves relative
resources.

### Versioning

`<property name="ustudio:format_version">2</property>` on the root tractor.
The reader refuses newer versions with a clear message and migrates older
ones in code (`core/xml/migrations.cpp`, one function per version bump).
Every migration has a fixture file in `tests/fixtures/projects/`.

## Save semantics

- Atomic: write to `<name>.ustudio.tmp` in the same directory, `fsync`,
  `rename` over the target.
- The `Document` tracks `isClean()` via the undo stack's clean point; the
  window title shows `•` when dirty; closing prompts (AdwAlertDialog).
- "Save As" re-relativises asset paths against the new location.

## Autosave and recovery

- Timer: 2 minutes after the last command while dirty, and on focus loss.
- Location: `$XDG_STATE_HOME/ustudio/autosave/<sha1(path or 'untitled-'+uuid)>.ustudio`
  plus a `.meta` JSON with the original path and timestamp.
- On startup, autosaves newer than their targets (or with no target) are
  offered for recovery in a dialog; discarded ones are deleted.
- Autosave never writes to the user's file.

## Edit journal (debug/testing aid, optional at runtime)

When `USTUDIO_JOURNAL=1`, every executed command's `record()` is appended as
one JSON line to `$XDG_STATE_HOME/ustudio/journal/<session>.jsonl`. The test
harness can replay a journal against a fixture project to reproduce a bug
report deterministically. This is the main reason commands are data
(doc 04).

## kdenlive import (best effort, M7)

Reader path 2: if the root has `kdenlive:docproperties`, run the *kdenlive
importer*: walk the MLT graph rather than our properties.

Supported: profile, bin producers (`kdenlive:folderid` → folder,
`kdenlive:clipname` → name), tracks (`kdenlive:audio_track` flag, names), clip
entries (in/out/position), the `kdenlive:id` mapping, markers
(`kdenlive:markers` JSON), simple effects whose MLT service is in our
catalogue (params copied through), `mix`-style dissolves (`luma`).

Dropped with a warning list shown after import: sequences other than the
main one, subtitles, groups, guides beyond markers, effects not in our
catalogue (kept as *opaque* effects: shown as "kdenlive: <service>", not
editable, still rendered since MLT knows the service, if the module is
present and Qt-free), `qtblend` compositing (replaced by `composite`),
speed changes (clip flattened at speed 1 with a warning).

Never write `.kdenlive` files.

## Settings

`GSettings` schema `com.ustudio.VideoEditor` (`data/*.gschema.xml`) for
preferences: audio backend, preview scale, real-time drop, thumbnail
interval, autosave interval, default still duration, last export preset,
recent files (`GtkRecentManager` handles the list itself). No project-level
data in GSettings.
