#include "canvas.h"

#include "tokens.h"

#include <algorithm>
#include <cmath>
#include <numbers>

// The widget: a plain GtkWidget subclass whose snapshot the C++ canvas
// draws (as the editor's timeline does, ADR-008). Registered by hand, like
// UsTimelineView: G_DEFINE_TYPE's expansion is full of C casts
// (-Wold-style-cast).
struct UsTitleCanvasWidget
{
    GtkWidget parent;
    ustudio::titles::app::TitleCanvas *owner;
};
struct UsTitleCanvasWidgetClass
{
    GtkWidgetClass parent;
};

namespace {

void canvasSnapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    auto *self = reinterpret_cast<UsTitleCanvasWidget *>(widget);
    if (self->owner)
        self->owner->snapshot(snapshot);
}

void canvasMeasure(GtkWidget *, GtkOrientation orientation, int, int *minimum, int *natural, int *, int *)
{
    *minimum = orientation == GTK_ORIENTATION_HORIZONTAL ? 320 : 180;
    *natural = orientation == GTK_ORIENTATION_HORIZONTAL ? 960 : 540;
}

void canvasClassInit(gpointer klass, gpointer)
{
    GtkWidgetClass *widgetClass = GTK_WIDGET_CLASS(klass);
    widgetClass->snapshot = canvasSnapshot;
    widgetClass->measure = canvasMeasure;
    gtk_widget_class_set_css_name(widgetClass, "ustitlecanvas");
}

void canvasInit(GTypeInstance *instance, gpointer)
{
    auto *self = reinterpret_cast<UsTitleCanvasWidget *>(instance);
    self->owner = nullptr;
    GtkWidget *widget = GTK_WIDGET(self);
    gtk_widget_set_focusable(widget, TRUE);
    gtk_widget_set_hexpand(widget, TRUE);
    gtk_widget_set_vexpand(widget, TRUE);
}

GType canvasWidgetType()
{
    static const GType type = g_type_register_static_simple(GTK_TYPE_WIDGET, g_intern_static_string("UsTitleCanvas"),
                                                            sizeof(UsTitleCanvasWidgetClass), canvasClassInit,
                                                            sizeof(UsTitleCanvasWidget), canvasInit, G_TYPE_FLAG_FINAL);
    return type;
}

} // namespace

namespace ustudio::titles::app {

namespace {

namespace tokens = ustudio::app::tokens;

constexpr double kMargin = 24.0;       // widget pixels around the canvas
constexpr double kHandleSize = 9.0;    // widget pixels
constexpr double kSnapPixels = 6.0;    // snapping reach, in widget pixels
constexpr double kCheckerSize = 12.0;  // widget pixels
constexpr double kDragThreshold = 3.0; // widget pixels before a press becomes a drag

void setColour(cairo_t *cr, tokens::Rgb colour, double alpha = 1.0)
{
    cairo_set_source_rgba(cr, colour.r, colour.g, colour.b, alpha);
}

// The frame to show while designing: the start of the hold, where the
// intro has finished and nothing has started leaving (T3 adds scrubbing).
double designFrame(const TitleDocument &doc)
{
    return static_cast<double>(doc.timing.intro);
}

} // namespace

TitleCanvas::TitleCanvas(Callbacks callbacks)
    : m_callbacks(std::move(callbacks)),
      m_worker([this](RenderResult result, uint64_t generation) { onRendered(std::move(result), generation); })
{
    m_widget = GTK_WIDGET(g_object_new(canvasWidgetType(), nullptr));
    g_object_ref_sink(m_widget);
    reinterpret_cast<UsTitleCanvasWidget *>(m_widget)->owner = this;
    gtk_widget_set_name(m_widget, "title-canvas");

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(onPressed), this);
    gtk_widget_add_controller(m_widget, GTK_EVENT_CONTROLLER(click));

    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(onDragBegin), this);
    g_signal_connect(drag, "drag-update", G_CALLBACK(onDragUpdate), this);
    g_signal_connect(drag, "drag-end", G_CALLBACK(onDragEnd), this);
    gtk_widget_add_controller(m_widget, GTK_EVENT_CONTROLLER(drag));

    // The canvas's own keys: on the canvas only, never app accelerators.
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(onKeyPressed), this);
    gtk_widget_add_controller(m_widget, keys);
}

TitleCanvas::~TitleCanvas()
{
    reinterpret_cast<UsTitleCanvasWidget *>(m_widget)->owner = nullptr;
    g_clear_object(&m_frame);
    g_clear_object(&m_backdropImage);
    g_object_unref(m_widget);
}

