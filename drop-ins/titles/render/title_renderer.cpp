#include "title_renderer.h"

#include "blur.h"
#include "core/animation.h"
#include "core/evaluate.h"
#include "core/media/utf8_path.h"
#include "platform/clock.h"

#include <cairo.h>
#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <set>

namespace ustudio::titles {

namespace {

// Bumped by addFontDirectory(): each thread's font map is rebuilt on its
// next render, so it sees the new fonts.
std::atomic<unsigned> g_fontGeneration{0};

// One per rendering thread (Pango's font maps aren't thread-safe, T0).
struct ThreadFonts
{
    PangoFontMap *map = nullptr;
    PangoContext *context = nullptr;
    unsigned generation = ~0u;
    // Requested family (lowercase) -> the family Pango actually picked.
    std::map<std::string, std::string> resolved;

    ~ThreadFonts()
    {
        reset();
    }
    void reset()
    {
        if (context)
            g_object_unref(context);
        if (map)
            g_object_unref(map);
        context = nullptr;
        map = nullptr;
        resolved.clear();
    }
    PangoContext *get()
    {
        const unsigned now = g_fontGeneration.load(std::memory_order_acquire);
        if (context && generation == now)
            return context;
        reset();
        generation = now;
        map = pango_cairo_font_map_new();
        context = pango_font_map_create_context(map);
        // Layout in canvas pixels, independent of the output size: no
        // hinting of outlines or metrics, which would snap glyphs to the
        // output's pixel grid and make a scaled preview differ from export.
        cairo_font_options_t *options = cairo_font_options_create();
        cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_NONE);
        cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
        cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
        pango_cairo_context_set_font_options(context, options);
        cairo_font_options_destroy(options);
        pango_cairo_context_set_resolution(context, 72.0);
        pango_context_set_round_glyph_positions(context, FALSE);
        return context;
    }
};

ThreadFonts &threadFonts()
{
    thread_local ThreadFonts fonts;
    return fonts;
}

std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

template <typename T, void (*Free)(T *)> struct Deleter
{
    void operator()(T *p) const
    {
        Free(p);
    }
};
using Surface = std::unique_ptr<cairo_surface_t, Deleter<cairo_surface_t, cairo_surface_destroy>>;
using Cairo = std::unique_ptr<cairo_t, Deleter<cairo_t, cairo_destroy>>;
using FontDescription =
    std::unique_ptr<PangoFontDescription, Deleter<PangoFontDescription, pango_font_description_free>>;

struct GObjectUnref
{
    void operator()(void *object) const
    {
        g_object_unref(object);
    }
};
using Layout = std::unique_ptr<PangoLayout, GObjectUnref>;

struct Box
{
    double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
};

// The family Pango uses for `desc`, recorded once per thread; a warning
// when it isn't the requested one.
void checkFont(ThreadFonts &fonts, const Font &font, const PangoFontDescription *desc, std::set<std::string> &warnings)
{
    const std::string key = lower(font.family);
    auto it = fonts.resolved.find(key);
    if (it == fonts.resolved.end()) {
        std::string used;
        if (PangoFont *loaded = pango_context_load_font(fonts.context, desc)) {
            PangoFontDescription *actual = pango_font_describe(loaded);
            if (const char *family = pango_font_description_get_family(actual))
                used = family;
            pango_font_description_free(actual);
            g_object_unref(loaded);
        }
        it = fonts.resolved.emplace(key, used).first;
    }
    // Generic names ("Sans") always resolve to some other family.
    static const std::set<std::string> kGeneric = {"sans", "sans-serif", "serif",   "monospace",
                                                   "mono", "system-ui",  "cursive", "fantasy"};
    if (lower(it->second) != key && !kGeneric.contains(key))
        warnings.insert(font.family + " isn't installed; using " + (it->second.empty() ? "a fallback" : it->second));
}

// Decoded PNGs, per thread (the producer draws every frame): the newest
// few, each re-read when its file changes.
struct CachedImage
{
    std::string path;
    std::filesystem::file_time_type modified;
    Surface surface;
};

std::string imagePath(const TitleDocument &doc, const Layer &layer)
{
    const std::filesystem::path src = core::pathFromUtf8(layer.src);
    if (src.is_absolute() || doc.baseDirectory.empty())
        return core::utf8String(src);
    return core::utf8String(core::pathFromUtf8(doc.baseDirectory) / src);
}

// The layer's picture, or null (and a warning) if it doesn't read.
cairo_surface_t *layerImage(const TitleDocument &doc, const Layer &layer, std::set<std::string> &warnings)
{
    thread_local std::vector<CachedImage> cache;
    if (layer.src.empty())
        return nullptr;
    const std::string path = imagePath(doc, layer);
    std::error_code ec;
    const auto modified = std::filesystem::last_write_time(core::pathFromUtf8(path), ec);
    if (ec) {
        warnings.insert("the picture " + layer.src + " isn't there");
        return nullptr;
    }
    for (CachedImage &cached : cache)
        if (cached.path == path && cached.modified == modified)
            return cached.surface.get();
    Surface surface(cairo_image_surface_create_from_png(path.c_str()));
    if (cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS) {
        warnings.insert("the picture " + layer.src + " isn't a PNG that reads");
        return nullptr;
    }
    constexpr size_t kCached = 16;
    std::erase_if(cache, [&](const CachedImage &c) { return c.path == path; });
    if (cache.size() >= kCached)
        cache.erase(cache.begin());
    cache.push_back({path, modified, std::move(surface)});
    return cache.back().surface.get();
}

// An image layer's box: its own size where w or h is 0.
Box imageBox(const Layer &layer, const LayerState &state, cairo_surface_t *image)
{
    double w = layer.w, h = layer.h;
    const double iw = image ? cairo_image_surface_get_width(image) : 0.0;
    const double ih = image ? cairo_image_surface_get_height(image) : 0.0;
    if (iw > 0 && ih > 0) {
        if (w <= 0 && h <= 0) {
            w = iw;
            h = ih;
        } else if (w <= 0) {
            w = h * iw / ih;
        } else if (h <= 0) {
            h = w * ih / iw;
        }
    }
    return {state.x, state.y, std::max(w, 1.0), std::max(h, 1.0)};
}

// A text layer's Pango layout in canvas pixels, fitted to its box, and
// where its top-left goes (`origin`) and the box it fills (`box`).
struct TextLayout
{
    Layout layout;
    double originX = 0.0, originY = 0.0;
    Box box;
};

FontDescription describe(const Font &font, double size)
{
    FontDescription desc(pango_font_description_new());
    // Always a generic fallback: the brand fonts aren't bundled everywhere.
    const std::string families = font.family + ",Sans";
    pango_font_description_set_family(desc.get(), families.c_str());
    pango_font_description_set_weight(desc.get(), static_cast<PangoWeight>(font.weight));
    pango_font_description_set_style(desc.get(), font.italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
    pango_font_description_set_absolute_size(desc.get(), size * PANGO_SCALE);
    return desc;
}

// A tags="basic" layer's <b>, <i> or <u>, by byte range in the text drawn.
struct StyleRun
{
    unsigned start = 0, end = 0;
    char kind = 'b';
};

// `text` without its <b>, <i> and <u> (exactly those; anything else stays
// literal text), and where they applied. An unclosed tag runs to the end.
std::string stripBasicTags(const std::string &text, std::vector<StyleRun> &runs)
{
    std::string out;
    std::map<char, std::vector<unsigned>> open;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '<') {
            const bool closing = i + 1 < text.size() && text[i + 1] == '/';
            const size_t k = i + (closing ? 2 : 1);
            if (k + 1 < text.size() && text[k + 1] == '>' && (text[k] == 'b' || text[k] == 'i' || text[k] == 'u')) {
                const char kind = text[k];
                const auto at = static_cast<unsigned>(out.size());
                if (!closing) {
                    open[kind].push_back(at);
                } else if (!open[kind].empty()) {
                    runs.push_back({open[kind].back(), at, kind});
                    open[kind].pop_back();
                }
                i = k + 1;
                continue;
            }
        }
        out += text[i];
    }
    for (auto &[kind, starts] : open)
        for (unsigned start : starts)
            runs.push_back({start, static_cast<unsigned>(out.size()), kind});
    return out;
}

