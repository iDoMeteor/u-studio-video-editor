#include "app/fx_lane.h"

#include "app/catalog.h"
#include "app/shell_host.h"
#include "core/blocks.h"
#include "core/log.h"
#include "tokens.h"

#include <gtk/gtk.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ustudio::effects {

namespace {

namespace tokens = app::tokens;
using app::timeline::RowLayout;
using app::timeline::Viewport;

constexpr double kLane = 24.0;       // the FX lane's height, above the tracks and under one
constexpr double kEdgeGrab = 6.0;    // how near an end a press resizes
constexpr double kFadeGrab = 8.0;    // how near a top corner's handle a press fades
constexpr core::FrameIndex kMinDraw = 2; // a draw shorter than this is a click

class FxLane : public app::timeline::TimelineOverlayProvider
{
  public:
    FxLane(app::ShellHost &host, Catalog &catalog) : m_host(host), m_catalog(catalog) {}

    ~FxLane() override
    {
        if (m_pango)
            g_object_unref(m_pango);
    }

    void install()
    {
        m_host.addHints({
            {"effects.fx-lane", "Effects", "FX lane",
             "Adjustment blocks: effects on everything beneath them for a stretch of time. Drag across the lane "
             "to draw one, click one to edit its effects, drag its ends to resize it and its top corners to fade it",
             nullptr, "Drag across the FX lane"},
        });
        m_host.addTimelineOverlay(this);
        m_catalog.blockSelected.connect([this] { m_host.redrawTimeline(); });
        // Choosing clips on the timeline takes the Rack and Browser back to
        // them: the block is deselected.
        m_host.selectionChanged().connect([this] {
            if (m_catalog.selectedBlock && !m_host.currentSelection().clips.empty())
                select(std::nullopt);
        });
        m_host.projectChanged().connect([this] {
            // An undone block: nothing selected.
            if (m_catalog.selectedBlock && !m_host.model().hasAdjustmentBlock(*m_catalog.selectedBlock)) {
                m_catalog.selectedBlock.reset();
                m_catalog.blockSelected.emit();
            }
        });
    }

    // --- TimelineOverlayProvider -------------------------------------------

    double topLaneHeight(const core::Model &) const override
    {
        return kLane;
    }

    // Under row r: the lane of blocks on lane r + 1 (they affect the rows
    // below it), when there are any.
    double laneHeight(const core::Model &model, const core::Track &track) const override
    {
        const int row = rowOf(model, track.id);
        for (const core::AdjustmentBlock &block : model.sequence().adjustmentBlocks)
            if (block.lane == row + 1)
                return kLane;
        return 0.0;
    }

