#include "app/transitions_page.h"

#include "app/shell_host.h"
#include "core/commands/primitives.h"
#include "core/log.h"
#include "core/model/transition_native.h"
#include "tokens.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ustudio::effects {

namespace {

namespace tokens = app::tokens;

constexpr int kTileWidth = 96, kTileHeight = 54;

// "video.softness" -> "Softness".
std::string displayName(const std::string &param)
{
    std::string name = param.substr(param.rfind('.') + 1);
    std::replace(name.begin(), name.end(), '_', ' ');
    if (!name.empty())
        name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    return name;
}

const core::Param *findParam(const std::vector<core::Param> &params, const std::string &name)
{
    for (const core::Param &param : params)
        if (param.name == name)
            return &param;
    return nullptr;
}

// "#rrggbb" as a token-like colour; nullopt for anything else.
std::optional<tokens::Rgb> parseSwatch(const std::string &text)
{
    if (text.size() != 7 || text[0] != '#')
        return std::nullopt;
    char *end = nullptr;
    const unsigned long value = std::strtoul(text.c_str() + 1, &end, 16);
    if (end != text.c_str() + 7)
        return std::nullopt;
    return tokens::Rgb{static_cast<float>((value >> 16) & 0xFF) / 255.0f,
                       static_cast<float>((value >> 8) & 0xFF) / 255.0f, static_cast<float>(value & 0xFF) / 255.0f};
}

// A motion recipe's incoming picture half-way through, in 0-1 of the frame:
// the middle of its "ramp:x% y% w% h%|x% y% w% h%" video.rect. Nullopt for
// anything else.
struct Box
{
    double x = 0, y = 0, w = 1, h = 1;
};
std::optional<Box> halfwayRect(const TransitionRecipe &recipe)
{
    const core::Param *param = findParam(recipe.params, "video.rect");
    const auto *text = param ? std::get_if<std::string>(&param->value) : nullptr;
    if (!text || !text->starts_with("ramp:"))
        return std::nullopt;
    const std::string body = text->substr(5);
    const size_t bar = body.find('|');
    if (bar == std::string::npos)
        return std::nullopt;
    auto parse = [](const std::string &rect) -> std::optional<Box> {
        double v[4];
        if (std::sscanf(rect.c_str(), "%lf%% %lf%% %lf%% %lf%%", &v[0], &v[1], &v[2], &v[3]) != 4)
            return std::nullopt;
        return Box{v[0] / 100, v[1] / 100, v[2] / 100, v[3] / 100};
    };
    const std::optional<Box> from = parse(body.substr(0, bar)), to = parse(body.substr(bar + 1));
    if (!from || !to)
        return std::nullopt;
    return Box{(from->x + to->x) / 2, (from->y + to->y) / 2, (from->w + to->w) / 2, (from->h + to->h) / 2};
}

// A tile's picture: the outgoing clip (magenta) and the incoming one (cyan)
// half-way through the recipe. A wipe shows its map's shape; a motion
// recipe the incoming picture where it is half-way; a dissolve the two
// blended; a dip or flash its swatch between them.
GdkTexture *tileTexture(const TransitionRecipe &recipe)
{
    const tokens::Rgb a = tokens::kBrandMagenta, b = tokens::kBrandCyan;
    std::vector<uint8_t> rgba(static_cast<size_t>(kTileWidth) * kTileHeight * 4);
    std::string luma;
    if (const core::Param *param = findParam(recipe.params, "video.luma"))
        if (const auto *name = std::get_if<std::string>(&param->value))
            luma = *name;
    const std::vector<uint16_t> map = luma.empty() ? std::vector<uint16_t>{} : core::lumaMapPixels(luma, kTileWidth, kTileHeight);
    const std::optional<tokens::Rgb> swatch = parseSwatch(recipe.swatch);
    const std::optional<Box> moving = halfwayRect(recipe);
    for (int y = 0; y < kTileHeight; ++y)
        for (int x = 0; x < kTileWidth; ++x) {
            const size_t i = static_cast<size_t>(y * kTileWidth + x);
            float r, g, bl;
            if (!map.empty()) {
                // The incoming clip where the map is darker than half-way.
                const float v = static_cast<float>(map[i]) / 65535.0f;
                const float w = std::clamp((0.5f - v) / 0.08f + 0.5f, 0.0f, 1.0f);
                r = a.r + (b.r - a.r) * w;
                g = a.g + (b.g - a.g) * w;
                bl = a.b + (b.b - a.b) * w;
            } else if (moving) {
                const double u = (x + 0.5) / kTileWidth, v = (y + 0.5) / kTileHeight;
                const bool inside = u >= moving->x && u < moving->x + moving->w && v >= moving->y &&
                                    v < moving->y + moving->h;
                const tokens::Rgb &side = inside ? b : a;
                r = side.r;
                g = side.g;
                bl = side.b;
            } else if (swatch && x >= kTileWidth / 3 && x < kTileWidth * 2 / 3) {
                r = swatch->r;
                g = swatch->g;
                bl = swatch->b;
            } else if (swatch) {
                const tokens::Rgb &side = x < kTileWidth / 3 ? a : b;
                r = side.r;
                g = side.g;
                bl = side.b;
            } else {
                // A dissolve: left to right, from one into the other.
                const float w = static_cast<float>(x) / static_cast<float>(kTileWidth - 1);
                r = a.r + (b.r - a.r) * w;
                g = a.g + (b.g - a.g) * w;
                bl = a.b + (b.b - a.b) * w;
            }
            rgba[i * 4 + 0] = static_cast<uint8_t>(std::lround(r * 255.0f));
            rgba[i * 4 + 1] = static_cast<uint8_t>(std::lround(g * 255.0f));
            rgba[i * 4 + 2] = static_cast<uint8_t>(std::lround(bl * 255.0f));
            rgba[i * 4 + 3] = 255;
        }
    GBytes *bytes = g_bytes_new(rgba.data(), rgba.size());
    GdkTexture *texture = gdk_memory_texture_new(kTileWidth, kTileHeight, GDK_MEMORY_R8G8B8A8, bytes,
                                                 static_cast<gsize>(kTileWidth) * 4);
    g_bytes_unref(bytes);
    return texture;
}

// GPU acceleration as Settings has it (the preview's pipeline follows it
// where the startup check passed). False when the schema isn't installed
// (tests, a build run without it).
bool gpuAccelerationSetting()
{
    GSettingsSchemaSource *source = g_settings_schema_source_get_default();
    GSettingsSchema *schema = source ? g_settings_schema_source_lookup(source, "com.ustudio.VideoEditor", TRUE) : nullptr;
    if (!schema)
        return false;
    const bool has = g_settings_schema_has_key(schema, "gpu-acceleration");
    g_settings_schema_unref(schema);
    if (!has)
        return false;
    GSettings *settings = g_settings_new("com.ustudio.VideoEditor");
    const bool on = g_settings_get_boolean(settings, "gpu-acceleration");
    g_object_unref(settings);
    return on;
}

class TransitionsPage : public app::timeline::TimelineOverlayProvider
{
  public:
    TransitionsPage(app::ShellHost &host, const std::vector<TransitionRecipe> &recipes)
        : m_host(host), m_recipes(recipes)
    {}