void TitleCanvas::setDocument(const TitleDocument &doc)
{
    m_doc = doc;
    m_geometry = measureLayers(m_doc, designFrame(m_doc), {});
    if (m_selection && !geometryOf(*m_selection))
        setSelection(std::nullopt);
    requestRender();
    gtk_widget_queue_draw(m_widget);
}

void TitleCanvas::setSelection(std::optional<std::string> id)
{
    if (id == m_selection)
        return;
    m_selection = std::move(id);
    gtk_widget_queue_draw(m_widget);
    if (m_callbacks.selected)
        m_callbacks.selected(m_selection);
}

void TitleCanvas::setBackdrop(Backdrop backdrop)
{
    m_backdrop = backdrop;
    gtk_widget_queue_draw(m_widget);
}

void TitleCanvas::setBackdropColour(const GdkRGBA &colour)
{
    m_backdropColour = colour;
    m_backdrop = Backdrop::Colour;
    gtk_widget_queue_draw(m_widget);
}

void TitleCanvas::setBackdropImage(GdkTexture *texture)
{
    g_clear_object(&m_backdropImage);
    if (texture)
        m_backdropImage = GDK_TEXTURE(g_object_ref(texture));
    m_backdrop = texture ? Backdrop::Image : Backdrop::Checkerboard;
    gtk_widget_queue_draw(m_widget);
}

void TitleCanvas::setGuidesVisible(bool visible)
{
    m_guides = visible;
    gtk_widget_queue_draw(m_widget);
}

TitleCanvas::Mapping TitleCanvas::mapping() const
{
    const double width = gtk_widget_get_width(m_widget), height = gtk_widget_get_height(m_widget);
    const double scale =
        std::max(0.01, std::min((width - 2 * kMargin) / m_doc.width, (height - 2 * kMargin) / m_doc.height));
    return {(width - m_doc.width * scale) / 2, (height - m_doc.height * scale) / 2, scale};
}

void TitleCanvas::requestRender()
{
    const Mapping map = mapping();
    const int factor = gtk_widget_get_scale_factor(m_widget);
    // At the canvas's size on screen (device pixels): sharp, and cheap.
    const int width = std::max(1, static_cast<int>(std::lround(m_doc.width * map.scale * factor)));
    const int height = std::max(1, static_cast<int>(std::lround(m_doc.height * map.scale * factor)));
    m_requestedWidth = width;
    m_requestedHeight = height;
    m_wantedGeneration = m_worker.request(m_doc, designFrame(m_doc), {}, width, height);
}

void TitleCanvas::onRendered(RenderResult result, uint64_t generation)
{
    if (generation < m_frameGeneration)
        return;
    m_frameGeneration = generation;
    const RenderedFrame &frame = result.frame;
    if (frame.width <= 0 || frame.height <= 0)
        return;
    GBytes *bytes = g_bytes_new(frame.pixels.data(), frame.pixels.size() * sizeof(uint32_t));
    // Cairo's ARGB32 is GDK's default memory format (premultiplied, native
    // endian), so the renderer's buffer goes up as is.
    g_clear_object(&m_frame);
    m_frame = gdk_memory_texture_new(frame.width, frame.height, GDK_MEMORY_DEFAULT, bytes,
                                     static_cast<gsize>(frame.width) * 4);
    g_bytes_unref(bytes);
    gtk_widget_queue_draw(m_widget);
}

const LayerGeometry *TitleCanvas::geometryOf(const std::string &id) const
{
    for (const LayerGeometry &geometry : m_geometry)
        if (geometry.id == id)
            return &geometry;
    return nullptr;
}

std::optional<std::string> TitleCanvas::layerAt(double canvasX, double canvasY) const
{
    // Topmost first.
    for (auto it = m_geometry.rbegin(); it != m_geometry.rend(); ++it)
        if (it->visible && !it->locked && hitsBox(it->box, it->rotation, it->scale, canvasX, canvasY))
            return it->id;
    return std::nullopt;
}

