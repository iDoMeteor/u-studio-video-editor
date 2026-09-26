// M4 F2 (ADR-018): OBS-style handles over the preview. Click a picture to
// select its clip (the timeline's selection too); drag inside it to move,
// a corner to scale (Shift: free aspect), an edge to stretch, the knob to
// rotate (Shift: 15-degree steps), Alt with an edge or corner to crop.
// Snapping to the frame and other pictures, Ctrl to bypass. Arrow keys
// nudge while the preview has focus (a click on it gives it focus; a click
// anywhere else takes it back, so the arrows step frames again). A drag is
// one undo step: every update is a SetClipTransform with the drag's own
// gesture id, which merges. The arithmetic is transform_gestures.cpp.
//
// It is the core shell's own overlay on the IP5 preview host (shell_host.h),
// the same one drop-ins use.

#include "app_window.h"

#include "tokens.h"
#include "core/commands/primitives.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ustudio::app {

namespace {

void setSource(cairo_t *cr, const tokens::Rgb &colour, double alpha)
{
    cairo_set_source_rgba(cr, colour.r, colour.g, colour.b, alpha);
}

// The CSS cursor for resizing along a handle's direction on screen.
const char *resizeCursor(const core::Placement &p, gestures::Handle handle)
{
    const gestures::Point h = gestures::handlePosition(p, handle, 1.0);
    double angle = std::atan2(h.y - p.cy, h.x - p.cx) * 180.0 / 3.14159265358979;
    angle = std::fmod(angle + 360.0, 180.0); // a line: 0-180
    if (angle < 22.5 || angle >= 157.5)
        return "ew-resize";
    if (angle < 67.5)
        return "nwse-resize"; // y down: towards the bottom right
    if (angle < 112.5)
        return "ns-resize";
    return "nesw-resize";
}

gestures::Modifiers modifiersOf(GdkModifierType state)
{
    gestures::Modifiers m;
    m.shift = (state & GDK_SHIFT_MASK) != 0;
    m.control = (state & GDK_CONTROL_MASK) != 0;
    m.alt = (state & GDK_ALT_MASK) != 0;
    return m;
}

} // namespace

