#include "title_renderer.h"

#include "blur.h"
#include "core/evaluate.h"
#include "core/media/utf8_path.h"

#include <cairo.h>
#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <filesystem>
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

void configure(PangoLayout *layout, const Layer &layer, const std::string &text, double size, bool wrap)
{
    FontDescription desc = describe(layer.font, size);
    pango_layout_set_font_description(layout, desc.get());
    pango_layout_set_text(layout, text.c_str(), -1); // plain text: never markup, whatever a field holds
    PangoAttrList *attrs = pango_attr_list_new();
    if (layer.font.tracking != 0.0)
        pango_attr_list_insert(attrs, pango_attr_letter_spacing_new(
                                          static_cast<int>(std::lround(layer.font.tracking * size * PANGO_SCALE))));
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
                      std::set<std::string> &warnings)
{
    TextLayout out;
    out.layout.reset(pango_layout_new(fonts.context));
    PangoLayout *layout = out.layout.get();
    const bool wrap = layer.fit == Fit::Wrap && layer.w > 0.0;
    double size = layer.font.size;
    configure(layout, layer, text, size, wrap);
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
            configure(layout, layer, text, size, false);
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

cairo_pattern_t *fillPattern(const Fill &fill, const Box &box, double alpha)
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
        cairo_pattern_t *pattern =
            cairo_pattern_create_linear(cx - dx * half, cy - dy * half, cx + dx * half, cy + dy * half);
        cairo_pattern_add_color_stop_rgba(pattern, 0.0, fill.from.r, fill.from.g, fill.from.b, fill.from.a * a);
        cairo_pattern_add_color_stop_rgba(pattern, 1.0, fill.to.r, fill.to.g, fill.to.b, fill.to.a * a);
        return pattern;
    }
    case FillKind::Radial: {
        const double cx = box.x + box.w / 2, cy = box.y + box.h / 2;
        const double radius = std::hypot(box.w, box.h) / 2;
        cairo_pattern_t *pattern = cairo_pattern_create_radial(cx, cy, 0.0, cx, cy, std::max(radius, 1e-3));
        cairo_pattern_add_color_stop_rgba(pattern, 0.0, fill.from.r, fill.from.g, fill.from.b, fill.from.a * a);
        cairo_pattern_add_color_stop_rgba(pattern, 1.0, fill.to.r, fill.to.g, fill.to.b, fill.to.a * a);
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
void strokeAndFill(cairo_t *cr, const Layer &layer, const Box &box)
{
    const bool line = layer.kind == LayerKind::Shape && layer.shape == ShapeKind::Line;
    if (layer.stroke.width > 0.0) {
        const Rgba &c = layer.stroke.color;
        cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a * layer.stroke.opacity);
        cairo_set_line_width(cr, line ? layer.stroke.width : layer.stroke.width * 2);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_stroke_preserve(cr);
    }
    if (!line) {
        if (cairo_pattern_t *pattern = fillPattern(layer.fill, box, 1.0)) {
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

void drawLayer(cairo_t *target, int width, int height, const TitleDocument &doc, const Layer &layer,
               const LayerState &state, const std::map<std::string, std::string> &fields, ThreadFonts &fonts,
               std::set<std::string> &warnings)
{
    const double sx = static_cast<double>(width) / doc.width;
    const double sy = static_cast<double>(height) / doc.height;

    TextLayout text;
    Box box{state.x, state.y, layer.w, layer.h};
    if (layer.kind == LayerKind::Text) {
        const std::string content = substituteFields(layer.text, doc.fields, fields);
        if (content.empty())
            return;
        text = layoutText(fonts, layer, state, content, warnings);
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
    const double strokePad = layer.stroke.width * deviceScale + 2.0; // + antialiasing
    Box bounds = deviceBounds(matrix, box, strokePad);
    double shadowSigma = 0.0, shadowDx = 0.0, shadowDy = 0.0;
    if (layer.shadow.enabled) {
        shadowSigma = layer.shadow.blur * deviceScale;
        shadowDx = layer.shadow.dx * sx;
        shadowDy = layer.shadow.dy * sy;
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
        if (layer.kind == LayerKind::Text) {
            pango_cairo_update_context(cr.get(), fonts.context);
            pango_layout_context_changed(text.layout.get());
            cairo_move_to(cr.get(), text.originX, text.originY);
            pango_cairo_layout_path(cr.get(), text.layout.get());
            strokeAndFill(cr.get(), layer, text.box);
        } else {
            shapePath(cr.get(), layer, box);
            strokeAndFill(cr.get(), layer, box);
        }
    }
    cairo_surface_flush(surface.get());
    // From here the surface is plain pixels placed at (x0, y0).
    cairo_surface_set_device_offset(surface.get(), 0, 0);

    if (layer.shadow.enabled && layer.shadow.opacity > 0.0) {
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
        const Rgba &c = layer.shadow.color;
        cairo_set_source_rgba(target, c.r, c.g, c.b, c.a * layer.shadow.opacity * state.opacity);
        cairo_mask_surface(target, alpha.get(), x0 + shadowDx, y0 + shadowDy);
    }
    cairo_set_source_surface(target, surface.get(), x0, y0);
    cairo_paint_with_alpha(target, state.opacity);
}

} // namespace

RenderResult renderTitle(const TitleDocument &doc, double titleFrame, const std::map<std::string, std::string> &fields,
                         int width, int height)
{
    RenderResult result;
    if (width <= 0 || height <= 0)
        return result;
    result.frame.width = width;
    result.frame.height = height;
    result.frame.pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0u);
    Surface surface(cairo_image_surface_create_for_data(reinterpret_cast<unsigned char *>(result.frame.pixels.data()),
                                                        CAIRO_FORMAT_ARGB32, width, height, width * 4));
    ThreadFonts &fonts = threadFonts();
    fonts.get();
    std::set<std::string> warnings;
    {
        Cairo cr(cairo_create(surface.get()));
        for (const Layer &layer : doc.layers) {
            if (!layer.visible)
                continue;
            const LayerState state = evaluateLayer(layer, doc.timing, titleFrame);
            if (state.opacity <= 0.0 || state.scale <= 0.0)
                continue;
            cairo_save(cr.get());
            drawLayer(cr.get(), width, height, doc, layer, state, fields, fonts, warnings);
            cairo_restore(cr.get());
        }
    }
    cairo_surface_flush(surface.get());
    result.warnings.assign(warnings.begin(), warnings.end());
    return result;
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
