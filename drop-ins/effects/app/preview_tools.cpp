#include "app/preview_tools.h"

#include "app/shell_host.h"
#include "core/log.h"
#include "engine/frame_renderer.h"
#include "tokens.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <optional>

namespace ustudio::effects {

namespace {

namespace tokens = app::tokens;

constexpr double kHandle = 6.0; // a corner handle's half-size, widget pixels

class Tools : public PreviewTools
{
  public:
    explicit Tools(app::ShellHost &host) : m_host(host) {}

    void install()
    {
        m_host.addHints({
            {"effects.eyedropper", "Effects", "Pick a colour from the picture",
             "Then click the picture: the colour of the selected clip there, before its effects", nullptr, nullptr},
            {"effects.rect-handles", "Effects", "Edit on the picture",
             "Shows the rectangle over the picture: drag it, or its corners", nullptr, nullptr},
        });
        m_area = gtk_drawing_area_new();
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(m_area), &onDrawTrampoline, this, nullptr);
        gtk_widget_set_can_target(m_area, FALSE);
        gtk_accessible_update_property(GTK_ACCESSIBLE(m_area), GTK_ACCESSIBLE_PROPERTY_LABEL, "Effects picture tools",
                                       -1);
        GtkGesture *drag = gtk_gesture_drag_new();
        g_signal_connect(drag, "drag-begin", G_CALLBACK(&onDragBeginTrampoline), this);
        g_signal_connect(drag, "drag-update", G_CALLBACK(&onDragUpdateTrampoline), this);
        g_signal_connect(drag, "drag-end", G_CALLBACK(&onDragEndTrampoline), this);
        gtk_widget_add_controller(m_area, GTK_EVENT_CONTROLLER(drag));
        GtkEventController *keys = gtk_event_controller_key_new();
        g_signal_connect(keys, "key-pressed", G_CALLBACK(&onKeyTrampoline), this);
        gtk_widget_add_controller(m_area, keys);
        g_signal_connect(m_area, "destroy", G_CALLBACK(&onDestroyTrampoline), this);
        m_host.addPreviewOverlay(m_area);
    }

    void pickColour(std::function<void(core::Color)> done) override
    {
        m_pick = std::move(done);
        updateTargeting();
        gtk_widget_set_cursor_from_name(m_area, "crosshair");
        gtk_widget_grab_focus(m_area);
        m_host.showStatus("Click the picture to pick a colour (Esc cancels).");
    }

    void showRect(core::Rect rect, std::function<void(core::Rect, uint64_t)> changed) override
    {
        m_rect = rect;
        m_rectChanged = std::move(changed);
        updateTargeting();
        gtk_widget_queue_draw(m_area);
    }

    void hideRect() override
    {
        m_rect.reset();
        m_rectChanged = nullptr;
        updateTargeting();
        gtk_widget_queue_draw(m_area);
    }

    bool showingRect() const override
    {
        return m_rect.has_value();
    }

  private:
    enum class Grab
    {
        None,
        Body,
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight,
    };

    // Targetable only while a tool is on, so the preview's own gestures
    // (the clip transform handles, drops) work otherwise.
    void updateTargeting()
    {
        gtk_widget_set_can_target(m_area, m_pick || m_rect);
        if (!m_pick)
            gtk_widget_set_cursor(m_area, nullptr);
    }

    void cancelPick()
    {
        if (!m_pick)
            return;
        m_pick = nullptr;
        updateTargeting();
        m_host.showStatus("Colour pick cancelled.");
    }

    void onDestroy()
    {
        m_renderer.stop(); // its thread holds MLT producers
    }

    // The clip whose frame the eyedropper samples: the selected one, else
    // the topmost video clip at the playhead.
    std::optional<core::ClipId> sourceClip() const
    {
        const core::Model &model = m_host.model();
        const core::FrameIndex frame = m_host.currentFrame();
        auto covers = [&](core::ClipId id) {
            const core::Clip &clip = model.clip(id);
            return frame >= clip.position && frame < clip.position + clip.length();
        };
        for (core::ClipId id : m_host.currentSelection().clips)
            if (model.hasClip(id) && covers(id))
                return id;
        std::optional<core::ClipId> top;
        for (const core::Track &track : model.sequence().tracks) {
            if (track.kind != core::Track::Kind::Video)
                continue;
            for (core::ClipId id : track.clips)
                if (covers(id))
                    return id; // tracks are top to bottom: the first is on top
        }
        return top;
    }

