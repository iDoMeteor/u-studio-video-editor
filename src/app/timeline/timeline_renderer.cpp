#include "timeline_renderer.h"

#include "core/model/model.h"
#include "tokens.h"

#include <algorithm>
#include <cmath>

namespace ustudio::app::timeline {

namespace {

using Mode = TimelineController::Mode;

GdkRGBA rgba(tokens::Rgb c, float alpha = 1.0f)
{
    return GdkRGBA{c.r, c.g, c.b, alpha};
}

graphene_rect_t rect(double x, double y, double w, double h)
{
    graphene_rect_t r;
    graphene_rect_init(&r, static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h));
    return r;
}

void fill(GtkSnapshot *s, double x, double y, double w, double h, GdkRGBA color)
{
    graphene_rect_t r = rect(x, y, w, h);
    gtk_snapshot_append_color(s, &color, &r);
}

// An outline drawn inside the rectangle.
void outline(GtkSnapshot *s, double x, double y, double w, double h, float lineWidth, GdkRGBA color)
{
    GskRoundedRect rounded;
    graphene_rect_t r = rect(x, y, w, h);
    gsk_rounded_rect_init_from_rect(&rounded, &r, 0.0f);
    const float widths[4] = {lineWidth, lineWidth, lineWidth, lineWidth};
    const GdkRGBA colors[4] = {color, color, color, color};
    gtk_snapshot_append_border(s, &rounded, widths, colors);
}

// Single-line text, ellipsized to `maxWidth`, top-left at (x, y).
void label(GtkSnapshot *s, PangoLayout *layout, const std::string &text, double x, double y, double maxWidth,
           GdkRGBA color)
{
    if (!layout || text.empty() || maxWidth <= 4.0)
        return;
    pango_layout_set_text(layout, text.c_str(), -1);
    pango_layout_set_width(layout, static_cast<int>(maxWidth * PANGO_SCALE));
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    gtk_snapshot_save(s);
    graphene_point_t at;
    graphene_point_init(&at, static_cast<float>(x), static_cast<float>(y));
    gtk_snapshot_translate(s, &at);
    gtk_snapshot_append_layout(s, layout, &color);
    gtk_snapshot_restore(s);
}

const GdkRGBA kLabel = rgba(tokens::kWindowFgColor, 0.85f);
constexpr double kMinWaveformWidthPx = 24.0;
constexpr double kMinThumbnailHeightPx = 24.0;

struct ClipBox
{
    double x, w;        // full extent (may be far off screen)
    double left, right; // the visible part
    double top, height;
};

// A clip's box on `row` in widget coordinates, or false if off screen.
bool clipBox(const TimelineScene &scene, int row, core::FrameIndex start, core::FrameIndex length, double width,
             ClipBox &box)
{
    box.x = scene.viewport.xForFrame(static_cast<double>(start));
    box.w = static_cast<double>(length) * scene.viewport.pxPerFrame();
    if (box.x > width || box.x + box.w < scene.handleWidth)
        return false;
    // GSK copes with huge rectangles, but only the visible part matters.
    box.left = std::max(box.x, scene.handleWidth - 4.0);
    box.right = std::min(box.x + box.w, width + 4.0);
    box.top = scene.layout.clipTop(row);
    box.height = scene.layout.clipHeight();
    return true;
}

// Fill, outline and label. Doc 06: under 2 px a clip is a 1 px tick,
// labels only from 40 px.
// `overPictures`: thumbnails are already drawn in the box, so only the
// outline and label go on top.
void drawClip(GtkSnapshot *s, const TimelineScene &scene, const ClipBox &box, const std::string &name, bool selected,
              bool ghost, bool invalid, bool overPictures = false)
{
    double w = std::max(box.right - box.left - 2.0, 1.0);
    GdkRGBA body = invalid ? rgba(tokens::kSemanticDanger, 0.45f) : rgba(tokens::kInk700, ghost ? 0.5f : 1.0f);
    if (!overPictures)
        fill(s, box.left + 1.0, box.top, w, box.height, body);
    // Below 4 px an outline would cover the fill; the clip is a tick.
    if (box.w < 4.0)
        return;
    GdkRGBA edge = invalid    ? rgba(tokens::kSemanticDanger, 0.9f)
                   : selected ? rgba(tokens::kBrandCyan, ghost ? 0.7f : 1.0f)
                              : rgba(tokens::kInk500, ghost ? 0.7f : 1.0f);
    outline(s, box.left + 1.0, box.top, w, box.height, selected || invalid ? 2.0f : 1.0f, edge);
    if (!ghost && box.w >= 40.0 && !name.empty()) {
        // Pinned to the visible left edge, so a long clip scrolled half
        // off screen still shows its name.
        double labelX = std::max(box.x, scene.handleWidth) + 4.0;
        label(s, scene.labelLayout, name, labelX, box.top + 1.0, box.x + box.w - labelX - 4.0, kLabel);
    }
}

// Thumbnails edge to edge along the clip, each the source frame at its
// left edge; only the ones on screen are requested. (Doc 06 spaced them
// 96 px apart, which left gaps between 16:9 frames at this row height.)
void drawThumbnails(GtkSnapshot *s, const TimelineScene &scene, const core::Clip &clip, const ClipBox &box,
                    double width)
{
    if (!scene.thumbnailFor || box.height < kMinThumbnailHeightPx || box.w < kMinWaveformWidthPx)
        return;
    double left = std::max(box.x, scene.handleWidth);
    double right = std::min(box.x + box.w, width);
    double aspect = 16.0 / 9.0;
    if (scene.model.hasAsset(clip.asset)) {
        const core::MediaInfo &info = scene.model.asset(clip.asset).info;
        if (info.width > 0 && info.height > 0)
            aspect = static_cast<double>(info.width) / info.height;
    }
    const double step = std::clamp(box.height * aspect, 16.0, 256.0);
    graphene_rect_t area = rect(left, box.top, right - left, box.height);
    gtk_snapshot_push_clip(s, &area);
    int first = std::max(0, static_cast<int>((left - box.x) / step));
    const double framesPerTile = step / scene.viewport.pxPerFrame();
    core::FrameIndex grid = 1;
    while (static_cast<double>(grid * 2) <= framesPerTile)
        grid *= 2;
    for (int k = first;; ++k) {
        double tx = box.x + k * step;
        if (tx > right || tx >= box.x + box.w)
            break;
        auto offset = static_cast<core::FrameIndex>(k * step / scene.viewport.pxPerFrame());
        // Snapped to a power-of-two grid a little finer than the tile
        // spacing, so zoom levels within a factor of two ask for the same
        // frames instead of a fresh set each (post-M3 audit P5).
        offset = offset / grid * grid;
        GdkTexture *texture = scene.thumbnailFor(clip, clip.in + offset);
        if (!texture)
            continue; // still being made: the plain fill shows meanwhile
        int tw = gdk_texture_get_width(texture), th = gdk_texture_get_height(texture);
        double w = th > 0 ? box.height * tw / th : box.height;
        graphene_rect_t where = rect(tx, box.top, std::min(w, step), box.height);
        gtk_snapshot_append_texture(s, texture, &where);
    }
    gtk_snapshot_pop(s);
}

void drawWaveform(GtkSnapshot *s, const std::vector<float> &peaks, const ClipBox &box, double width, double handleWidth)
{
    // Doc 06: waveforms drop out on narrow clips, where they'd be a smear
    // and among the most expensive things in the snapshot.
    if (peaks.empty() || box.w < kMinWaveformWidthPx)
        return;
    double visibleLeft = std::max(box.x, handleWidth);
    double visibleRight = std::min(box.x + box.w, width);
    if (visibleRight <= visibleLeft)
        return;
    double midY = box.top + box.height / 2.0;
    double maxHalf = (box.height - 4.0) / 2.0;
    int pixelWidth = std::max(static_cast<int>(box.w), 1);
    size_t peakCount = peaks.size();
    int firstPx = std::clamp(static_cast<int>(visibleLeft - box.x), 0, pixelWidth);
    int endPx = std::clamp(static_cast<int>(visibleRight - box.x) + 1, 0, pixelWidth);
    auto halfAt = [&](int px) {
        auto from = static_cast<size_t>(static_cast<double>(px) / pixelWidth * static_cast<double>(peakCount));
        auto to = std::min(peakCount, std::max(from + 1, static_cast<size_t>(static_cast<double>(px + 1) / pixelWidth *
                                                                             static_cast<double>(peakCount))));
        // At most 8 samples per 1 px column: scanning every peak under a
        // narrow clip was most of the snapshot's time, for no visible gain.
        size_t stride = std::max<size_t>(1, (to - from) / 8);
        float peak = 0.0f;
        for (size_t k = from; k < to; k += stride)
            peak = std::max(peak, peaks[k]);
        return std::max(static_cast<double>(peak) * maxHalf, 1.0);
    };

#if GTK_CHECK_VERSION(4, 14, 0)
    // One filled path of 1 px columns: an order of magnitude cheaper to
    // snapshot than a cairo node per clip (measured in
    // tests/app/test_timeline_render.cpp).
    GskPathBuilder *builder = gsk_path_builder_new();
    for (int px = firstPx; px < endPx; ++px) {
        double half = halfAt(px);
        graphene_rect_t bar = rect(box.x + px, midY - half, 1.0, 2.0 * half);
        gsk_path_builder_add_rect(builder, &bar);
    }
    GskPath *path = gsk_path_builder_free_to_path(builder);
    GdkRGBA violet = rgba(tokens::kBrandViolet, 0.85f);
    gtk_snapshot_append_fill(s, path, GSK_FILL_RULE_WINDING, &violet);
    gsk_path_unref(path);
#else
    graphene_rect_t bounds = rect(visibleLeft, box.top, visibleRight - visibleLeft, box.height);
    cairo_t *cr = gtk_snapshot_append_cairo(s, &bounds);
    const tokens::Rgb v = tokens::kBrandViolet;
    cairo_set_source_rgba(cr, v.r, v.g, v.b, 0.85);
    cairo_set_line_width(cr, 1.0);
    for (int px = firstPx; px < endPx; ++px) {
        double half = halfAt(px);
        double colX = box.x + px + 0.5;
        cairo_move_to(cr, colX, midY - half);
        cairo_line_to(cr, colX, midY + half);
    }
    cairo_stroke(cr);
    cairo_destroy(cr);
#endif
}

void drawDissolveHatch(GtkSnapshot *s, double left, double right, double top, double height)
{
    graphene_rect_t bounds = rect(left, top, right - left, height);
    cairo_t *cr = gtk_snapshot_append_cairo(s, &bounds);
    cairo_rectangle(cr, left, top, right - left, height);
    cairo_clip(cr);
    const tokens::Rgb c = tokens::kBrandCyan;
    cairo_set_source_rgba(cr, c.r, c.g, c.b, 0.6);
    cairo_set_line_width(cr, 1.5);
    constexpr double kSpacing = 7.0;
    for (double sx = left - height; sx < right; sx += kSpacing) {
        cairo_move_to(cr, sx, top + height);
        cairo_line_to(cr, sx + height, top);
    }
    cairo_stroke(cr);
    cairo_destroy(cr);
}

int rowOfTrack(const core::Model &model, core::TrackId track)
{
    const auto &tracks = model.sequence().tracks;
    for (size_t r = 0; r < tracks.size(); ++r) {
        if (tracks[r].id == track)
            return static_cast<int>(r);
    }
    return -1;
}

void drawRows(GtkSnapshot *s, const TimelineScene &scene, double width)
{
    const auto &tracks = scene.model.sequence().tracks;
    const TimelineController::Preview &preview = scene.controller.preview();
    bool reordering = scene.controller.mode() == Mode::TrackReorder;
    const double rowH = scene.layout.rowHeight;
    const double hw = scene.handleWidth;

    for (size_t i = 0; i < tracks.size(); ++i) {
        int row = static_cast<int>(i);
        const core::Track &track = tracks[i];
        double y = scene.layout.rowTop(row);

        // Flat tints, not glows (glow is for selection and focus only).
        if (track.locked)
            fill(s, 0, y, width, rowH, rgba(tokens::kSemanticWarning, 0.08f));
        if (reordering && row == preview.reorderHoverRow)
            fill(s, 0, y, width, rowH, rgba(tokens::kBrandCyan, 0.12f));
        else if (row == scene.activeRow)
            fill(s, 0, y, width, rowH, rgba(tokens::kBrandCyan, 0.07f));
        if (row > 0)
            fill(s, 0, y, width, 1.0, rgba(tokens::kInk500));

        // The drag grip in the handle strip, warning-tinted when locked
        // (the clips can't move, but the track still can).
        GdkRGBA grip = track.locked ? rgba(tokens::kSemanticWarning) : rgba(tokens::kInk500);
        double cx = hw / 2.0, cy = y + rowH / 2.0;
        for (int line = -1; line <= 1; ++line)
            fill(s, cx - 5.0, cy + line * 5.0 - 1.0, 10.0, 2.0, grip);
        fill(s, hw - 1.0, y, 2.0, rowH, grip);

        std::string name = track.name;
        for (auto [on, word] : {std::pair{track.hidden, "Hidden"}, std::pair{track.muted, "Muted"}}) {
            if (on)
                name += name.empty() ? word : std::string(" · ") + word;
        }
        if (row != scene.nameEditRow)
            label(s, scene.labelLayout, name, hw + 4.0, y + 1.0, width - hw - 8.0, kLabel);
    }
}

} // namespace

