# ADR-004: Project files are MLT XML with `ustudio:` properties

**Status:** Proposed

## Context
The model needs a durable format. Options: (a) our own JSON, rendered to
MLT XML only at export; (b) MLT XML produced by MLT's `xml` consumer; (c) MLT
XML produced by our own serialiser carrying our model in namespaced
properties. Kdenlive does (c) with `kdenlive:` properties, which is why
`.kdenlive` files render with plain `melt`.

## Decision
(c). Files are valid MLT XML that `melt` and `u-studio-render` can render
without the editor. Our reader only trusts `ustudio:*` properties and ids;
the MLT structure is regenerated on every save. A separate best-effort
importer walks the MLT graph for `.kdenlive` files.

## Consequences
- Exports are testable as "render the saved file", independent of the
  editor's live state.
- Kdenlive projects are importable with the same reader infrastructure.
- We must keep our serialiser producing graphs MLT accepts; the render tests
  cover that.
- libxml2 becomes a direct dependency of `core/` (it is already a transitive
  dependency via MLT's xml module).
