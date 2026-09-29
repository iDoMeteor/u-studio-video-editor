// M4 F2 (ADR-018): the transform's window actions ("Transform" in Help's
// shortcuts), the preview's right-click menu over them, and the Edit
// Transform dialog. Each action is one undo step on the selected clip;
// the dialog is one undo step for all its changes (one gesture id).

#include "app_window.h"

#include "core/commands/primitives.h"

#include <cmath>
#include <cstring>

namespace ustudio::app {

namespace {

double quarterTurns(double rotation, double degrees)
{
    double r = std::fmod(rotation + degrees, 360.0);
    if (r > 180.0)
        r -= 360.0;
    else if (r <= -180.0)
        r += 360.0;
    return r;
}

// The Edit Transform dialog's rows and what they edit; owned by the dialog.
struct EditTransformState
{
    AppWindow *window = nullptr;
    core::ClipId clip;
    uint64_t gesture = 0;
    int sourceWidth = 0, sourceHeight = 0;
    AdwComboRow *bounds = nullptr;
    AdwSpinRow *x = nullptr, *y = nullptr, *width = nullptr, *height = nullptr, *rotation = nullptr;
    AdwSpinRow *crop[4] = {}; // left, top, right, bottom
    AdwSwitchRow *keepAspect = nullptr, *flipH = nullptr, *flipV = nullptr;
    bool updating = false;
    void (*show)(EditTransformState *) = nullptr; // the rows from the clip's transform
};

enum class Field
{
    Bounds,
    Place, // x, y
    Width,
    Height,
    Other,
};

} // namespace

std::optional<core::ClipId> AppWindow::transformActionTarget()
{
    const core::ClipId selected = m_timelineController.selection().single();
    if (!selected.isValid() || !m_model.hasClip(selected) ||
        m_model.track(m_model.clip(selected).track).kind != core::Track::Kind::Video) {
        showStatus("Select one clip on a video track to transform it.");
        return std::nullopt;
    }
    if (m_model.track(m_model.clip(selected).track).locked) {
        showStatus("The clip's track is locked.");
        return std::nullopt;
    }
    return selected;
}

core::Transform AppWindow::explicitTransformOf(core::ClipId clipId) const
{
    const core::Clip &clip = m_model.clip(clipId);
    int width = 0, height = 0;
    if (m_model.hasAsset(clip.asset)) {
        width = m_model.asset(clip.asset).info.width;
        height = m_model.asset(clip.asset).info.height;
    }
    return core::explicitTransform(
        gestures::transformShownAt(m_model, clipId, static_cast<core::FrameIndex>(m_engine->currentFrame())), width,
        height, m_model.sequence().profile);
}

void AppWindow::runTransformAction(const std::string &name)
{
    if (name == "transform-edit") {
        showEditTransformDialog();
        return;
    }
    const std::optional<core::ClipId> clip = transformActionTarget();
    if (!clip)
        return;
    const core::Profile &profile = m_model.sequence().profile;
    const core::Transform current = m_model.clip(*clip).transform.get();
    // As it is at the playhead; a keyed clip's change keys it there.
    const core::FrameIndex frame = static_cast<core::FrameIndex>(m_engine->currentFrame());
    core::Transform t = gestures::transformShownAt(m_model, *clip, frame);
    auto bounded = [&](core::Transform::Bounds bounds) {
        // Fit and Stretch place the picture themselves; crop, flip and
        // rotation stay.
        t.bounds = bounds;
        t.x.value = t.y.value = t.width.value = t.height.value = 0;
    };
    if (name == "transform-reset") {
        t = core::Transform{};
    } else if (name == "transform-fit") {
        bounded(core::Transform::Bounds::Fit);
    } else if (name == "transform-stretch") {
        bounded(core::Transform::Bounds::Stretch);
    } else if (name == "transform-centre" || name == "transform-centre-h" || name == "transform-centre-v") {
        t = explicitTransformOf(*clip);
        if (name != "transform-centre-v")
            t.x.value = profile.width / 2.0;
        if (name != "transform-centre-h")
            t.y.value = profile.height / 2.0;
    } else if (name == "transform-flip-h") {
        t.flipH = !t.flipH;
    } else if (name == "transform-flip-v") {
        t.flipV = !t.flipV;
    } else if (name == "transform-rotate-cw") {
        t.rotation.value = quarterTurns(t.rotation.value, 90);
    } else if (name == "transform-rotate-ccw") {
        t.rotation.value = quarterTurns(t.rotation.value, -90);
    } else if (name == "transform-rotate-180") {
        t.rotation.value = quarterTurns(t.rotation.value, 180);
    }
    t = gestures::transformEditedAt(m_model, *clip, frame, t);
    if (t == current) {
        showStatus("The transform is already that.");
        return;
    }
    applyTransform(*clip, t, 0);
}

void AppWindow::transformActionActivated(GSimpleAction *action, GVariant *, gpointer userData)
{
    static_cast<AppWindow *>(userData)->runTransformAction(g_action_get_name(G_ACTION(action)));
}

void AppWindow::buildTransformMenu()
{
    GMenu *menu = g_menu_new();
    auto section = [&](std::initializer_list<std::pair<const char *, const char *>> items) {
        GMenu *part = g_menu_new();
        for (const auto &[label, action] : items)
            g_menu_append(part, label, action);
        g_menu_append_section(menu, nullptr, G_MENU_MODEL(part));
        g_object_unref(part);
    };
    section({{"Edit Transform…", "win.transform-edit"}});
    section({{"Reset Transform", "win.transform-reset"},
             {"Fit to Frame", "win.transform-fit"},
             {"Stretch to Frame", "win.transform-stretch"}});
    section({{"Centre", "win.transform-centre"},
             {"Centre Horizontally", "win.transform-centre-h"},
             {"Centre Vertically", "win.transform-centre-v"}});
    section({{"Flip Horizontally", "win.transform-flip-h"}, {"Flip Vertically", "win.transform-flip-v"}});
    section({{"Rotate 90° Clockwise", "win.transform-rotate-cw"},
             {"Rotate 90° Anticlockwise", "win.transform-rotate-ccw"},
             {"Rotate 180°", "win.transform-rotate-180"}});
    m_transformMenu = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    g_object_unref(menu);
    gtk_popover_set_has_arrow(GTK_POPOVER(m_transformMenu), FALSE);
    gtk_widget_set_halign(m_transformMenu, GTK_ALIGN_START);
    gtk_widget_set_parent(m_transformMenu, m_transformOverlay);
    // Unparented by hand before its parent goes (post-M3 audit P10).
    g_signal_connect(m_transformOverlay, "destroy", G_CALLBACK(&AppWindow::unparentPopoverTrampoline), m_transformMenu);

    GtkGesture *secondary = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(secondary), GDK_BUTTON_SECONDARY);
    g_signal_connect(secondary, "pressed", G_CALLBACK(+[](GtkGestureClick *, int, double x, double y, gpointer self) {
                         static_cast<AppWindow *>(self)->onTransformMenuRequested(x, y);
                     }),
                     this);
    gtk_widget_add_controller(m_transformOverlay, GTK_EVENT_CONTROLLER(secondary));

