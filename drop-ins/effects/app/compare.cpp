#include "app/compare.h"

#include "app/catalog.h"
#include "app/shell_host.h"
#include "core/log.h"
#include "engine/frame_renderer.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <vector>

namespace ustudio::effects {

namespace {

class Compare
{
  public:
    Compare(app::ShellHost &host, Catalog &catalog) : m_host(host), m_catalog(catalog) {}

    void install()
    {
        m_host.addHints({
            {"effects.compare", "Effects", "Compare",
             "The picture without the clip's effects on the left of a divider, with them on the right; drag the "
             "divider",
             "effects-compare", nullptr},
            {"effects.bypass-hold", "Effects", "Hold to see without effects",
             "Hold \\ to see the picture without the selected clip's effects; let go to see them again", nullptr,
             "Hold \\"},
        });
        static const std::vector<app::ActionSpec> actions = {
            {"effects-compare", "Compare with and without effects", "Effects", {}, &onCompareActionTrampoline},
        };
        m_host.addActions(actions, this);

        m_area = gtk_drawing_area_new();
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(m_area), &onDrawTrampoline, this, nullptr);
        gtk_widget_set_can_target(m_area, FALSE);
        gtk_accessible_update_property(GTK_ACCESSIBLE(m_area), GTK_ACCESSIBLE_PROPERTY_LABEL, "Effects compare", -1);
        GtkGesture *drag = gtk_gesture_drag_new();
        g_signal_connect(drag, "drag-begin", G_CALLBACK(&onDividerTrampoline), this);
        g_signal_connect(drag, "drag-update", G_CALLBACK(&onDividerUpdateTrampoline), this);
        gtk_widget_add_controller(m_area, GTK_EVENT_CONTROLLER(drag));
        g_signal_connect(m_area, "map", G_CALLBACK(&onMapTrampoline), this);
        g_signal_connect(m_area, "destroy", G_CALLBACK(&onDestroyTrampoline), this);
        m_host.addPreviewOverlay(m_area);

        m_host.selectionChanged().connect([this] { onSourceChanged(); });
        m_host.projectChanged().connect([this] { onSourceChanged(); });
        m_host.playheadMoved().connect([this] { onSourceChanged(); });
    }

    void toggleCompare()
    {
        m_comparing = !m_comparing;
        gtk_widget_set_can_target(m_area, m_comparing);
        m_host.showStatus(m_comparing ? "Comparing: before the effects on the left, after on the right"
                                      : "Compare off");
        requestBefore();
        gtk_widget_queue_draw(m_area);
    }

    // The window's keys, once it exists: \ held shows the picture without
    // effects (application accelerators can't see a key's release).
    void onMap()
    {
        if (m_keysInstalled)
            return;
        GtkRoot *root = gtk_widget_get_root(m_area);
        if (!root)
            return;
        m_keysInstalled = true;
        GtkEventController *keys = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
        g_signal_connect(keys, "key-pressed", G_CALLBACK(&onKeyPressedTrampoline), this);
        g_signal_connect(keys, "key-released", G_CALLBACK(&onKeyReleasedTrampoline), this);
        gtk_widget_add_controller(GTK_WIDGET(root), keys);
    }

    bool onKey(guint keyval, bool pressed)
    {
        if (keyval != GDK_KEY_backslash)
            return false;
        // Typing a \ in a text field stays typing.
        if (GtkRoot *root = gtk_widget_get_root(m_area))
            if (GtkWidget *focus = gtk_root_get_focus(root); focus && GTK_IS_EDITABLE(focus))
                return false;
        if (pressed == m_holding)
            return true; // key repeat
        m_holding = pressed;
        if (m_holding)
            requestBefore();
        gtk_widget_queue_draw(m_area);
        return true;
    }

    void onDestroy()
    {
        m_renderer.stop(); // its thread holds MLT producers
    }

    void onSourceChanged()
    {
        if (!m_comparing && !m_holding)
            return;
        if (m_timer)
            g_source_remove(m_timer);
        m_timer = g_timeout_add(120, &onTimerTrampoline, this);
    }

