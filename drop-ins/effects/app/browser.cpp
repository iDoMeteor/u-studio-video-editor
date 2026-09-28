#include "app/browser.h"

#include "app/catalog.h"
#include "app/shell_host.h"
#include "core/commands.h"
#include "core/descriptor.h"
#include "core/log.h"
#include "engine/frame_renderer.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace ustudio::effects {

namespace {

constexpr int kTileWidth = 144;
constexpr int kTileHeight = 81;
// The audition's size: large enough to judge on the preview, small enough
// to render in a few frames' time.
constexpr int kAuditionWidth = 960;
constexpr int kAuditionHeight = 540;
constexpr int kRecentMax = 12;
// Sections before the categories.
enum Section
{
    Featured,
    Recent,
    Looks,
    All,
    FirstCategory,
};
// A tile's drag payload: what it adds (Catalog::effectsFor()).
constexpr std::string_view kDragPrefix = "ustudio-effects:";

std::string lower(std::string text)
{
    for (char &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

GdkTexture *textureFor(const RenderedFrame &frame)
{
    GBytes *bytes = g_bytes_new(frame.rgba.data(), frame.rgba.size());
    GdkTexture *texture = gdk_memory_texture_new(frame.width, frame.height, GDK_MEMORY_R8G8B8A8, bytes,
                                                 static_cast<gsize>(frame.width) * 4);
    g_bytes_unref(bytes);
    return texture;
}

class Browser;

struct Tile
{
    Browser *browser;
    std::string item;                      // an effect's service, or a look (Catalog::effectsFor())
    std::optional<std::string> fixedBadge; // "look"
    GtkWidget *child = nullptr;            // the GtkFlowBoxChild
    GtkWidget *picture = nullptr;          // GtkPicture
    GtkWidget *badge = nullptr;            // its cost, "checking…", or "look"
};

class Browser
{
  public:
    Browser(app::ShellHost &host, Catalog &catalog) : m_host(host), m_catalog(catalog) {}

    void install()
    {
        build();
        m_host.addHints({
            {"effects.browser-search", "Effects", "Search effects",
             "By name, category or tag; Enter adds the best match", "effects-browser", nullptr},
            {"effects.browser-section", "Effects", "Show",
             "Featured picks, what you added lately, looks, or a category", nullptr, nullptr},
            {"effects.browser-unstable", "Effects", "Show unstable effects",
             "Effects that crashed, hung or misbehaved in the stability check on this computer; they may take "
             "the editor down",
             nullptr, nullptr},
            {"effects.browser-tile", "Effects", "Effect or look",
             "Point at it to try it on the picture; click or press Enter to add it to the selected clips, or drag "
             "it onto the picture or the Effects page",
             nullptr, "Hover to preview on the picture; drag onto the picture to add"},
        });
        // E: the Browser (doc 15, "Keyboard summary").
        static const std::vector<app::ActionSpec> actions = {
            {"effects-browser", "Effect Browser", "Effects", {"e"}, &onBrowserActionTrampoline},
        };
        m_host.addActions(actions, this);
        m_host.setTooltip(m_search, "effects.browser-search");
        m_host.setTooltip(m_section, "effects.browser-section");
        m_host.setTooltip(m_unstable, "effects.browser-unstable");
        m_host.addInspectorPage({"effects.browser", "Add", "list-add-symbolic", m_root});

        // The audition, over the preview (the same size; a GtkPicture that
        // fits its frame the way the preview does).
        m_audition = gtk_picture_new();
        gtk_picture_set_content_fit(GTK_PICTURE(m_audition), GTK_CONTENT_FIT_CONTAIN);
        gtk_widget_set_can_target(m_audition, FALSE);
        gtk_widget_set_visible(m_audition, FALSE);
        m_host.addPreviewOverlay(m_audition);
        // A drop zone over the picture that takes the pointer only while one
        // of our tiles is being dragged (the preview's own handles work the
        // rest of the time).
        m_dropZone = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_accessible_update_property(GTK_ACCESSIBLE(m_dropZone), GTK_ACCESSIBLE_PROPERTY_LABEL, "Effects drop zone",
                                       -1);
        gtk_widget_set_can_target(m_dropZone, FALSE);
        GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_COPY);
        g_signal_connect(drop, "drop", G_CALLBACK(&onPreviewDropTrampoline), this);
        gtk_widget_add_controller(m_dropZone, GTK_EVENT_CONTROLLER(drop));
        m_host.addPreviewOverlay(m_dropZone);

        m_catalog.changed.connect([this] { scheduleRefill(); });
        m_catalog.healthChanged.connect([this](const std::string &service) { onHealthChanged(service); });
        m_host.selectionChanged().connect([this] { onSourceChanged(); });
        m_host.projectChanged().connect([this] { onProjectChanged(); });
        m_host.playheadMoved().connect([this] { onPlayheadMoved(); });
        // The renderer's thread holds MLT producers: it stops with the
        // window, before Mlt::Factory::close().
        g_signal_connect(m_root, "destroy", G_CALLBACK(&onDestroyTrampoline), this);
        refill();
    }

    // One effect checked: its tiles' badges, and pictures now that it may
    // run here (an effect's tile, and any look using it).
    void onHealthChanged(const std::string &service)
    {
        const core::Model &model = m_host.model();
        for (const std::unique_ptr<Tile> &tile : m_tiles) {
            const std::vector<std::string> services = m_catalog.servicesOf(model, tile->item);
            if (std::find(services.begin(), services.end(), service) == services.end())
                continue;
            setBadge(*tile);
            requestTile(*tile, m_tileGeneration);
        }
    }

    void onBrowserAction()
    {
        m_host.showInspectorPage("effects.browser");
        gtk_widget_grab_focus(m_search);
        gtk_editable_select_region(GTK_EDITABLE(m_search), 0, -1);
    }

    void onDestroy()
    {
        stopAudition();
        m_renderer.stop();
    }

    void scheduleRefill()
    {
        if (m_refillPending)
            return;
        m_refillPending = true;
        g_idle_add(&onRefillTrampoline, this);
    }

    // The project's looks may have changed (Save as Look): the Looks section
    // lists them; any edit also changes the source frame.
    void onProjectChanged()
    {
        const size_t looks = m_host.model().project().looks.size();
        if (looks != m_projectLooks) {
            m_projectLooks = looks;
            scheduleRefill();
        }
        onSourceChanged();
    }

    // A new source frame (selection, edit, a paused playhead): the tiles
    // re-render from it, a moment later so a scrub doesn't flood the worker.
    void onSourceChanged()
    {
        if (m_sourceTimer)
            g_source_remove(m_sourceTimer);
        m_sourceTimer = g_timeout_add(250, &onSourceTimerTrampoline, this);
    }

    void onPlayheadMoved()
    {
        const core::FrameIndex frame = m_host.currentFrame();
        if (frame == m_lastFrame)
            return;
        m_lastFrame = frame;
        onSourceChanged();
    }

    void onSourceTimer()
    {
        m_sourceTimer = 0;
        stopAudition();
        requestTiles();
    }

    // --- Audition and apply --------------------------------------------------

    void audition(const std::string &item)
    {
        std::optional<FrameRequest> request = requestFor(item, kAuditionWidth, kAuditionHeight);
        if (!request)
            return;
        m_auditioning = item;
        const uint64_t generation = ++m_auditionGeneration;
        m_renderer.request(std::move(*request), 0, generation, [this, generation](const RenderedFrame &frame) {
            if (generation != m_auditionGeneration || m_auditioning.empty() || frame.rgba.empty())
                return;
            GdkTexture *texture = textureFor(frame);
            gtk_picture_set_paintable(GTK_PICTURE(m_audition), GDK_PAINTABLE(texture));
            g_object_unref(texture);
            gtk_widget_set_visible(m_audition, TRUE);
            core::Log::debug("[effects] auditioning " + m_auditioning + " on the preview");
        });
    }

    void stopAudition()
    {
        m_auditioning.clear();
        ++m_auditionGeneration;
        if (m_audition)
            gtk_widget_set_visible(m_audition, FALSE);
    }

    void apply(const std::string &item)
    {
        stopAudition();
        applyTo(item, applyTargets());
    }

    // Adds `item` (an effect or a look) to each target, as one undo step.
    void applyTo(const std::string &item, const std::vector<core::Model::EffectTarget> &targets)
    {
        const core::Model &model = m_host.model();
        const std::vector<core::Effect> effects = m_catalog.effectsFor(model, item);
        const std::string name = m_catalog.nameOf(model, item);
        if (effects.empty() || targets.empty())
            return;
        const bool look = item.starts_with("look:");
        if (!m_host.execute(
                pasteEffects(model, targets, effects, PasteMode::Append, (look ? "Apply look " : "Add ") + name))) {
            m_host.showStatus("Couldn't add " + name);
            return;
        }
        std::erase(m_recent, item);
        m_recent.insert(m_recent.begin(), item);
        if (m_recent.size() > static_cast<size_t>(kRecentMax))
            m_recent.resize(kRecentMax);
        const core::Model::EffectTarget &target = targets.front();
        m_host.showStatus((look ? "Applied " : "Added ") + name +
                          (target.kind != core::Model::EffectTarget::Kind::Clip ? " to the whole sequence"
                           : targets.size() > 1 ? " to " + std::to_string(targets.size()) + " clips"
                                                : " to the clip"));
    }

    void onSearchActivate()
    {
        refill();
        if (!m_tiles.empty())
            apply(m_tiles.front()->item);
    }

    void onChildActivated(GtkFlowBoxChild *child)
    {
        if (const char *item = static_cast<const char *>(g_object_get_data(G_OBJECT(child), "item")))
            apply(item);
    }

    void onSelectionChanged()
    {
        GList *selected = gtk_flow_box_get_selected_children(GTK_FLOW_BOX(m_grid));
        if (selected)
            if (const char *item = static_cast<const char *>(g_object_get_data(G_OBJECT(selected->data), "item")))
                audition(item);
        g_list_free(selected);
    }

    void onKeyEscape()
    {
        stopAudition();
        gtk_flow_box_unselect_all(GTK_FLOW_BOX(m_grid));
    }

    // --- Dropping on the picture ----------------------------------------------

    void onDragBegin()
    {
        core::Log::debug("[effects] dragging a Browser tile");
        gtk_widget_set_can_target(m_dropZone, TRUE);
        stopAudition();
    }

    void onDragEnd()
    {
        gtk_widget_set_can_target(m_dropZone, FALSE);
    }

    // A drop on the picture: the topmost clip under the playhead (doc 15:
    // "the topmost visible clip under the pointer"; at the playhead, since
    // the frame's clips aren't mapped to the picture yet).
    bool onPreviewDrop(const std::string &payload)
    {
        if (!payload.starts_with(kDragPrefix))
            return false;
        const std::string item = payload.substr(kDragPrefix.size());
        core::Log::debug("[effects] " + item + " dropped on the picture");
        const core::Model &model = m_host.model();
        const core::FrameIndex frame = m_host.currentFrame();
        std::optional<core::ClipId> top;
        for (const core::Track &track : model.sequence().tracks) {
            if (track.kind != core::Track::Kind::Video || track.hidden)
                continue;
            for (core::ClipId id : track.clips) {
                const core::Clip &clip = model.clip(id);
                if (frame >= clip.position && frame < clip.position + clip.length())
                    top = id; // later tracks are higher
            }
        }
        applyTo(item, {top ? core::Model::EffectTarget::clip(*top) : core::Model::EffectTarget::sequence()});
        return true;
    }

    void refill()
    {
        m_refillPending = false;
        while (GtkWidget *child = gtk_widget_get_first_child(m_grid))
            gtk_flow_box_remove(GTK_FLOW_BOX(m_grid), child);
        m_tiles.clear();
        rebuildSections();
        if (!m_catalog.ready()) {
            gtk_label_set_text(GTK_LABEL(m_status), "Finding the effects this install offers…");
            return;
        }
        const std::string query = lower(gtk_editable_get_text(GTK_EDITABLE(m_search)));
        const guint section = gtk_drop_down_get_selected(GTK_DROP_DOWN(m_section));
        const bool unstable = gtk_check_button_get_active(GTK_CHECK_BUTTON(m_unstable));

        // Tiles to show, best first. A search ranks effects and looks alike
        // (the name itself, then names starting with it, then any match),
        // an effect before a look of the same rank; without one, the
        // section's own order.
        struct Entry
        {
            int rank;
            std::string item, title, description;
            std::optional<std::string> badge;
        };
        std::vector<Entry> entries;
        auto rankOf = [&](const std::string &name, const std::string &haystack) {
            if (name == query)
                return 0;
            if (name.starts_with(query))
                return 1;
            if (haystack.find(query) != std::string::npos)
                return 2;
            return -1;
        };

        const core::Model &model = m_host.model();
        auto addLooks = [&](const std::vector<core::Look> &looks, const std::string &prefix, bool byIndex) {
            for (size_t i = 0; i < looks.size(); ++i) {
                const std::string name = lower(looks[i].name);
                const int rank = query.empty() ? (section == Looks ? 0 : -1) : rankOf(name, name);
                if (rank >= 0)
                    entries.push_back({rank * 2 + 1, prefix + std::to_string(byIndex ? i : looks[i].id.value),
                                       looks[i].name, "A look: " + std::to_string(looks[i].effects.size()) + " effects",
                                       "look"});
            }
        };
        addLooks(m_catalog.brandLooks(), "look:brand:", true);
        addLooks(model.project().looks, "look:project:", false);

        for (const EffectDescriptor *d : m_catalog.offered(unstable)) {
            if (query.empty() && !inSection(*d, section))
                continue;
            const std::string name = lower(d->name);
            std::string haystack = name + " " + lower(d->category + " " + d->service);
            for (const std::string &tag : d->tags)
                haystack += " " + lower(tag);
            const int rank = query.empty() ? (section == Recent ? recentRank(d->service) : 0) : rankOf(name, haystack);
            if (rank >= 0)
                entries.push_back({rank * 2, d->service, d->name, d->description, std::nullopt});
        }
        std::stable_sort(entries.begin(), entries.end(),
                         [](const Entry &a, const Entry &b) { return a.rank < b.rank; });
        for (const Entry &entry : entries)
            addTile(entry.item, entry.title, entry.description, entry.badge);
        gtk_label_set_text(GTK_LABEL(m_status), m_tiles.empty()            ? "No effect matches"
                                                : sourceClip().has_value() ? "Point at an effect to try it"
                                                                           : "Select a clip to try effects on it");
        requestTiles();
    }

  private:
    // --- Sources ----------------------------------------------------------

    // The clip the tiles show: the selected one, else the topmost under the
    // playhead.
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

    // Every selected clip, else the whole sequence.
    std::vector<core::Model::EffectTarget> applyTargets() const
    {
        const core::Model &model = m_host.model();
        std::vector<core::Model::EffectTarget> targets;
        for (core::ClipId id : m_host.currentSelection().clips)
            if (model.hasClip(id))
                targets.push_back(core::Model::EffectTarget::clip(id));
        if (targets.empty())
            targets.push_back(core::Model::EffectTarget::sequence());
        return targets;
    }

    // The source clip at the playhead with its own effects and the item's
    // added; nullopt without a clip, or when any effect it adds hasn't
    // passed the stability check (it would run in this process).
    std::optional<FrameRequest> requestFor(const std::string &item, int width, int height) const
    {
        const core::Model &model = m_host.model();
        const std::vector<core::Effect> added = m_catalog.effectsFor(model, item);
        if (added.empty())
            return std::nullopt;
        for (const core::Effect &effect : added) {
            const std::optional<HealthRecord> health = m_catalog.health(effect.service);
            if (!health || !health->usable() || !m_catalog.usable(effect.service))
                return std::nullopt;
        }
        const std::optional<core::ClipId> id = sourceClip();
        if (!id)
            return std::nullopt;
        const core::Clip &clip = model.clip(*id);
        if (!model.hasAsset(clip.asset))
            return std::nullopt;
        FrameRequest request;
        request.resource = model.asset(clip.asset).path;
        request.profile = model.sequence().profile;
        const core::FrameIndex into =
            std::clamp<core::FrameIndex>(m_host.currentFrame() - clip.position, 0, clip.length() - 1);
        request.sourceFrame = clip.in + into;
        request.clipIn = clip.in;
        request.clipOut = clip.out;
        for (const core::Effect &effect : clip.effects)
            if (effect.owner == kOwner && effect.enabled && m_catalog.usable(effect.service))
                request.effects.push_back(effect);
        request.effects.insert(request.effects.end(), added.begin(), added.end());
        request.width = width;
        request.height = height;
        return request;
    }

    void requestTiles()
    {
        const uint64_t generation = ++m_tileGeneration;
        for (const std::unique_ptr<Tile> &tile : m_tiles)
            requestTile(*tile, generation);
    }

    void requestTile(Tile &tile, uint64_t generation)
    {
        std::optional<FrameRequest> request = requestFor(tile.item, kTileWidth, kTileHeight);
        if (!request) {
            gtk_picture_set_paintable(GTK_PICTURE(tile.picture), nullptr);
            return;
        }
        Tile *t = &tile;
        m_renderer.request(std::move(*request), 1, generation, [this, t, generation](const RenderedFrame &frame) {
            if (generation != m_tileGeneration || frame.rgba.empty())
                return; // the tiles were rebuilt since (t may be gone)
            GdkTexture *texture = textureFor(frame);
            gtk_picture_set_paintable(GTK_PICTURE(t->picture), GDK_PAINTABLE(texture));
            g_object_unref(texture);
        });
    }

    void setBadge(Tile &tile)
    {
        for (const char *c : {"dim-label", "success", "warning", "error", "accent"})
            gtk_widget_remove_css_class(tile.badge, c);
        if (tile.fixedBadge) {
            gtk_label_set_text(GTK_LABEL(tile.badge), tile.fixedBadge->c_str());
            gtk_widget_add_css_class(tile.badge, "accent");
            return;
        }
        const std::optional<HealthRecord> health = m_catalog.health(tile.item);
        const char *text = !health ? "checking…" : "unstable";
        const char *style = "dim-label";
        if (health && health->usable()) {
            const CostBadge badge = costBadge(health->msPerFrame);
            text = badge == CostBadge::Light ? "light" : badge == CostBadge::Medium ? "medium" : "heavy";
            style = badge == CostBadge::Light ? "success" : badge == CostBadge::Medium ? "warning" : "error";
        }
        gtk_label_set_text(GTK_LABEL(tile.badge), text);
        gtk_widget_add_css_class(tile.badge, style);
    }

    // --- Sections -------------------------------------------------------------

    bool inSection(const EffectDescriptor &d, guint section) const
    {
        if (section == Featured)
            return d.featured;
        if (section == Recent)
            return std::find(m_recent.begin(), m_recent.end(), d.service) != m_recent.end();
        if (section == Looks)
            return false; // looks are tiles of their own
        if (section == All)
            return true;
        const size_t index = section - FirstCategory;
        return index < m_categories.size() && d.category == m_categories[index];
    }

    int recentRank(const std::string &service) const
    {
        auto it = std::find(m_recent.begin(), m_recent.end(), service);
        return it == m_recent.end() ? 3 : static_cast<int>(it - m_recent.begin());
    }

    // The categories on offer, as the section list's tail; rebuilt only
    // when they change (it keeps the choice).
    void rebuildSections()
    {
        std::set<std::string> seen;
        std::vector<std::string> categories;
        for (const EffectDescriptor *d : m_catalog.offered(true))
            if (seen.insert(d->category).second)
                categories.push_back(d->category);
        std::sort(categories.begin(), categories.end());
        if (categories == m_categories)
            return;
        m_categories = categories;
        GtkStringList *list = gtk_string_list_new(nullptr);
        for (const char *fixed : {"Featured", "Recent", "Looks", "All"})
            gtk_string_list_append(list, fixed);
        for (const std::string &c : categories)
            gtk_string_list_append(list, c.c_str());
        const guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(m_section));
        m_updating = true;
        gtk_drop_down_set_model(GTK_DROP_DOWN(m_section), G_LIST_MODEL(list));
        gtk_drop_down_set_selected(GTK_DROP_DOWN(m_section), selected < FirstCategory ? selected : 0);
        m_updating = false;
        g_object_unref(list);
    }

    // --- Building -----------------------------------------------------------

    void build()
    {
        m_root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_set_margin_start(m_root, 12);
        gtk_widget_set_margin_end(m_root, 12);
        gtk_widget_set_margin_top(m_root, 12);
        gtk_widget_set_margin_bottom(m_root, 12);

        m_search = gtk_search_entry_new();
        gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(m_search), "Search effects");
        gtk_accessible_update_property(GTK_ACCESSIBLE(m_search), GTK_ACCESSIBLE_PROPERTY_LABEL, "Search effects", -1);
        g_signal_connect(m_search, "search-changed", G_CALLBACK(&onSearchChangedTrampoline), this);
        g_signal_connect(m_search, "activate", G_CALLBACK(&onSearchActivateTrampoline), this);
        gtk_box_append(GTK_BOX(m_root), m_search);

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        const char *sections[] = {"Featured", "Recent", "Looks", "All", nullptr};
        m_section = gtk_drop_down_new_from_strings(sections);
        gtk_widget_set_hexpand(m_section, TRUE);
        g_signal_connect(m_section, "notify::selected", G_CALLBACK(&onSectionTrampoline), this);
        gtk_box_append(GTK_BOX(row), m_section);
        m_unstable = gtk_check_button_new_with_label("Unstable");
        g_signal_connect(m_unstable, "toggled", G_CALLBACK(&onUnstableTrampoline), this);
        gtk_box_append(GTK_BOX(row), m_unstable);
        gtk_box_append(GTK_BOX(m_root), row);

        m_status = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(m_status), 0.0f);
        gtk_label_set_wrap(GTK_LABEL(m_status), TRUE);
        gtk_widget_add_css_class(m_status, "dim-label");
        gtk_widget_add_css_class(m_status, "caption");
        gtk_box_append(GTK_BOX(m_root), m_status);