void configure(PangoLayout *layout, const Layer &layer, const std::string &text, double size, double tracking,
               bool wrap, const std::vector<StyleRun> &runs = {})
{
    FontDescription desc = describe(layer.font, size);
    pango_layout_set_font_description(layout, desc.get());
    pango_layout_set_text(layout, text.c_str(), -1); // plain text: never markup, whatever a field holds
    PangoAttrList *attrs = pango_attr_list_new();
    if (tracking != 0.0)
        pango_attr_list_insert(
            attrs, pango_attr_letter_spacing_new(static_cast<int>(std::lround(tracking * size * PANGO_SCALE))));
    for (const StyleRun &run : runs) {
        PangoAttribute *style =
            run.kind == 'i' ? pango_attr_style_new(PANGO_STYLE_ITALIC)
            : run.kind == 'u'
                ? pango_attr_underline_new(PANGO_UNDERLINE_SINGLE)
                : pango_attr_weight_new(layer.font.weight >= 600 ? PANGO_WEIGHT_ULTRABOLD : PANGO_WEIGHT_BOLD);
        style->start_index = run.start;
        style->end_index = run.end;
        pango_attr_list_insert(attrs, style);
    }
    pango_layout_set_attributes(layout, attrs);
    pango_attr_list_unref(attrs);
    if (layer.font.lineHeight != 1.0)
        pango_layout_set_line_spacing(layout, static_cast<float>(layer.font.lineHeight));
    static constexpr PangoAlignment kAlign[] = {PANGO_ALIGN_LEFT, PANGO_ALIGN_CENTER, PANGO_ALIGN_RIGHT};
    pango_layout_set_alignment(layout, kAlign[static_cast<size_t>(layer.align)]);
    if (wrap) {
        pango_layout_set_width(layout, static_cast<int>(layer.w * PANGO_SCALE));
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    } else {
        pango_layout_set_width(layout, -1);
    }
}