    void paintOverlay(GtkSnapshot *snapshot, const core::Model &model, const Viewport &viewport,
                      const RowLayout &layout, double width, double height) const override
    {
        graphene_rect_t bounds =
            GRAPHENE_RECT_INIT(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
        cairo_t *cr = gtk_snapshot_append_cairo(snapshot, &bounds);
        // The lanes: the top one always, the others where blocks are.
        std::vector<int> lanes{0};
        for (const core::AdjustmentBlock &block : model.sequence().adjustmentBlocks)
            if (block.lane > 0 && std::find(lanes.begin(), lanes.end(), block.lane) == lanes.end())
                lanes.push_back(block.lane);
        for (int lane : lanes) {
            const double top = laneTop(model, layout, lane);
            if (top < 0)
                continue;
            set(cr, tokens::kInk850, 1.0);
            cairo_rectangle(cr, 0, top, width, kLane);
            cairo_fill(cr);
            set(tokens::kWindowFgColor, 0.45, cr);
            label(cr, lane == 0 ? "FX" : "FX ↓", 4, top + 5, 40);
        }
        for (const core::AdjustmentBlock &block : model.sequence().adjustmentBlocks)
            paintBlock(cr, model, viewport, layout, block);
        // A block being drawn.
        if (m_drag && m_drag->mode == Mode::Draw && m_drag->drawn >= 0) {
            const double top = laneTop(model, layout, m_drag->lane);
            const auto [first, last] = std::minmax(m_drag->anchor, m_drag->drawn);
            set(cr, tokens::kBrandViolet, 0.35);
            cairo_rectangle(cr, viewport.xForFrame(static_cast<double>(first)), top + 2,
                            viewport.xForFrame(static_cast<double>(last + 1)) - viewport.xForFrame(double(first)),
                            kLane - 4);
            cairo_fill(cr);
        }
        cairo_destroy(cr);
    }

    bool pressed(const core::Model &model, const Viewport &viewport, const RowLayout &layout, double x, double y,
                 int) override
    {
        const std::optional<int> lane = laneAt(model, layout, y);
        if (!lane)
            return false;
        const double top = laneTop(model, layout, *lane);
        for (const core::AdjustmentBlock &block : model.sequence().adjustmentBlocks) {
            if (block.lane != *lane)
                continue;
            const double x0 = viewport.xForFrame(static_cast<double>(block.start));
            const double x1 = viewport.xForFrame(static_cast<double>(block.end()));
            if (x < x0 - kEdgeGrab || x > x1 + kEdgeGrab)
                continue;
            select(block.id);
            Mode mode = Mode::Move;
            const double fadeInX = x0 + fadeWidth(viewport, block.fadeIn);
            const double fadeOutX = x1 - fadeWidth(viewport, block.fadeOut);
            if (y < top + kLane / 2 && std::abs(x - fadeInX) <= kFadeGrab)
                mode = Mode::FadeIn;
            else if (y < top + kLane / 2 && std::abs(x - fadeOutX) <= kFadeGrab)
                mode = Mode::FadeOut;
            else if (std::abs(x - x0) <= kEdgeGrab)
                mode = Mode::ResizeStart;
            else if (std::abs(x - x1) <= kEdgeGrab)
                mode = Mode::ResizeEnd;
            m_drag = Drag{mode, block.id, *lane, frameAt(viewport, x), -1, block, ++m_nextGesture};
            return true;
        }
        // Empty lane: draw a new block from here.
        select(std::nullopt);
        m_drag = Drag{Mode::Draw, {}, *lane, std::max<core::FrameIndex>(frameAt(viewport, x), 0), -1, {}, 0};
        return true;
    }

    void dragged(const core::Model &model, const Viewport &viewport, const RowLayout &, double x, double,
                 bool finished) override
    {
        if (!m_drag)
            return;
        const core::FrameIndex frame = std::max<core::FrameIndex>(frameAt(viewport, x), 0);
        if (m_drag->mode == Mode::Draw) {
            m_drag->drawn = frame;
            if (finished) {
                const auto [first, last] = std::minmax(m_drag->anchor, frame);
                if (last - first + 1 >= kMinDraw) {
                    core::AdjustmentBlock block;
                    block.lane = m_drag->lane;
                    block.start = first;
                    block.length = last - first + 1;
                    auto add = std::make_unique<AddAdjustmentBlock>(block);
                    AddAdjustmentBlock *adding = add.get();
                    if (m_host.execute(std::move(add))) {
                        core::Log::debug("[effects] adjustment block drawn: " + std::to_string(block.start) + "+" +
                                         std::to_string(block.length));
                        select(adding->id());
                        m_host.showInspectorPage("effects.rack");
                        m_host.showStatus("Adjustment block added: add effects to it on the Effects page.");
                    } else {
                        m_host.showStatus("A block can't overlap another on its lane.");
                    }
                }
                m_drag.reset();
            }
            return;
        }
        if (!model.hasAdjustmentBlock(m_drag->block)) {
            m_drag.reset();
            return;
        }
        const core::AdjustmentBlock &was = m_drag->original;
        const core::FrameIndex delta = frame - m_drag->anchor;
        switch (m_drag->mode) {
        case Mode::Move:
            m_host.execute(std::make_unique<SetAdjustmentBlockRange>(
                m_drag->block, was.lane, std::max<core::FrameIndex>(was.start + delta, 0), was.length,
                m_drag->gesture));
            break;
        case Mode::ResizeStart: {
            const core::FrameIndex start = std::clamp(was.start + delta, core::FrameIndex{0}, was.end() - 1);
            m_host.execute(std::make_unique<SetAdjustmentBlockRange>(m_drag->block, was.lane, start,
                                                                     was.end() - start, m_drag->gesture));
            break;
        }
        case Mode::ResizeEnd:
            m_host.execute(std::make_unique<SetAdjustmentBlockRange>(
                m_drag->block, was.lane, was.start, std::max<core::FrameIndex>(was.length + delta, 1),
                m_drag->gesture));
            break;
        case Mode::FadeIn:
        case Mode::FadeOut: {
            const bool in = m_drag->mode == Mode::FadeIn;
            const core::FrameIndex before = in ? (was.fadeIn ? was.fadeIn->length : 0)
                                               : (was.fadeOut ? was.fadeOut->length : 0);
            const core::FrameIndex length = std::clamp(before + (in ? delta : -delta), core::FrameIndex{0}, was.length);
            const std::optional<core::FadeSpec> fade =
                length > 0 ? std::optional(core::FadeSpec{length}) : std::nullopt;
            m_host.execute(std::make_unique<SetAdjustmentBlockFades>(m_drag->block, in ? fade : was.fadeIn,
                                                                     in ? was.fadeOut : fade, m_drag->gesture));
            break;
        }
        case Mode::Draw:
            break;
        }
        if (finished)
            m_drag.reset();
    }

    void dragCancelled() override
    {
        m_drag.reset();
    }

  private:
    enum class Mode
    {
        Draw,
        Move,
        ResizeStart,
        ResizeEnd,
        FadeIn,
        FadeOut,
    };
    struct Drag
    {
        Mode mode;
        core::AdjustmentBlockId block;
        int lane;
        core::FrameIndex anchor; // the press's frame
        core::FrameIndex drawn;  // Draw: where the pointer is (-1 until it moves)
        core::AdjustmentBlock original;
        uint64_t gesture;
    };

    static int rowOf(const core::Model &model, core::TrackId track)
    {
        const auto &tracks = model.sequence().tracks;
        for (size_t row = 0; row < tracks.size(); ++row)
            if (tracks[row].id == track)
                return static_cast<int>(row);
        return -1;
    }

    // Lane 0: the top lane. Lane k: under row k - 1, below any other
    // drop-in's lanes there (curve lanes), -1 when there's no such row.
    static double laneTop(const core::Model &model, const RowLayout &layout, int lane)
    {
        if (lane == 0)
            return layout.topLane - kLane;
        const int row = lane - 1;
        if (row >= static_cast<int>(model.sequence().tracks.size()))
            return -1;
        return layout.laneTop(row) + layout.laneHeight(row) - kLane;
    }

    static std::optional<int> laneAt(const core::Model &model, const RowLayout &layout, double y)
    {
        if (layout.inTopLane(y))
            return y >= layout.topLane - kLane ? std::optional(0) : std::nullopt;
        if (!layout.inLane(y))
            return std::nullopt;
        const int row = layout.rowAt(y);
        const double top = layout.laneTop(row) + layout.laneHeight(row) - kLane;
        if (y < top)
            return std::nullopt; // another drop-in's lane above ours
        for (const core::AdjustmentBlock &block : model.sequence().adjustmentBlocks)
            if (block.lane == row + 1)
                return row + 1;
        return std::nullopt;
    }

    static core::FrameIndex frameAt(const Viewport &viewport, double x)
    {
        // Viewport's inline members only (the render tool links this drop-in
        // without the shell).
        return static_cast<core::FrameIndex>(
            std::floor((x - viewport.originX() + viewport.scrollX()) / viewport.pxPerFrame()));
    }

    static double fadeWidth(const Viewport &viewport, const std::optional<core::FadeSpec> &fade)
    {
        return fade ? static_cast<double>(fade->length) * viewport.pxPerFrame() : 0.0;
    }

    void select(std::optional<core::AdjustmentBlockId> id)
    {
        if (m_catalog.selectedBlock == id)
            return;
        m_catalog.selectedBlock = id;
        m_catalog.blockSelected.emit();
    }

    static void set(cairo_t *cr, tokens::Rgb c, double alpha)
    {
        cairo_set_source_rgba(cr, c.r, c.g, c.b, alpha);
    }
    static void set(tokens::Rgb c, double alpha, cairo_t *cr)
    {
        set(cr, c, alpha);
    }

    void label(cairo_t *cr, const std::string &text, double x, double y, double width) const
    {
        if (!m_pango)
            m_pango = pango_font_map_create_context(pango_cairo_font_map_get_default());
        PangoLayout *layout = pango_layout_new(m_pango);
        PangoFontDescription *font = pango_font_description_from_string("Sans 8");
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
        pango_layout_set_text(layout, text.c_str(), -1);
        pango_layout_set_width(layout, static_cast<int>(std::max(width, 1.0) * PANGO_SCALE));
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        cairo_move_to(cr, x, y);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }

    void paintBlock(cairo_t *cr, const core::Model &model, const Viewport &viewport, const RowLayout &layout,
                    const core::AdjustmentBlock &block) const
    {
        const double top = laneTop(model, layout, block.lane);
        if (top < 0)
            return;
        const double x0 = viewport.xForFrame(static_cast<double>(block.start));
        const double x1 = viewport.xForFrame(static_cast<double>(block.end()));
        const double y0 = top + 2, h = kLane - 4;
        set(cr, tokens::kBrandViolet, 0.55);
        cairo_rectangle(cr, x0, y0, x1 - x0, h);
        cairo_fill(cr);
        // The fades: a ramp from the bottom corner to where the fade ends.
        set(cr, tokens::kWindowFgColor, 0.8);
        cairo_set_line_width(cr, 1.0);
        const double in = fadeWidth(viewport, block.fadeIn), out = fadeWidth(viewport, block.fadeOut);
        cairo_move_to(cr, x0, y0 + h);
        cairo_line_to(cr, x0 + in, y0);
        cairo_line_to(cr, x1 - out, y0);
        cairo_line_to(cr, x1, y0 + h);
        cairo_stroke(cr);
        // Fade handles at the top.
        for (double hx : {x0 + in, x1 - out}) {
            cairo_arc(cr, hx, y0 + 2, 2.5, 0, 2 * M_PI);
            cairo_fill(cr);
        }
        // Selected: the selection treatment (cyan outline), not a glow.
        const bool selected = m_catalog.selectedBlock && *m_catalog.selectedBlock == block.id;
        set(cr, selected ? tokens::kBrandCyan : tokens::kBrandViolet, 1.0);
        cairo_set_line_width(cr, selected ? 2.0 : 1.0);
        cairo_rectangle(cr, x0 + 0.5, y0 + 0.5, x1 - x0 - 1, h - 1);
        cairo_stroke(cr);
        const std::string text = block.effects.empty()
                                     ? std::string("Adjustment: no effects yet")
                                     : "Adjustment: " + std::to_string(block.effects.size()) + " effect" +
                                           (block.effects.size() == 1 ? "" : "s");
        set(cr, tokens::kWindowFgColor, 0.9);
        label(cr, text, x0 + std::max(in, 4.0), y0 + 3, x1 - x0 - std::max(in, 4.0) - std::max(out, 4.0));
    }

    app::ShellHost &m_host;
    Catalog &m_catalog;
    std::optional<Drag> m_drag;
    uint64_t m_nextGesture = 0;
    mutable PangoContext *m_pango = nullptr;
};

} // namespace

void addFxLane(app::ShellHost &host, Catalog &catalog)
{
    // For the window's life: its widgets and signal handlers point here.
    static std::vector<std::unique_ptr<FxLane>> lanes;
    lanes.push_back(std::make_unique<FxLane>(host, catalog));
    lanes.back()->install();
}

} // namespace ustudio::effects
