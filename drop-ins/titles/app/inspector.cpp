#include "inspector.h"

#include <adwaita.h>

#include <array>
#include <cmath>
#include <utility>

namespace ustudio::titles::app {

namespace {

// A handler owned by the widget it's connected to: freed with it.
struct Handler
{
    std::function<void()> fn;
};

void freeHandler(gpointer data, GClosure *)
{
    delete static_cast<Handler *>(data);
}

// --- GTK trampolines -------------------------------------------------------
void onNotify(GObject *, GParamSpec *, gpointer data)
{
    static_cast<Handler *>(data)->fn();
}
void onSignal(GObject *, gpointer data)
{
    static_cast<Handler *>(data)->fn();
}

void onProperty(gpointer object, const char *property, std::function<void()> fn)
{
    const std::string signal = std::string("notify::") + property;
    g_signal_connect_data(object, signal.c_str(), G_CALLBACK(onNotify), new Handler{std::move(fn)}, freeHandler,
                          G_CONNECT_DEFAULT);
}
void onEvent(gpointer object, const char *signal, std::function<void()> fn)
{
    g_signal_connect_data(object, signal, G_CALLBACK(onSignal), new Handler{std::move(fn)}, freeHandler,
                          G_CONNECT_DEFAULT);
}

GdkRGBA toGdk(const Rgba &c)
{
    return {static_cast<float>(c.r), static_cast<float>(c.g), static_cast<float>(c.b), static_cast<float>(c.a)};
}
Rgba fromGdk(const GdkRGBA &c)
{
    return {c.red, c.green, c.blue, c.alpha};
}

GtkWidget *group(const char *title)
{
    GtkWidget *g = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g), title);
    return g;
}

void add(GtkWidget *groupWidget, GtkWidget *row)
{
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(groupWidget), row);
}

GtkWidget *spinRow(const char *title, double value, double min, double max, double step, int digits,
                   std::function<void(double)> set)
{
    GtkWidget *row = adw_spin_row_new_with_range(min, max, step);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_spin_row_set_digits(ADW_SPIN_ROW(row), static_cast<guint>(digits));
    adw_spin_row_set_value(ADW_SPIN_ROW(row), value);
    onProperty(row, "value", [row, set = std::move(set)] { set(adw_spin_row_get_value(ADW_SPIN_ROW(row))); });
    return row;
}

GtkWidget *comboRow(const char *title, std::initializer_list<const char *> items, size_t selected,
                    std::function<void(size_t)> set)
{
    GtkStringList *model = gtk_string_list_new(nullptr);
    for (const char *item : items)
        gtk_string_list_append(model, item);
    GtkWidget *row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
    g_object_unref(model);
    adw_combo_row_set_selected(ADW_COMBO_ROW(row), static_cast<guint>(selected));
    onProperty(row, "selected", [row, set = std::move(set)] { set(adw_combo_row_get_selected(ADW_COMBO_ROW(row))); });
    return row;
}

GtkWidget *switchRow(const char *title, bool active, std::function<void(bool)> set)
{
    GtkWidget *row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_switch_row_set_active(ADW_SWITCH_ROW(row), active);
    onProperty(row, "active", [row, set = std::move(set)] { set(adw_switch_row_get_active(ADW_SWITCH_ROW(row))); });
    return row;
}

GtkWidget *colourButton(const Rgba &colour, bool alpha, std::function<void(const Rgba &)> set)
{
    GtkColorDialog *dialog = gtk_color_dialog_new();
    gtk_color_dialog_set_with_alpha(dialog, alpha);
    GtkWidget *button = gtk_color_dialog_button_new(dialog); // takes the dialog
    const GdkRGBA rgba = toGdk(colour);
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(button), &rgba);
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    onProperty(button, "rgba", [button, set = std::move(set)] {
        set(fromGdk(*gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(button))));
    });
    return button;
}

// A row holding `child` under a small caption: chips and swatches wrap
// inside the inspector's width instead of widening it.
GtkWidget *captionRow(const char *caption, GtkWidget *child)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    GtkWidget *label = gtk_label_new(caption);
    gtk_widget_add_css_class(label, "caption");
    gtk_widget_add_css_class(label, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), child);
    GtkWidget *row = adw_preferences_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    return row;
}

GtkWidget *chipBox()
{
    GtkWidget *box = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(box), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(box), FALSE);
    // At least one a line, up to three: the chips never widen the inspector.
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(box), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(box), 3);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(box), 6);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(box), 6);
    return box;
}