TextLayout layoutText(ThreadFonts &fonts, const Layer &layer, const LayerState &state, const std::string &text,
                      std::set<std::string> &warnings, const std::vector<StyleRun> &runs = {})
{
    TextLayout out;
    out.layout.reset(pango_layout_new(fonts.context));
    PangoLayout *layout = out.layout.get();
    const bool wrap = layer.fit == Fit::Wrap && layer.w > 0.0;
    double size = layer.font.size;
    configure(layout, layer, text, size, state.tracking, wrap, runs);
    checkFont(fonts, layer.font, pango_layout_get_font_description(layout), warnings);

    PangoRectangle logical;
    pango_layout_get_extents(layout, nullptr, &logical);
    if (layer.fit == Fit::Shrink && layer.w > 0.0) {
        // Scale the size by how much it overflows; kerning and hinting-free
        // metrics are close to linear in size, so this settles in a step or
        // two. Never grows.
        for (int attempt = 0; attempt < 6; ++attempt) {
            const double width = static_cast<double>(logical.width) / PANGO_SCALE;
            const double height = static_cast<double>(logical.height) / PANGO_SCALE;
            double ratio = width > layer.w ? layer.w / width : 1.0;
            if (layer.h > 0.0 && height > layer.h)
                ratio = std::min(ratio, layer.h / height);
            if (ratio >= 1.0)
                break;
            size *= attempt == 0 ? ratio : ratio * 0.98;
            configure(layout, layer, text, size, state.tracking, false, runs);
            pango_layout_get_extents(layout, nullptr, &logical);
        }
    }

    const double textW = static_cast<double>(logical.width) / PANGO_SCALE;
    const double textH = static_cast<double>(logical.height) / PANGO_SCALE;
    const double logicalX = static_cast<double>(logical.x) / PANGO_SCALE;
    static constexpr double kAlignFactor[] = {0.0, 0.5, 1.0};
    const double align = kAlignFactor[static_cast<size_t>(layer.align)];
    out.box.y = state.y;
    out.box.h = layer.h > 0.0 ? layer.h : textH;
    out.originY = state.y;
    if (layer.w > 0.0) {
        out.box.x = state.x;
        out.box.w = layer.w;
        // Wrapped text is aligned by Pango within w; otherwise place the
        // block in the box (its lines are aligned to each other by Pango).
        out.originX = wrap ? state.x : state.x + (layer.w - textW) * align - logicalX;
    } else {
        // No box: x is the anchor the alignment refers to.
        out.box.x = state.x - textW * align;
        out.box.w = textW;
        out.originX = out.box.x - logicalX;
    }
    return out;
}

void addStops(cairo_pattern_t *pattern, const Fill &fill, double alpha)
{
    cairo_pattern_add_color_stop_rgba(pattern, 0.0, fill.from.r, fill.from.g, fill.from.b, fill.from.a * alpha);
    if (fill.via)
        cairo_pattern_add_color_stop_rgba(pattern, 0.5, fill.via->r, fill.via->g, fill.via->b, fill.via->a * alpha);
    cairo_pattern_add_color_stop_rgba(pattern, 1.0, fill.to.r, fill.to.g, fill.to.b, fill.to.a * alpha);
}

// `shift` slides a gradient along its axis by that much of its length
// (a shimmer), repeating it mirrored beyond its ends.
cairo_pattern_t *fillPattern(const Fill &fill, const Box &box, double alpha, double shift = 0.0)
{
    const double a = fill.opacity * alpha;
    switch (fill.kind) {
    case FillKind::None:
        return nullptr;
    case FillKind::Solid:
        return cairo_pattern_create_rgba(fill.color.r, fill.color.g, fill.color.b, fill.color.a * a);
    case FillKind::Linear: {
        const double radians = fill.angle * std::numbers::pi / 180.0;
        const double dx = std::cos(radians), dy = std::sin(radians);
        const double cx = box.x + box.w / 2, cy = box.y + box.h / 2;
        // Half the box's extent along the gradient's direction, so the
        // stops sit on the box's edges at any angle.
        const double half = std::abs(box.w / 2 * dx) + std::abs(box.h / 2 * dy);
        const double slide = shift * 2 * half;
        cairo_pattern_t *pattern =
            cairo_pattern_create_linear(cx - dx * half + dx * slide, cy - dy * half + dy * slide,
                                        cx + dx * half + dx * slide, cy + dy * half + dy * slide);
        addStops(pattern, fill, a);
        if (shift != 0.0)
            cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REFLECT);
        return pattern;
    }
    case FillKind::Radial: {
        const double cx = box.x + box.w / 2, cy = box.y + box.h / 2;
        const double radius = std::hypot(box.w, box.h) / 2;
        cairo_pattern_t *pattern = cairo_pattern_create_radial(cx, cy, 0.0, cx, cy, std::max(radius, 1e-3));
        addStops(pattern, fill, a);
        return pattern;
    }
    }
    return nullptr;
}

void shapePath(cairo_t *cr, const Layer &layer, const Box &box)
{
    switch (layer.shape) {
    case ShapeKind::Rect:
        cairo_rectangle(cr, box.x, box.y, box.w, box.h);
        break;
    case ShapeKind::RoundedRect: {
        const double r = std::min({layer.radius, box.w / 2, box.h / 2});
        const double x = box.x, y = box.y, w = box.w, h = box.h;
        const double pi = std::numbers::pi;
        cairo_new_sub_path(cr);
        cairo_arc(cr, x + w - r, y + r, r, -pi / 2, 0);
        cairo_arc(cr, x + w - r, y + h - r, r, 0, pi / 2);
        cairo_arc(cr, x + r, y + h - r, r, pi / 2, pi);
        cairo_arc(cr, x + r, y + r, r, pi, 3 * pi / 2);
        cairo_close_path(cr);
        break;
    }
    case ShapeKind::Ellipse:
        if (box.w > 0.0 && box.h > 0.0) {
            cairo_save(cr);
            cairo_translate(cr, box.x + box.w / 2, box.y + box.h / 2);
            cairo_scale(cr, box.w / 2, box.h / 2);
            cairo_new_sub_path(cr);
            cairo_arc(cr, 0, 0, 1, 0, 2 * std::numbers::pi);
            cairo_restore(cr);
        }
        break;
    case ShapeKind::Line:
        cairo_move_to(cr, box.x, box.y);
        cairo_line_to(cr, box.x + box.w, box.y + box.h);
        break;
    }
}