void AppWindow::setUpTransformOverlay()
{
    m_transformOverlay = gtk_drawing_area_new();
    gtk_widget_add_css_class(m_transformOverlay, "transform-overlay");
    gtk_widget_set_focusable(m_transformOverlay, TRUE);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(m_transformOverlay),
        +[](GtkDrawingArea *, cairo_t *cr, int, int, gpointer self) {
            static_cast<AppWindow *>(self)->drawTransformOverlay(cr);
        },
        this, nullptr);
    // No tooltip: the overlay covers the whole preview, and one popping up
    // over the picture all day would be noise. Help lists the gestures.

    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    m_transformDragGesture = drag;
    g_signal_connect(drag, "drag-begin", G_CALLBACK(+[](GtkGestureDrag *, double x, double y, gpointer self) {
                         static_cast<AppWindow *>(self)->onTransformDragBegin(x, y);
                     }),
                     this);
    g_signal_connect(drag, "drag-update", G_CALLBACK(+[](GtkGestureDrag *, double dx, double dy, gpointer self) {
                         static_cast<AppWindow *>(self)->onTransformDragUpdate(dx, dy);
                     }),
                     this);
    g_signal_connect(drag, "drag-end", G_CALLBACK(+[](GtkGestureDrag *, double, double, gpointer self) {
                         static_cast<AppWindow *>(self)->onTransformDragEnd();
                     }),
                     this);
    gtk_widget_add_controller(m_transformOverlay, GTK_EVENT_CONTROLLER(drag));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(+[](GtkEventControllerMotion *, double x, double y, gpointer self) {
                         static_cast<AppWindow *>(self)->onTransformMotion(x, y);
                     }),
                     this);
    g_signal_connect(motion, "leave", G_CALLBACK(+[](GtkEventControllerMotion *, gpointer self) {
                         auto *window = static_cast<AppWindow *>(self);
                         if (window->m_transformHover.isValid()) {
                             window->m_transformHover = {};
                             gtk_widget_queue_draw(window->m_transformOverlay);
                         }
                     }),
                     this);
    gtk_widget_add_controller(m_transformOverlay, motion);

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(
        keys, "key-pressed",
        G_CALLBACK(+[](GtkEventControllerKey *, guint keyval, guint, GdkModifierType state, gpointer self) -> gboolean {
            return static_cast<AppWindow *>(self)->onTransformKey(keyval, state);
        }),
        this);
    gtk_widget_add_controller(m_transformOverlay, keys);
    g_signal_connect(m_transformOverlay, "notify::has-focus",
                     G_CALLBACK(+[](GtkWidget *overlay, GParamSpec *, gpointer self) {
                         static_cast<AppWindow *>(self)->setPreviewOwnsArrows(gtk_widget_has_focus(overlay));
                         gtk_widget_queue_draw(overlay);
                     }),
                     this);

    // A press anywhere but the preview hands the keyboard back to the
    // window's shortcuts (the timeline and transport aren't focusable, so
    // focus would otherwise stay here and the arrows keep nudging).
    GtkGesture *elsewhere = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(elsewhere), 0);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(elsewhere), GTK_PHASE_CAPTURE);
    g_signal_connect(elsewhere, "pressed", G_CALLBACK(+[](GtkGestureClick *, int, double x, double y, gpointer self) {
                         auto *window = static_cast<AppWindow *>(self);
                         GtkRoot *root = GTK_ROOT(window->m_window);
                         if (gtk_root_get_focus(root) != window->m_transformOverlay)
                             return;
                         GtkWidget *picked = gtk_widget_pick(GTK_WIDGET(window->m_window), x, y, GTK_PICK_DEFAULT);
                         if (picked != window->m_transformOverlay)
                             gtk_root_set_focus(root, nullptr);
                     }),
                     this);
    gtk_widget_add_controller(GTK_WIDGET(m_window), GTK_EVENT_CONTROLLER(elsewhere));

    addPreviewOverlay(m_transformOverlay);
    buildTransformMenu();
}

std::optional<gestures::VisibleClip> AppWindow::transformTarget() const
{
    const core::ClipId selected = m_timelineController.selection().single();
    if (!selected.isValid() || !m_model.hasClip(selected))
        return std::nullopt;
    for (const gestures::VisibleClip &visible :
         gestures::visibleClips(m_model, static_cast<core::FrameIndex>(m_engine->currentFrame())))
        if (visible.clip == selected)
            return visible;
    return std::nullopt;
}

void AppWindow::noteTransformSelection()
{
    // From the timeline's redraws: the handles follow a selection made there.
    const core::ClipId selected = m_timelineController.selection().single();
    if (m_transformOverlay && selected != m_transformShownSelection) {
        m_transformShownSelection = selected;
        gtk_widget_queue_draw(m_transformOverlay);
    }
}

bool AppWindow::applyTransform(core::ClipId clip, const core::Transform &transform, uint64_t gesture)
{
    if (m_model.clip(clip).transform.get() == transform)
        return true;
    m_applyingTransform = true;
    const bool applied = m_undoStack.execute(std::make_unique<core::SetClipTransform>(clip, transform, gesture));
    m_applyingTransform = false;
    gtk_widget_queue_draw(m_transformOverlay);
    return applied;
}