GtkWidget *actionRow(const char *title, GtkWidget *suffix)
{
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), suffix);
    return row;
}

// A row of small swatch buttons: one per brand colour.
GtkWidget *swatches(const BrandKit &kit, std::function<void(const Rgba &)> pick)
{
    GtkWidget *box = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(box), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(box), 12);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    auto shared = std::make_shared<std::function<void(const Rgba &)>>(std::move(pick));
    for (const BrandKit::Colour &colour : kit.colours) {
        GtkWidget *button = gtk_button_new();
        gtk_widget_add_css_class(button, "flat");
        gtk_widget_set_tooltip_text(button, colour.name.c_str());
        // The colour itself, painted: no per-widget CSS (design system rule).
        GtkWidget *chip = gtk_drawing_area_new();
        gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(chip), 18);
        gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(chip), 18);
        gtk_drawing_area_set_draw_func(
            GTK_DRAWING_AREA(chip),
            [](GtkDrawingArea *, cairo_t *cr, int width, int height, gpointer data) {
                const Rgba &c = *static_cast<const Rgba *>(data);
                cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
                cairo_rectangle(cr, 0, 0, width, height);
                cairo_fill_preserve(cr);
                cairo_set_source_rgba(cr, 1, 1, 1, 0.25);
                cairo_set_line_width(cr, 1);
                cairo_stroke(cr);
            },
            new Rgba(colour.value), [](gpointer data) { delete static_cast<Rgba *>(data); });
        gtk_button_set_child(GTK_BUTTON(button), chip);
        const Rgba value = colour.value;
        onEvent(button, "clicked", [shared, value] { (*shared)(value); });
        gtk_flow_box_append(GTK_FLOW_BOX(box), button);
    }
    return box;
}

std::string layerKey(const std::string &id, const char *property)
{
    return "inspector:" + id + ":" + property;
}

} // namespace

Inspector::Inspector(Callbacks callbacks, BrandKit kit) : m_callbacks(std::move(callbacks)), m_kit(std::move(kit))
{
    m_scroller = gtk_scrolled_window_new();
    g_object_ref_sink(m_scroller);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(m_scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    // A fixed width, so the canvas never jumps when the selection changes.
    gtk_widget_set_size_request(m_scroller, 360, -1);
    gtk_widget_set_hexpand(m_scroller, FALSE);
    gtk_widget_set_name(m_scroller, "title-inspector");
}

Inspector::~Inspector()
{
    if (m_rebuildSource)
        g_source_remove(m_rebuildSource);
    g_object_unref(m_scroller);
}

void Inspector::rebuildLater()
{
    if (!m_rebuildSource)
        m_rebuildSource = g_idle_add(&onRebuild, this);
}

gboolean Inspector::onRebuild(gpointer self)
{
    auto *inspector = static_cast<Inspector *>(self);
    inspector->m_rebuildSource = 0;
    if (inspector->m_callbacks.rebuild)
        inspector->m_callbacks.rebuild();
    return G_SOURCE_REMOVE;
}

void Inspector::show(const TitleDocument &doc, const std::optional<std::string> &selection)
{
    m_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_start(m_box, 12);
    gtk_widget_set_margin_end(m_box, 12);
    gtk_widget_set_margin_top(m_box, 12);
    gtk_widget_set_margin_bottom(m_box, 12);
    const Layer *layer = nullptr;
    if (selection)
        for (const Layer &candidate : doc.layers)
            if (candidate.id == *selection)
                layer = &candidate;
    if (layer)
        buildLayer(*layer);
    else
        buildDocument(doc);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(m_scroller), m_box);
}

