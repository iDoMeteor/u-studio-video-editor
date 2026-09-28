#include "app/curve_lanes.h"

#include "app/catalog.h"
#include "app/shell_host.h"
#include "core/commands.h"
#include "core/keyframes.h"
#include "core/log.h"
#include "core/model/animation.h"
#include "tokens.h"

#include <gtk/gtk.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ustudio::effects {

namespace {

namespace tokens = app::tokens;
using app::timeline::RowLayout;
using app::timeline::Viewport;

constexpr double kLaneHeight = 36.0;
constexpr double kPad = 5.0;      // above and below the curve inside a lane
constexpr double kKeyRadius = 3.5;
constexpr double kKeyGrab = 7.0;  // how near a press must be to take a key

// One animated value of a clip's effects.
struct Curve
{
    core::EffectId effect;
    std::string param; // "": the mix
    std::string title; // "Glow · Blur"
    double low = 0.0, high = 1.0;
    std::vector<core::Keyframe> keys;
};

// The value range a curve is drawn over: the parameter's own when the
// registry knows it, else the keys' spread (padded; flat keys sit mid-lane).
std::pair<double, double> rangeOf(const std::vector<core::Keyframe> &keys, std::optional<double> low,
                                  std::optional<double> high)
{
    if (low && high && *high > *low)
        return {*low, *high};
    double lo = keys.front().value, hi = keys.front().value;
    for (const core::Keyframe &key : keys) {
        lo = std::min(lo, key.value);
        hi = std::max(hi, key.value);
    }
    const double pad = std::max((hi - lo) * 0.1, 0.5);
    return {lo - pad, hi + pad};
}

std::vector<Curve> curvesOf(const core::Model &model, const Catalog &catalog, core::ClipId clip)
{
    std::vector<Curve> curves;
    if (!model.hasClip(clip))
        return curves;
    for (const core::Effect &effect : model.clip(clip).effects) {
        const EffectDescriptor *descriptor = catalog.find(effect.service);
        const std::string name = descriptor ? descriptor->name : effect.service;
        if (!effect.mix.keyframes.empty())
            curves.push_back({effect.id, "", name + " · Mix", 0.0, 1.0, effect.mix.keyframes});
        for (const core::Param &param : effect.params) {
            if (param.keyframes.empty())
                continue;
            std::string title = param.name;
            std::optional<double> low, high;
            if (descriptor)
                for (const ParamDescriptor &p : descriptor->params)
                    if (p.id == param.name) {
                        title = p.title.empty() ? p.id : p.title;
                        low = p.minimum;
                        high = p.maximum;
                    }
            const auto [lo, hi] = rangeOf(param.keyframes, low, high);
            curves.push_back({effect.id, param.name, name + " · " + title, lo, hi, param.keyframes});
        }
    }
    return curves;
}

int rowOf(const core::Model &model, core::TrackId track)
{
    const auto &tracks = model.sequence().tracks;
    for (size_t row = 0; row < tracks.size(); ++row)
        if (tracks[row].id == track)
            return static_cast<int>(row);
    return -1;
}

class CurveLanes : public app::timeline::TimelineOverlayProvider
{
  public:
    CurveLanes(app::ShellHost &host, Catalog &catalog) : m_host(host), m_catalog(catalog) {}

    ~CurveLanes() override
    {
        if (m_pango)
            g_object_unref(m_pango);
    }

    void install()
    {
        m_host.addHints({
            {"effects.curve-lanes", "Effects", "Curve lanes",
             "Under the selected clip, the curve each animated value follows: drag a keyframe to move it, "
             "double-click a lane to add one",
             "effects-curve-lanes", "Drag a keyframe in a curve lane"},
        });
        static const std::vector<app::ActionSpec> actions = {
            {"effects-curve-lanes", "Show or hide curve lanes", "Effects", {"c"}, &onToggleTrampoline},
        };
        m_host.addActions(actions, this);
        m_host.addTimelineOverlay(this);
        m_host.projectChanged().connect([this] {
            if (!m_expanded.empty())
                m_host.redrawTimeline(); // lanes appear, go or change with the keys
        });
    }