TitleCanvas::Handle TitleCanvas::handleAt(double widgetX, double widgetY) const
{
    if (!m_selection)
        return Handle::None;
    const LayerGeometry *geometry = geometryOf(*m_selection);
    if (!geometry)
        return Handle::None;
    const Mapping map = mapping();
    const double cx = (widgetX - map.x) / map.scale, cy = (widgetY - map.y) / map.scale;
    // Handles only on an unturned, unscaled box; otherwise it just moves.
    if (geometry->rotation == 0.0 && geometry->scale == 1.0) {
        const Rect &b = geometry->box;
        const double reach = kHandleSize / map.scale;
        const auto near = [&](double x, double y) { return std::abs(cx - x) <= reach && std::abs(cy - y) <= reach; };
        const double mx = b.x + b.w / 2, my = b.y + b.h / 2;
        if (near(b.x, b.y))
            return Handle::TopLeft;
        if (near(b.right(), b.y))
            return Handle::TopRight;
        if (near(b.x, b.bottom()))
            return Handle::BottomLeft;
        if (near(b.right(), b.bottom()))
            return Handle::BottomRight;
        if (near(b.x, my))
            return Handle::Left;
        if (near(b.right(), my))
            return Handle::Right;
        if (near(mx, b.y))
            return Handle::Top;
        if (near(mx, b.bottom()))
            return Handle::Bottom;
    }
    if (hitsBox(geometry->box, geometry->rotation, geometry->scale, cx, cy))
        return Handle::Move;
    return Handle::None;
}

void TitleCanvas::snapshot(GtkSnapshot *snapshot)
{
    const Mapping map = mapping();
    const float fx = static_cast<float>(map.x), fy = static_cast<float>(map.y);
    const float fw = static_cast<float>(m_doc.width * map.scale), fh = static_cast<float>(m_doc.height * map.scale);
    const graphene_rect_t frame = GRAPHENE_RECT_INIT(fx, fy, fw, fh);

    // A new size on screen needs a new render.
    const int factor = gtk_widget_get_scale_factor(m_widget);
    if (std::lround(static_cast<double>(fw) * factor) != m_requestedWidth ||
        std::lround(static_cast<double>(fh) * factor) != m_requestedHeight)
        requestRender();

    switch (m_backdrop) {
    case Backdrop::Colour:
        gtk_snapshot_append_color(snapshot, &m_backdropColour, &frame);
        break;
    case Backdrop::Image:
        if (m_backdropImage) {
            gtk_snapshot_append_texture(snapshot, m_backdropImage, &frame);
            break;
        }
        [[fallthrough]];
    case Backdrop::Checkerboard: {
        cairo_t *cr = gtk_snapshot_append_cairo(snapshot, &frame);
        setColour(cr, tokens::kInk600);
        cairo_paint(cr);
        setColour(cr, tokens::kInk500);
        for (double y = 0; y < fh; y += kCheckerSize)
            for (double x = ((static_cast<int>(y / kCheckerSize)) % 2) * kCheckerSize; x < fw; x += 2 * kCheckerSize)
                cairo_rectangle(cr, fx + x, fy + y, kCheckerSize, kCheckerSize);
        cairo_fill(cr);
        cairo_destroy(cr);
        break;
    }
    }
    if (m_frame)
        gtk_snapshot_append_texture(snapshot, m_frame, &frame);

    const graphene_rect_t all = GRAPHENE_RECT_INIT(0.0f, 0.0f, static_cast<float>(gtk_widget_get_width(m_widget)),
                                                   static_cast<float>(gtk_widget_get_height(m_widget)));
    cairo_t *cr = gtk_snapshot_append_cairo(snapshot, &all);
    drawOverlay(cr);
    cairo_destroy(cr);
}

