# ADR-012: Titles are rendered by our own MLT module with Pango and Cairo, authored in a separate app

**Status:** Proposed (2026-09-23)

## Context
The owner wants an easy, robust text design and animation tool, separate
from the editor. MLT's rich title producers (`kdenlivetitle`, `qtext`,
`glaxnimate`) are Qt and excluded by ADR-007. The Qt-free services (`text`,
`dynamictext`, `pango`, `avfilter.drawtext`) cannot do per-character
animation or layered styling. Pango, PangoCairo, Cairo and fontconfig are
mature and already loaded via GTK4.

## Decision
- A new library, `titlerender`, draws a `.ustitle` document at a given time
  using Pango and Cairo. It links Pango, PangoCairo, Cairo and fontconfig,
  and nothing from GTK or MLT.
- A new MLT module, `libmltustudio.so`, exposes it as the producer
  `ustudio_title`. It is built in-tree, installed under our own libdir,
  and linked into the curated module directory by `FactoryPolicy`, so the
  editor and `u-studio-render` both load it.
- A separate GTK application, `u-studio-titles`, authors `.ustitle` files
  using its own `core/` and `render/` libraries directly, with no MLT.
- The document format is versioned XML via libxml2, like project files.

## Consequences
- Preview, export and the titles app share one renderer, so titles are
  identical everywhere.
- Stock `melt` cannot play `ustudio_title` without our module. A "Bake
  title" command renders a title to a video file with alpha for
  portability. This is a deliberate, narrow exception to ADR-004's "`melt`
  plays the project" property.
- Three new build targets (`render`, `mltmodule`, `app`), all inside the
  `drop-ins/titles/` folder (ADR-013), with their own layer rules in doc 02.
- Pango's threading rules apply inside an MLT producer: each producer owns
  its own font map (verified in T0, doc 16).