// Stroke under fill: the stroke is drawn twice as wide and the fill covers
// its inner half, so `width` is what shows outside the shape.
void strokeAndFill(cairo_t *cr, const Layer &layer, const Fill &fill, const Box &box, double shift = 0.0,
                   double alpha = 1.0)
{
    const bool line = layer.kind == LayerKind::Shape && layer.shape == ShapeKind::Line;
    if (layer.stroke.width > 0.0) {
        const Rgba &c = layer.stroke.color;
        cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a * layer.stroke.opacity * alpha);
        cairo_set_line_width(cr, line ? layer.stroke.width : layer.stroke.width * 2);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_stroke_preserve(cr);
    }
    if (!line) {
        if (cairo_pattern_t *pattern = fillPattern(fill, box, alpha, shift)) {
            cairo_set_source(cr, pattern);
            cairo_fill_preserve(cr);
            cairo_pattern_destroy(pattern);
        }
    }
    cairo_new_path(cr);
}

// Where `box`, drawn through `matrix`, lands in device pixels, grown by
// `pad` device pixels.
Box deviceBounds(const cairo_matrix_t &matrix, const Box &box, double pad)
{
    std::array<std::pair<double, double>, 4> corners = {
        std::pair{box.x, box.y}, {box.x + box.w, box.y}, {box.x, box.y + box.h}, {box.x + box.w, box.y + box.h}};
    double minX = INFINITY, minY = INFINITY, maxX = -INFINITY, maxY = -INFINITY;
    for (auto &[x, y] : corners) {
        cairo_matrix_transform_point(&matrix, &x, &y);
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
    }
    return {minX - pad, minY - pad, maxX - minX + 2 * pad, maxY - minY + 2 * pad};
}

// --- Text in units (animators, the typewriter) --------------------------------

// One grapheme cluster of the laid-out text: its glyphs, where they sit
// (canvas pixels, relative to the layout's origin) and which character,
// word and line it belongs to.
struct Cluster
{
    PangoFont *font = nullptr; // the run's; owned by the layout
    std::unique_ptr<PangoGlyphString, Deleter<PangoGlyphString, pango_glyph_string_free>> glyphs;
    double x = 0.0, baseline = 0.0; // where the glyphs start
    Box box;                        // logical extents
    size_t character = 0, word = 0, line = 0;
    bool space = false;
};

std::vector<Cluster> clustersOf(PangoLayout *layout, const std::string &text)
{
    std::vector<Cluster> clusters;
    PangoLayoutIter *iter = pango_layout_get_iter(layout);
    const PangoLayoutLine *lastLine = nullptr;
    size_t line = 0, character = 0, word = 0;
    bool afterSpace = true, anyWord = false;
    do {
        const PangoLayoutLine *current = pango_layout_iter_get_line_readonly(iter);
        if (lastLine && current != lastLine) {
            ++line;
            afterSpace = true; // a new line starts a new word
        }
        lastLine = current;
        PangoGlyphItem *run = pango_layout_iter_get_run_readonly(iter);
        if (!run)
            continue;
        PangoRectangle runLogical;
        pango_layout_iter_get_run_extents(iter, nullptr, &runLogical);
        const double baseline = static_cast<double>(pango_layout_iter_get_baseline(iter)) / PANGO_SCALE;
        const PangoGlyphString *glyphs = run->glyphs;
        PangoGlyphItemIter cluster;
        for (bool more = pango_glyph_item_iter_init_start(&cluster, run, text.c_str()); more;
             more = pango_glyph_item_iter_next_cluster(&cluster)) {
            // The cluster's glyphs, in visual order.
            int lo = cluster.start_glyph, hi = cluster.end_glyph;
            if (lo > hi) { // right to left: the glyphs (end, start]
                const int first = hi + 1, last = lo + 1;
                lo = first;
                hi = last;
            }
            int before = 0;
            for (int g = 0; g < lo; ++g)
                before += glyphs->glyphs[g].geometry.width;
            Cluster c;
            c.font = run->item->analysis.font;
            c.glyphs.reset(pango_glyph_string_new());
            pango_glyph_string_set_size(c.glyphs.get(), hi - lo);
            int width = 0;
            for (int g = lo; g < hi; ++g) {
                c.glyphs->glyphs[g - lo] = glyphs->glyphs[g];
                c.glyphs->log_clusters[g - lo] = 0;
                width += glyphs->glyphs[g].geometry.width;
            }
            c.x = static_cast<double>(runLogical.x + before) / PANGO_SCALE;
            c.baseline = baseline;
            c.box = {c.x, static_cast<double>(runLogical.y) / PANGO_SCALE, static_cast<double>(width) / PANGO_SCALE,
                     static_cast<double>(runLogical.height) / PANGO_SCALE};
            const std::string_view bytes(text.data() + cluster.start_index,
                                         static_cast<size_t>(cluster.end_index - cluster.start_index));
            c.space = bytes.find_first_not_of(" \t\n") == std::string_view::npos;
            c.line = line;
            if (!c.space) {
                if (afterSpace && anyWord)
                    ++word;
                anyWord = true;
                c.word = word;
                c.character = character++;
            }
            afterSpace = c.space;
            clusters.push_back(std::move(c));
        }
    } while (pango_layout_iter_next_run(iter));
    pango_layout_iter_free(iter);
    return clusters;
}