    // --- TimelineOverlayProvider -------------------------------------------

    double laneHeight(const core::Model &model, const core::Track &track) const override
    {
        size_t most = 0;
        for (core::ClipId id : m_expanded)
            if (model.hasClip(id) && model.clip(id).track == track.id)
                most = std::max(most, curvesOf(model, m_catalog, id).size());
        return static_cast<double>(most) * kLaneHeight;
    }

    void paintOverlay(GtkSnapshot *snapshot, const core::Model &model, const Viewport &viewport,
                      const RowLayout &layout, double width, double height) const override
    {
        for (core::ClipId id : m_expanded) {
            if (!model.hasClip(id))
                continue;
            const core::Clip &clip = model.clip(id);
            const int row = rowOf(model, clip.track);
            if (row < 0)
                continue;
            const std::vector<Curve> curves = curvesOf(model, m_catalog, id);
            const double x0 = std::max(viewport.xForFrame(static_cast<double>(clip.position)), 0.0);
            const double x1 = std::min(viewport.xForFrame(static_cast<double>(clip.end())), width);
            if (x1 <= x0)
                continue;
            graphene_rect_t bounds = GRAPHENE_RECT_INIT(0.0f, 0.0f, static_cast<float>(width),
                                                        static_cast<float>(height));
            cairo_t *cr = gtk_snapshot_append_cairo(snapshot, &bounds);
            for (size_t i = 0; i < curves.size(); ++i)
                paintCurve(cr, curves[i], clip, viewport, x0, x1, layout.laneTop(row) + static_cast<double>(i) * kLaneHeight);
            cairo_destroy(cr);
        }
    }

    bool pressed(const core::Model &model, const Viewport &viewport, const RowLayout &layout, double x, double y,
                 int nPress) override
    {
        const std::optional<Hit> hit = hitAt(model, viewport, layout, x, y);
        if (!hit)
            return false;
        const Curve &curve = hit->curve;
        const core::Clip &clip = model.clip(hit->clip);
        // A key under the pointer: drag it.
        for (const core::Keyframe &key : curve.keys) {
            const double kx = viewport.xForFrame(static_cast<double>(clip.position + key.at));
            const double ky = yOf(curve, key.value, hit->top);
            if (std::abs(kx - x) <= kKeyGrab && std::abs(ky - y) <= kKeyGrab) {
                m_drag = Drag{hit->clip, curve.effect, curve.param, key.at, curve.low, curve.high, hit->top,
                              ++m_nextGesture};
                return true;
            }
        }
        // A double-click on the lane: a key there, on the curve.
        if (nPress >= 2) {
            const core::FrameIndex at = std::clamp(frameAt(viewport, x) - clip.position, core::FrameIndex{0},
                                                   clip.length() - 1);
            std::vector<core::Keyframe> keys = withKeyAt(curve.keys, at, valueAt(curve, y, hit->top));
            apply(model, curve.effect, curve.param, std::move(keys), 0);
        }
        return true; // the lane is ours: never a clip move or marquee
    }

    void dragged(const core::Model &model, const Viewport &viewport, const RowLayout &, double x, double y,
                 bool finished) override
    {
        if (!m_drag || !model.hasClip(m_drag->clip) || !model.hasEffect(m_drag->effect)) {
            m_drag.reset();
            return;
        }
        const core::Clip &clip = model.clip(m_drag->clip);
        std::vector<core::Keyframe> keys = keysOf(model, m_drag->effect, m_drag->param);
        auto it = std::find_if(keys.begin(), keys.end(), [&](const core::Keyframe &k) { return k.at == m_drag->at; });
        if (it == keys.end()) {
            m_drag.reset();
            return;
        }
        // Between its neighbours (keys never swap), inside the clip.
        const core::FrameIndex lower = it == keys.begin() ? 0 : std::prev(it)->at + 1;
        const core::FrameIndex upper = std::next(it) == keys.end() ? clip.length() - 1 : std::next(it)->at - 1;
        const core::FrameIndex at = std::clamp(frameAt(viewport, x) - clip.position, lower, std::max(lower, upper));
        const Curve range{m_drag->effect, m_drag->param, {}, m_drag->low, m_drag->high, {}};
        it->at = at;
        it->value = valueAt(range, y, m_drag->top);
        apply(model, m_drag->effect, m_drag->param, std::move(keys), m_drag->gesture);
        m_drag->at = at;
        if (finished)
            m_drag.reset();
    }

