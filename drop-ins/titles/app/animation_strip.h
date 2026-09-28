#pragma once

// The designer's animation strip (doc 16, "Animation strip along the
// bottom"): the title's three zones on a ruler with draggable dividers, a
// row per layer (topmost first) with its behaviour chips and keyframe
// diamonds, and the playhead. Click or drag on it to scrub; drag a divider
// to change the intro, the hold or the outro; click a row to select its
// layer. Drawn in C++ like the canvas; its colours are the design tokens.

#include "core/title_document.h"

#include <gtk/gtk.h>

#include <functional>
#include <optional>
#include <string>

namespace ustudio::titles::app {

class AnimationStrip
{
  public:
    struct Callbacks
    {
        std::function<void(double titleFrame)> scrubbed;
        // New timing while a divider drags; `final` on release.
        std::function<void(const Timing &timing, bool final)> retimed;
        std::function<void(const std::string &id)> selected;
    };

    explicit AnimationStrip(Callbacks callbacks);
    ~AnimationStrip();
    AnimationStrip(const AnimationStrip &) = delete;
    AnimationStrip &operator=(const AnimationStrip &) = delete;

    GtkWidget *widget() const
    {
        return m_widget;
    }

    void show(const TitleDocument &doc, const std::optional<std::string> &selection, double titleFrame);

    // Called by the widget class (animation_strip.cpp).
    void snapshot(GtkSnapshot *snapshot);

  private:
    enum class Drag
    {
        None,
        Playhead,
        IntroEnd, // the intro/hold divider
        HoldEnd,  // the hold/outro divider
        OutroEnd, // the end
    };

    double xOf(double frame) const;
    double frameAt(double x) const;
    void press(double x, double y);
    void drag(double x, bool final);

    Callbacks m_callbacks;
    GtkWidget *m_widget = nullptr;
    TitleDocument m_doc;
    std::optional<std::string> m_selection;
    double m_frame = 0.0;
    Drag m_drag = Drag::None;
    double m_pressX = 0.0;
    double m_dragScale = 0.0; // pixels a frame, fixed while a divider drags
    Timing m_dragTiming;

    // --- GTK trampolines -------------------------------------------------
    static void onDragBegin(GtkGestureDrag *, double x, double y, gpointer self);
    static void onDragUpdate(GtkGestureDrag *, double dx, double dy, gpointer self);
    static void onDragEnd(GtkGestureDrag *, double dx, double dy, gpointer self);
};

} // namespace ustudio::titles::app