void AppWindow::onTransformDragBegin(double x, double y)
{
    gtk_widget_grab_focus(m_transformOverlay);
    const PreviewMapping mapping = previewMapping();
    const gestures::Point point{mapping.frameX(x), mapping.frameY(y)};
    // The selected picture's handles first, even over another picture;
    // inside a picture, the topmost one under the pointer (as OBS: a
    // full-frame clip selected below mustn't hide the ones above it).
    std::optional<gestures::VisibleClip> target = transformTarget();
    gestures::Handle handle =
        target ? gestures::hitTest(target->placement, point, mapping.scale()) : gestures::Handle::None;
    if (handle == gestures::Handle::None || handle == gestures::Handle::Body) {
        target = gestures::clipAt(m_model, static_cast<core::FrameIndex>(m_engine->currentFrame()), point);
        if (target) {
            m_timelineController.selection().selectOnly(target->clip);
            handle = gestures::Handle::Body;
        } else {
            m_timelineController.selection().clear();
        }
        gtk_widget_queue_draw(m_timeline);
        gtk_widget_queue_draw(m_transformOverlay);
    }
    if (!target || handle == gestures::Handle::None)
        return;
    const core::Clip &clip = m_model.clip(target->clip);
    if (m_model.track(clip.track).locked) {
        showStatus("The clip's track is locked.");
        return;
    }
    int width = 0, height = 0;
    if (m_model.hasAsset(clip.asset)) {
        width = m_model.asset(clip.asset).info.width;
        height = m_model.asset(clip.asset).info.height;
    }
    const core::Transform start =
        core::explicitTransform(clip.transform.get(), width, height, m_model.sequence().profile);
    m_transformDrag =
        TransformDrag{target->clip, {start, width, height, handle, point}, m_nextTransformGesture++, x, y};
}

void AppWindow::onTransformDragUpdate(double dx, double dy)
{
    if (!m_transformDrag || !m_model.hasClip(m_transformDrag->clip) || (dx == 0 && dy == 0))
        return;
    const PreviewMapping mapping = previewMapping();
    const gestures::Point now{mapping.frameX(m_transformDrag->widgetX + dx),
                              mapping.frameY(m_transformDrag->widgetY + dy)};
    const GdkModifierType state =
        gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(m_transformDragGesture));
    const gestures::DragResult result = gestures::drag(
        m_transformDrag->start, now, modifiersOf(state),
        gestures::snapTargets(m_model, static_cast<core::FrameIndex>(m_engine->currentFrame()), m_transformDrag->clip),
        gestures::kSnapDistance / mapping.scale());
    m_transformGuides = result.guides;
    applyTransform(m_transformDrag->clip, result.transform, m_transformDrag->gesture);
}

void AppWindow::onTransformDragEnd()
{
    m_transformDrag.reset();
    m_transformGuides.clear();
    gtk_widget_queue_draw(m_transformOverlay);
}

void AppWindow::endTransformDrag()
{
    // Something else changed the model mid-drag (an undo): the drag was
    // measured against the old one, so it stops here.
    if (!m_transformDrag || m_applyingTransform)
        return;
    m_transformDrag.reset();
    m_transformGuides.clear();
    gtk_event_controller_reset(GTK_EVENT_CONTROLLER(m_transformDragGesture));
}

void AppWindow::onTransformMotion(double x, double y)
{
    const PreviewMapping mapping = previewMapping();
    const gestures::Point point{mapping.frameX(x), mapping.frameY(y)};
    const char *cursor = nullptr;
    core::ClipId hover;
    const std::optional<gestures::VisibleClip> target = transformTarget();
    const gestures::Handle handle =
        target ? gestures::hitTest(target->placement, point, mapping.scale()) : gestures::Handle::None;
    if (m_transformDrag) {
        cursor = m_transformDrag->start.handle == gestures::Handle::Body     ? "move"
                 : m_transformDrag->start.handle == gestures::Handle::Rotate ? "grabbing"
                                                                             : nullptr;
        if (!cursor && target)
            cursor = resizeCursor(target->placement, m_transformDrag->start.handle);
    } else if (handle == gestures::Handle::Body &&
               gestures::clipAt(m_model, static_cast<core::FrameIndex>(m_engine->currentFrame()), point)->clip ==
                   target->clip) {
        cursor = "move";
    } else if (handle == gestures::Handle::Rotate) {
        cursor = "grab";
    } else if (handle != gestures::Handle::None && handle != gestures::Handle::Body) {
        cursor = resizeCursor(target->placement, handle);
    } else if (auto under = gestures::clipAt(m_model, static_cast<core::FrameIndex>(m_engine->currentFrame()), point)) {
        hover = under->clip;
        cursor = "pointer";
    }
    gtk_widget_set_cursor_from_name(m_transformOverlay, cursor);
    if (hover != m_transformHover) {
        m_transformHover = hover;
        gtk_widget_queue_draw(m_transformOverlay);
    }
}