    void onTimer()
    {
        m_timer = 0;
        requestBefore();
    }

    void onDivider(double x, bool begin)
    {
        if (begin)
            m_dragStartX = x;
        const app::PreviewMapping mapping = m_host.previewMapping();
        if (mapping.imageWidth <= 1.0)
            return;
        m_split = std::clamp((x - mapping.imageX) / mapping.imageWidth, 0.0, 1.0);
        gtk_widget_queue_draw(m_area);
    }

    void onDividerUpdate(double dx)
    {
        onDivider(m_dragStartX + dx, false);
    }

    void draw(cairo_t *cr)
    {
        if ((!m_comparing && !m_holding) || !m_before)
            return;
        const app::PreviewMapping mapping = m_host.previewMapping();
        if (mapping.imageWidth <= 1.0 || mapping.imageHeight <= 1.0)
            return;
        const double split = m_holding ? 1.0 : m_split;
        cairo_save(cr);
        cairo_rectangle(cr, mapping.imageX, mapping.imageY, mapping.imageWidth * split, mapping.imageHeight);
        cairo_clip(cr);
        cairo_translate(cr, mapping.imageX, mapping.imageY);
        cairo_scale(cr, mapping.imageWidth / cairo_image_surface_get_width(m_before),
                    mapping.imageHeight / cairo_image_surface_get_height(m_before));
        cairo_set_source_surface(cr, m_before, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
        if (m_holding)
            return;
        // The divider, and which side is which.
        const double x = mapping.imageX + mapping.imageWidth * split;
        cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
        cairo_set_line_width(cr, 2.0);
        cairo_move_to(cr, x, mapping.imageY);
        cairo_line_to(cr, x, mapping.imageY + mapping.imageHeight);
        cairo_stroke(cr);
        drawTag(cr, "Before", mapping.imageX + 8, mapping.imageY + 8);
        drawTag(cr, "After", mapping.imageX + mapping.imageWidth - 52, mapping.imageY + 8);
    }

  private:
    static void drawTag(cairo_t *cr, const char *text, double x, double y)
    {
        cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
        cairo_rectangle(cr, x - 4, y - 2, 48, 18);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11);
        cairo_move_to(cr, x, y + 12);
        cairo_show_text(cr, text);
    }

    // The selected clip, else the topmost under the playhead.
    std::optional<core::ClipId> sourceClip() const
    {
        const core::Model &model = m_host.model();
        for (core::ClipId id : m_host.currentSelection().clips)
            if (model.hasClip(id))
                return id;
        const core::FrameIndex frame = m_host.currentFrame();
        std::optional<core::ClipId> top;
        for (const core::Track &track : model.sequence().tracks) {
            if (track.kind != core::Track::Kind::Video)
                continue;
            for (core::ClipId id : track.clips) {
                const core::Clip &clip = model.clip(id);
                if (frame >= clip.position && frame < clip.position + clip.length())
                    top = id;
            }
        }
        return top;
    }

    void requestBefore()
    {
        if (!m_comparing && !m_holding)
            return;
        const core::Model &model = m_host.model();
        const std::optional<core::ClipId> id = sourceClip();
        const app::PreviewMapping mapping = m_host.previewMapping();
        if (!id || !model.hasAsset(model.clip(*id).asset) || mapping.imageWidth <= 1.0)
            return;
        const core::Clip &clip = model.clip(*id);
        const core::FrameIndex frame = m_host.currentFrame();
        if (frame < clip.position || frame >= clip.position + clip.length())
            return; // the clip isn't in the picture now
        FrameRequest request;
        request.resource = model.asset(clip.asset).path;
        request.profile = model.sequence().profile;
        request.sourceFrame = clip.in + (frame - clip.position);
        request.clipIn = clip.in;
        request.clipOut = clip.out;
        // Before: the clip with none of its effects (the frame the effects
        // start from). At the picture's size on screen, at most the frame's.
        request.width = std::max(16, static_cast<int>(std::min(mapping.imageWidth, mapping.frameWidth)) / 2 * 2);
        request.height = std::max(16, static_cast<int>(std::min(mapping.imageHeight, mapping.frameHeight)) / 2 * 2);
        const uint64_t generation = ++m_generation;
        m_renderer.request(std::move(request), 0, generation, [this, generation](const RenderedFrame &before) {
            if (generation != m_generation || before.rgba.empty())
                return;
            setBefore(before);
            core::Log::debug(std::string("[effects] ") + (m_holding ? "held \\: the picture without effects"
                                                                    : "comparing: the before frame shown"));
            gtk_widget_queue_draw(m_area);
        });
    }

