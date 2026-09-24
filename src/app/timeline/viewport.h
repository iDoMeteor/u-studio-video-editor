#pragma once

#include "core/model/frame_time.h"

namespace ustudio::app::timeline {

// The timeline's horizontal mapping between frames and pixels (doc 06,
// "Viewport model"): zoom as pixels per frame, scroll as the content x at
// the view's left edge. Plain C++, no GTK, so the maths is unit-tested
// (tests/app/test_viewport.cpp) and shared by the timeline, the ruler and
// the playhead overlay.
//
// Widget x = originX + frame * pxPerFrame - scrollX. originX is the width
// of whatever sits left of frame 0 in the same widget (the track-handle
// strip today).
//
// Starts in "fit" mode: the whole sequence plus 10% fills the width and
// stays that way as the sequence or the widget changes size, which is how
// the timeline behaved before zoom existed. Any explicit zoom or scroll
// leaves fit mode; fit() returns to it.
class Viewport
{
  public:
    static constexpr double kMinPxPerFrame = 0.005;
    static constexpr double kMaxPxPerFrame = 200.0;
    // Room past the end of the sequence to drop and trim into.
    static constexpr double kExtentFactor = 1.25;
    static constexpr double kFitFactor = 1.1;

    void setOriginX(double originX);
    // The width available to frames: the widget's width minus originX.
    void setVisibleWidth(double width);
    // `minimumFitFrames` keeps an empty or very short sequence from being
    // zoomed in absurdly far in fit mode (e.g. 30 seconds' worth).
    void setSequenceLength(core::FrameIndex length, core::FrameIndex minimumFitFrames);

    double originX() const
    {
        return m_originX;
    }
    double visibleWidth() const
    {
        return m_visibleWidth;
    }
    double pxPerFrame() const
    {
        return m_pxPerFrame;
    }
    double scrollX() const
    {
        return m_scrollX;
    }
    bool fitMode() const
    {
        return m_fitMode;
    }

    double xForFrame(double frame) const
    {
        return m_originX + frame * m_pxPerFrame - m_scrollX;
    }
    // Floor, never negative.
    core::FrameIndex frameForX(double x) const;
    // A pixel distance as frames (rounded toward zero), for drag deltas.
    core::FrameIndex framesForPixels(double dx) const;

    // Total scrollable content width, >= the visible width.
    double extent() const;
    double maxScrollX() const;
    // First and one-past-last frame at least partly visible.
    core::FrameIndex firstVisibleFrame() const;
    core::FrameIndex endVisibleFrame() const;

    // Zooms by `factor` keeping the frame under widget x `anchorX` fixed.
    void zoomAround(double anchorX, double factor);
    void setScrollX(double scrollX);
    void fit();
    // Scrolls by whole pages so `frame` is on screen (the playhead during
    // playback). Returns true if the scroll changed.
    bool ensureVisible(core::FrameIndex frame);

  private:
    void applyFit();
    void clampScroll();

    double m_originX = 0.0;
    double m_visibleWidth = 1.0;
    core::FrameIndex m_length = 0;
    core::FrameIndex m_minimumFitFrames = 1;
    double m_pxPerFrame = 1.0;
    double m_scrollX = 0.0;
    bool m_fitMode = true;
};

} // namespace ustudio::app::timeline