void AppWindow::setPreviewOwnsArrows(bool owns)
{
    // Application accelerators run before the focused widget's key
    // handlers (measured: with the preview focused, Right still stepped a
    // frame), so while the preview has focus its plain arrows are lifted
    // from the window's actions and put back when focus leaves. Read from
    // the action table each time, so a rebinding later only changes it.
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(m_window));
    if (!app)
        return;
    auto plainArrow = [](const char *accel) {
        return std::strcmp(accel, "Left") == 0 || std::strcmp(accel, "Right") == 0 || std::strcmp(accel, "Up") == 0 ||
               std::strcmp(accel, "Down") == 0;
    };
    for (const ActionSpec &spec : allActionSpecs()) {
        if (std::none_of(spec.accels.begin(), spec.accels.end(), plainArrow))
            continue;
        std::vector<const char *> accels;
        for (const char *accel : spec.accels)
            if (!owns || !plainArrow(accel))
                accels.push_back(accel);
        accels.push_back(nullptr);
        gtk_application_set_accels_for_action(app, (std::string("win.") + spec.name).c_str(), accels.data());
    }
}

bool AppWindow::onTransformKey(guint keyval, GdkModifierType state)
{
    double dx = 0, dy = 0;
    const char *windowAction = nullptr; // what the arrow does elsewhere
    switch (keyval) {
    case GDK_KEY_Left:
        dx = -1;
        windowAction = "win.step-backward";
        break;
    case GDK_KEY_Right:
        dx = 1;
        windowAction = "win.step-forward";
        break;
    case GDK_KEY_Up:
        dy = -1;
        windowAction = "win.active-track-up";
        break;
    case GDK_KEY_Down:
        dy = 1;
        windowAction = "win.active-track-down";
        break;
    default:
        return false;
    }
    if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK))
        return false; // the window's step shortcuts
    const std::optional<gestures::VisibleClip> target = transformTarget();
    if (!target) {
        // Nothing to nudge: the arrow does what it does elsewhere.
        if (!(state & GDK_SHIFT_MASK))
            gtk_widget_activate_action(m_transformOverlay, windowAction, nullptr);
        return true;
    }
    const core::Clip &clip = m_model.clip(target->clip);
    if (m_model.track(clip.track).locked) {
        showStatus("The clip's track is locked.");
        return true;
    }
    const double step = (state & GDK_SHIFT_MASK) ? 10.0 : 1.0;
    int width = 0, height = 0;
    if (m_model.hasAsset(clip.asset)) {
        width = m_model.asset(clip.asset).info.width;
        height = m_model.asset(clip.asset).info.height;
    }
    const core::Transform start =
        core::explicitTransform(clip.transform.get(), width, height, m_model.sequence().profile);
    applyTransform(target->clip, gestures::nudged(start, dx * step, dy * step), 0);
    return true;
}