GtkWidget *Inspector::fillGroup(const char *title, const Fill &fill, bool allowNone,
                                std::function<void(const std::function<void(Fill &)> &, const std::string &)> edit)
{
    GtkWidget *g = group(title);
    auto shared = std::make_shared<decltype(edit)>(std::move(edit));
    // The kinds offered: None only where it means something (a shape's
    // fill, the title's background).
    const size_t offset = allowNone ? 0 : 1;
    const size_t selected = static_cast<size_t>(fill.kind) >= offset ? static_cast<size_t>(fill.kind) - offset : 0;
    GtkWidget *kind =
        allowNone ? comboRow("Kind", {"None", "Colour", "Linear gradient", "Radial gradient"}, selected,
                             [this, shared, offset](size_t i) {
                                 (*shared)([&](Fill &f) { f.kind = static_cast<FillKind>(i + offset); }, "kind");
                                 rebuildLater();
                             })
                  : comboRow("Kind", {"Colour", "Linear gradient", "Radial gradient"}, selected,
                             [this, shared, offset](size_t i) {
                                 (*shared)([&](Fill &f) { f.kind = static_cast<FillKind>(i + offset); }, "kind");
                                 rebuildLater();
                             });
    add(g, kind);
    if (fill.kind == FillKind::Solid) {
        add(g, actionRow("Colour", colourButton(fill.color, true, [shared](const Rgba &c) {
                             (*shared)([&](Fill &f) { f.color = c; }, "colour");
                         })));
        add(g, captionRow("Brand colours", swatches(m_kit, [shared](const Rgba &c) {
                              (*shared)([&](Fill &f) { f.color = c; }, "colour");
                          })));
    } else if (fill.kind == FillKind::Linear || fill.kind == FillKind::Radial) {
        add(g, actionRow("From", colourButton(fill.from, true, [shared](const Rgba &c) {
                             (*shared)([&](Fill &f) { f.from = c; }, "from");
                         })));
        GtkWidget *viaBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *viaSwitch = gtk_switch_new();
        gtk_switch_set_active(GTK_SWITCH(viaSwitch), fill.via.has_value());
        gtk_widget_set_valign(viaSwitch, GTK_ALIGN_CENTER);
        onProperty(viaSwitch, "active", [this, shared, viaSwitch, from = fill.from, to = fill.to] {
            const bool on = gtk_switch_get_active(GTK_SWITCH(viaSwitch));
            (*shared)(
                [&](Fill &f) {
                    if (on)
                        f.via =
                            Rgba{(from.r + to.r) / 2, (from.g + to.g) / 2, (from.b + to.b) / 2, (from.a + to.a) / 2};
                    else
                        f.via.reset();
                },
                "via-on");
            rebuildLater();
        });
        gtk_box_append(GTK_BOX(viaBox), viaSwitch);
        if (fill.via)
            gtk_box_append(GTK_BOX(viaBox), colourButton(*fill.via, true, [shared](const Rgba &c) {
                               (*shared)([&](Fill &f) { f.via = c; }, "via");
                           }));
        add(g, actionRow("Middle", viaBox));
        add(g, actionRow("To", colourButton(fill.to, true,
                                            [shared](const Rgba &c) { (*shared)([&](Fill &f) { f.to = c; }, "to"); })));
        if (fill.kind == FillKind::Linear)
            add(g, spinRow("Angle", fill.angle, -360, 360, 5, 0,
                           [shared](double v) { (*shared)([&](Fill &f) { f.angle = v; }, "angle"); }));
        // The brand's gradients, one click each.
        if (!m_kit.gradients.empty()) {
            GtkWidget *chips = chipBox();
            for (const BrandKit::Gradient &gradient : m_kit.gradients) {
                GtkWidget *chip = gtk_button_new_with_label(gradient.name.c_str());
                gtk_widget_add_css_class(chip, "pill");
                gtk_widget_add_css_class(chip, "flat");
                const Fill value = gradient.fill;
                onEvent(chip, "clicked", [shared, value] {
                    (*shared)(
                        [&](Fill &f) {
                            const double opacity = f.opacity;
                            const FillKind keepKind = f.kind;
                            f = value;
                            f.kind = keepKind;
                            f.opacity = opacity;
                        },
                        "brand-gradient");
                });
                gtk_flow_box_append(GTK_FLOW_BOX(chips), chip);
            }
            add(g, captionRow("Brand gradients", chips));
        }
    }
    if (fill.kind != FillKind::None)
        add(g, spinRow("Opacity", fill.opacity * 100, 0, 100, 5, 0,
                       [shared](double v) { (*shared)([&](Fill &f) { f.opacity = v / 100; }, "opacity"); }));
    return g;
}

