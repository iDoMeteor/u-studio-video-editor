#include "app/rack.h"

#include "app/catalog.h"
#include "app/shell_host.h"
#include "core/commands.h"
#include "core/descriptor.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace ustudio::effects {

namespace {

// What the Rack edits: the selected clip, that clip's track, or the whole
// sequence (doc 15: "clip, several clips, track header ... or the
// sequence"; several clips at once is a later slice).
enum class Scope
{
    Clip,
    Track,
    Sequence,
};

// A new undo step when a control rests this long between changes; closer
// changes (a slider drag, typing) merge into one.
constexpr gint64 kGestureGapUs = 600'000;

std::string lower(std::string text)
{
    for (char &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

const char *costLabel(CostBadge badge)
{
    switch (badge) {
    case CostBadge::Light:
        return "light";
    case CostBadge::Medium:
        return "medium";
    case CostBadge::Heavy:
        return "heavy";
    }
    return "";
}

const char *costClass(CostBadge badge)
{
    switch (badge) {
    case CostBadge::Light:
        return "success";
    case CostBadge::Medium:
        return "warning";
    case CostBadge::Heavy:
        return "error";
    }
    return "";
}

class Rack;

// One control bound to one effect parameter (or the mix).
struct Control
{
    Rack *rack;
    core::EffectId effect;
    std::string param; // empty: the mix
    ParamKind kind = ParamKind::Scalar;
    std::optional<DisplayMap> display;
    GtkWidget *widget = nullptr;         // spin, switch, colour button, drop-down, entry
    GtkAdjustment *adjustment = nullptr; // scalar, integer, mix
    std::vector<GtkAdjustment *> rect;   // x, y, w, h
    std::vector<std::string> choices;
    uint64_t gesture = 0;
    gint64 lastChange = 0;
};

// A card's own buttons.
struct CardAction
{
    Rack *rack;
    core::EffectId effect;
    int move; // -1 up, +1 down, 0 remove
};

class Rack
{
  public:
    Rack(app::ShellHost &host, Catalog &catalog) : m_host(host), m_catalog(catalog) {}

    void install()
    {
        build();
        m_host.addHints({
            {"effects.rack-scope", "Effects", "Effects on",
             "The selected clip, its whole track, or the whole sequence (the finished picture)", nullptr, nullptr},
            {"effects.rack-add", "Effects", "Add effect", "Search every effect this install offers", "effects-browser",
             nullptr},
            {"effects.card-bypass", "Effects", "Effect on or off", "Off keeps the effect and its settings", nullptr,
             nullptr},
            {"effects.card-up", "Effects", "Move up", "Effects apply top to bottom", nullptr, nullptr},
            {"effects.card-down", "Effects", "Move down", "Effects apply top to bottom", nullptr, nullptr},
            {"effects.card-remove", "Effects", "Remove effect", nullptr, nullptr, nullptr},
            {"effects.card-mix", "Effects", "Mix",
             "How much of the effect shows: 0% is the picture without it, 100% the full effect", nullptr, nullptr},
            {"effects.card-cost", "Effects", "Cost",
             "How much work the effect is for each frame, measured when it was checked", nullptr, nullptr},
        });
        // E: the Effect Browser (doc 15, "Keyboard summary"); until FX2's
        // Browser lands it opens the Add search.
        static const std::vector<app::ActionSpec> actions = {
            {"effects-browser", "Add an effect", "Effects", {"e"}, &onBrowserActionTrampoline},
        };
        m_host.addActions(actions, this);
        m_host.setTooltip(m_scope, "effects.rack-scope");
        m_host.setTooltip(m_add, "effects.rack-add");
        m_host.addInspectorPage({"effects.rack", "Effects", "applications-graphics-symbolic", m_root});
        m_host.selectionChanged().connect([this] { refresh(); });
        m_host.projectChanged().connect([this] { refresh(); });
        m_catalog.changed.connect([this] {
            m_structure.clear(); // names and badges may have arrived
            refresh();
        });
        refresh();
    }

    void onScopeChanged()
    {
        m_structure.clear();
        refresh();
    }

    void onControlChanged(Control &control)
    {
        if (m_updating)
            return;
        const gint64 now = g_get_monotonic_time();
        if (control.gesture == 0 || now - control.lastChange > kGestureGapUs)
            control.gesture = ++m_nextGesture;
        control.lastChange = now;
        const core::Model &model = m_host.model();
        if (!model.hasEffect(control.effect))
            return;
        if (control.param.empty()) {
            core::KeyframedValue mix = model.effect(control.effect).mix;
            mix.value = std::clamp(gtk_adjustment_get_value(control.adjustment) / 100.0, 0.0, 1.0);
            m_host.execute(std::make_unique<SetMix>(control.effect, mix, control.gesture));
            return;
        }
        core::Param param = currentParam(model.effect(control.effect), control.param, control.kind);
        param.value = readControl(control);
        m_host.execute(std::make_unique<SetParam>(control.effect, param, control.gesture));
    }

    void onCardAction(const CardAction &action)
    {
        const core::Model &model = m_host.model();
        auto found = model.findEffect(action.effect);
        if (!found)
            return;
        if (action.move == 0) {
            m_host.execute(std::make_unique<RemoveEffect>(action.effect));
            return;
        }
        const size_t size = model.effects(found->first).size();
        const long index = static_cast<long>(found->second) + action.move;
        if (index < 0 || static_cast<size_t>(index) >= size)
            return;
        m_host.execute(std::make_unique<MoveEffect>(action.effect, static_cast<size_t>(index)));
    }

    void onBypass(core::EffectId effect, bool enabled)
    {
        if (m_updating)
            return;
        m_host.execute(std::make_unique<SetEffectEnabled>(effect, enabled));
    }

    void onSearchChanged()
    {
        fillAddList();
    }

    void onAddRow(GtkListBoxRow *row)
    {
        const char *service = static_cast<const char *>(g_object_get_data(G_OBJECT(row), "service"));
        const EffectDescriptor *descriptor = service ? m_catalog.find(service) : nullptr;
        std::optional<core::Model::EffectTarget> target = currentTarget();
        if (!descriptor || !target)
            return;
        gtk_popover_popdown(GTK_POPOVER(m_addPopover));
        const size_t end = m_host.model().effects(*target).size();
        if (!m_host.execute(std::make_unique<AddEffect>(*target, makeEffect(*descriptor), end)))
            m_host.showStatus("Couldn't add " + descriptor->name);
    }

    void onSearchActivate()
    {
        fillAddList(); // search-changed is delayed; the list must match the text now
        if (GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(m_addList), 0))
            if (g_object_get_data(G_OBJECT(first), "service"))
                onAddRow(first);
    }

    void onBrowserAction()
    {
        gtk_menu_button_popup(GTK_MENU_BUTTON(m_add));
    }

    void onAddShown()
    {
        gtk_editable_set_text(GTK_EDITABLE(m_search), "");
        fillAddList();
        gtk_widget_grab_focus(m_search);
    }

  private:
    // --- Target ------------------------------------------------------------

    std::optional<core::Model::EffectTarget> currentTarget() const
    {
        const core::Model &model = m_host.model();
        const app::ShellSelection selection = m_host.currentSelection();
        std::optional<core::ClipId> clip;
        for (core::ClipId id : selection.clips)
            if (model.hasClip(id)) {
                clip = id;
                break;
            }
        const auto scope = static_cast<Scope>(gtk_drop_down_get_selected(GTK_DROP_DOWN(m_scope)));
        if (scope == Scope::Clip && clip)
            return core::Model::EffectTarget::clip(*clip);
        if (scope != Scope::Sequence) {
            if (clip)
                return core::Model::EffectTarget::track(model.clip(*clip).track);
            if (selection.track && model.hasTrack(*selection.track))
                return core::Model::EffectTarget::track(*selection.track);
        }
        return core::Model::EffectTarget::sequence();
    }

    std::string targetTitle(const core::Model::EffectTarget &target) const
    {
        const core::Model &model = m_host.model();
        switch (target.kind) {
        case core::Model::EffectTarget::Kind::Clip:
            return "Clip: " + model.clip(core::ClipId{target.id}).name;
        case core::Model::EffectTarget::Kind::Track:
            return "Track: " + model.track(core::TrackId{target.id}).name;
        case core::Model::EffectTarget::Kind::Sequence:
            return "The whole sequence";
        case core::Model::EffectTarget::Kind::AdjustmentBlock:
            return "Adjustment block";
        }
        return "";
    }

    // --- Values ------------------------------------------------------------

    const ParamDescriptor *paramDescriptor(const std::string &service, const std::string &name) const
    {
        const EffectDescriptor *descriptor = m_catalog.find(service);
        if (!descriptor)
            return nullptr;
        for (const ParamDescriptor &p : descriptor->params)
            if (p.id == name)
                return &p;
        return nullptr;
    }

    // The effect's parameter, or the descriptor's default when it doesn't
    // carry one yet (left to the service until set).
    core::Param currentParam(const core::Effect &effect, const std::string &name, ParamKind kind) const
    {
        for (const core::Param &p : effect.params)
            if (p.name == name)
                return p;
        core::Param param;
        param.name = name;
        const ParamDescriptor *p = paramDescriptor(effect.service, name);
        param.value = p ? p->defaultValue : parseValue(kind, "");
        return param;
    }

    static double asNumber(const core::Param::Value &value)
    {
        if (const double *d = std::get_if<double>(&value))
            return *d;
        if (const int64_t *i = std::get_if<int64_t>(&value))
            return static_cast<double>(*i);
        if (const bool *b = std::get_if<bool>(&value))
            return *b ? 1.0 : 0.0;
        return 0.0;
    }

    core::Param::Value readControl(const Control &control) const
    {
        switch (control.kind) {
        case ParamKind::Scalar: {
            const double shown = gtk_adjustment_get_value(control.adjustment);
            return control.display ? control.display->fromDisplay(shown) : shown;
        }
        case ParamKind::Integer:
            return static_cast<int64_t>(std::llround(gtk_adjustment_get_value(control.adjustment)));
        case ParamKind::Toggle:
            return static_cast<bool>(gtk_switch_get_active(GTK_SWITCH(control.widget)));
        case ParamKind::Color: {
            const GdkRGBA *rgba = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(control.widget));
            auto channel = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
            return core::Color{channel(rgba->red), channel(rgba->green), channel(rgba->blue), channel(rgba->alpha)};
        }
        case ParamKind::Choice: {
            const guint index = gtk_drop_down_get_selected(GTK_DROP_DOWN(control.widget));
            return index < control.choices.size() ? control.choices[index] : std::string();
        }
        case ParamKind::Rect:
            return core::Rect{gtk_adjustment_get_value(control.rect[0]), gtk_adjustment_get_value(control.rect[1]),
                              gtk_adjustment_get_value(control.rect[2]), gtk_adjustment_get_value(control.rect[3])};
        case ParamKind::File:
        case ParamKind::Text:
            return std::string(gtk_editable_get_text(GTK_EDITABLE(control.widget)));
        }
        return 0.0;
    }

    void writeControl(Control &control, const core::Param::Value &value)
    {
        switch (control.kind) {
        case ParamKind::Scalar: {
            const double v = asNumber(value);
            gtk_adjustment_set_value(control.adjustment, control.display ? control.display->toDisplay(v) : v);
            break;
        }
        case ParamKind::Integer:
            gtk_adjustment_set_value(control.adjustment, asNumber(value));
            break;
        case ParamKind::Toggle:
            gtk_switch_set_active(GTK_SWITCH(control.widget), asNumber(value) != 0.0);
            break;
        case ParamKind::Color:
            if (const core::Color *c = std::get_if<core::Color>(&value)) {
                const GdkRGBA rgba{c->r / 255.0f, c->g / 255.0f, c->b / 255.0f, c->a / 255.0f};
                gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(control.widget), &rgba);
            }
            break;
        case ParamKind::Choice:
            if (const std::string *s = std::get_if<std::string>(&value)) {
                auto it = std::find(control.choices.begin(), control.choices.end(), *s);
                if (it != control.choices.end())
                    gtk_drop_down_set_selected(GTK_DROP_DOWN(control.widget),
                                               static_cast<guint>(it - control.choices.begin()));
            }
            break;
        case ParamKind::Rect:
            if (const core::Rect *r = std::get_if<core::Rect>(&value)) {
                gtk_adjustment_set_value(control.rect[0], r->x);
                gtk_adjustment_set_value(control.rect[1], r->y);
                gtk_adjustment_set_value(control.rect[2], r->w);
                gtk_adjustment_set_value(control.rect[3], r->h);
            }
            break;
        case ParamKind::File:
        case ParamKind::Text:
            if (const std::string *s = std::get_if<std::string>(&value);
                s && *s != gtk_editable_get_text(GTK_EDITABLE(control.widget)))
                gtk_editable_set_text(GTK_EDITABLE(control.widget), s->c_str());
            break;
        }
    }