void AppWindow::drawTransformOverlay(cairo_t *cr)
{
    const PreviewMapping mapping = previewMapping();
    auto path = [&](const core::Placement &p) {
        const std::array<gestures::Point, 4> c = gestures::corners(p);
        cairo_move_to(cr, mapping.widgetX(c[0].x), mapping.widgetY(c[0].y));
        for (size_t i = 1; i < 4; ++i)
            cairo_line_to(cr, mapping.widgetX(c[i].x), mapping.widgetY(c[i].y));
        cairo_close_path(cr);
    };

    // Keyboard focus: the arrows nudge. A focus treatment, so it may glow.
    const std::optional<gestures::VisibleClip> target = transformTarget();
    if (gtk_widget_has_focus(m_transformOverlay) && target) {
        cairo_rectangle(cr, mapping.imageX + 1, mapping.imageY + 1, mapping.imageWidth - 2, mapping.imageHeight - 2);
        setSource(cr, tokens::kBrandMagenta, 0.25);
        cairo_set_line_width(cr, 4);
        cairo_stroke_preserve(cr);
        setSource(cr, tokens::kBrandMagenta, 0.8);
        cairo_set_line_width(cr, 1.5);
        cairo_stroke(cr);
    }

    // Hover: a quiet outline on the picture a click would select.
    if (m_transformHover.isValid() && m_model.hasClip(m_transformHover) &&
        (!target || target->clip != m_transformHover)) {
        path(gestures::placementOf(m_model, m_transformHover));
        setSource(cr, tokens::kBrandCyan, 0.5);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    }

    // Snap guides while dragging, across the frame.
    for (const gestures::Guide &guide : m_transformGuides) {
        setSource(cr, tokens::kBrandMagenta, 0.9);
        cairo_set_line_width(cr, 1);
        if (guide.vertical) {
            const double x = std::round(mapping.widgetX(guide.position)) + 0.5;
            cairo_move_to(cr, x, mapping.imageY);
            cairo_line_to(cr, x, mapping.imageY + mapping.imageHeight);
        } else {
            const double y = std::round(mapping.widgetY(guide.position)) + 0.5;
            cairo_move_to(cr, mapping.imageX, y);
            cairo_line_to(cr, mapping.imageX + mapping.imageWidth, y);
        }
        cairo_stroke(cr);
    }

    if (!target)
        return;
    // The selection: outline with a glow (selection may glow), handles,
    // the rotate knob. Every line has a dark underlay so it reads on any
    // picture, cyan footage included.
    const core::Placement &p = target->placement;
    const double scale = mapping.scale();
    const gestures::Point top = gestures::handlePosition(p, gestures::Handle::Top, scale);
    const gestures::Point knob = gestures::handlePosition(p, gestures::Handle::Rotate, scale);
    auto lines = [&] {
        path(p);
        cairo_move_to(cr, mapping.widgetX(top.x), mapping.widgetY(top.y));
        cairo_line_to(cr, mapping.widgetX(knob.x), mapping.widgetY(knob.y));
    };
    lines();
    setSource(cr, tokens::kBrandCyan, 0.25);
    cairo_set_line_width(cr, 6);
    cairo_stroke(cr);
    lines();
    setSource(cr, tokens::kInk900, 0.85);
    cairo_set_line_width(cr, 3.5);
    cairo_stroke(cr);
    lines();
    setSource(cr, tokens::kBrandCyan, 1.0);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    cairo_arc(cr, mapping.widgetX(knob.x), mapping.widgetY(knob.y), gestures::kHandleRadius - 1, 0, 2 * M_PI);
    setSource(cr, tokens::kInk900, 1.0);
    cairo_fill_preserve(cr);
    setSource(cr, tokens::kBrandCyan, 1.0);
    cairo_stroke(cr);

    const double half = gestures::kHandleRadius - 2;
    for (gestures::Handle handle : {gestures::Handle::TopLeft, gestures::Handle::Top, gestures::Handle::TopRight,
                                    gestures::Handle::Right, gestures::Handle::BottomRight, gestures::Handle::Bottom,
                                    gestures::Handle::BottomLeft, gestures::Handle::Left}) {
        const gestures::Point h = gestures::handlePosition(p, handle, scale);
        const double x = mapping.widgetX(h.x), y = mapping.widgetY(h.y);
        cairo_save(cr);
        cairo_translate(cr, x, y);
        cairo_rotate(cr, p.rotation * M_PI / 180.0);
        cairo_rectangle(cr, -half, -half, 2 * half, 2 * half);
        cairo_restore(cr);
        setSource(cr, tokens::kInk900, 1.0);
        cairo_fill_preserve(cr);
        setSource(cr, tokens::kBrandCyan, 1.0);
        cairo_set_line_width(cr, 1.5);
        cairo_stroke(cr);
    }
}

} // namespace ustudio::app