void Inspector::buildLayer(const Layer &layer)
{
    const std::string id = layer.id;
    auto editLayer = m_callbacks.editLayer;
    const auto set = [editLayer, id](const char *label, const char *property, std::function<void(Layer &)> change) {
        editLayer(label, id, change, layerKey(id, property));
    };

    // Text first: it's what a text layer is for.
    if (layer.kind == LayerKind::Text) {
        GtkWidget *g = group("Text");
        GtkWidget *view = gtk_text_view_new();
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 8);
        gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view), 8);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 8);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 8);
        gtk_widget_set_size_request(view, -1, 64);
        gtk_widget_add_css_class(view, "card");
        gtk_widget_set_name(view, "title-text");
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
        gtk_text_buffer_set_text(buffer, layer.text.c_str(), -1);
        onEvent(buffer, "changed", [set, buffer] {
            GtkTextIter start, end;
            gtk_text_buffer_get_bounds(buffer, &start, &end);
            char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
            const std::string value = text;
            g_free(text);
            set("Edit Text", "text", [value](Layer &l) { l.text = value; });
        });
        add(g, view);
        gtk_box_append(GTK_BOX(m_box), g);

        GtkWidget *font = group("Font");
        GtkFontDialog *dialog = gtk_font_dialog_new();
        GtkWidget *family = gtk_font_dialog_button_new(dialog);
        gtk_font_dialog_button_set_level(GTK_FONT_DIALOG_BUTTON(family), GTK_FONT_LEVEL_FAMILY);
        gtk_font_dialog_button_set_use_font(GTK_FONT_DIALOG_BUTTON(family), TRUE);
        PangoFontDescription *desc = pango_font_description_from_string(layer.font.family.c_str());
        gtk_font_dialog_button_set_font_desc(GTK_FONT_DIALOG_BUTTON(family), desc);
        pango_font_description_free(desc);
        gtk_widget_set_valign(family, GTK_ALIGN_CENTER);
        onProperty(family, "font-desc", [set, family] {
            const PangoFontDescription *chosen = gtk_font_dialog_button_get_font_desc(GTK_FONT_DIALOG_BUTTON(family));
            const char *name = chosen ? pango_font_description_get_family(chosen) : nullptr;
            if (name) {
                const std::string value = name;
                set("Change Font", "family", [value](Layer &l) { l.font.family = value; });
            }
        });
        add(font, actionRow("Family", family));
        // The brand's fonts, one click each.
        GtkWidget *brandFonts = chipBox();
        for (const std::string &name : {m_kit.displayFont, m_kit.sansFont, m_kit.monoFont}) {
            if (name.empty())
                continue;
            GtkWidget *chip = gtk_button_new_with_label(name.c_str());
            gtk_widget_add_css_class(chip, "pill");
            gtk_widget_add_css_class(chip, "flat");
            onEvent(chip, "clicked",
                    [set, name] { set("Change Font", "family", [name](Layer &l) { l.font.family = name; }); });
            gtk_flow_box_append(GTK_FLOW_BOX(brandFonts), chip);
        }
        add(font, captionRow("Brand fonts", brandFonts));
        static constexpr std::array<int, 9> kWeights = {100, 200, 300, 400, 500, 600, 700, 800, 900};
        size_t weight = 3;
        for (size_t i = 0; i < kWeights.size(); ++i)
            if (std::abs(kWeights[i] - layer.font.weight) < std::abs(kWeights[weight] - layer.font.weight))
                weight = i;
        add(font,
            comboRow("Weight",
                     {"Thin", "Extra light", "Light", "Regular", "Medium", "Semibold", "Bold", "Extra bold", "Black"},
                     weight, [set](size_t i) {
                         set("Change Weight", "weight", [i](Layer &l) { l.font.weight = kWeights[i]; });
                     }));
        add(font, spinRow("Size", layer.font.size, 1, 4000, 1, 0,
                          [set](double v) { set("Change Size", "size", [v](Layer &l) { l.font.size = v; }); }));
        add(font, switchRow("Italic", layer.font.italic,
                            [set](bool on) { set("Italic", "italic", [on](Layer &l) { l.font.italic = on; }); }));
        add(font, spinRow("Letter spacing", layer.font.tracking * 100, -50, 200, 1, 0, [set](double v) {
                set("Letter Spacing", "tracking", [v](Layer &l) { l.font.tracking = v / 100; });
            }));
        add(font, spinRow("Line height", layer.font.lineHeight, 0.5, 3, 0.05, 2, [set](double v) {
                set("Line Height", "line-height", [v](Layer &l) { l.font.lineHeight = v; });
            }));
        add(font, comboRow("Align", {"Left", "Centre", "Right"}, static_cast<size_t>(layer.align), [set](size_t i) {
                set("Align", "align", [i](Layer &l) { l.align = static_cast<Align>(i); });
            }));
        add(font,
            comboRow("Fit", {"As written", "Wrap to the box", "Shrink to the box"}, static_cast<size_t>(layer.fit),
                     [set](size_t i) { set("Fit", "fit", [i](Layer &l) { l.fit = static_cast<Fit>(i); }); }));
        gtk_box_append(GTK_BOX(m_box), font);
    } else if (layer.kind == LayerKind::Image) {
        GtkWidget *g = group("Picture");
        GtkWidget *replace = gtk_button_new_with_label("Replace…");
        gtk_widget_set_valign(replace, GTK_ALIGN_CENTER);
        auto replaceImage = m_callbacks.replaceImage;
        onEvent(replace, "clicked", [replaceImage, id] {
            if (replaceImage)
                replaceImage(id);
        });
        const std::string name = layer.src.substr(
            layer.src.find_last_of("/\\") == std::string::npos ? 0 : layer.src.find_last_of("/\\") + 1);
        add(g, actionRow(name.empty() ? "No picture" : name.c_str(), replace));
        gtk_box_append(GTK_BOX(m_box), g);
    } else {
        GtkWidget *g = group("Shape");
        add(g, comboRow("Shape", {"Rectangle", "Rounded rectangle", "Ellipse", "Line"},
                        static_cast<size_t>(layer.shape), [set, this](size_t i) {
                            set("Change Shape", "shape", [i](Layer &l) { l.shape = static_cast<ShapeKind>(i); });
                            rebuildLater();
                        }));
        if (layer.shape == ShapeKind::RoundedRect)
            add(g, spinRow("Corner radius", layer.radius, 0, 2000, 1, 0,
                           [set](double v) { set("Corner Radius", "radius", [v](Layer &l) { l.radius = v; }); }));
        gtk_box_append(GTK_BOX(m_box), g);
    }

    if (layer.kind != LayerKind::Image && !(layer.kind == LayerKind::Shape && layer.shape == ShapeKind::Line)) {
        auto editLayerFill = m_callbacks.editLayer;
        gtk_box_append(GTK_BOX(m_box), fillGroup("Fill", layer.fill, layer.kind == LayerKind::Shape,
                                                 [editLayerFill, id](const std::function<void(Fill &)> &change,
                                                                     const std::string &key) {
                                                     editLayerFill(
                                                         "Change Fill", id, [&](Layer &l) { change(l.fill); },
                                                         layerKey(id, ("fill-" + key).c_str()));
                                                 }));
    }

    GtkWidget *outline = group("Outline");
    add(outline, spinRow("Width", layer.stroke.width, 0, 200, 1, 1, [set](double v) {
            set("Outline Width", "stroke-width", [v](Layer &l) { l.stroke.width = v; });
        }));
    add(outline, actionRow("Colour", colourButton(layer.stroke.color, true, [set](const Rgba &c) {
                               set("Outline Colour", "stroke-colour", [c](Layer &l) { l.stroke.color = c; });
                           })));
    gtk_box_append(GTK_BOX(m_box), outline);

    GtkWidget *shadow = group("Shadow");
    add(shadow, switchRow("Shadow", layer.shadow.enabled, [set, this](bool on) {
            set("Shadow", "shadow", [on](Layer &l) { l.shadow.enabled = on; });
            rebuildLater();
        }));
    if (layer.shadow.enabled) {
        add(shadow, spinRow("Across", layer.shadow.dx, -500, 500, 1, 0,
                            [set](double v) { set("Shadow", "shadow-dx", [v](Layer &l) { l.shadow.dx = v; }); }));
        add(shadow, spinRow("Down", layer.shadow.dy, -500, 500, 1, 0,
                            [set](double v) { set("Shadow", "shadow-dy", [v](Layer &l) { l.shadow.dy = v; }); }));
        add(shadow, spinRow("Softness", layer.shadow.blur, 0, 200, 1, 0,
                            [set](double v) { set("Shadow", "shadow-blur", [v](Layer &l) { l.shadow.blur = v; }); }));
        add(shadow, actionRow("Colour", colourButton(layer.shadow.color, false, [set](const Rgba &c) {
                                  set("Shadow Colour", "shadow-colour", [c](Layer &l) { l.shadow.color = c; });
                              })));
        add(shadow, spinRow("Opacity", layer.shadow.opacity * 100, 0, 100, 5, 0, [set](double v) {
                set("Shadow", "shadow-opacity", [v](Layer &l) { l.shadow.opacity = v / 100; });
            }));
    }
    gtk_box_append(GTK_BOX(m_box), shadow);

    GtkWidget *box = group("Position");
    add(box, spinRow("X", layer.x, -100000, 100000, 1, 0,
                     [set](double v) { set("Move Layer", "x", [v](Layer &l) { l.x = v; }); }));
    add(box, spinRow("Y", layer.y, -100000, 100000, 1, 0,
                     [set](double v) { set("Move Layer", "y", [v](Layer &l) { l.y = v; }); }));
    add(box, spinRow("Width", layer.w, 0, 100000, 1, 0,
                     [set](double v) { set("Resize Layer", "w", [v](Layer &l) { l.w = v; }); }));
    add(box, spinRow("Height", layer.h, 0, 100000, 1, 0,
                     [set](double v) { set("Resize Layer", "h", [v](Layer &l) { l.h = v; }); }));
    add(box, spinRow("Rotation", layer.rotation, -360, 360, 1, 0,
                     [set](double v) { set("Rotate Layer", "rotation", [v](Layer &l) { l.rotation = v; }); }));
    add(box, spinRow("Scale", layer.scale * 100, 1, 1000, 1, 0,
                     [set](double v) { set("Scale Layer", "scale", [v](Layer &l) { l.scale = v / 100; }); }));
    add(box, spinRow("Opacity", layer.opacity * 100, 0, 100, 5, 0,
                     [set](double v) { set("Layer Opacity", "opacity", [v](Layer &l) { l.opacity = v / 100; }); }));
    gtk_box_append(GTK_BOX(m_box), box);
}