    void dragCancelled() override
    {
        m_drag.reset();
    }

    void toggle()
    {
        const std::vector<core::ClipId> clips = m_host.currentSelection().clips;
        if (clips.empty()) {
            m_host.showStatus("Select a clip to see its curve lanes.");
            return;
        }
        const bool allShown =
            std::all_of(clips.begin(), clips.end(), [&](core::ClipId id) { return m_expanded.contains(id); });
        for (core::ClipId id : clips) {
            if (allShown)
                m_expanded.erase(id);
            else
                m_expanded.insert(id);
        }
        size_t lanes = 0;
        for (core::ClipId id : clips)
            lanes += curvesOf(m_host.model(), m_catalog, id).size();
        if (!allShown && lanes == 0)
            m_host.showStatus("No animated values on that clip yet: pin one in the Effects page (P).");
        core::Log::debug("[effects] curve lanes " + std::string(allShown ? "hidden" : "shown") + ": " +
                         std::to_string(lanes));
        m_host.redrawTimeline();
    }

  private:
    struct Hit
    {
        core::ClipId clip;
        Curve curve;
        double top; // the lane's top
    };
    struct Drag
    {
        core::ClipId clip;
        core::EffectId effect;
        std::string param;
        core::FrameIndex at;
        double low, high, top;
        uint64_t gesture;
    };

    static core::FrameIndex frameAt(const Viewport &viewport, double x)
    {
        // Viewport's inline members only: the render tool links this drop-in
        // without the shell (frameForX() lives there).
        return static_cast<core::FrameIndex>(
            std::llround((x - viewport.originX() + viewport.scrollX()) / viewport.pxPerFrame()));
    }
    static double yOf(const Curve &curve, double value, double top)
    {
        const double t = std::clamp((value - curve.low) / (curve.high - curve.low), 0.0, 1.0);
        return top + kLaneHeight - kPad - t * (kLaneHeight - 2 * kPad);
    }
    static double valueAt(const Curve &curve, double y, double top)
    {
        const double t = std::clamp((top + kLaneHeight - kPad - y) / (kLaneHeight - 2 * kPad), 0.0, 1.0);
        return curve.low + t * (curve.high - curve.low);
    }

    std::optional<Hit> hitAt(const core::Model &model, const Viewport &viewport, const RowLayout &layout, double x,
                             double y) const
    {
        if (!layout.inLane(y))
            return std::nullopt;
        const int row = layout.rowAt(y);
        for (core::ClipId id : m_expanded) {
            if (!model.hasClip(id))
                continue;
            const core::Clip &clip = model.clip(id);
            if (rowOf(model, clip.track) != row)
                continue;
            if (x < viewport.xForFrame(static_cast<double>(clip.position)) - kKeyGrab ||
                x > viewport.xForFrame(static_cast<double>(clip.end())) + kKeyGrab)
                continue;
            const std::vector<Curve> curves = curvesOf(model, m_catalog, id);
            const auto lane = static_cast<size_t>((y - layout.laneTop(row)) / kLaneHeight);
            if (lane < curves.size())
                return Hit{id, curves[lane], layout.laneTop(row) + static_cast<double>(lane) * kLaneHeight};
        }
        return std::nullopt;
    }

    static std::vector<core::Keyframe> keysOf(const core::Model &model, core::EffectId effect,
                                              const std::string &param)
    {
        const core::Effect &e = model.effect(effect);
        if (param.empty())
            return e.mix.keyframes;
        for (const core::Param &p : e.params)
            if (p.name == param)
                return p.keyframes;
        return {};
    }