    // --- Building ----------------------------------------------------------

    void build()
    {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_widget_set_margin_start(box, 12);
        gtk_widget_set_margin_end(box, 12);
        gtk_widget_set_margin_top(box, 12);
        gtk_widget_set_margin_bottom(box, 12);

        GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        const char *scopes[] = {"Selected clip", "Its track", "Whole sequence", nullptr};
        m_scope = gtk_drop_down_new_from_strings(scopes);
        gtk_widget_set_hexpand(m_scope, TRUE);
        g_signal_connect(m_scope, "notify::selected", G_CALLBACK(&onScopeTrampoline), this);
        gtk_box_append(GTK_BOX(header), m_scope);

        m_add = gtk_menu_button_new();
        gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(m_add), "list-add-symbolic");
        gtk_menu_button_set_label(GTK_MENU_BUTTON(m_add), "Add");
        m_addPopover = gtk_popover_new();
        GtkWidget *addBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        m_search = gtk_search_entry_new();
        g_signal_connect(m_search, "search-changed", G_CALLBACK(&onSearchTrampoline), this);
        // Enter adds the best match.
        g_signal_connect(m_search, "activate", G_CALLBACK(&onSearchActivateTrampoline), this);
        gtk_box_append(GTK_BOX(addBox), m_search);
        GtkWidget *scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 320);
        gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(scroll), 280);
        m_addList = gtk_list_box_new();
        g_signal_connect(m_addList, "row-activated", G_CALLBACK(&onAddRowTrampoline), this);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), m_addList);
        gtk_box_append(GTK_BOX(addBox), scroll);
        gtk_popover_set_child(GTK_POPOVER(m_addPopover), addBox);
        g_signal_connect(m_addPopover, "show", G_CALLBACK(&onAddShownTrampoline), this);
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(m_add), m_addPopover);
        gtk_box_append(GTK_BOX(header), m_add);
        gtk_box_append(GTK_BOX(box), header);

        m_title = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(m_title), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(m_title), PANGO_ELLIPSIZE_MIDDLE);
        gtk_widget_add_css_class(m_title, "heading");
        gtk_box_append(GTK_BOX(box), m_title);

        m_empty = gtk_label_new("No effects yet. Add one to change how this looks or sounds.");
        gtk_label_set_wrap(GTK_LABEL(m_empty), TRUE);
        gtk_label_set_xalign(GTK_LABEL(m_empty), 0.0f);
        gtk_widget_add_css_class(m_empty, "dim-label");
        gtk_box_append(GTK_BOX(box), m_empty);

        m_cards = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_box_append(GTK_BOX(box), m_cards);

        m_root = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(m_root), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(m_root), box);
    }

    void fillAddList()
    {
        while (GtkWidget *child = gtk_widget_get_first_child(m_addList))
            gtk_list_box_remove(GTK_LIST_BOX(m_addList), child);
        if (!m_catalog.ready()) {
            GtkWidget *wait = gtk_label_new("Still finding the effects this install offers…");
            gtk_widget_add_css_class(wait, "dim-label");
            gtk_list_box_append(GTK_LIST_BOX(m_addList), wait);
            return;
        }
        const std::string query = lower(gtk_editable_get_text(GTK_EDITABLE(m_search)));
        // Best first: the name itself, then names starting with the query,
        // then any other match (name, category, service or tag).
        std::vector<std::pair<int, const EffectDescriptor *>> matches;
        for (const EffectDescriptor *d : m_catalog.offered()) {
            const std::string name = lower(d->name);
            std::string haystack = name + " " + lower(d->category + " " + d->service);
            for (const std::string &tag : d->tags)
                haystack += " " + lower(tag);
            int rank = 3;
            if (query.empty())
                rank = d->featured ? 0 : 1;
            else if (name == query)
                rank = 0;
            else if (name.starts_with(query))
                rank = 1;
            else if (haystack.find(query) != std::string::npos)
                rank = 2;
            if (rank < 3)
                matches.emplace_back(rank, d);
        }
        std::stable_sort(matches.begin(), matches.end(),
                         [](const auto &a, const auto &b) { return a.first < b.first; });
        int shown = 0;
        for (const auto &[rank, d] : matches) {
            if (++shown > 300)
                break;
            GtkWidget *row = gtk_list_box_row_new();
            GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            GtkWidget *name = gtk_label_new(d->name.c_str());
            gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
            gtk_widget_set_hexpand(name, TRUE);
            gtk_box_append(GTK_BOX(line), name);
            GtkWidget *category = gtk_label_new(d->category.c_str());
            gtk_widget_add_css_class(category, "dim-label");
            gtk_widget_add_css_class(category, "caption");
            gtk_box_append(GTK_BOX(line), category);
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), line);
            gtk_widget_set_tooltip_text(row, d->description.empty() ? nullptr : d->description.c_str());
            g_object_set_data_full(G_OBJECT(row), "service", g_strdup(d->service.c_str()), g_free);
            gtk_accessible_update_property(GTK_ACCESSIBLE(row), GTK_ACCESSIBLE_PROPERTY_LABEL, d->name.c_str(), -1);
            gtk_list_box_append(GTK_LIST_BOX(m_addList), row);
        }
        if (shown == 0) {
            GtkWidget *none = gtk_label_new("No effect matches");
            gtk_widget_add_css_class(none, "dim-label");
            gtk_list_box_append(GTK_LIST_BOX(m_addList), none);
        }
    }

    // What the cards show, minus values: rebuilt when this changes, else
    // only the values move (so a drag keeps its widget).
    std::string structureOf(const core::Model::EffectTarget &target) const
    {
        std::string key = std::to_string(static_cast<int>(target.kind)) + ":" + std::to_string(target.id);
        for (const core::Effect &e : m_host.model().effects(target)) {
            key += "|" + std::to_string(e.id.value) + e.service + (e.enabled ? "+" : "-") +
                   (e.mix.keyframes.empty() ? "" : "k");
            for (const core::Param &p : e.params)
                key += "," + p.name + (p.keyframes.empty() ? "" : "k");
        }
        return key;
    }

    void refresh()
    {
        std::optional<core::Model::EffectTarget> target = currentTarget();
        if (!target || !m_host.model().hasEffectTarget(*target))
            return;
        gtk_label_set_text(GTK_LABEL(m_title), targetTitle(*target).c_str());
        const std::string structure = structureOf(*target);
        if (structure == m_structure) {
            updateValues();
            return;
        }
        // From an idle: this may run inside a card's own signal handler
        // (remove, bypass), which must not destroy its widget under itself.
        if (!m_rebuildPending) {
            m_rebuildPending = true;
            g_idle_add(&onRebuildTrampoline, this);
        }
    }

    void rebuildNow()
    {
        m_rebuildPending = false;
        std::optional<core::Model::EffectTarget> target = currentTarget();
        if (!target || !m_host.model().hasEffectTarget(*target))
            return;
        m_structure = structureOf(*target);
        rebuildCards(*target);
    }

    void rebuildCards(const core::Model::EffectTarget &target)
    {
        while (GtkWidget *child = gtk_widget_get_first_child(m_cards))
            gtk_box_remove(GTK_BOX(m_cards), child);
        m_controls.clear();
        m_actions.clear();
        const std::vector<core::Effect> &effects = m_host.model().effects(target);
        gtk_widget_set_visible(m_empty, effects.empty());
        m_updating = true;
        for (size_t i = 0; i < effects.size(); ++i)
            gtk_box_append(GTK_BOX(m_cards), buildCard(effects[i], i, effects.size()));
        m_updating = false;
    }

    GtkWidget *iconButton(const char *icon, const char *hint, core::EffectId effect, int move, bool sensitive)
    {
        GtkWidget *button = gtk_button_new_from_icon_name(icon);
        gtk_widget_add_css_class(button, "flat");
        gtk_widget_set_sensitive(button, sensitive);
        m_host.setTooltip(button, hint);
        m_actions.push_back(std::make_unique<CardAction>(CardAction{this, effect, move}));
        g_signal_connect(button, "clicked", G_CALLBACK(&onCardActionTrampoline), m_actions.back().get());
        return button;
    }

    GtkWidget *buildCard(const core::Effect &effect, size_t index, size_t count)
    {
        const EffectDescriptor *descriptor = m_catalog.find(effect.service);
        const bool ours = effect.owner == kOwner;

        GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_add_css_class(card, "card");
        GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_margin_start(inner, 10);
        gtk_widget_set_margin_end(inner, 10);
        gtk_widget_set_margin_top(inner, 8);
        gtk_widget_set_margin_bottom(inner, 8);
        gtk_box_append(GTK_BOX(card), inner);

        GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *bypass = gtk_switch_new();
        gtk_switch_set_active(GTK_SWITCH(bypass), effect.enabled);
        gtk_widget_set_valign(bypass, GTK_ALIGN_CENTER);
        gtk_widget_set_sensitive(bypass, ours);
        m_host.setTooltip(bypass, "effects.card-bypass");
        g_object_set_data(G_OBJECT(bypass), "rack", this);
        g_signal_connect(bypass, "notify::active", G_CALLBACK(&onBypassTrampoline),
                         GSIZE_TO_POINTER(static_cast<gsize>(effect.id.value)));
        gtk_box_append(GTK_BOX(header), bypass);

        const std::string name = !effect.displayName.empty() ? effect.displayName
                                 : descriptor                ? descriptor->name
                                                             : effect.service;
        GtkWidget *label = gtk_label_new(name.c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_widget_add_css_class(label, "heading");
        if (!effect.enabled)
            gtk_widget_add_css_class(label, "dim-label");
        gtk_box_append(GTK_BOX(header), label);

        if (std::optional<HealthRecord> health = m_catalog.health(effect.service); health && health->usable()) {
            const CostBadge badge = costBadge(health->msPerFrame);
            GtkWidget *cost = gtk_label_new(costLabel(badge));
            gtk_widget_add_css_class(cost, "caption");
            gtk_widget_add_css_class(cost, costClass(badge));
            m_host.setTooltip(cost, "effects.card-cost");
            gtk_box_append(GTK_BOX(header), cost);
        }
        gtk_box_append(GTK_BOX(header), iconButton("go-up-symbolic", "effects.card-up", effect.id, -1, index > 0));
        gtk_box_append(GTK_BOX(header),
                       iconButton("go-down-symbolic", "effects.card-down", effect.id, +1, index + 1 < count));
        gtk_box_append(GTK_BOX(header), iconButton("user-trash-symbolic", "effects.card-remove", effect.id, 0, true));
        gtk_box_append(GTK_BOX(inner), header);

        // Why it may not play (the extension skips these; doc 15, "Gating").
        std::string note;
        if (!ours)
            note = "Added by the " + effect.owner + " drop-in; its settings are its own.";
        else if (!descriptor && m_catalog.ready())
            note = "Not available on this computer: it plays without this effect.";
        else if (descriptor && !m_catalog.usable(effect.service))
            note = "Turned off: it failed the stability check on this computer.";
        if (!note.empty()) {
            GtkWidget *warning = gtk_label_new(note.c_str());
            gtk_label_set_wrap(GTK_LABEL(warning), TRUE);
            gtk_label_set_xalign(GTK_LABEL(warning), 0.0f);
            gtk_widget_add_css_class(warning, "warning");
            gtk_box_append(GTK_BOX(inner), warning);
        }
        if (!ours)
            return card;

        GtkWidget *grid = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
        gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
        int row = 0;
        // The mix: video effects only (MLT's mask pair composites pictures).
        if (!descriptor || descriptor->media == MediaKind::Video) {
            auto control = std::make_unique<Control>();
            control->rack = this;
            control->effect = effect.id;
            control->adjustment = gtk_adjustment_new(effect.mix.value * 100.0, 0.0, 100.0, 1.0, 10.0, 0.0);
            GtkWidget *scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, control->adjustment);
            gtk_scale_set_digits(GTK_SCALE(scale), 0);
            gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
            gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
            gtk_widget_set_hexpand(scale, TRUE);
            gtk_widget_set_sensitive(scale, effect.mix.keyframes.empty());
            m_host.setTooltip(scale, "effects.card-mix");
            control->widget = scale;
            g_signal_connect(control->adjustment, "value-changed", G_CALLBACK(&onControlTrampoline), control.get());
            addRow(grid, row++, "Mix", scale, !effect.mix.keyframes.empty());
            m_controls.push_back(std::move(control));
        }
        if (descriptor)
            for (const ParamDescriptor &p : descriptor->params) {
                if (p.hidden)
                    continue;
                const core::Param current = currentParam(effect, p.id, p.kind);
                GtkWidget *widget = buildControl(effect.id, p, current);
                if (widget)
                    addRow(grid, row++, p.title, widget, !current.keyframes.empty(), p.description);
            }
        gtk_box_append(GTK_BOX(inner), grid);
        return card;
    }

    void addRow(GtkWidget *grid, int row, const std::string &title, GtkWidget *widget, bool animated,
                const std::string &description = {})
    {
        GtkWidget *label = gtk_label_new(title.c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(label), 10);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 16);
        if (!description.empty())
            gtk_widget_set_tooltip_text(label, description.c_str());
        gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), widget, 1, row, 1, 1);
        if (animated) {
            // Keyframes are edited in a later slice; until then an animated
            // value shows but doesn't change.
            gtk_widget_set_sensitive(widget, FALSE);
            gtk_widget_set_tooltip_text(widget, "Animated: this value changes over time");
        }
    }

    GtkWidget *buildControl(core::EffectId effect, const ParamDescriptor &p, const core::Param &current)
    {
        auto control = std::make_unique<Control>();
        control->rack = this;
        control->effect = effect;
        control->param = p.id;
        control->kind = p.kind;
        control->display = p.display;
        GtkWidget *widget = nullptr;
        switch (p.kind) {
        case ParamKind::Scalar: {
            double lo = p.minimum.value_or(0.0), hi = p.maximum.value_or(std::max(1.0, lo + 1.0));
            if (p.display) {
                lo = p.display->toMin;
                hi = p.display->toMax;
            }
            const double span = hi - lo;
            control->adjustment = gtk_adjustment_new(lo, lo, hi, span / 100.0, span / 10.0, 0.0);
            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
            GtkWidget *scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, control->adjustment);
            gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
            gtk_widget_set_hexpand(scale, TRUE);
            gtk_box_append(GTK_BOX(box), scale);
            GtkWidget *spin = gtk_spin_button_new(control->adjustment, span / 100.0, span <= 10.0 ? 3 : 1);
            gtk_box_append(GTK_BOX(box), spin);
            if (p.display && !p.display->unit.empty()) {
                GtkWidget *unit = gtk_label_new(p.display->unit.c_str());
                gtk_widget_add_css_class(unit, "dim-label");
                gtk_box_append(GTK_BOX(box), unit);
            }
            widget = box;
            g_signal_connect(control->adjustment, "value-changed", G_CALLBACK(&onControlTrampoline), control.get());
            break;
        }
        case ParamKind::Integer: {
            const double lo = p.minimum.value_or(-1'000'000.0), hi = p.maximum.value_or(1'000'000.0);
            control->adjustment = gtk_adjustment_new(lo, lo, hi, 1.0, 10.0, 0.0);
            widget = gtk_spin_button_new(control->adjustment, 1.0, 0);
            g_signal_connect(control->adjustment, "value-changed", G_CALLBACK(&onControlTrampoline), control.get());
            break;
        }
        case ParamKind::Toggle:
            widget = gtk_switch_new();
            gtk_widget_set_halign(widget, GTK_ALIGN_START);
            g_signal_connect(widget, "notify::active", G_CALLBACK(&onControlNotifyTrampoline), control.get());
            break;
        case ParamKind::Color:
            widget = gtk_color_dialog_button_new(gtk_color_dialog_new());
            gtk_widget_set_halign(widget, GTK_ALIGN_START);
            g_signal_connect(widget, "notify::rgba", G_CALLBACK(&onControlNotifyTrampoline), control.get());
            break;
        case ParamKind::Choice: {
            control->choices = p.choices;
            std::vector<const char *> strings;
            for (const std::string &c : p.choices)
                strings.push_back(c.c_str());
            strings.push_back(nullptr);
            widget = gtk_drop_down_new_from_strings(strings.data());
            g_signal_connect(widget, "notify::selected", G_CALLBACK(&onControlNotifyTrampoline), control.get());
            break;
        }
        case ParamKind::Rect: {
            widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
            for (int i = 0; i < 4; ++i) {
                GtkAdjustment *adjustment = gtk_adjustment_new(0.0, -100'000.0, 100'000.0, 1.0, 10.0, 0.0);
                control->rect.push_back(adjustment);
                GtkWidget *spin = gtk_spin_button_new(adjustment, 1.0, 0);
                gtk_editable_set_width_chars(GTK_EDITABLE(spin), 5);
                gtk_box_append(GTK_BOX(widget), spin);
                g_signal_connect(adjustment, "value-changed", G_CALLBACK(&onControlTrampoline), control.get());
            }
            break;
        }
        case ParamKind::File:
        case ParamKind::Text:
            widget = gtk_entry_new();
            gtk_widget_set_hexpand(widget, TRUE);
            g_signal_connect(widget, "changed", G_CALLBACK(&onControlTrampoline), control.get());
            break;
        }
        control->widget = control->widget ? control->widget : widget;
        writeControl(*control, current.value);
        m_controls.push_back(std::move(control));
        return widget;
    }

    void updateValues()
    {
        const core::Model &model = m_host.model();
        m_updating = true;
        for (const std::unique_ptr<Control> &control : m_controls) {
            if (!model.hasEffect(control->effect))
                continue;
            const core::Effect &effect = model.effect(control->effect);
            if (control->param.empty())
                gtk_adjustment_set_value(control->adjustment, effect.mix.value * 100.0);
            else
                writeControl(*control, currentParam(effect, control->param, control->kind).value);
        }
        m_updating = false;
    }

    // --- GTK signal trampolines ---------------------------------------------

    static void onSearchActivateTrampoline(GtkSearchEntry *, gpointer self)
    {
        static_cast<Rack *>(self)->onSearchActivate();
    }
    static void onBrowserActionTrampoline(GSimpleAction *, GVariant *, gpointer self)
    {
        static_cast<Rack *>(self)->onBrowserAction();
    }
    static gboolean onRebuildTrampoline(gpointer self)
    {
        static_cast<Rack *>(self)->rebuildNow();
        return G_SOURCE_REMOVE;
    }
    static void onScopeTrampoline(GObject *, GParamSpec *, gpointer self)
    {
        static_cast<Rack *>(self)->onScopeChanged();
    }
    static void onSearchTrampoline(GtkSearchEntry *, gpointer self)
    {
        static_cast<Rack *>(self)->onSearchChanged();
    }
    static void onAddRowTrampoline(GtkListBox *, GtkListBoxRow *row, gpointer self)
    {
        static_cast<Rack *>(self)->onAddRow(row);
    }
    static void onAddShownTrampoline(GtkWidget *, gpointer self)
    {
        static_cast<Rack *>(self)->onAddShown();
    }
    static void onControlTrampoline(gpointer, gpointer control)
    {
        auto *c = static_cast<Control *>(control);
        c->rack->onControlChanged(*c);
    }
    static void onControlNotifyTrampoline(GObject *, GParamSpec *, gpointer control)
    {
        auto *c = static_cast<Control *>(control);
        c->rack->onControlChanged(*c);
    }
    static void onCardActionTrampoline(GtkButton *, gpointer action)
    {
        auto *a = static_cast<CardAction *>(action);
        a->rack->onCardAction(*a);
    }
    static void onBypassTrampoline(GObject *widget, GParamSpec *, gpointer effect)
    {
        auto *rack = static_cast<Rack *>(g_object_get_data(widget, "rack"));
        rack->onBypass(core::EffectId{static_cast<uint64_t>(GPOINTER_TO_SIZE(effect))},
                       gtk_switch_get_active(GTK_SWITCH(widget)));
    }

    app::ShellHost &m_host;
    Catalog &m_catalog;
    GtkWidget *m_root = nullptr, *m_scope = nullptr, *m_add = nullptr, *m_addPopover = nullptr;
    GtkWidget *m_search = nullptr, *m_addList = nullptr, *m_title = nullptr, *m_empty = nullptr;
    GtkWidget *m_cards = nullptr;
    std::vector<std::unique_ptr<Control>> m_controls;
    std::vector<std::unique_ptr<CardAction>> m_actions;
    std::string m_structure;
    bool m_updating = false;
    bool m_rebuildPending = false;
    uint64_t m_nextGesture = 0;
};

} // namespace

void addRack(app::ShellHost &host, Catalog &catalog)
{
    // For the window's life: its widgets and signal handlers point here.
    static std::vector<std::unique_ptr<Rack>> racks;
    racks.push_back(std::make_unique<Rack>(host, catalog));
    racks.back()->install();
}

} // namespace ustudio::effects
