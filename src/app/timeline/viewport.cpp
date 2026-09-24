#include "viewport.h"

#include <algorithm>
#include <cmath>

namespace ustudio::app::timeline {

void Viewport::setOriginX(double originX)
{
    m_originX = originX;
}

void Viewport::setVisibleWidth(double width)
{
    m_visibleWidth = std::max(width, 1.0);
    if (m_fitMode)
        applyFit();
    clampScroll();
}

void Viewport::setSequenceLength(core::FrameIndex length, core::FrameIndex minimumFitFrames)
{
    m_length = std::max<core::FrameIndex>(length, 0);
    m_minimumFitFrames = std::max<core::FrameIndex>(minimumFitFrames, 1);
    if (m_fitMode)
        applyFit();
    clampScroll();
}

core::FrameIndex Viewport::frameForX(double x) const
{
    double frame = std::floor((x - m_originX + m_scrollX) / m_pxPerFrame);
    return std::max<core::FrameIndex>(0, static_cast<core::FrameIndex>(frame));
}

core::FrameIndex Viewport::framesForPixels(double dx) const
{
    return static_cast<core::FrameIndex>(dx / m_pxPerFrame);
}

double Viewport::extent() const
{
    return std::max(static_cast<double>(m_length) * kExtentFactor * m_pxPerFrame, m_visibleWidth);
}

double Viewport::maxScrollX() const
{
    return std::max(extent() - m_visibleWidth, 0.0);
}

core::FrameIndex Viewport::firstVisibleFrame() const
{
    return std::max<core::FrameIndex>(0, static_cast<core::FrameIndex>(std::floor(m_scrollX / m_pxPerFrame)));
}

core::FrameIndex Viewport::endVisibleFrame() const
{
    return static_cast<core::FrameIndex>(std::ceil((m_scrollX + m_visibleWidth) / m_pxPerFrame)) + 1;
}

void Viewport::zoomAround(double anchorX, double factor)
{
    if (factor <= 0.0)
        return;
    double anchorFrame = (anchorX - m_originX + m_scrollX) / m_pxPerFrame;
    m_pxPerFrame = std::clamp(m_pxPerFrame * factor, kMinPxPerFrame, kMaxPxPerFrame);
    m_fitMode = false;
    m_scrollX = anchorFrame * m_pxPerFrame - (anchorX - m_originX);
    clampScroll();
}

void Viewport::setScrollX(double scrollX)
{
    m_scrollX = scrollX;
    clampScroll();
    // Scrolling away from the fitted view is a choice too; resizing the
    // window afterwards must not snap it back.
    if (m_scrollX > 0.0)
        m_fitMode = false;
}

void Viewport::fit()
{
    m_fitMode = true;
    applyFit();
    m_scrollX = 0.0;
}

bool Viewport::ensureVisible(core::FrameIndex frame)
{
    double x = static_cast<double>(frame) * m_pxPerFrame;
    double before = m_scrollX;
    if (x < m_scrollX || x >= m_scrollX + m_visibleWidth) {
        // Page so the frame lands a little in from the left edge, the way
        // editors follow the playhead, rather than scrolling every frame.
        m_scrollX = x - m_visibleWidth * 0.05;
        clampScroll();
    }
    return m_scrollX != before;
}

void Viewport::applyFit()
{
    double frames = static_cast<double>(std::max(m_length, m_minimumFitFrames)) * kFitFactor;
    m_pxPerFrame = std::clamp(m_visibleWidth / frames, kMinPxPerFrame, kMaxPxPerFrame);
    m_scrollX = 0.0;
}

void Viewport::clampScroll()
{
    m_scrollX = std::clamp(m_scrollX, 0.0, maxScrollX());
}

} // namespace ustudio::app::timeline