    void pickAt(double x, double y)
    {
        const app::PreviewMapping mapping = m_host.previewMapping();
        const double u = (x - mapping.imageX) / mapping.imageWidth, v = (y - mapping.imageY) / mapping.imageHeight;
        if (u < 0 || u >= 1 || v < 0 || v >= 1) {
            m_host.showStatus("That's outside the picture: click on it, or press Esc.");
            return;
        }
        const core::Model &model = m_host.model();
        const std::optional<core::ClipId> id = sourceClip();
        if (!id || !model.hasAsset(model.clip(*id).asset)) {
            m_host.showStatus("No clip in the picture here to pick from.");
            cancelPick();
            return;
        }
        const core::Clip &clip = model.clip(*id);
        FrameRequest request;
        request.resource = model.asset(clip.asset).path;
        request.profile = model.sequence().profile;
        request.sourceFrame = clip.in + (m_host.currentFrame() - clip.position);
        request.clipIn = clip.in;
        request.clipOut = clip.out;
        // Enough to hit a detail, never more than the frame.
        request.width = std::max(16, static_cast<int>(std::min(640.0, mapping.frameWidth)) / 2 * 2);
        request.height = std::max(16, static_cast<int>(request.width * mapping.frameHeight / mapping.frameWidth) / 2 * 2);
        std::function<void(core::Color)> done = std::move(m_pick);
        m_pick = nullptr;
        updateTargeting();
        m_renderer.request(std::move(request), 0, ++m_generation, [this, u, v, done](const RenderedFrame &frame) {
            if (frame.rgba.empty())
                return;
            const int px = std::clamp(static_cast<int>(u * frame.width), 0, frame.width - 1);
            const int py = std::clamp(static_cast<int>(v * frame.height), 0, frame.height - 1);
            const size_t at = (static_cast<size_t>(py) * static_cast<size_t>(frame.width) + static_cast<size_t>(px)) * 4;
            const core::Color colour{frame.rgba[at], frame.rgba[at + 1], frame.rgba[at + 2], 255};
            char text[16];
            std::snprintf(text, sizeof text, "#%02x%02x%02x", colour.r, colour.g, colour.b);
            core::Log::debug(std::string("[effects] eyedropper picked ") + text);
            m_host.showStatus(std::string("Picked ") + text + ".");
            done(colour);
        });
    }

    // --- Rect handles ------------------------------------------------------

    struct Box
    {
        double x0, y0, x1, y1; // widget pixels
    };
    Box widgetBox(const app::PreviewMapping &mapping, const core::Rect &rect) const
    {
        return {mapping.widgetX(rect.x), mapping.widgetY(rect.y), mapping.widgetX(rect.x + rect.w),
                mapping.widgetY(rect.y + rect.h)};
    }

    Grab grabAt(double x, double y) const
    {
        if (!m_rect)
            return Grab::None;
        const Box box = widgetBox(m_host.previewMapping(), *m_rect);
        auto near = [&](double hx, double hy) { return std::abs(x - hx) <= kHandle * 1.5 && std::abs(y - hy) <= kHandle * 1.5; };
        // Bottom-right first: on a rect small enough for its handles to
        // overlap, growing it is what a drag there means.
        if (near(box.x1, box.y1))
            return Grab::BottomRight;
        if (near(box.x0, box.y1))
            return Grab::BottomLeft;
        if (near(box.x1, box.y0))
            return Grab::TopRight;
        if (near(box.x0, box.y0))
            return Grab::TopLeft;
        if (x >= box.x0 && x <= box.x1 && y >= box.y0 && y <= box.y1)
            return Grab::Body;
        return Grab::None;
    }

    void onDragBegin(double x, double y)
    {
        if (m_pick) {
            pickAt(x, y);
            m_grab = Grab::None;
            return;
        }
        m_grab = grabAt(x, y);
        if (m_grab == Grab::None)
            return;
        m_dragStart = *m_rect;
        m_pressX = x;
        m_pressY = y;
        ++m_gesture;
    }