void snapshotTimeline(GtkSnapshot *s, const TimelineScene &scene, double width, double height)
{
    const core::Model &model = scene.model;
    const auto &tracks = model.sequence().tracks;
    if (tracks.empty())
        return;
    drawRows(s, scene, width);

    const double contentHeight = static_cast<double>(tracks.size()) * scene.layout.rowHeight;
    graphene_rect_t area = rect(scene.handleWidth, 0, std::max(width - scene.handleWidth, 0.0), contentHeight);
    gtk_snapshot_push_clip(s, &area);

    const Mode mode = scene.controller.mode();
    const TimelineController::Preview &preview = scene.controller.preview();
    const Selection &selection = scene.controller.selection();
    const bool clipDrag = mode == Mode::MoveClip || mode == Mode::TrimClipStart || mode == Mode::TrimClipEnd ||
                          mode == Mode::RippleTrimStart || mode == Mode::RippleTrimEnd || mode == Mode::Slip;

    // Clips and their waveforms, skipping whatever a drag draws as a ghost.
    for (size_t r = 0; r < tracks.size(); ++r) {
        int row = static_cast<int>(r);
        for (core::ClipId id : tracks[r].clips) {
            const core::Clip &clip = model.clip(id);
            bool dragged = clipDrag && id == preview.clip;
            bool selected = selection.contains(id);
            if (dragged && mode == Mode::MoveClip)
                continue;
            if (mode == Mode::MoveClip && preview.group && selected)
                continue;
            int drawRow = dragged ? preview.row : row;
            core::FrameIndex start = dragged ? preview.start : clip.position;
            core::FrameIndex length = dragged ? preview.length : clip.length();
            ClipBox box;
            if (!clipBox(scene, drawRow, start, length, width, box))
                continue;
            const std::string &name = !clip.name.empty() ? clip.name : tracks[r].name;
            bool thumbnails =
                scene.thumbnailFor && !dragged && clip.videoEnabled && tracks[r].kind == core::Track::Kind::Video;
            if (thumbnails) {
                // The fill first, so it shows while thumbnails load.
                fill(s, box.left + 1.0, box.top, std::max(box.right - box.left - 2.0, 1.0), box.height,
                     rgba(tokens::kInk700));
                drawThumbnails(s, scene, clip, box, width);
            }
            drawClip(s, scene, box, name, selected, false, dragged && !preview.valid, thumbnails);
            if (!dragged && scene.waveformFor && box.w >= kMinWaveformWidthPx) {
                if (const std::vector<float> *peaks = scene.waveformFor(clip)) {
                    // Under thumbnails, the waveform takes the bottom 40%.
                    ClipBox wave = box;
                    if (thumbnails && box.height >= kMinThumbnailHeightPx) {
                        wave.height = box.height * 0.4;
                        wave.top = box.top + box.height - wave.height;
                    }
                    drawWaveform(s, *peaks, wave, width, scene.handleWidth);
                }
            }
        }
    }

    // Ghosts: the dragged group, or the one moved or copied clip.
    if (mode == Mode::MoveClip && preview.group) {
        for (size_t r = 0; r < tracks.size(); ++r) {
            for (core::ClipId id : tracks[r].clips) {
                if (!selection.contains(id))
                    continue;
                const core::Clip &clip = model.clip(id);
                ClipBox box;
                if (clipBox(scene, static_cast<int>(r) + preview.groupRowDelta, clip.position + preview.groupDelta,
                            clip.length(), width, box))
                    drawClip(s, scene, box, {}, true, true, !preview.valid);
            }
        }
    } else if (mode == Mode::MoveClip || mode == Mode::CopyClip) {
        ClipBox box;
        if (clipBox(scene, preview.row, preview.start, preview.length, width, box))
            drawClip(s, scene, box, {}, true, true, !preview.valid);
    }
    if (mode == Mode::Slip) {
        ClipBox box;
        if (clipBox(scene, preview.row, preview.start, preview.length, width, box)) {
            std::string text =
                "Slip " + std::string(preview.slipDelta > 0 ? "+" : "") + std::to_string(preview.slipDelta) + " frames";
            label(s, scene.labelLayout, text, std::max(box.x, scene.handleWidth) + 4.0, box.top + box.height - 14.0,
                  200.0, kLabel);
        }
    }

    // Dissolves: hatch over the overlap, live while one is being resized.
    for (const core::Transition &t : model.sequence().transitions) {
        if (!model.hasClip(t.a) || !model.hasClip(t.b))
            continue; // mid-undo: the next refresh catches up
        int row = rowOfTrack(model, t.track);
        if (row < 0)
            continue;
        bool resizing =
            t.id == preview.transition && (mode == Mode::TransitionResizeLeft || mode == Mode::TransitionResizeRight);
        core::FrameIndex from = resizing ? preview.transitionLeft : model.clip(t.b).position;
        core::FrameIndex to = resizing ? preview.transitionRight : model.clip(t.a).end();
        double left = std::max(scene.viewport.xForFrame(static_cast<double>(from)), scene.handleWidth - 20.0);
        double right = std::min(scene.viewport.xForFrame(static_cast<double>(to)), width + 20.0);
        if (right > left)
            drawDissolveHatch(s, left, right, scene.layout.clipTop(row), scene.layout.clipHeight());
    }

    // Markers run through the tracks as faint lines.
    for (const core::Marker &marker : model.sequence().markers) {
        double mx = std::floor(scene.viewport.xForFrame(static_cast<double>(marker.at)));
        if (mx >= scene.handleWidth && mx <= width)
            fill(s, mx, 0, 1.0, contentHeight, rgba(tokens::kBrandMagenta, 0.35f));
    }
    // Doc 06: a 1 px brand-magenta line where a dragged edge snapped.
    if (preview.snappedTo) {
        double sx = std::floor(scene.viewport.xForFrame(static_cast<double>(*preview.snappedTo)));
        fill(s, sx, 0, 1.0, contentHeight, rgba(tokens::kBrandMagenta));
    }
    if (mode == Mode::RubberBand) {
        double bx = std::min(preview.bandX0, preview.bandX1), by = std::min(preview.bandY0, preview.bandY1);
        double bw = std::abs(preview.bandX1 - preview.bandX0), bh = std::abs(preview.bandY1 - preview.bandY0);
        fill(s, bx, by, bw, bh, rgba(tokens::kBrandCyan, 0.12f));
        outline(s, bx, by, bw, bh, 1.0f, rgba(tokens::kBrandCyan, 0.8f));
    }

    for (const TimelineOverlayProvider *overlay : scene.overlays)
        overlay->paintOverlay(s, model, scene.viewport, scene.layout, width, height);

    gtk_snapshot_pop(s);
}