    // A double click on a picture opens the dialog.
    GtkGesture *twice = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(twice), GDK_BUTTON_PRIMARY);
    g_signal_connect(twice, "pressed", G_CALLBACK(+[](GtkGestureClick *, int presses, double, double, gpointer self) {
                         auto *window = static_cast<AppWindow *>(self);
                         if (presses == 2 && window->transformTarget())
                             window->showEditTransformDialog();
                     }),
                     this);
    gtk_widget_add_controller(m_transformOverlay, GTK_EVENT_CONTROLLER(twice));
}

void AppWindow::onTransformMenuRequested(double x, double y)
{
    // As a primary click: the selected picture if the pointer is on it,
    // else the topmost picture under it.
    const PreviewMapping mapping = previewMapping();
    const gestures::Point point{mapping.frameX(x), mapping.frameY(y)};
    const std::optional<gestures::VisibleClip> target = transformTarget();
    const gestures::Handle handle =
        target ? gestures::hitTest(target->placement, point, mapping.scale()) : gestures::Handle::None;
    if (handle == gestures::Handle::None || handle == gestures::Handle::Body) {
        const auto under = gestures::clipAt(m_model, static_cast<core::FrameIndex>(m_engine->currentFrame()), point);
        if (!under)
            return;
        m_timelineController.selection().selectOnly(under->clip);
        gtk_widget_queue_draw(m_timeline);
        gtk_widget_queue_draw(m_transformOverlay);
    }
    const GdkRectangle at{static_cast<int>(x), static_cast<int>(y), 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(m_transformMenu), &at);
    gtk_popover_popup(GTK_POPOVER(m_transformMenu));
}