    void install()
    {
        m_host.addHints({
            {"effects.transitions-add", "Transitions", "Add transition",
             "A dissolve at the cut nearest the playhead on the active track; pick its style on the Transitions page",
             "effects-add-transition", nullptr},
            {"effects.transitions-open", "Transitions", "Transition styles",
             "Opens the Transitions page on a transition", nullptr, "Double-click a transition"},
            {"effects.transitions-tile", "Transitions", "Transition style",
             "Click to play the transition this way; Ctrl+Z puts the last one back", nullptr, nullptr},
            {"effects.transitions-softness", "Transitions", "Softness", "How blurred a wipe's edge is", nullptr,
             nullptr},
            {"effects.transitions-reverse", "Transitions", "Reverse", "Runs the wipe the other way", nullptr,
             nullptr},
        });
        static const std::vector<app::ActionSpec> actions = {
            {"effects-add-transition", "Add transition at the nearest cut", "Transitions", {"t"},
             &onAddActionTrampoline},
        };
        m_host.addActions(actions, this);

        m_root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_set_margin_start(m_root, 12);
        gtk_widget_set_margin_end(m_root, 12);
        gtk_widget_set_margin_top(m_root, 12);
        gtk_widget_set_margin_bottom(m_root, 12);
        m_title = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(m_title), 0.0f);
        gtk_widget_add_css_class(m_title, "heading");
        gtk_box_append(GTK_BOX(m_root), m_title);
        m_subtitle = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(m_subtitle), 0.0f);
        gtk_label_set_wrap(GTK_LABEL(m_subtitle), TRUE);
        gtk_widget_add_css_class(m_subtitle, "dim-label");
        gtk_box_append(GTK_BOX(m_root), m_subtitle);
        m_gpuNote = gtk_label_new("With GPU acceleration on, a wipe can stutter in the preview at Full quality; "
                                  "at Half it plays smoothly, and the export is exact.");
        gtk_label_set_xalign(GTK_LABEL(m_gpuNote), 0.0f);
        gtk_label_set_wrap(GTK_LABEL(m_gpuNote), TRUE);
        gtk_widget_add_css_class(m_gpuNote, "dim-label");
        gtk_widget_add_css_class(m_gpuNote, "caption");
        gtk_box_append(GTK_BOX(m_root), m_gpuNote);

