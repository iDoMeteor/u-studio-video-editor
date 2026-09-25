#pragma once

#include "core/model/frame_time.h"
#include "row_layout.h"
#include "timeline_controller.h"
#include "viewport.h"

#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <vector>

namespace ustudio::core {
class Model;
struct Clip;
struct Track;
} // namespace ustudio::core

namespace ustudio::app::timeline {

// ADR-013 / doc 15 IP5's timeline hook: a drop-in paints over the tracks
// after the clips (adjustment regions, curve overlays, the FX lane's
// markings), may give a track a lane of its own under the clips, and may
// claim a click. Registered with the window (ShellHost::
// addTimelineOverlay()); painting runs on every timeline snapshot and must
// be as cheap as the clips' own drawing. Main thread only.
class TimelineOverlayProvider
{
  public:
    virtual ~TimelineOverlayProvider() = default;
    virtual void paintOverlay(GtkSnapshot *snapshot, const core::Model &model, const Viewport &viewport,
                              const RowLayout &layout, double width, double height) const = 0;
    // Extra height under `track`'s clips (RowLayout::lanes); lanes from
    // several providers add up. Asked on every refresh, so it may change
    // with the model (a lane appears when a track gains a curve).
    virtual double laneHeight(const core::Model &, const core::Track &) const
    {
        return 0.0;
    }
    // A primary-button press at (x, y) in timeline coordinates, before the
    // timeline's own handling; true claims it (the timeline does nothing
    // more with it).
    virtual bool pressed(const core::Model &, const Viewport &, const RowLayout &, double /*x*/, double /*y*/,
                         int /*nPress*/)
    {
        return false;
    }
};

// What one timeline snapshot draws from. Everything here is read-only.
struct TimelineScene
{
    const core::Model &model;
    const Viewport &viewport;
    const TimelineController &controller;
    RowLayout layout;
    double handleWidth = 22.0;
    int activeRow = -1;
    int nameEditRow = -1; // the row whose name strip is being edited: don't draw its name
    // Waveform peaks (0..1) for a clip's source range, or null while they
    // are still being computed.
    std::function<const std::vector<float> *(const core::Clip &clip)> waveformFor;
    // A thumbnail of the clip's source at `sourceFrame`, or null while it is
    // being made (the caller redraws when it arrives).
    std::function<GdkTexture *(const core::Clip &clip, core::FrameIndex sourceFrame)> thumbnailFor;
    std::vector<const TimelineOverlayProvider *> overlays;
    // For labels. Reused for every label in a snapshot (text and width
    // set per use); the caller owns them.
    PangoLayout *labelLayout = nullptr;
};

// doc 06 "Drawing (snapshot)": tracks, clips, waveforms, dissolves, the
// drag preview, markers, the marquee and snap line, then the overlays.
// GSK nodes throughout; cairo only for waveforms and the dissolve hatch,
// which have no node type. Clips shorter than 4 px draw as a tick, labels
// only on clips at least 40 px wide, waveforms from 24 px. Video clips
// carry thumbnails edge to edge, with the waveform (if any) in the bottom
// 40% over them.
void snapshotTimeline(GtkSnapshot *snapshot, const TimelineScene &scene, double width, double height);

// The playhead line, on its own overlay widget so playback redraws only it.
void snapshotPlayhead(GtkSnapshot *snapshot, const Viewport &viewport, core::FrameIndex frame, double handleWidth,
                      double width, double height);

// The ruler: time ticks and labels at a spacing that stays readable, and
// the markers as flags.
struct RulerScene
{
    const core::Model &model;
    const Viewport &viewport;
    double handleWidth = 22.0;
    double fps = 25.0;
    std::function<std::string(core::FrameIndex)> formatTimecode;
    PangoLayout *labelLayout = nullptr; // monospace
};
void snapshotRuler(GtkSnapshot *snapshot, const RulerScene &scene, double width, double height);

} // namespace ustudio::app::timeline