void snapshotPlayhead(GtkSnapshot *s, const Viewport &viewport, core::FrameIndex frame, double handleWidth,
                      double width, double height)
{
    double x = viewport.xForFrame(static_cast<double>(frame));
    if (x < handleWidth || x > width)
        return; // scrolled out of view
    fill(s, x - 1.0, 0, 2.0, height, rgba(tokens::kBrandCyan));
}

void snapshotRuler(GtkSnapshot *s, const RulerScene &scene, double width, double height)
{
    double fps = scene.fps > 0.0 ? scene.fps : 25.0;
    double pxPerSecond = scene.viewport.pxPerFrame() * fps;
    if (pxPerSecond <= 0.0)
        return;

    // The smallest of these that keeps ticks at least 60 px apart: a short
    // or zoomed-in view ticks every second, a long one every few minutes.
    static constexpr double kIntervals[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600};
    constexpr double kMinSpacing = 60.0;
    double interval = kIntervals[std::size(kIntervals) - 1];
    for (double candidate : kIntervals) {
        if (candidate * pxPerSecond >= kMinSpacing) {
            interval = candidate;
            break;
        }
    }
    auto step = std::max<core::FrameIndex>(1, static_cast<core::FrameIndex>(interval * fps));
    double spacing = interval * pxPerSecond;

    graphene_rect_t area = rect(scene.handleWidth, 0, std::max(width - scene.handleWidth, 0.0), height);
    gtk_snapshot_push_clip(s, &area);
    // One interval early, so a label whose tick is just off the left edge
    // still shows its tail.
    core::FrameIndex first = std::max<core::FrameIndex>(0, (scene.viewport.firstVisibleFrame() / step - 1) * step);
    core::FrameIndex last = scene.viewport.endVisibleFrame();
    const GdkRGBA tick = rgba(tokens::kInk500);
    for (core::FrameIndex frame = first; frame <= last; frame += step) {
        double x = std::floor(scene.viewport.xForFrame(static_cast<double>(frame)));
        fill(s, x, height - 6.0, 1.0, 6.0, tick);
        if (scene.formatTimecode)
            label(s, scene.labelLayout, scene.formatTimecode(frame), x + 2.0, 2.0, std::max(spacing - 4.0, 1.0),
                  kLabel);
    }

    // Markers: a small flag hanging from the bottom edge.
    for (const core::Marker &marker : scene.model.sequence().markers) {
        double mx = scene.viewport.xForFrame(static_cast<double>(marker.at));
        if (mx < scene.handleWidth - 6.0 || mx > width + 6.0)
            continue;
        graphene_rect_t bounds = rect(mx - 6.0, height - 9.0, 12.0, 9.0);
        cairo_t *cr = gtk_snapshot_append_cairo(s, &bounds);
        const tokens::Rgb m = tokens::kBrandMagenta;
        cairo_set_source_rgb(cr, m.r, m.g, m.b);
        cairo_move_to(cr, mx - 5.0, height - 8.0);
        cairo_line_to(cr, mx + 5.0, height - 8.0);
        cairo_line_to(cr, mx, height);
        cairo_close_path(cr);
        cairo_fill(cr);
        cairo_destroy(cr);
        if (!marker.text.empty())
            label(s, scene.labelLayout, marker.text, mx + 6.0, 2.0, 120.0, kLabel);
    }
    gtk_snapshot_pop(s);
}

} // namespace ustudio::app::timeline