// The union of the boxes of the clusters that `pick` selects.
template <typename Pick> Box unionOf(const std::vector<Cluster> &clusters, Pick pick)
{
    double left = INFINITY, top = INFINITY, right = -INFINITY, bottom = -INFINITY;
    for (const Cluster &c : clusters)
        if (!c.space && pick(c)) {
            left = std::min(left, c.box.x);
            top = std::min(top, c.box.y);
            right = std::max(right, c.box.x + c.box.w);
            bottom = std::max(bottom, c.box.y + c.box.h);
        }
    return left > right ? Box{} : Box{left, top, right - left, bottom - top};
}

// A unit's offsets as a transform about `pivot` (canvas pixels).
void applyUnit(cairo_matrix_t &m, const UnitState &u, double px, double py)
{
    cairo_matrix_translate(&m, u.dx + px, u.dy + py);
    cairo_matrix_rotate(&m, u.rotation * std::numbers::pi / 180.0);
    cairo_matrix_scale(&m, u.scale, u.scale);
    cairo_matrix_translate(&m, -px, -py);
}

// Draws text one cluster at a time: each on its line's, word's and
// character's offsets (whichever are animated), with the typewriter's
// cursor. `cr` has the layer's transform; `ox, oy` is the layout's origin.
void drawTextUnits(cairo_t *cr, const Layer &layer, const Fill &fill, const Box &textBox, double shift,
                   PangoLayout *layout, const std::string &text, double ox, double oy, const Expansion &expansion,
                   const Timing &timing, double titleFrame)
{
    const std::vector<Cluster> clusters = clustersOf(layout, text);
    size_t characters = 0, words = 0, lines = 0;
    for (const Cluster &c : clusters) {
        lines = std::max(lines, c.line + 1);
        if (!c.space) {
            characters = std::max(characters, c.character + 1);
            words = std::max(words, c.word + 1);
        }
    }
    const auto states = [&](AnimatorUnit unit, size_t count) {
        return evaluateUnits(layer, expansion, timing, titleFrame, unit, count);
    };
    const std::vector<UnitState> byChar = states(AnimatorUnit::Character, characters);
    const std::vector<UnitState> byWord = states(AnimatorUnit::Word, words);
    const std::vector<UnitState> byLine = states(AnimatorUnit::Line, lines);
    // Pivots: each word's and line's own centre.
    std::vector<Box> wordBoxes(words), lineBoxes(lines);
    for (size_t w = 0; w < words; ++w)
        wordBoxes[w] = unionOf(clusters, [w](const Cluster &c) { return c.word == w; });
    for (size_t l = 0; l < lines; ++l)
        lineBoxes[l] = unionOf(clusters, [l](const Cluster &c) { return c.line == l; });

    cairo_matrix_t layerMatrix;
    cairo_get_matrix(cr, &layerMatrix);
    cairo_surface_t *surface = cairo_get_target(cr);
    for (const Cluster &c : clusters) {
        if (c.space || c.glyphs->num_glyphs == 0)
            continue;
        const UnitState &line = byLine[c.line], &word = byWord[c.word], &character = byChar[c.character];
        const double opacity = line.opacity * word.opacity * character.opacity;
        if (opacity <= 0.0)
            continue;
        const double blur = line.blur + word.blur + character.blur;
        // Canvas-space transform: line, then word, then character, each
        // about its own centre.
        cairo_matrix_t unit;
        cairo_matrix_init_identity(&unit);
        const Box &lb = lineBoxes[c.line], &wb = wordBoxes[c.word];
        applyUnit(unit, line, ox + lb.x + lb.w / 2, oy + lb.y + lb.h / 2);
        applyUnit(unit, word, ox + wb.x + wb.w / 2, oy + wb.y + wb.h / 2);
        applyUnit(unit, character, ox + c.box.x + c.box.w / 2, oy + c.box.y + c.box.h / 2);
        cairo_matrix_t full;
        cairo_matrix_multiply(&full, &unit, &layerMatrix);

        const auto draw = [&](cairo_t *into) {
            cairo_set_matrix(into, &full);
            cairo_move_to(into, ox + c.x, oy + c.baseline);
            pango_cairo_glyph_string_path(into, c.font, c.glyphs.get());
            strokeAndFill(into, layer, fill, textBox, shift, opacity);
        };
        if (blur < 0.5) {
            draw(cr);
            continue;
        }
        // Blurred: drawn alone in a small surface around it, then blurred.
        const double sigma = blur * std::sqrt(std::abs(full.xx * full.yy - full.xy * full.yx));
        const Box device = deviceBounds(full, {ox + c.box.x, oy + c.box.y, c.box.w, c.box.h},
                                        std::ceil(3 * sigma) + layer.stroke.width + 2);
        const int x0 = static_cast<int>(std::floor(device.x)), y0 = static_cast<int>(std::floor(device.y));
        const int w = static_cast<int>(std::ceil(device.w)) + 1, h = static_cast<int>(std::ceil(device.h)) + 1;
        if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
            continue;
        Surface piece(cairo_surface_create_similar_image(surface, CAIRO_FORMAT_ARGB32, w, h));
        cairo_surface_set_device_offset(piece.get(), -x0, -y0);
        {
            Cairo pc(cairo_create(piece.get()));
            draw(pc.get());
        }
        cairo_surface_flush(piece.get());
        blurArgb(cairo_image_surface_get_data(piece.get()), w, h, cairo_image_surface_get_stride(piece.get()), sigma);
        cairo_surface_mark_dirty(piece.get());
        cairo_save(cr);
        cairo_identity_matrix(cr);
        cairo_set_source_surface(cr, piece.get(), 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    }
    cairo_set_matrix(cr, &layerMatrix);

    // The typewriter's cursor: after the last character shown.
    if (auto cursor = cursorState(expansion, titleFrame, characters); cursor && cursor->visible) {
        double x = ox, top = oy, height = layer.font.size;
        for (const Cluster &c : clusters)
            if (!c.space && cursor->shown > 0 && c.character == cursor->shown - 1) {
                x = ox + c.box.x + c.box.w;
                top = oy + c.box.y;
                height = c.box.h;
            }
        if (cursor->shown == 0 && !clusters.empty()) {
            x = ox + clusters.front().box.x;
            top = oy + clusters.front().box.y;
            height = clusters.front().box.h;
        }
        const double barWidth = std::max(2.0, layer.font.size * 0.06);
        cairo_rectangle(cr, x + barWidth * 0.5, top + height * 0.1, barWidth, height * 0.8);
        if (cairo_pattern_t *pattern = fillPattern(fill, textBox, 1.0, shift)) {
            cairo_set_source(cr, pattern);
            cairo_fill(cr);
            cairo_pattern_destroy(pattern);
        }
        cairo_new_path(cr);
    }
}

void drawLayer(cairo_t *target, int width, int height, const TitleDocument &doc, const Layer &layer,
               const Expansion &expansion, const LayerState &state, double titleFrame,
               const std::map<std::string, std::string> &fields, const FieldClock &clock, ThreadFonts &fonts,
               std::set<std::string> &warnings)
{
    const double sx = static_cast<double>(width) / doc.width;
    const double sy = static_cast<double>(height) / doc.height;
    // The layer as it is now: its fill's animated colour, and a glow when a
    // behaviour asks for one and it has no shadow of its own.
    Fill fill = layer.fill;
    if (fill.kind == FillKind::Solid)
        fill.color = state.fill;
    // A shimmer on a solid fill sweeps a highlight: the colour, a lighter
    // band, the colour, as a gradient the Shift loop slides.
    const bool shimmers = std::any_of(expansion.loops.begin(), expansion.loops.end(),
                                      [](const Loop &loop) { return loop.property == Property::Shift; });
    if (shimmers && fill.kind == FillKind::Solid) {
        const Rgba base = fill.color;
        fill.kind = FillKind::Linear;
        fill.from = fill.to = base;
        fill.via =
            Rgba{base.r + (1.0 - base.r) * 0.7, base.g + (1.0 - base.g) * 0.7, base.b + (1.0 - base.b) * 0.7, base.a};
        fill.angle = 20.0;
    }
    Shadow shadow = layer.shadow;
    if (!shadow.enabled && expansion.glow)
        shadow = *expansion.glow;
    shadow.opacity *= state.shadowOpacity;

    TextLayout text;
    std::string content;
    Box box{state.x, state.y, layer.w, layer.h};
    cairo_surface_t *image = nullptr;
    const bool units =
        layer.kind == LayerKind::Text && (!animatedUnits(layer, expansion).empty() || expansion.cursor.has_value());
    // Room for units to move beyond the text's own box.
    double unitReach = 0.0;
    if (layer.kind == LayerKind::Image) {
        image = layerImage(doc, layer, warnings);
        if (!image)
            return;
        box = imageBox(layer, state, image);
    }
    if (layer.kind == LayerKind::Text) {
        std::string filled = substituteFields(layer.text, doc.fields, fields, clock);
        std::vector<StyleRun> runs;
        if (layer.basicTags)
            filled = stripBasicTags(filled, runs);
        content = scrambledText(filled, expansion, titleFrame);
        if (content != filled)
            runs.clear(); // a scramble's letters: the styled ranges no longer line up
        if (content.empty())
            return;
        text = layoutText(fonts, layer, state, content, warnings, runs);
        box = text.box;
        // Ink can reach outside the logical box (italics, accents).
        PangoRectangle ink;
        pango_layout_get_extents(text.layout.get(), &ink, nullptr);
        const double inkX = text.originX + static_cast<double>(ink.x) / PANGO_SCALE;
        const double inkY = text.originY + static_cast<double>(ink.y) / PANGO_SCALE;
        const double inkW = static_cast<double>(ink.width) / PANGO_SCALE;
        const double inkH = static_cast<double>(ink.height) / PANGO_SCALE;
        const double left = std::min(box.x, inkX), top = std::min(box.y, inkY);
        const double right = std::max(box.x + box.w, inkX + inkW), bottom = std::max(box.y + box.h, inkY + inkH);
        box.x = left;
        box.y = top;
        box.w = right - left;
        box.h = bottom - top;
        if (units) {
            // As far as any unit's offsets can carry it.
            for (const Animator *a : [&] {
                     std::vector<const Animator *> all;
                     for (const Animator &x : layer.animators)
                         all.push_back(&x);
                     for (const Animator &x : expansion.animators)
                         all.push_back(&x);
                     return all;
                 }())
                for (const AnimatorKey &k : a->keys)
                    unitReach = std::max(unitReach, std::abs(k.dx) + std::abs(k.dy) + 3 * k.blur +
                                                        std::max(0.0, k.scale - 1.0) * std::hypot(box.w, box.h));
        }
        // The fill's gradient still spans the layer's own box.
    }

    // Canvas -> device: the output scale, then the layer's rotation and
    // scale about its box's centre.
    const Box &own = layer.kind == LayerKind::Text ? text.box : box;
    const double cx = own.x + own.w / 2, cy = own.y + own.h / 2;
    cairo_matrix_t matrix;
    cairo_matrix_init_scale(&matrix, sx, sy);
    cairo_matrix_translate(&matrix, cx, cy);
    cairo_matrix_rotate(&matrix, state.rotation * std::numbers::pi / 180.0);
    cairo_matrix_scale(&matrix, state.scale, state.scale);
    cairo_matrix_translate(&matrix, -cx, -cy);

    const double deviceScale = (sx + sy) / 2 * state.scale;
    const double layerSigma = state.blur * deviceScale;
    const double strokePad = (layer.stroke.width + unitReach) * deviceScale + 2.0 + std::ceil(3 * layerSigma);
    Box bounds = deviceBounds(matrix, box, strokePad);
    double shadowSigma = 0.0, shadowDx = 0.0, shadowDy = 0.0;
    if (shadow.enabled) {
        shadowSigma = shadow.blur * deviceScale;
        shadowDx = shadow.dx * sx;
        shadowDy = shadow.dy * sy;
        // Room for the blur to spread inside the layer's own surface.
        const double spread = std::ceil(3 * shadowSigma);
        bounds.x -= spread;
        bounds.y -= spread;
        bounds.w += 2 * spread;
        bounds.h += 2 * spread;
    }
    // Only what can reach the frame (the shadow may bring in content from
    // just outside it).
    const double reachX = std::abs(shadowDx), reachY = std::abs(shadowDy);
    const int x0 = static_cast<int>(std::floor(std::max(bounds.x, -reachX)));
    const int y0 = static_cast<int>(std::floor(std::max(bounds.y, -reachY)));
    const int x1 = static_cast<int>(std::ceil(std::min(bounds.x + bounds.w, width + reachX)));
    const int y1 = static_cast<int>(std::ceil(std::min(bounds.y + bounds.h, height + reachY)));
    if (x1 <= x0 || y1 <= y0)
        return;

    Surface surface(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, x1 - x0, y1 - y0));
    cairo_surface_set_device_offset(surface.get(), -x0, -y0);
    {
        Cairo cr(cairo_create(surface.get()));
        cairo_set_matrix(cr.get(), &matrix);
        cairo_set_antialias(cr.get(), CAIRO_ANTIALIAS_GRAY);
        if (state.reveal < 1.0) {
            // A wipe: the layer's own box, from its left edge (generously
            // tall, so ink and stroke above and below aren't cut).
            const double tall = own.h + 2 * (layer.stroke.width + layer.font.size + unitReach);
            cairo_rectangle(cr.get(), own.x - layer.stroke.width - 1, own.y - tall / 2 + own.h / 2,
                            (own.w + 2 * layer.stroke.width + 2) * state.reveal, tall);
            cairo_clip(cr.get());
        }
        if (layer.kind == LayerKind::Text) {
            pango_cairo_update_context(cr.get(), fonts.context);
            pango_layout_context_changed(text.layout.get());
            if (units) {
                drawTextUnits(cr.get(), layer, fill, text.box, state.shift, text.layout.get(), content, text.originX,
                              text.originY, expansion, doc.timing, titleFrame);
            } else {
                cairo_move_to(cr.get(), text.originX, text.originY);
                pango_cairo_layout_path(cr.get(), text.layout.get());
                strokeAndFill(cr.get(), layer, fill, text.box, state.shift);
            }
        } else if (layer.kind == LayerKind::Image) {
            cairo_save(cr.get());
            cairo_translate(cr.get(), box.x, box.y);
            cairo_scale(cr.get(), box.w / cairo_image_surface_get_width(image),
                        box.h / cairo_image_surface_get_height(image));
            cairo_set_source_surface(cr.get(), image, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr.get()), CAIRO_FILTER_GOOD);
            cairo_paint(cr.get());
            cairo_restore(cr.get());
            if (layer.stroke.width > 0.0) {
                cairo_rectangle(cr.get(), box.x, box.y, box.w, box.h);
                const Rgba &c = layer.stroke.color;
                cairo_set_source_rgba(cr.get(), c.r, c.g, c.b, c.a * layer.stroke.opacity);
                cairo_set_line_width(cr.get(), layer.stroke.width * 2);
                cairo_stroke(cr.get());
            }
        } else {
            shapePath(cr.get(), layer, box);
            strokeAndFill(cr.get(), layer, fill, box, state.shift);
        }
    }
    cairo_surface_flush(surface.get());
    // From here the surface is plain pixels placed at (x0, y0).
    cairo_surface_set_device_offset(surface.get(), 0, 0);
    if (layerSigma >= 0.5) {
        blurArgb(cairo_image_surface_get_data(surface.get()), x1 - x0, y1 - y0,
                 cairo_image_surface_get_stride(surface.get()), layerSigma);
        cairo_surface_mark_dirty(surface.get());
    }

    if (shadow.enabled && shadow.opacity > 0.0) {
        Surface alpha(cairo_image_surface_create(CAIRO_FORMAT_A8, x1 - x0, y1 - y0));
        {
            Cairo cr(cairo_create(alpha.get()));
            cairo_set_source_surface(cr.get(), surface.get(), 0, 0);
            cairo_set_operator(cr.get(), CAIRO_OPERATOR_SOURCE);
            cairo_paint(cr.get());
        }
        cairo_surface_flush(alpha.get());
        blurAlpha(cairo_image_surface_get_data(alpha.get()), x1 - x0, y1 - y0,
                  cairo_image_surface_get_stride(alpha.get()), shadowSigma);
        cairo_surface_mark_dirty(alpha.get());
        const Rgba &c = shadow.color;
        cairo_set_source_rgba(target, c.r, c.g, c.b, std::min(1.0, c.a * shadow.opacity) * state.opacity);
        cairo_mask_surface(target, alpha.get(), x0 + shadowDx, y0 + shadowDy);
    }
    cairo_set_source_surface(target, surface.get(), x0, y0);
    cairo_paint_with_alpha(target, state.opacity);
}

} // namespace