void TitleCanvas::drawOverlay(cairo_t *cr)
{
    const Mapping map = mapping();
    const auto toWidget = [&](const Rect &r) {
        return Rect{map.x + r.x * map.scale, map.y + r.y * map.scale, r.w * map.scale, r.h * map.scale};
    };
    // The frame's edge.
    const Rect frame = toWidget({0, 0, static_cast<double>(m_doc.width), static_cast<double>(m_doc.height)});
    cairo_set_line_width(cr, 1.0);
    setColour(cr, tokens::kInk500);
    cairo_rectangle(cr, frame.x - 0.5, frame.y - 0.5, frame.w + 1, frame.h + 1);
    cairo_stroke(cr);

    if (m_guides) {
        const double dashes[] = {4.0, 4.0};
        cairo_set_dash(cr, dashes, 2, 0);
        setColour(cr, tokens::kBrandCyan, 0.35);
        for (const Rect &safe : {actionSafe(m_doc), titleSafe(m_doc)}) {
            const Rect r = toWidget(safe);
            cairo_rectangle(cr, std::round(r.x) + 0.5, std::round(r.y) + 0.5, std::round(r.w), std::round(r.h));
            cairo_stroke(cr);
        }
        cairo_set_dash(cr, nullptr, 0, 0);
    }

    // The guides a drag snapped to.
    setColour(cr, tokens::kBrandCyan, 0.9);
    if (m_snapGuideX) {
        const double x = std::round(map.x + *m_snapGuideX * map.scale) + 0.5;
        cairo_move_to(cr, x, frame.y);
        cairo_line_to(cr, x, frame.bottom());
        cairo_stroke(cr);
    }
    if (m_snapGuideY) {
        const double y = std::round(map.y + *m_snapGuideY * map.scale) + 0.5;
        cairo_move_to(cr, frame.x, y);
        cairo_line_to(cr, frame.right(), y);
        cairo_stroke(cr);
    }

    // The selection: its box as drawn (turned and scaled), and handles.
    if (!m_selection)
        return;
    const LayerGeometry *geometry = geometryOf(*m_selection);
    if (!geometry)
        return;
    const Rect box = toWidget(geometry->box);
    cairo_save(cr);
    cairo_translate(cr, box.x + box.w / 2, box.y + box.h / 2);
    cairo_rotate(cr, geometry->rotation * std::numbers::pi / 180.0);
    cairo_scale(cr, geometry->scale, geometry->scale);
    cairo_rectangle(cr, -box.w / 2, -box.h / 2, box.w, box.h);
    cairo_restore(cr);
    cairo_set_line_width(cr, 1.5);
    setColour(cr, tokens::kBrandMagenta);
    cairo_stroke(cr);
    if (geometry->rotation != 0.0 || geometry->scale != 1.0)
        return;
    const double h = kHandleSize / 2;
    const double xs[] = {box.x, box.x + box.w / 2, box.right()};
    const double ys[] = {box.y, box.y + box.h / 2, box.bottom()};
    for (double x : xs)
        for (double y : ys) {
            if (x == xs[1] && y == ys[1])
                continue;
            cairo_rectangle(cr, x - h, y - h, kHandleSize, kHandleSize);
        }
    setColour(cr, tokens::kInk900);
    cairo_fill_preserve(cr);
    setColour(cr, tokens::kBrandMagenta);
    cairo_stroke(cr);
}

void TitleCanvas::pressed(int nPress, double x, double y)
{
    gtk_widget_grab_focus(m_widget);
    // Keep the selection when pressing its own handles.
    if (handleAt(x, y) != Handle::None && handleAt(x, y) != Handle::Move)
        return;
    const Mapping map = mapping();
    const std::optional<std::string> hit = layerAt((x - map.x) / map.scale, (y - map.y) / map.scale);
    setSelection(hit);
    if (nPress == 2 && hit && m_callbacks.editText) {
        for (const Layer &layer : m_doc.layers)
            if (layer.id == *hit && layer.kind == LayerKind::Text)
                m_callbacks.editText(*hit);
    }
}

void TitleCanvas::dragBegin(double x, double y)
{
    m_dragHandle = handleAt(x, y);
    if (m_dragHandle == Handle::None) {
        // Pressed on another layer: select it and move it in one go.
        const Mapping map = mapping();
        if (auto hit = layerAt((x - map.x) / map.scale, (y - map.y) / map.scale)) {
            setSelection(hit);
            m_dragHandle = Handle::Move;
        }
    }
    if (m_dragHandle == Handle::None || !m_selection)
        return;
    const LayerGeometry *geometry = geometryOf(*m_selection);
    if (!geometry) {
        m_dragHandle = Handle::None;
        return;
    }
    m_dragStartBox = geometry->box;
    m_dragStartX = x;
    m_dragStartY = y;
}