void Inspector::buildDocument(const TitleDocument &doc)
{
    auto editDocument = m_callbacks.editDocument;

    GtkWidget *brand = group(("Brand: " + m_kit.name).c_str());
    GtkWidget *apply = gtk_button_new_with_label("Apply Brand");
    gtk_widget_add_css_class(apply, "suggested-action");
    gtk_widget_set_valign(apply, GTK_ALIGN_CENTER);
    auto applyBrand = m_callbacks.applyBrand;
    onEvent(apply, "clicked", [applyBrand] { applyBrand(); });
    GtkWidget *applyRow = actionRow("Restyle every layer in the brand", apply);
    add(brand, applyRow);
    gtk_box_append(GTK_BOX(m_box), brand);

    gtk_box_append(GTK_BOX(m_box),
                   fillGroup("Background", doc.background, true,
                             [editDocument](const std::function<void(Fill &)> &change, const std::string &key) {
                                 editDocument(
                                     "Change Background", [&](TitleDocument &d) { change(d.background); },
                                     "document:background-" + key);
                             }));

    GtkWidget *timing = group("Timing (frames)");
    add(timing, spinRow("Intro", static_cast<double>(doc.timing.intro), 0, 100000, 1, 0, [editDocument](double v) {
            editDocument(
                "Change Timing", [v](TitleDocument &d) { d.timing.intro = std::llround(v); }, "document:intro");
        }));
    add(timing, spinRow("Hold", static_cast<double>(doc.timing.hold), 0, 100000, 1, 0, [editDocument](double v) {
            editDocument("Change Timing", [v](TitleDocument &d) { d.timing.hold = std::llround(v); }, "document:hold");
        }));
    add(timing, spinRow("Outro", static_cast<double>(doc.timing.outro), 0, 100000, 1, 0, [editDocument](double v) {
            editDocument(
                "Change Timing", [v](TitleDocument &d) { d.timing.outro = std::llround(v); }, "document:outro");
        }));
    gtk_box_append(GTK_BOX(m_box), timing);

    GtkWidget *canvas = group("Canvas");
    add(canvas, spinRow("Width", doc.width, 16, 8192, 2, 0, [editDocument](double v) {
            editDocument("Canvas Size", [v](TitleDocument &d) { d.width = static_cast<int>(v); }, "document:width");
        }));
    add(canvas, spinRow("Height", doc.height, 16, 8192, 2, 0, [editDocument](double v) {
            editDocument("Canvas Size", [v](TitleDocument &d) { d.height = static_cast<int>(v); }, "document:height");
        }));
    static constexpr std::array<std::pair<int, int>, 7> kRates = {
        {{24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {50, 1}, {60000, 1001}, {60, 1}}};
    size_t rate = 3;
    for (size_t i = 0; i < kRates.size(); ++i)
        if (kRates[i].first == doc.fpsNum && kRates[i].second == doc.fpsDen)
            rate = i;
    add(canvas,
        comboRow("Frame rate", {"24", "25", "29.97", "30", "50", "59.94", "60"}, rate, [editDocument](size_t i) {
            editDocument(
                "Frame Rate",
                [i](TitleDocument &d) {
                    d.fpsNum = kRates[i].first;
                    d.fpsDen = kRates[i].second;
                },
                "document:fps");
        }));
    gtk_box_append(GTK_BOX(m_box), canvas);
}

} // namespace ustudio::titles::app