FieldClock designerClock(const TitleDocument &doc, double titleFrame)
{
    FieldClock clock;
    clock.clipFrame = clock.timelineFrame = titleFrame;
    clock.fps = doc.fpsDen > 0 ? static_cast<double>(doc.fpsNum) / doc.fpsDen : 30.0;
    clock.localTime = platform::localTime(std::time(nullptr));
    return clock;
}

RenderResult renderTitle(const TitleDocument &doc, double titleFrame, const std::map<std::string, std::string> &fields,
                         int width, int height, const FieldClock *clock)
{
    RenderResult result;
    if (width <= 0 || height <= 0)
        return result;
    result.frame.width = width;
    result.frame.height = height;
    result.frame.pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0u);
    Surface surface(cairo_image_surface_create_for_data(reinterpret_cast<unsigned char *>(result.frame.pixels.data()),
                                                        CAIRO_FORMAT_ARGB32, width, height, width * 4));
    const FieldClock at = clock ? *clock : designerClock(doc, titleFrame);
    ThreadFonts &fonts = threadFonts();
    fonts.get();
    std::set<std::string> warnings;
    {
        Cairo cr(cairo_create(surface.get()));
        if (cairo_pattern_t *background = fillPattern(
                doc.background, {0.0, 0.0, static_cast<double>(doc.width), static_cast<double>(doc.height)}, 1.0)) {
            cairo_scale(cr.get(), static_cast<double>(width) / doc.width, static_cast<double>(height) / doc.height);
            cairo_set_source(cr.get(), background);
            cairo_paint(cr.get());
            cairo_pattern_destroy(background);
            cairo_identity_matrix(cr.get());
        }
        for (const Layer &layer : doc.layers) {
            if (!layer.visible)
                continue;
            const Expansion expansion = expandBehaviors(layer, doc.timing);
            const LayerState state = evaluateLayer(layer, expansion, doc.timing, titleFrame);
            if (state.opacity <= 0.0 || state.scale <= 0.0)
                continue;
            cairo_save(cr.get());
            drawLayer(cr.get(), width, height, doc, layer, expansion, state, titleFrame, fields, at, fonts, warnings);
            cairo_restore(cr.get());
        }
    }
    cairo_surface_flush(surface.get());
    result.warnings.assign(warnings.begin(), warnings.end());
    return result;
}