    void onDragUpdate(double dx, double dy)
    {
        if (m_grab == Grab::None || !m_rect || !m_rectChanged)
            return;
        const app::PreviewMapping mapping = m_host.previewMapping();
        const double fx = dx / mapping.scale(), fy = dy / mapping.scale();
        const core::Rect &s0 = m_dragStart;
        core::Rect r = s0;
        if (m_grab == Grab::Body) {
            r.x += fx;
            r.y += fy;
        } else {
            // The dragged corner moves; the opposite one stays. Dragged past
            // it, the rect turns over rather than collapsing.
            const bool left = m_grab == Grab::TopLeft || m_grab == Grab::BottomLeft;
            const bool top = m_grab == Grab::TopLeft || m_grab == Grab::TopRight;
            const double fixedX = left ? s0.x + s0.w : s0.x, fixedY = top ? s0.y + s0.h : s0.y;
            const double movedX = (left ? s0.x : s0.x + s0.w) + fx, movedY = (top ? s0.y : s0.y + s0.h) + fy;
            r = {std::min(fixedX, movedX), std::min(fixedY, movedY), std::max(std::abs(movedX - fixedX), 1.0),
                 std::max(std::abs(movedY - fixedY), 1.0)};
        }
        r = {std::round(r.x), std::round(r.y), std::round(r.w), std::round(r.h)};
        m_rect = r;
        gtk_widget_queue_draw(m_area);
        m_rectChanged(r, m_gesture);
    }

    void draw(cairo_t *cr)
    {
        if (!m_rect)
            return;
        const Box box = widgetBox(m_host.previewMapping(), *m_rect);
        // The selection treatment: a cyan outline and square handles.
        cairo_set_source_rgba(cr, tokens::kBrandCyan.r, tokens::kBrandCyan.g, tokens::kBrandCyan.b, 1.0);
        cairo_set_line_width(cr, 1.5);
        cairo_rectangle(cr, box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0);
        cairo_stroke(cr);
        for (double hx : {box.x0, box.x1})
            for (double hy : {box.y0, box.y1}) {
                cairo_rectangle(cr, hx - kHandle / 2, hy - kHandle / 2, kHandle, kHandle);
                cairo_fill(cr);
            }
    }

    // --- GTK signal trampolines ---------------------------------------------

    static void onDrawTrampoline(GtkDrawingArea *, cairo_t *cr, int, int, gpointer self)
    {
        static_cast<Tools *>(self)->draw(cr);
    }
    static void onDragBeginTrampoline(GtkGestureDrag *, double x, double y, gpointer self)
    {
        static_cast<Tools *>(self)->onDragBegin(x, y);
    }
    static void onDragUpdateTrampoline(GtkGestureDrag *, double dx, double dy, gpointer self)
    {
        static_cast<Tools *>(self)->onDragUpdate(dx, dy);
    }
    static void onDragEndTrampoline(GtkGestureDrag *, double, double, gpointer self)
    {
        static_cast<Tools *>(self)->m_grab = Grab::None;
    }
    static gboolean onKeyTrampoline(GtkEventControllerKey *, guint keyval, guint, GdkModifierType, gpointer self)
    {
        if (keyval != GDK_KEY_Escape)
            return FALSE;
        static_cast<Tools *>(self)->cancelPick();
        return TRUE;
    }
    static void onDestroyTrampoline(GtkWidget *, gpointer self)
    {
        static_cast<Tools *>(self)->onDestroy();
    }

    app::ShellHost &m_host;
    FrameRenderer m_renderer{2};
    GtkWidget *m_area = nullptr;
    std::function<void(core::Color)> m_pick;
    std::optional<core::Rect> m_rect;
    std::function<void(core::Rect, uint64_t)> m_rectChanged;
    core::Rect m_dragStart{};
    Grab m_grab = Grab::None;
    double m_pressX = 0, m_pressY = 0;
    uint64_t m_gesture = 0, m_generation = 0;
};

} // namespace

PreviewTools &previewTools(app::ShellHost &host)
{
    // For the window's life: its widgets and signal handlers point here.
    static std::map<app::ShellHost *, std::unique_ptr<Tools>> tools;
    auto [it, made] = tools.try_emplace(&host, nullptr);
    if (made) {
        it->second = std::make_unique<Tools>(host);
        it->second->install();
    }
    return *it->second;
}

} // namespace ustudio::effects