void TitleCanvas::dragUpdate(double dx, double dy, bool final)
{
    if (m_dragHandle == Handle::None || !m_selection)
        return;
    if (!m_dragActive && std::hypot(dx, dy) < kDragThreshold) {
        if (final)
            m_dragHandle = Handle::None;
        return;
    }
    m_dragActive = true;
    const Mapping map = mapping();
    const double cdx = dx / map.scale, cdy = dy / map.scale;
    Rect box = m_dragStartBox;
    m_snapGuideX.reset();
    m_snapGuideY.reset();

    if (m_dragHandle == Handle::Move) {
        box.x += cdx;
        box.y += cdy;
        if (!m_freeDrag) {
            std::vector<Rect> others;
            for (const LayerGeometry &geometry : m_geometry)
                if (geometry.id != *m_selection && geometry.visible)
                    others.push_back(geometry.box);
            const Snap snap = snapBox(box, snapGuides(m_doc, others), kSnapPixels / map.scale);
            box.x += snap.dx;
            box.y += snap.dy;
            m_snapGuideX = snap.guideX;
            m_snapGuideY = snap.guideY;
        }
        if (m_callbacks.moved)
            m_callbacks.moved(*m_selection, box.x, box.y, final);
    } else {
        const bool left =
            m_dragHandle == Handle::Left || m_dragHandle == Handle::TopLeft || m_dragHandle == Handle::BottomLeft;
        const bool right =
            m_dragHandle == Handle::Right || m_dragHandle == Handle::TopRight || m_dragHandle == Handle::BottomRight;
        const bool top =
            m_dragHandle == Handle::Top || m_dragHandle == Handle::TopLeft || m_dragHandle == Handle::TopRight;
        const bool bottom =
            m_dragHandle == Handle::Bottom || m_dragHandle == Handle::BottomLeft || m_dragHandle == Handle::BottomRight;
        constexpr double kMinSize = 4.0;
        if (left) {
            const double x = std::min(box.x + cdx, box.right() - kMinSize);
            box.w = box.right() - x;
            box.x = x;
        }
        if (right)
            box.w = std::max(kMinSize, box.w + cdx);
        if (top) {
            const double y = std::min(box.y + cdy, box.bottom() - kMinSize);
            box.h = box.bottom() - y;
            box.y = y;
        }
        if (bottom)
            box.h = std::max(kMinSize, box.h + cdy);
        if (m_callbacks.resized)
            m_callbacks.resized(*m_selection, box, final);
    }
    if (final) {
        m_snapGuideX.reset();
        m_snapGuideY.reset();
        m_dragHandle = Handle::None;
        m_dragActive = false;
    }
    gtk_widget_queue_draw(m_widget);
}

bool TitleCanvas::keyPressed(guint keyval, GdkModifierType state)
{
    const bool shift = (state & GDK_SHIFT_MASK) != 0;
    const bool modified = (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) != 0;
    if (modified)
        return false; // Ctrl+Z and friends are the window's
    switch (keyval) {
    case GDK_KEY_Escape:
        setSelection(std::nullopt);
        return true;
    case GDK_KEY_Delete:
    case GDK_KEY_BackSpace:
        if (m_selection && m_callbacks.deleteRequested)
            m_callbacks.deleteRequested(*m_selection);
        return m_selection.has_value();
    case GDK_KEY_Left:
    case GDK_KEY_Right:
    case GDK_KEY_Up:
    case GDK_KEY_Down: {
        if (!m_selection)
            return false;
        const LayerGeometry *geometry = geometryOf(*m_selection);
        if (!geometry)
            return false;
        const double step = shift ? 10.0 : 1.0;
        const double dx = keyval == GDK_KEY_Left ? -step : keyval == GDK_KEY_Right ? step : 0.0;
        const double dy = keyval == GDK_KEY_Up ? -step : keyval == GDK_KEY_Down ? step : 0.0;
        if (m_callbacks.moved)
            m_callbacks.moved(*m_selection, geometry->box.x + dx, geometry->box.y + dy, true);
        return true;
    }
    default:
        return false;
    }
}

// --- GTK trampolines ---------------------------------------------------------

void TitleCanvas::onPressed(GtkGestureClick *, int nPress, double x, double y, gpointer self)
{
    static_cast<TitleCanvas *>(self)->pressed(nPress, x, y);
}

void TitleCanvas::onDragBegin(GtkGestureDrag *gesture, double x, double y, gpointer self)
{
    auto *canvas = static_cast<TitleCanvas *>(self);
    const GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
    canvas->m_freeDrag = (state & GDK_ALT_MASK) != 0;
    canvas->dragBegin(x, y);
}

void TitleCanvas::onDragUpdate(GtkGestureDrag *, double dx, double dy, gpointer self)
{
    static_cast<TitleCanvas *>(self)->dragUpdate(dx, dy, false);
}

void TitleCanvas::onDragEnd(GtkGestureDrag *, double dx, double dy, gpointer self)
{
    static_cast<TitleCanvas *>(self)->dragUpdate(dx, dy, true);
}

gboolean TitleCanvas::onKeyPressed(GtkEventControllerKey *, guint keyval, guint, GdkModifierType state, gpointer self)
{
    return static_cast<TitleCanvas *>(self)->keyPressed(keyval, state) ? TRUE : FALSE;
}

} // namespace ustudio::titles::app