std::vector<LayerGeometry> measureLayers(const TitleDocument &doc, double titleFrame,
                                         const std::map<std::string, std::string> &fields, const FieldClock *clock)
{
    const FieldClock at = clock ? *clock : designerClock(doc, titleFrame);
    ThreadFonts &fonts = threadFonts();
    fonts.get();
    std::set<std::string> warnings;
    std::vector<LayerGeometry> out;
    out.reserve(doc.layers.size());
    for (const Layer &layer : doc.layers) {
        const LayerState state = evaluateLayer(layer, doc.timing, titleFrame);
        LayerGeometry geometry{
            layer.id, {state.x, state.y, layer.w, layer.h}, state.rotation, state.scale, layer.visible};
        geometry.locked = layer.locked;
        if (layer.kind == LayerKind::Image) {
            const Box box = imageBox(layer, state, layerImage(doc, layer, warnings));
            geometry.box = {box.x, box.y, box.w, box.h};
        }
        if (layer.kind == LayerKind::Text) {
            std::string content = substituteFields(layer.text, doc.fields, fields, at);
            std::vector<StyleRun> runs;
            if (layer.basicTags)
                content = stripBasicTags(content, runs);
            if (content.empty())
                content = " "; // an empty text layer still has a line's height to grab
            const TextLayout text = layoutText(fonts, layer, state, content, warnings, runs);
            geometry.box = {text.box.x, text.box.y, text.box.w, text.box.h};
        }
        out.push_back(std::move(geometry));
    }
    return out;
}