        m_controls = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_box_append(GTK_BOX(m_root), m_controls);

        GtkWidget *scroller = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_vexpand(scroller, TRUE);
        GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list);
        gtk_box_append(GTK_BOX(m_root), scroller);
        m_tiles = list;

        // One flow box per category, in the recipes' order.
        std::vector<std::string> categories;
        for (const TransitionRecipe &recipe : m_recipes)
            if (std::find(categories.begin(), categories.end(), recipe.category) == categories.end())
                categories.push_back(recipe.category);
        for (const std::string &category : categories) {
            GtkWidget *heading = gtk_label_new(category.c_str());
            gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
            gtk_widget_add_css_class(heading, "dim-label");
            gtk_widget_add_css_class(heading, "caption");
            gtk_box_append(GTK_BOX(list), heading);
            GtkWidget *flow = gtk_flow_box_new();
            gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_SINGLE);
            gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(flow), TRUE);
            gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), TRUE);
            gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 6);
            g_signal_connect(flow, "child-activated", G_CALLBACK(&onTileActivatedTrampoline), this);
            gtk_box_append(GTK_BOX(list), flow);
            m_flows.push_back(flow);
            for (size_t i = 0; i < m_recipes.size(); ++i) {
                const TransitionRecipe &recipe = m_recipes[i];
                if (recipe.category != category)
                    continue;
                GtkWidget *tile = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
                GdkTexture *texture = tileTexture(recipe);
                GtkWidget *picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(texture));
                g_object_unref(texture);
                gtk_picture_set_can_shrink(GTK_PICTURE(picture), FALSE);
                gtk_box_append(GTK_BOX(tile), picture);
                GtkWidget *name = gtk_label_new(recipe.name.c_str());
                gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
                gtk_label_set_max_width_chars(GTK_LABEL(name), 12);
                gtk_widget_add_css_class(name, "caption");
                gtk_box_append(GTK_BOX(tile), name);
                GtkWidget *child = gtk_flow_box_child_new();
                gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(child), tile);
                gtk_flow_box_append(GTK_FLOW_BOX(flow), child);
                g_object_set_data(G_OBJECT(child), "ustudio-recipe", GSIZE_TO_POINTER(i + 1));
                m_host.setTooltip(child, "effects.transitions-tile");
                const std::string label = recipe.name + (recipe.description.empty() ? "" : ": " + recipe.description);
                gtk_widget_set_tooltip_text(child, label.c_str());
                gtk_accessible_update_property(GTK_ACCESSIBLE(child), GTK_ACCESSIBLE_PROPERTY_LABEL, label.c_str(),
                                               -1);
                m_children.push_back(child);
            }
        }

        m_host.addInspectorPage({"effects.transitions", "Transitions", "view-dual-symbolic", m_root});
        m_host.addTimelineOverlay(this);
        m_host.projectChanged().connect([this] { refresh(true); });
        m_host.playheadMoved().connect([this] { refresh(false); });
        m_host.selectionChanged().connect([this] { refresh(false); });
        refresh(true);
    }

    // --- TimelineOverlayProvider -------------------------------------------

    void paintOverlay(GtkSnapshot *snapshot, const core::Model &model, const app::timeline::Viewport &viewport,
                      const app::timeline::RowLayout &layout, double, double) const override
    {
        if (!m_target.isValid() || !model.hasTransition(m_target))
            return;
        const core::Transition &t = model.transition(m_target);
        if (!model.hasClip(t.a) || !model.hasClip(t.b))
            return;
        const auto &tracks = model.sequence().tracks;
        for (size_t row = 0; row < tracks.size(); ++row) {
            if (tracks[row].id != t.track)
                continue;
            const double x0 = viewport.xForFrame(static_cast<double>(model.clip(t.b).position));
            const double x1 = viewport.xForFrame(static_cast<double>(model.clip(t.a).end()));
            GskRoundedRect rounded;
            graphene_rect_t r = GRAPHENE_RECT_INIT(static_cast<float>(x0), static_cast<float>(layout.clipTop(static_cast<int>(row))),
                                                   static_cast<float>(x1 - x0), static_cast<float>(layout.clipHeight()));
            gsk_rounded_rect_init_from_rect(&rounded, &r, 2.0f);
            const float widths[4] = {2.0f, 2.0f, 2.0f, 2.0f};
            const GdkRGBA cyan{tokens::kBrandCyan.r, tokens::kBrandCyan.g, tokens::kBrandCyan.b, 1.0f};
            const GdkRGBA colors[4] = {cyan, cyan, cyan, cyan};
            gtk_snapshot_append_border(snapshot, &rounded, widths, colors);
        }
    }

    // A double-click on a transition's overlap opens this page on it (the
    // timeline's own double-click, renaming the clip, stays everywhere else).
    bool pressed(const core::Model &model, const app::timeline::Viewport &viewport,
                 const app::timeline::RowLayout &layout, double x, double y, int nPress) override
    {
        if (nPress < 2 || layout.inNameStrip(y) || layout.inLane(y))
            return false;
        const int row = layout.rowAt(y);
        const auto &tracks = model.sequence().tracks;
        if (row < 0 || static_cast<size_t>(row) >= tracks.size())
            return false;
        // By x, not frameForX(): Viewport's inline members only, since the
        // render tool links this drop-in without the shell.
        for (const core::Transition &t : model.sequence().transitions) {
            if (t.track != tracks[static_cast<size_t>(row)].id || !model.hasClip(t.a) || !model.hasClip(t.b))
                continue;
            if (x >= viewport.xForFrame(static_cast<double>(model.clip(t.b).position)) &&
                x < viewport.xForFrame(static_cast<double>(model.clip(t.a).end()))) {
                focus(t.id);
                return true;
            }
        }
        return false;
    }

  private:
    // The page is about `id`: the playhead goes to its middle, so the
    // preview shows it, and the inspector opens here.
    void focus(core::TransitionId id)
    {
        const core::Model &model = m_host.model();
        if (!model.hasTransition(id))
            return;
        const core::Transition &t = model.transition(id);
        m_pinned = id;
        m_host.seek(model.clip(t.b).position + t.length / 2);
        m_host.showInspectorPage("effects.transitions");
        refresh(true);
    }

    // The transition covering the playhead, on the active track first; else
    // the last one opened, while it exists.
    core::TransitionId resolveTarget() const
    {
        const core::Model &model = m_host.model();
        const core::FrameIndex frame = m_host.currentFrame();
        const std::optional<core::TrackId> active = m_host.currentSelection().track;
        core::TransitionId any;
        for (const core::Transition &t : model.sequence().transitions) {
            if (!model.hasClip(t.a) || !model.hasClip(t.b))
                continue;
            if (frame < model.clip(t.b).position || frame >= model.clip(t.a).end())
                continue;
            if (active && t.track == *active)
                return t.id;
            if (!any.isValid())
                any = t.id;
        }
        if (any.isValid())
            return any;
        if (m_pinned.isValid() && model.hasTransition(m_pinned))
            return m_pinned;
        return {};
    }

    // `rebuild`: the model changed, so the controls may have too; otherwise
    // only a change of target redraws anything.
    void refresh(bool rebuild)
    {
        const core::TransitionId target = resolveTarget();
        if (target == m_target && !rebuild)
            return;
        const bool moved = target != m_target;
        m_target = target;
        if (moved)
            m_host.redrawTimeline();
        const core::Model &model = m_host.model();
        const bool has = m_target.isValid() && model.hasTransition(m_target);
        for (GtkWidget *flow : m_flows)
            gtk_widget_set_sensitive(flow, has);
        if (!has) {
            gtk_label_set_text(GTK_LABEL(m_title), "No transition here");
            gtk_label_set_text(GTK_LABEL(m_subtitle),
                               "Put the playhead on a transition, double-click one on the timeline, or press T to "
                               "add one at the nearest cut.");
            for (GtkWidget *flow : m_flows)
                gtk_flow_box_unselect_all(GTK_FLOW_BOX(flow));
            gtk_widget_set_visible(m_gpuNote, FALSE);
            clearControls();
            return;
        }
        const core::Transition &t = model.transition(m_target);
        const int index = recipeIndexOf(m_recipes, t);
        const TransitionRecipe *recipe = index >= 0 ? &m_recipes[static_cast<size_t>(index)] : nullptr;
        gtk_label_set_text(GTK_LABEL(m_title), recipe ? recipe->name.c_str() : "Dissolve");
        auto nameOf = [&](core::ClipId id) {
            const core::Clip &clip = model.clip(id);
            if (!clip.name.empty())
                return clip.name;
            return model.hasAsset(clip.asset) ? model.asset(clip.asset).displayName : std::string("a clip");
        };
        const std::string subtitle = std::to_string(t.length) + " frames, from " + nameOf(t.a) + " to " + nameOf(t.b);
        gtk_label_set_text(GTK_LABEL(m_subtitle), subtitle.c_str());
        gtk_widget_set_visible(m_gpuNote, findParam(t.params, "video.luma") && gpuAccelerationSetting());
        // The current style selected, without activating it.
        for (GtkWidget *child : m_children) {
            GtkWidget *flow = gtk_widget_get_parent(child);
            const size_t i = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(child), "ustudio-recipe")) - 1;
            if (static_cast<int>(i) == index)
                gtk_flow_box_select_child(GTK_FLOW_BOX(flow), GTK_FLOW_BOX_CHILD(child));
            else
                gtk_flow_box_unselect_child(GTK_FLOW_BOX(flow), GTK_FLOW_BOX_CHILD(child));
        }
        if (m_dragging)
            return; // the control being dragged stays as it is
        clearControls();
        if (!recipe)
            return;
        for (const std::string &name : recipe->exposed) {
            const core::Param *param = findParam(t.params, name);
            if (!param)
                param = findParam(recipe->params, name);
            if (!param)
                continue;
            addControl(name, *param);
        }
    }

    void clearControls()
    {
        while (GtkWidget *child = gtk_widget_get_first_child(m_controls))
            gtk_box_remove(GTK_BOX(m_controls), child);
        m_controlNames.clear();
    }

    void addControl(const std::string &name, const core::Param &param)
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *label = gtk_label_new(displayName(name).c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_set_size_request(label, 80, -1);
        gtk_box_append(GTK_BOX(row), label);
        const size_t index = m_controlNames.size();
        m_controlNames.push_back(name);
        const char *hint = name.ends_with("softness") ? "effects.transitions-softness" : "effects.transitions-reverse";
        if (const auto *on = std::get_if<bool>(&param.value)) {
            GtkWidget *toggle = gtk_switch_new();
            gtk_switch_set_active(GTK_SWITCH(toggle), *on);
            gtk_widget_set_halign(toggle, GTK_ALIGN_START);
            g_object_set_data(G_OBJECT(toggle), "ustudio-control", GSIZE_TO_POINTER(index + 1));
            g_signal_connect(toggle, "notify::active", G_CALLBACK(&onSwitchTrampoline), this);
            gtk_accessible_update_property(GTK_ACCESSIBLE(toggle), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                           displayName(name).c_str(), -1);
            m_host.setTooltip(toggle, hint);
            gtk_box_append(GTK_BOX(row), toggle);
        } else if (const auto *value = std::get_if<double>(&param.value)) {
            GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.01);
            gtk_range_set_value(GTK_RANGE(scale), *value);
            gtk_scale_set_digits(GTK_SCALE(scale), 2);
            gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
            gtk_widget_set_hexpand(scale, TRUE);
            g_object_set_data(G_OBJECT(scale), "ustudio-control", GSIZE_TO_POINTER(index + 1));
            g_signal_connect(scale, "value-changed", G_CALLBACK(&onScaleTrampoline), this);
            gtk_accessible_update_property(GTK_ACCESSIBLE(scale), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                           displayName(name).c_str(), -1);
            m_host.setTooltip(scale, hint);
            // One undo step per drag: a gesture id from press to release.
            GtkGesture *click = gtk_gesture_click_new();
            gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
            g_signal_connect(click, "pressed", G_CALLBACK(&onScalePressedTrampoline), this);
            g_signal_connect(click, "stopped", G_CALLBACK(&onScaleReleasedTrampoline), this);
            g_signal_connect(click, "released", G_CALLBACK(&onScaleReleasedTrampoline), this);
            gtk_widget_add_controller(scale, GTK_EVENT_CONTROLLER(click));
            gtk_box_append(GTK_BOX(row), scale);
        } else {
            return;
        }
        gtk_box_append(GTK_BOX(m_controls), row);
    }

    void setParam(size_t control, core::Param::Value value, uint64_t gesture)
    {
        const core::Model &model = m_host.model();
        if (control >= m_controlNames.size() || !m_target.isValid() || !model.hasTransition(m_target))
            return;
        const core::Transition &t = model.transition(m_target);
        const int index = recipeIndexOf(m_recipes, t);
        if (index < 0)
            return;
        const TransitionRecipe &recipe = m_recipes[static_cast<size_t>(index)];
        // A transition whose params were never set (the plain dissolve as
        // it was made) starts from its recipe's.
        std::vector<core::Param> params = t.params.empty() ? recipe.params : t.params;
        params = withParam(std::move(params), core::Param{m_controlNames[control], std::move(value), {}});
        m_host.execute(std::make_unique<SetTransitionRecipe>(m_target, recipe.id, std::move(params), gesture));
    }

    void onTileActivated(GtkFlowBoxChild *child)
    {
        const size_t i = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(child), "ustudio-recipe"));
        if (i == 0 || i > m_recipes.size() || !m_target.isValid())
            return;
        const TransitionRecipe &recipe = m_recipes[i - 1];
        if (!m_host.execute(std::make_unique<SetTransitionRecipe>(m_target, recipe.id, recipe.params)))
            m_host.showStatus("Couldn't change that transition (is its track locked?)");
        else
            core::Log::debug("[effects] transition style: " + recipe.id);
    }

    // T: the default transition (a dissolve, about half a second) at the
    // cut nearest the playhead on the active track, as the timeline's Add
    // Transition makes it (AppWindow::onAddTransitionClicked()).
    void addAtNearestCut()
    {
        const core::Model &model = m_host.model();
        const std::optional<core::TrackId> track = m_host.currentSelection().track;
        if (!track || !model.hasTrack(*track)) {
            m_host.showStatus("Choose a track first: click its name.");
            return;
        }
        const std::vector<core::ClipId> &clips = model.track(*track).clips;
        const core::FrameIndex playhead = m_host.currentFrame();
        std::optional<size_t> best;
        core::FrameIndex bestDistance = 0;
        for (size_t i = 0; i + 1 < clips.size(); ++i) {
            const core::Clip &a = model.clip(clips[i]);
            const core::Clip &b = model.clip(clips[i + 1]);
            if (a.end() != b.position)
                continue; // a gap, or already overlapping (a transition)
            const core::FrameIndex distance = std::abs(b.position - playhead);
            if (!best || distance < bestDistance) {
                best = i;
                bestDistance = distance;
            }
        }
        if (!best) {
            m_host.showStatus("No cut on this track to add a transition to.");
            return;
        }
        const core::Clip &a = model.clip(clips[*best]);
        const core::Clip &b = model.clip(clips[*best + 1]);
        const core::Rational fps = model.sequence().profile.fps;
        const core::FrameIndex targetLength = std::max<core::FrameIndex>(
            1, static_cast<core::FrameIndex>(static_cast<double>(fps.num) / fps.den * 0.5 + 0.5));
        core::FrameIndex handleA = 0;
        if (model.hasAsset(a.asset)) {
            const core::Asset &asset = model.asset(a.asset);
            handleA = asset.info.isBoundless()
                          ? targetLength
                          : std::max<core::FrameIndex>(0, asset.info.lengthInSequenceFrames - 1 - a.out);
        }
        const core::FrameIndex handleB = b.in;
        core::FrameIndex extendA = std::min(targetLength / 2, handleA);
        const core::FrameIndex remaining = targetLength - extendA;
        const core::FrameIndex extendB = std::min(remaining, handleB);
        if (remaining - extendB > 0)
            extendA = std::min(handleA, extendA + (remaining - extendB));
        if (extendA + extendB <= 0) {
            m_host.showStatus("Couldn't add a transition there: neither clip has spare frames.");
            return;
        }
        auto command = std::make_unique<core::AddTransition>(*track, a.id, b.id, extendA, extendB);
        core::AddTransition *added = command.get();
        if (!m_host.execute(std::move(command))) {
            m_host.showStatus("Couldn't add a transition there.");
            return;
        }
        m_host.showStatus("Added a " + std::to_string(extendA + extendB) +
                          "-frame dissolve; pick its style on the Transitions page.");
        focus(added->transitionId());
    }

    // --- GTK signal trampolines ---------------------------------------------

    static void onAddActionTrampoline(GSimpleAction *, GVariant *, gpointer self)
    {
        static_cast<TransitionsPage *>(self)->addAtNearestCut();
    }
    static void onTileActivatedTrampoline(GtkFlowBox *, GtkFlowBoxChild *child, gpointer self)
    {
        static_cast<TransitionsPage *>(self)->onTileActivated(child);
    }
    static void onSwitchTrampoline(GtkSwitch *toggle, GParamSpec *, gpointer self)
    {
        auto *page = static_cast<TransitionsPage *>(self);
        const size_t control = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(toggle), "ustudio-control")) - 1;
        page->setParam(control, static_cast<bool>(gtk_switch_get_active(toggle)), 0);
    }
    static void onScaleTrampoline(GtkRange *range, gpointer self)
    {
        auto *page = static_cast<TransitionsPage *>(self);
        const size_t control = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(range), "ustudio-control")) - 1;
        page->setParam(control, gtk_range_get_value(range), page->m_dragging ? page->m_gesture : 0);
    }
    static void onScalePressedTrampoline(GtkGestureClick *, int, double, double, gpointer self)
    {
        auto *page = static_cast<TransitionsPage *>(self);
        page->m_dragging = true;
        ++page->m_gesture;
    }
    static void onScaleReleasedTrampoline(GtkGestureClick *, gpointer self)
    {
        auto *page = static_cast<TransitionsPage *>(self);
        page->m_dragging = false;
        page->refresh(true);
    }

    app::ShellHost &m_host;
    std::vector<TransitionRecipe> m_recipes;
    GtkWidget *m_root = nullptr, *m_title = nullptr, *m_subtitle = nullptr, *m_gpuNote = nullptr;
    GtkWidget *m_controls = nullptr, *m_tiles = nullptr;
    std::vector<GtkWidget *> m_flows, m_children;
    std::vector<std::string> m_controlNames;
    core::TransitionId m_target, m_pinned;
    uint64_t m_gesture = 0;
    bool m_dragging = false;
};

} // namespace

void addTransitions(app::ShellHost &host, const std::vector<TransitionRecipe> &recipes)
{
    if (recipes.empty())
        return;
    // For the window's life: its widgets and signal handlers point here.
    static std::vector<std::unique_ptr<TransitionsPage>> pages;
    pages.push_back(std::make_unique<TransitionsPage>(host, recipes));
    pages.back()->install();
}

} // namespace ustudio::effects
