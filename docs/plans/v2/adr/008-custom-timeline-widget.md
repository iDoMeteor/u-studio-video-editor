# ADR-008: One custom GtkWidget for the timeline

**Status:** Proposed

> REVIEW: Claude (2026-09-24): built as `app/timeline/us_timeline_view.*`
> (widget), `timeline_renderer.*` (snapshot) and `timeline_controller.*`.
> Two deviations: it doesn't implement GtkScrollable (the Viewport keeps the
> horizontal scroll and drives a separate scrollbar; tracks scroll in a
> plain GtkScrolledWindow), and the playhead overlay and ruler are two more
> UsTimelineViews rather than ordinary widgets. Waveforms use a GskPath
> fill where GTK >= 4.14, cairo below that. Budget met: see doc 12, M3.

## Context
Two ways to draw a timeline in GTK4: a widget per clip inside a custom
layout manager, or one widget that paints everything in `snapshot`. Per-clip
widgets give free accessibility and CSS but scale poorly (thousands of
widgets, each with allocation and event machinery) and make continuous
zoom/scroll and overlays awkward. v1 already draws with a `GtkDrawingArea`.

## Decision
`UsTimelineView` is one `GtkWidget` subclass implementing `GtkScrollable`,
drawing tracks, clips, thumbnails, waveforms, and overlays in `snapshot`
with GSK nodes (cairo only where a node type is missing). The playhead is a
separate overlay child so playback doesn't invalidate the whole view.
Interaction is a plain-C++ state machine (`TimelineController`) fed by GTK
event controllers, testable without GTK. Track headers and the ruler are
ordinary widgets sharing adjustments.

## Consequences
- Performance budget (4 ms for 10×500 clips) is achievable.
- Accessibility must be provided manually via `gtk_accessible_*`; basic
  roles in v2.0, full AT-SPI later.
- Colours for drawn items come from a generated token header, not CSS.