    // RGBA (straight) into a cairo ARGB32 surface (premultiplied, native
    // byte order).
    void setBefore(const RenderedFrame &frame)
    {
        if (m_before)
            cairo_surface_destroy(m_before);
        m_before = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, frame.width, frame.height);
        cairo_surface_flush(m_before);
        unsigned char *data = cairo_image_surface_get_data(m_before);
        const int stride = cairo_image_surface_get_stride(m_before);
        for (int y = 0; y < frame.height; ++y) {
            auto *row = reinterpret_cast<uint32_t *>(data + static_cast<ptrdiff_t>(y) * stride);
            const uint8_t *src = frame.rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(frame.width) * 4;
            for (int x = 0; x < frame.width; ++x, src += 4) {
                const uint32_t a = src[3];
                const uint32_t r = src[0] * a / 255, g = src[1] * a / 255, b = src[2] * a / 255;
                row[x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
        cairo_surface_mark_dirty(m_before);
    }

    // --- GTK signal trampolines ---------------------------------------------

    static void onCompareActionTrampoline(GSimpleAction *, GVariant *, gpointer self)
    {
        static_cast<Compare *>(self)->toggleCompare();
    }
    static void onDrawTrampoline(GtkDrawingArea *, cairo_t *cr, int, int, gpointer self)
    {
        static_cast<Compare *>(self)->draw(cr);
    }
    static void onMapTrampoline(GtkWidget *, gpointer self)
    {
        static_cast<Compare *>(self)->onMap();
    }
    static void onDestroyTrampoline(GtkWidget *, gpointer self)
    {
        static_cast<Compare *>(self)->onDestroy();
    }
    static gboolean onTimerTrampoline(gpointer self)
    {
        static_cast<Compare *>(self)->onTimer();
        return G_SOURCE_REMOVE;
    }
    static void onDividerTrampoline(GtkGestureDrag *, double x, double, gpointer self)
    {
        static_cast<Compare *>(self)->onDivider(x, true);
    }
    static void onDividerUpdateTrampoline(GtkGestureDrag *, double dx, double, gpointer self)
    {
        static_cast<Compare *>(self)->onDividerUpdate(dx);
    }
    static gboolean onKeyPressedTrampoline(GtkEventControllerKey *, guint keyval, guint, GdkModifierType, gpointer self)
    {
        return static_cast<Compare *>(self)->onKey(keyval, true);
    }
    static void onKeyReleasedTrampoline(GtkEventControllerKey *, guint keyval, guint, GdkModifierType, gpointer self)
    {
        static_cast<Compare *>(self)->onKey(keyval, false);
    }

    app::ShellHost &m_host;
    Catalog &m_catalog;
    FrameRenderer m_renderer{4};
    GtkWidget *m_area = nullptr;
    cairo_surface_t *m_before = nullptr;
    double m_split = 0.5, m_dragStartX = 0.0;
    uint64_t m_generation = 0;
    guint m_timer = 0;
    bool m_comparing = false, m_holding = false, m_keysInstalled = false;
};

} // namespace

void addCompare(app::ShellHost &host, Catalog &catalog)
{
    // For the window's life: its widgets and signal handlers point here.
    static std::vector<std::unique_ptr<Compare>> compares;
    compares.push_back(std::make_unique<Compare>(host, catalog));
    compares.back()->install();
}

} // namespace ustudio::effects