void toStraightRgba(const RenderedFrame &frame, uint8_t *out)
{
    for (uint32_t pixel : frame.pixels) {
        const uint32_t a = pixel >> 24;
        uint32_t r = (pixel >> 16) & 0xff, g = (pixel >> 8) & 0xff, b = pixel & 0xff;
        if (a != 0 && a != 255) {
            r = (r * 255 + a / 2) / a;
            g = (g * 255 + a / 2) / a;
            b = (b * 255 + a / 2) / a;
        }
        out[0] = static_cast<uint8_t>(std::min<uint32_t>(r, 255));
        out[1] = static_cast<uint8_t>(std::min<uint32_t>(g, 255));
        out[2] = static_cast<uint8_t>(std::min<uint32_t>(b, 255));
        out[3] = static_cast<uint8_t>(a);
        out += 4;
    }
}

bool addFontDirectory(const std::string &directory)
{
    // fontconfig's current config is process-wide; guard our own writers.
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    // fontconfig accepts a directory that doesn't exist.
    std::error_code ec;
    if (!std::filesystem::is_directory(core::pathFromUtf8(directory), ec))
        return false;
    if (!FcConfigAppFontAddDir(nullptr, reinterpret_cast<const FcChar8 *>(directory.c_str())))
        return false;
    g_fontGeneration.fetch_add(1, std::memory_order_release);
    return true;
}

} // namespace ustudio::titles