    void apply(const core::Model &model, core::EffectId effect, const std::string &param,
               std::vector<core::Keyframe> keys, uint64_t gesture)
    {
        const core::Effect &e = model.effect(effect);
        if (param.empty()) {
            core::KeyframedValue mix = e.mix;
            mix.keyframes = std::move(keys);
            m_host.execute(std::make_unique<SetMix>(effect, mix, gesture));
            return;
        }
        for (const core::Param &p : e.params)
            if (p.name == param) {
                core::Param updated = p;
                updated.keyframes = std::move(keys);
                m_host.execute(std::make_unique<SetParam>(effect, updated, gesture));
                return;
            }
    }

    void paintCurve(cairo_t *cr, const Curve &curve, const core::Clip &clip, const Viewport &viewport, double x0,
                    double x1, double top) const
    {
        auto set = [cr](tokens::Rgb c, double alpha) { cairo_set_source_rgba(cr, c.r, c.g, c.b, alpha); };
        set(tokens::kInk850, 0.92);
        cairo_rectangle(cr, x0, top + 1, x1 - x0, kLaneHeight - 2);
        cairo_fill(cr);
        // The curve, a point every other pixel.
        set(tokens::kBrandCyan, 0.9);
        cairo_set_line_width(cr, 1.5);
        for (double x = x0; x <= x1; x += 2.0) {
            const double frame = (x - viewport.originX() + viewport.scrollX()) / viewport.pxPerFrame() -
                                 static_cast<double>(clip.position);
            const double y = yOf(curve, core::easedValue(curve.keys, frame), top);
            if (x == x0)
                cairo_move_to(cr, x, y);
            else
                cairo_line_to(cr, x, y);
        }
        cairo_stroke(cr);
        // The keys.
        set(tokens::kBrandMagenta, 1.0);
        for (const core::Keyframe &key : curve.keys) {
            const double kx = viewport.xForFrame(static_cast<double>(clip.position + key.at));
            if (kx < x0 - kKeyRadius || kx > x1 + kKeyRadius)
                continue;
            cairo_arc(cr, kx, yOf(curve, key.value, top), kKeyRadius, 0, 2 * M_PI);
            cairo_fill(cr);
        }
        // What it is, at the lane's left.
        if (!m_pango) {
            PangoFontMap *fonts = pango_cairo_font_map_get_default();
            m_pango = pango_font_map_create_context(fonts);
        }
        PangoLayout *label = pango_layout_new(m_pango);
        PangoFontDescription *font = pango_font_description_from_string("Sans 8");
        pango_layout_set_font_description(label, font);
        pango_font_description_free(font);
        pango_layout_set_text(label, curve.title.c_str(), -1);
        pango_layout_set_width(label, static_cast<int>(std::max(x1 - x0 - 8, 1.0) * PANGO_SCALE));
        pango_layout_set_ellipsize(label, PANGO_ELLIPSIZE_END);
        set(tokens::kWindowFgColor, 0.75);
        cairo_move_to(cr, x0 + 4, top + 2);
        pango_cairo_show_layout(cr, label);
        g_object_unref(label);
    }

    // --- GTK signal trampolines ---------------------------------------------

    static void onToggleTrampoline(GSimpleAction *, GVariant *, gpointer self)
    {
        static_cast<CurveLanes *>(self)->toggle();
    }

    app::ShellHost &m_host;
    Catalog &m_catalog;
    std::set<core::ClipId> m_expanded;
    std::optional<Drag> m_drag;
    uint64_t m_nextGesture = 0;
    mutable PangoContext *m_pango = nullptr;
};

} // namespace

void addCurveLanes(app::ShellHost &host, Catalog &catalog)
{
    // For the window's life: its widgets and signal handlers point here.
    static std::vector<std::unique_ptr<CurveLanes>> lanes;
    lanes.push_back(std::make_unique<CurveLanes>(host, catalog));
    lanes.back()->install();
}

} // namespace ustudio::effects
