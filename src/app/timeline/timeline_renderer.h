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
} // namespace ustudio::core

namespace ustudio::app::timeline {

// ADR-013 / doc 15 IP5's timeline hook: a drop-in paints over the tracks
// after the clips (adjustment regions, curve overlays, the FX lane's
// markings). Registered with the window, called on every timeline
// snapshot; it must be as cheap as the clips' own drawing. Extra lane
// height below a track arrives with per-track row heights (doc 06).
class TimelineOverlayProvider
{
  public:
    virtual ~TimelineOverlayProvider() = default;
    virtual void paintOverlay(GtkSnapshot *snapshot, const core::Model &model, const Viewport &viewport,
                              const RowLayout &layout, double width, double height) const = 0;
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
