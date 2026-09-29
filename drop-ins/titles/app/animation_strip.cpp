#include "animation_strip.h"
#include "signal_guard.h"

#include "tokens.h"

#include "core/animation.h"
#include "core/evaluate.h"

#include <algorithm>
#include <cmath>

// Registered by hand, like the canvas (G_DEFINE_TYPE's C casts).
struct UsAnimationStrip
{
    GtkWidget parent;
    ustudio::titles::app::AnimationStrip *owner;
};
struct UsAnimationStripClass
{
    GtkWidgetClass parent;
};

namespace {

namespace tokens = ustudio::app::tokens;

constexpr double kLabel = 150.0; // the layer names' column
constexpr double kRuler = 30.0;  // the zones and the time
constexpr double kRow = 24.0;
constexpr double kMargin = 14.0; // right of the timeline
constexpr double kGrab = 6.0;    // pixels either side of a divider

void stripSnapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    auto *self = reinterpret_cast<UsAnimationStrip *>(widget);
    if (self->owner)
        self->owner->snapshot(snapshot);
}

void stripClassInit(gpointer klass, gpointer)
{
    GtkWidgetClass *widgetClass = GTK_WIDGET_CLASS(klass);
    widgetClass->snapshot = stripSnapshot;
    gtk_widget_class_set_css_name(widgetClass, "usanimationstrip");
    gtk_widget_class_set_accessible_role(widgetClass, GTK_ACCESSIBLE_ROLE_GROUP);
}

void stripInit(GTypeInstance *instance, gpointer)
{
    auto *self = reinterpret_cast<UsAnimationStrip *>(instance);
    self->owner = nullptr;
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
}

GType stripType()
{
    static const GType type = g_type_register_static_simple(GTK_TYPE_WIDGET, g_intern_static_string("UsAnimationStrip"),
                                                            sizeof(UsAnimationStripClass), stripClassInit,
                                                            sizeof(UsAnimationStrip), stripInit, G_TYPE_FLAG_FINAL);
    return type;
}

void colour(cairo_t *cr, tokens::Rgb c, double alpha = 1.0)
{
    cairo_set_source_rgba(cr, c.r, c.g, c.b, alpha);
}

