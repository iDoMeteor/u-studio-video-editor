#pragma once

#include <gtk/gtk.h>

#include <functional>

G_BEGIN_DECLS
#define US_TYPE_TIMELINE_VIEW (us_timeline_view_get_type())
G_DECLARE_FINAL_TYPE(UsTimelineView, us_timeline_view, US, TIMELINE_VIEW, GtkWidget)
G_END_DECLS

namespace ustudio::app::timeline {

using SnapshotFunc = std::function<void(GtkSnapshot *snapshot, int width, int height)>;
using ResizeFunc = std::function<void(int width, int height)>;

// ADR-008's one custom widget: draws in `snapshot` with GSK nodes (through
// `draw`, which timeline_renderer.h provides) instead of a cairo
// GtkDrawingArea, and reports its size through `resized`. The timeline,
// its ruler and the playhead overlay are each one of these; the gestures,
// drop targets and tooltips stay on the widget, owned by the window.
// Popovers parented to it are presented on every allocation.
GtkWidget *newTimelineView(SnapshotFunc draw, ResizeFunc resized = {});

} // namespace ustudio::app::timeline