void AppWindow::showEditTransformDialog()
{
    const std::optional<core::ClipId> clipId = transformActionTarget();
    if (!clipId)
        return;
    const core::Clip &clip = m_model.clip(*clipId);
    auto *state = new EditTransformState;
    state->window = this;
    state->clip = *clipId;
    state->gesture = m_nextTransformGesture++;
    if (m_model.hasAsset(clip.asset)) {
        state->sourceWidth = m_model.asset(clip.asset).info.width;
        state->sourceHeight = m_model.asset(clip.asset).info.height;
    }
    const core::Profile &profile = m_model.sequence().profile;
    const double frameW = profile.width, frameH = profile.height;
    const double srcW = state->sourceWidth > 0 ? state->sourceWidth : frameW;
    const double srcH = state->sourceHeight > 0 ? state->sourceHeight : frameH;

    // A window of its own, not a modal AdwDialog: it stays open beside the
    // preview while the handles, the menu or undo change the picture, and
    // its rows follow them (refreshTransformDialog()).
    if (m_transformDialog)
        gtk_window_destroy(GTK_WINDOW(m_transformDialog)); // one at a time, for the clip selected now
    GtkWidget *dialog = adw_window_new();
    m_transformDialog = dialog;
    m_transformDialogState = state;
    gtk_window_set_title(GTK_WINDOW(dialog), "Edit Transform");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(m_window));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE); // else it would keep the app running
    gtk_window_set_default_size(GTK_WINDOW(dialog), 400, 640);
    g_object_set_data_full(
        G_OBJECT(dialog), "state", state, +[](gpointer data) { delete static_cast<EditTransformState *>(data); });
    g_signal_connect(dialog, "destroy", G_CALLBACK(+[](GtkWidget *widget, gpointer self) {
                         auto *window = static_cast<AppWindow *>(self);
                         if (window->m_transformDialog == widget) {
                             window->m_transformDialog = nullptr;
                             window->m_transformDialogState = nullptr;
                         }
                     }),
                     this);
    GtkEventController *escape = gtk_shortcut_controller_new();
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(escape),
                                         gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, GdkModifierType{}),
                                                          gtk_named_action_new("window.close")));
    gtk_widget_add_controller(dialog, escape);

    GtkWidget *toolbar = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    GtkWidget *reset = gtk_button_new_with_label("Reset");
    setTooltip(reset, "transform-dialog.reset");
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), reset);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    GtkWidget *page = adw_preferences_page_new();
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
    adw_window_set_content(ADW_WINDOW(dialog), toolbar);

    auto group = [&](const char *title) {
        GtkWidget *g = adw_preferences_group_new();
        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g), title);
        adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g));
        return ADW_PREFERENCES_GROUP(g);
    };
    auto spin = [&](AdwPreferencesGroup *g, const char *title, double min, double max, int digits, Field field,
                    const char *hint) {
        AdwSpinRow *row = ADW_SPIN_ROW(adw_spin_row_new_with_range(min, max, digits > 0 ? 0.1 : 1.0));
        adw_spin_row_set_digits(row, static_cast<guint>(digits));
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
        g_object_set_data(G_OBJECT(row), "field", GINT_TO_POINTER(static_cast<int>(field)));
        setTooltip(GTK_WIDGET(row), hint);
        adw_preferences_group_add(g, GTK_WIDGET(row));
        return row;
    };
    auto toggle = [&](AdwPreferencesGroup *g, const char *title, const char *hint) {
        AdwSwitchRow *row = ADW_SWITCH_ROW(adw_switch_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
        g_object_set_data(G_OBJECT(row), "field", GINT_TO_POINTER(static_cast<int>(Field::Other)));
        setTooltip(GTK_WIDGET(row), hint);
        adw_preferences_group_add(g, GTK_WIDGET(row));
        return row;
    };

    AdwPreferencesGroup *placement = group("Placement");
    const char *boundsLabels[] = {"Fit to frame", "Stretch to frame", "Placed", nullptr};
    state->bounds = ADW_COMBO_ROW(adw_combo_row_new());
    adw_combo_row_set_model(state->bounds, G_LIST_MODEL(gtk_string_list_new(boundsLabels)));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(state->bounds), "Bounds");
    g_object_set_data(G_OBJECT(state->bounds), "field", GINT_TO_POINTER(static_cast<int>(Field::Bounds)));
    setTooltip(GTK_WIDGET(state->bounds), "transform-dialog.bounds");
    adw_preferences_group_add(placement, GTK_WIDGET(state->bounds));
    state->x = spin(placement, "Centre X", -4 * frameW, 5 * frameW, 0, Field::Place, "transform-dialog.position");
    state->y = spin(placement, "Centre Y", -4 * frameH, 5 * frameH, 0, Field::Place, "transform-dialog.position");
    state->width = spin(placement, "Width", 1, 8 * frameW, 0, Field::Width, "transform-dialog.size");
    state->height = spin(placement, "Height", 1, 8 * frameH, 0, Field::Height, "transform-dialog.size");
    state->keepAspect = toggle(placement, "Keep aspect ratio", "transform-dialog.keep-aspect");
    adw_switch_row_set_active(state->keepAspect, TRUE);
    state->rotation = spin(placement, "Rotation (degrees)", -360, 360, 1, Field::Other, "transform-dialog.rotation");

    AdwPreferencesGroup *crop = group("Crop (source pixels)");
    const char *cropTitles[] = {"Left", "Top", "Right", "Bottom"};
    for (int i = 0; i < 4; ++i)
        state->crop[i] =
            spin(crop, cropTitles[i], 0, (i % 2 == 0 ? srcW : srcH) - 1, 0, Field::Other, "transform-dialog.crop");

    AdwPreferencesGroup *flip = group("Flip");
    state->flipH = toggle(flip, "Flip horizontally", "transform-dialog.flip");
    state->flipV = toggle(flip, "Flip vertically", "transform-dialog.flip");

    // The rows show the clip's transform; placement as it lands on screen.
    auto show = +[](EditTransformState *s) {
        AppWindow *w = s->window;
        if (!w->m_model.hasClip(s->clip))
            return;
        const core::Transform t =
            gestures::transformShownAt(w->m_model, s->clip, static_cast<core::FrameIndex>(w->m_engine->currentFrame()));
        const core::Placement p = core::placementFor(t, s->sourceWidth, s->sourceHeight, w->m_model.sequence().profile);
        s->updating = true;
        adw_combo_row_set_selected(s->bounds, t.bounds == core::Transform::Bounds::Fit       ? 0
                                              : t.bounds == core::Transform::Bounds::Stretch ? 1
                                                                                             : 2);
        adw_spin_row_set_value(s->x, p.cx);
        adw_spin_row_set_value(s->y, p.cy);
        adw_spin_row_set_value(s->width, p.w);
        adw_spin_row_set_value(s->height, p.h);
        adw_spin_row_set_value(s->rotation, t.rotation.value);
        adw_spin_row_set_value(s->crop[0], t.cropLeft.value);
        adw_spin_row_set_value(s->crop[1], t.cropTop.value);
        adw_spin_row_set_value(s->crop[2], t.cropRight.value);
        adw_spin_row_set_value(s->crop[3], t.cropBottom.value);
        adw_switch_row_set_active(s->flipH, t.flipH);
        adw_switch_row_set_active(s->flipV, t.flipV);
        s->updating = false;
    };
    state->show = show;
    show(state);

    // Any row: build the transform from them all and apply it (merging
    // into the dialog's one undo step), then show what landed.
    auto changed = +[](GObject *row, GParamSpec *, gpointer data) {
        auto *s = static_cast<EditTransformState *>(data);
        if (s->updating || !s->window->m_model.hasClip(s->clip))
            return;
        const auto field = static_cast<Field>(GPOINTER_TO_INT(g_object_get_data(row, "field")));
        s->updating = true;
        guint bounds = adw_combo_row_get_selected(s->bounds);
        if (field == Field::Place || field == Field::Width || field == Field::Height)
            adw_combo_row_set_selected(s->bounds, bounds = 2); // typing a place or size places it
        if (adw_switch_row_get_active(s->keepAspect)) {
            const core::Placement p = core::placementFor(
                gestures::transformShownAt(s->window->m_model, s->clip,
                                           static_cast<core::FrameIndex>(s->window->m_engine->currentFrame())),
                s->sourceWidth, s->sourceHeight, s->window->m_model.sequence().profile);
            if (field == Field::Width && p.w > 0)
                adw_spin_row_set_value(s->height, adw_spin_row_get_value(s->width) * p.h / p.w);
            else if (field == Field::Height && p.h > 0)
                adw_spin_row_set_value(s->width, adw_spin_row_get_value(s->height) * p.w / p.h);
        }
        s->updating = false;
        core::Transform t;
        t.bounds = bounds == 0   ? core::Transform::Bounds::Fit
                   : bounds == 1 ? core::Transform::Bounds::Stretch
                                 : core::Transform::Bounds::None;
        if (t.bounds == core::Transform::Bounds::None) {
            t.x.value = adw_spin_row_get_value(s->x);
            t.y.value = adw_spin_row_get_value(s->y);
            t.width.value = adw_spin_row_get_value(s->width);
            t.height.value = adw_spin_row_get_value(s->height);
        }
        t.rotation.value = adw_spin_row_get_value(s->rotation);
        t.cropLeft.value = adw_spin_row_get_value(s->crop[0]);
        t.cropTop.value = adw_spin_row_get_value(s->crop[1]);
        t.cropRight.value = adw_spin_row_get_value(s->crop[2]);
        t.cropBottom.value = adw_spin_row_get_value(s->crop[3]);
        t.flipH = adw_switch_row_get_active(s->flipH);
        t.flipV = adw_switch_row_get_active(s->flipV);
        t = gestures::transformEditedAt(s->window->m_model, s->clip,
                                        static_cast<core::FrameIndex>(s->window->m_engine->currentFrame()), t);
        if (!s->window->applyTransform(s->clip, t, s->gesture))
            s->window->showStatus("That transform isn't valid.");
        s->show(s);
    };
    for (AdwSpinRow *row : {state->x, state->y, state->width, state->height, state->rotation, state->crop[0],
                            state->crop[1], state->crop[2], state->crop[3]})
        g_signal_connect(row, "notify::value", G_CALLBACK(changed), state);
    for (AdwSwitchRow *row : {state->flipH, state->flipV})
        g_signal_connect(row, "notify::active", G_CALLBACK(changed), state);
    g_signal_connect(state->bounds, "notify::selected", G_CALLBACK(changed), state);
    g_signal_connect(reset, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
                         auto *s = static_cast<EditTransformState *>(data);
                         if (!s->window->m_model.hasClip(s->clip))
                             return;
                         s->window->applyTransform(s->clip, core::Transform{}, s->gesture);
                         s->show(s);
                     }),
                     state);
    gtk_window_present(GTK_WINDOW(dialog));
}

void AppWindow::refreshTransformDialog()
{
    if (!m_transformDialogState)
        return;
    auto *state = static_cast<EditTransformState *>(m_transformDialogState);
    if (!m_model.hasClip(state->clip)) {
        gtk_window_destroy(GTK_WINDOW(m_transformDialog)); // its clip is gone (an undo, a delete)
        return;
    }
    if (!state->updating)
        state->show(state);
}

} // namespace ustudio::app