void label(cairo_t *cr, GtkWidget *widget, const std::string &text, double x, double y, double maxWidth, tokens::Rgb c,
           double alpha = 1.0, bool small = true)
{
    PangoLayout *layout = gtk_widget_create_pango_layout(widget, text.c_str());
    PangoFontDescription *font = pango_font_description_from_string(small ? "Sans 8" : "Sans 9");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    pango_layout_set_width(layout, static_cast<int>(std::max(1.0, maxWidth) * PANGO_SCALE));
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    int w = 0, h = 0;
    pango_layout_get_pixel_size(layout, &w, &h);
    colour(cr, c, alpha);
    cairo_move_to(cr, x, y - h / 2.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

std::string layerName(const ustudio::titles::Layer &layer)
{
    if (layer.kind == ustudio::titles::LayerKind::Text && !layer.text.empty())
        return layer.text.substr(0, layer.text.find('\n'));
    return layer.id;
}

} // namespace

namespace ustudio::titles::app {

AnimationStrip::AnimationStrip(Callbacks callbacks) : m_callbacks(std::move(callbacks))
{
    m_widget = GTK_WIDGET(g_object_new(stripType(), nullptr));
    g_object_ref_sink(m_widget);
    reinterpret_cast<UsAnimationStrip *>(m_widget)->owner = this;
    gtk_accessible_update_property(GTK_ACCESSIBLE(m_widget), GTK_ACCESSIBLE_PROPERTY_LABEL, "Animation strip", -1);
    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(onDragBegin), this);
    g_signal_connect(drag, "drag-update", G_CALLBACK(onDragUpdate), this);
    g_signal_connect(drag, "drag-end", G_CALLBACK(onDragEnd), this);
    gtk_widget_add_controller(m_widget, GTK_EVENT_CONTROLLER(drag));
}

AnimationStrip::~AnimationStrip()
{
    disconnectFromTree(m_widget, this); // its drag: dispose ends it
    reinterpret_cast<UsAnimationStrip *>(m_widget)->owner = nullptr;
    g_object_unref(m_widget);
}

void AnimationStrip::show(const TitleDocument &doc, const std::optional<std::string> &selection, double titleFrame)
{
    m_doc = doc;
    m_selection = selection;
    m_frame = titleFrame;
    gtk_widget_set_size_request(
        m_widget, -1,
        static_cast<int>(kRuler + kRow * static_cast<double>(std::max<size_t>(1, doc.layers.size())) + 6));
    gtk_widget_queue_draw(m_widget);
}

double AnimationStrip::xOf(double frame) const
{
    const double total = static_cast<double>(std::max<int64_t>(1, m_doc.timing.length()));
    const double scale = m_drag != Drag::None && m_drag != Drag::Playhead
                             ? m_dragScale
                             : (gtk_widget_get_width(m_widget) - kLabel - kMargin) / total;
    return kLabel + frame * scale;
}

double AnimationStrip::frameAt(double x) const
{
    const double total = static_cast<double>(std::max<int64_t>(1, m_doc.timing.length()));
    const double scale = m_drag != Drag::None && m_drag != Drag::Playhead
                             ? m_dragScale
                             : (gtk_widget_get_width(m_widget) - kLabel - kMargin) / total;
    return (x - kLabel) / std::max(scale, 1e-6);
}

void AnimationStrip::snapshot(GtkSnapshot *snapshot)
{
    const double width = gtk_widget_get_width(m_widget), height = gtk_widget_get_height(m_widget);
    const graphene_rect_t all = GRAPHENE_RECT_INIT(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    cairo_t *cr = gtk_snapshot_append_cairo(snapshot, &all);
    colour(cr, tokens::kInk850);
    cairo_paint(cr);

    const Timing &t = m_doc.timing;
    const auto intro = static_cast<double>(t.intro), holdEnd = static_cast<double>(t.intro + t.hold),
               end = static_cast<double>(t.length());
    // The zones on the ruler.
    const struct
    {
        double from, to;
        tokens::Rgb c;
        double alpha;
        std::string name;
    } zones[] = {
        {0, intro, tokens::kBrandMagenta, 0.28, "Intro " + std::to_string(t.intro)},
        {intro, holdEnd, tokens::kInk600, 1.0, "Hold " + std::to_string(t.hold)},
        {holdEnd, end, tokens::kBrandCyan, 0.25, "Outro " + std::to_string(t.outro)},
    };
    for (const auto &zone : zones) {
        if (zone.to <= zone.from)
            continue;
        colour(cr, zone.c, zone.alpha);
        cairo_rectangle(cr, xOf(zone.from), 2, xOf(zone.to) - xOf(zone.from), kRuler - 6);
        cairo_fill(cr);
        label(cr, m_widget, zone.name, xOf(zone.from) + 4, kRuler / 2 - 1, xOf(zone.to) - xOf(zone.from) - 8,
              tokens::kWindowFgColor, 0.9);
    }
    // A tick a second.
    const double fps = m_doc.fpsDen > 0 ? static_cast<double>(m_doc.fpsNum) / m_doc.fpsDen : 30.0;
    colour(cr, tokens::kInk500);
    cairo_set_line_width(cr, 1);
    for (double s = 0; s <= end + 0.5; s += fps) {
        cairo_move_to(cr, std::round(xOf(s)) + 0.5, kRuler - 4);
        cairo_line_to(cr, std::round(xOf(s)) + 0.5, kRuler);
    }
    cairo_stroke(cr);

    // A row per layer, topmost first.
    for (size_t row = 0; row < m_doc.layers.size(); ++row) {
        const Layer &layer = m_doc.layers[m_doc.layers.size() - 1 - row];
        const double top = kRuler + kRow * static_cast<double>(row), middle = top + kRow / 2;
        const bool selected = m_selection && *m_selection == layer.id;
        colour(cr, selected ? tokens::kInk600 : (row % 2 ? tokens::kInk850 : tokens::kInk800));
        cairo_rectangle(cr, 0, top, width, kRow);
        cairo_fill(cr);
        label(cr, m_widget, layerName(layer), 10, middle, kLabel - 16, tokens::kWindowFgColor,
              layer.visible ? 0.9 : 0.45, false);
        // Behaviour chips in their windows.
        for (const Behavior &b : layer.behaviors) {
            const BehaviorInfo *info = behaviorInfo(b.id);
            const auto d = static_cast<double>(b.duration);
            double from = 0, to = std::min(d, end);
            tokens::Rgb c = tokens::kBrandMagenta;
            if (b.slot == BehaviorSlot::Out) {
                from = std::max(holdEnd, end - d);
                to = end;
                c = tokens::kBrandCyan;
            } else if (b.slot == BehaviorSlot::Loop) {
                from = intro;
                to = holdEnd;
                c = tokens::kBrandViolet;
            }
            const double x0 = xOf(from), x1 = std::max(xOf(to), x0 + 6);
            colour(cr, c, 0.55);
            cairo_rectangle(cr, x0, top + 4, x1 - x0, kRow - 8);
            cairo_fill(cr);
            label(cr, m_widget, info ? info->label : b.id, x0 + 4, middle, x1 - x0 - 6, tokens::kInk900);
        }
        // The layer's own keyframes.
        colour(cr, tokens::kWindowFgColor, 0.95);
        for (const PropertyTrack &track : layer.animation)
            for (const TitleKey &key : track.keys) {
                const double x = xOf(keyPosition(t, key));
                cairo_move_to(cr, x, middle - 5);
                cairo_line_to(cr, x + 5, middle);
                cairo_line_to(cr, x, middle + 5);
                cairo_line_to(cr, x - 5, middle);
                cairo_close_path(cr);
            }
        cairo_fill(cr);
    }

    // The zone dividers: grips on the ruler, faint lines through the rows.
    for (double at : {intro, holdEnd, end}) {
        const double x = std::round(xOf(at)) + 0.5;
        colour(cr, tokens::kWindowFgColor, 0.25);
        cairo_move_to(cr, x, kRuler);
        cairo_line_to(cr, x, height);
        cairo_stroke(cr);
        colour(cr, tokens::kWindowFgColor, 0.85);
        cairo_rectangle(cr, x - 2, 3, 4, kRuler - 8);
        cairo_fill(cr);
    }
    // The playhead.
    const double px = std::round(xOf(m_frame)) + 0.5;
    colour(cr, tokens::kBrandMagenta);
    cairo_set_line_width(cr, 1.5);
    cairo_move_to(cr, px, 0);
    cairo_line_to(cr, px, height);
    cairo_stroke(cr);
    cairo_destroy(cr);
}

void AnimationStrip::press(double x, double y)
{
    m_pressX = x;
    const Timing &t = m_doc.timing;
    m_drag = Drag::Playhead;
    if (y < kRuler) {
        const double grips[] = {static_cast<double>(t.intro), static_cast<double>(t.intro + t.hold),
                                static_cast<double>(t.length())};
        const Drag kinds[] = {Drag::IntroEnd, Drag::HoldEnd, Drag::OutroEnd};
        for (size_t i = 0; i < 3; ++i)
            if (std::abs(x - xOf(grips[i])) <= kGrab) {
                // The scale stays put while the title's length changes.
                const double total = static_cast<double>(std::max<int64_t>(1, t.length()));
                m_dragScale = (gtk_widget_get_width(m_widget) - kLabel - kMargin) / total;
                m_drag = kinds[i];
                m_dragTiming = t;
                return;
            }
    } else {
        const auto row = static_cast<size_t>((y - kRuler) / kRow);
        if (row < m_doc.layers.size() && m_callbacks.selected)
            m_callbacks.selected(m_doc.layers[m_doc.layers.size() - 1 - row].id);
    }
    if (x >= kLabel)
        drag(x, false);
}

void AnimationStrip::drag(double x, bool final)
{
    const double frame = frameAt(x);
    if (m_drag == Drag::Playhead) {
        const double clamped = std::clamp(std::round(frame), 0.0, static_cast<double>(m_doc.timing.length()));
        if (m_callbacks.scrubbed)
            m_callbacks.scrubbed(clamped);
        return;
    }
    Timing t = m_dragTiming;
    const auto at = static_cast<int64_t>(std::lround(std::max(0.0, frame)));
    if (m_drag == Drag::IntroEnd) {
        const int64_t boundary = t.intro + t.hold;
        t.intro = std::clamp<int64_t>(at, 0, boundary);
        t.hold = boundary - t.intro;
    } else if (m_drag == Drag::HoldEnd) {
        t.hold = std::max<int64_t>(0, at - t.intro);
    } else if (m_drag == Drag::OutroEnd) {
        t.outro = std::max<int64_t>(0, at - t.intro - t.hold);
    }
    if (m_callbacks.retimed)
        m_callbacks.retimed(t, final);
}

// --- GTK trampolines ---------------------------------------------------------

void AnimationStrip::onDragBegin(GtkGestureDrag *, double x, double y, gpointer self)
{
    static_cast<AnimationStrip *>(self)->press(x, y);
}

void AnimationStrip::onDragUpdate(GtkGestureDrag *, double dx, double, gpointer self)
{
    auto *strip = static_cast<AnimationStrip *>(self);
    strip->drag(strip->m_pressX + dx, false);
}

void AnimationStrip::onDragEnd(GtkGestureDrag *, double dx, double, gpointer self)
{
    auto *strip = static_cast<AnimationStrip *>(self);
    if (strip->m_drag != Drag::Playhead && strip->m_drag != Drag::None)
        strip->drag(strip->m_pressX + dx, true);
    strip->m_drag = Drag::None;
    gtk_widget_queue_draw(strip->m_widget);
}

} // namespace ustudio::titles::app