        m_grid = gtk_flow_box_new();
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(m_grid), GTK_SELECTION_SINGLE);
        gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(m_grid), TRUE);
        gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(m_grid), TRUE);
        gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(m_grid), 2);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(m_grid), 4);
        gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(m_grid), 6);
        gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(m_grid), 6);
        gtk_widget_set_valign(m_grid, GTK_ALIGN_START);
        g_signal_connect(m_grid, "child-activated", G_CALLBACK(&onChildActivatedTrampoline), this);
        g_signal_connect(m_grid, "selected-children-changed", G_CALLBACK(&onSelectionTrampoline), this);
        GtkEventController *keys = gtk_event_controller_key_new();
        g_signal_connect(keys, "key-pressed", G_CALLBACK(&onKeyTrampoline), this);
        gtk_widget_add_controller(m_root, keys);
        GtkEventController *leave = gtk_event_controller_motion_new();
        g_signal_connect(leave, "leave", G_CALLBACK(&onGridLeaveTrampoline), this);
        gtk_widget_add_controller(m_grid, leave);

        GtkWidget *scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_vexpand(scroll, TRUE);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), m_grid);
        gtk_box_append(GTK_BOX(m_root), scroll);
    }

    void addTile(const std::string &item, const std::string &title, const std::string &description,
                 std::optional<std::string> fixedBadge)
    {
        auto tile = std::make_unique<Tile>();
        tile->browser = this;
        tile->item = item;
        tile->fixedBadge = std::move(fixedBadge);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        tile->picture = gtk_picture_new();
        gtk_picture_set_content_fit(GTK_PICTURE(tile->picture), GTK_CONTENT_FIT_COVER);
        gtk_picture_set_can_shrink(GTK_PICTURE(tile->picture), TRUE);
        gtk_widget_set_size_request(tile->picture, kTileWidth / 2, kTileHeight / 2);
        gtk_widget_add_css_class(tile->picture, "card");
        gtk_box_append(GTK_BOX(box), tile->picture);
        GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *name = gtk_label_new(title.c_str());
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_widget_add_css_class(name, "caption");
        gtk_box_append(GTK_BOX(line), name);
        tile->badge = gtk_label_new("");
        gtk_widget_add_css_class(tile->badge, "caption");
        setBadge(*tile);
        gtk_box_append(GTK_BOX(line), tile->badge);
        gtk_box_append(GTK_BOX(box), line);

        tile->child = gtk_flow_box_child_new();
        gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(tile->child), box);
        g_object_set_data_full(G_OBJECT(tile->child), "item", g_strdup(item.c_str()), g_free);
        gtk_accessible_update_property(GTK_ACCESSIBLE(tile->child), GTK_ACCESSIBLE_PROPERTY_LABEL, title.c_str(), -1);
        const std::string tip = title + (description.empty() ? "" : "\n" + description);
        gtk_widget_set_tooltip_text(tile->child, tip.c_str());
        GtkEventController *hover = gtk_event_controller_motion_new();
        g_signal_connect(hover, "enter", G_CALLBACK(&onTileEnterTrampoline), tile.get());
        gtk_widget_add_controller(tile->child, hover);
        // Drag it onto the picture (the clip under the playhead) or the Rack.
        GtkDragSource *drag = gtk_drag_source_new();
        gtk_drag_source_set_actions(drag, GDK_ACTION_COPY);
        const std::string payload = std::string(kDragPrefix) + item;
        GdkContentProvider *content = gdk_content_provider_new_typed(G_TYPE_STRING, payload.c_str());
        gtk_drag_source_set_content(drag, content);
        g_object_unref(content);
        g_signal_connect(drag, "drag-begin", G_CALLBACK(&onDragBeginTrampoline), this);
        g_signal_connect(drag, "drag-end", G_CALLBACK(&onDragEndTrampoline), this);
        gtk_widget_add_controller(tile->child, GTK_EVENT_CONTROLLER(drag));
        gtk_flow_box_append(GTK_FLOW_BOX(m_grid), tile->child);
        m_tiles.push_back(std::move(tile));
    }

    // --- GTK signal trampolines ---------------------------------------------

    static void onBrowserActionTrampoline(GSimpleAction *, GVariant *, gpointer self)
    {
        static_cast<Browser *>(self)->onBrowserAction();
    }
    static void onDestroyTrampoline(GtkWidget *, gpointer self)
    {
        static_cast<Browser *>(self)->onDestroy();
    }
    static gboolean onRefillTrampoline(gpointer self)
    {
        static_cast<Browser *>(self)->refill();
        return G_SOURCE_REMOVE;
    }
    static gboolean onSourceTimerTrampoline(gpointer self)
    {
        static_cast<Browser *>(self)->onSourceTimer();
        return G_SOURCE_REMOVE;
    }
    static void onSearchChangedTrampoline(GtkSearchEntry *, gpointer self)
    {
        static_cast<Browser *>(self)->refill();
    }
    static void onSearchActivateTrampoline(GtkSearchEntry *, gpointer self)
    {
        static_cast<Browser *>(self)->onSearchActivate();
    }
    static void onSectionTrampoline(GObject *, GParamSpec *, gpointer self)
    {
        auto *browser = static_cast<Browser *>(self);
        if (!browser->m_updating)
            browser->scheduleRefill();
    }
    static void onUnstableTrampoline(GtkCheckButton *, gpointer self)
    {
        static_cast<Browser *>(self)->scheduleRefill();
    }
    static void onChildActivatedTrampoline(GtkFlowBox *, GtkFlowBoxChild *child, gpointer self)
    {
        static_cast<Browser *>(self)->onChildActivated(child);
    }
    static void onSelectionTrampoline(GtkFlowBox *, gpointer self)
    {
        static_cast<Browser *>(self)->onSelectionChanged();
    }
    static void onTileEnterTrampoline(GtkEventControllerMotion *, double, double, gpointer tile)
    {
        auto *t = static_cast<Tile *>(tile);
        t->browser->audition(t->item);
    }
    static void onGridLeaveTrampoline(GtkEventControllerMotion *, gpointer self)
    {
        static_cast<Browser *>(self)->stopAudition();
    }
    static gboolean onKeyTrampoline(GtkEventControllerKey *, guint keyval, guint, GdkModifierType, gpointer self)
    {
        if (keyval != GDK_KEY_Escape)
            return FALSE;
        static_cast<Browser *>(self)->onKeyEscape();
        return TRUE;
    }
    static void onDragBeginTrampoline(GtkDragSource *, GdkDrag *, gpointer self)
    {
        static_cast<Browser *>(self)->onDragBegin();
    }
    static void onDragEndTrampoline(GtkDragSource *, GdkDrag *, gboolean, gpointer self)
    {
        static_cast<Browser *>(self)->onDragEnd();
    }
    static gboolean onPreviewDropTrampoline(GtkDropTarget *, const GValue *value, double, double, gpointer self)
    {
        const char *payload = G_VALUE_HOLDS_STRING(value) ? g_value_get_string(value) : nullptr;
        return payload && static_cast<Browser *>(self)->onPreviewDrop(payload);
    }

    app::ShellHost &m_host;
    Catalog &m_catalog;
    FrameRenderer m_renderer;
    GtkWidget *m_root = nullptr, *m_search = nullptr, *m_section = nullptr, *m_unstable = nullptr;
    GtkWidget *m_status = nullptr, *m_grid = nullptr, *m_audition = nullptr, *m_dropZone = nullptr;
    std::vector<std::unique_ptr<Tile>> m_tiles;
    std::vector<std::string> m_categories, m_recent;
    std::string m_auditioning;
    uint64_t m_tileGeneration = 0, m_auditionGeneration = 0;
    core::FrameIndex m_lastFrame = -1;
    size_t m_projectLooks = 0;
    guint m_sourceTimer = 0;
    bool m_refillPending = false, m_updating = false;
};

} // namespace

void addBrowser(app::ShellHost &host, Catalog &catalog)
{
    // For the window's life: its widgets and signal handlers point here.
    static std::vector<std::unique_ptr<Browser>> browsers;
    browsers.push_back(std::make_unique<Browser>(host, catalog));
    browsers.back()->install();
}

} // namespace ustudio::effects
