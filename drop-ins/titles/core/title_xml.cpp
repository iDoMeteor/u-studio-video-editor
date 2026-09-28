#include "title_xml.h"

#include "core/media/utf8_path.h"
#include "core/model/animation.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>

// libxml2's BAD_CAST is a C cast.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"

namespace ustudio::titles {

namespace {

// Far beyond any real title; a bigger file is refused unread.
constexpr std::uintmax_t kMaxFileBytes = 16u << 20;
constexpr size_t kMaxLayers = 1000;
constexpr size_t kMaxKeys = 10000;
constexpr int kMaxCanvas = 8192;
constexpr int64_t kMaxZoneFrames = 1'000'000;
// Canvas pixels: a 8K canvas and some room to slide in from off-screen.
constexpr double kMaxCoordinate = 100'000.0;

struct DocFree
{
    void operator()(xmlDoc *doc) const
    {
        xmlFreeDoc(doc);
    }
};

bool is(const xmlNode *node, const char *name)
{
    return node->type == XML_ELEMENT_NODE && xmlStrcmp(node->name, BAD_CAST name) == 0;
}

std::optional<std::string> attr(const xmlNode *node, const char *name)
{
    xmlChar *value = xmlGetProp(node, BAD_CAST name);
    if (!value)
        return std::nullopt;
    std::string out(reinterpret_cast<const char *>(value));
    xmlFree(value);
    return out;
}

std::optional<double> parseNumber(const std::string &text)
{
    double value = 0.0;
    const char *end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc() || ptr != end || !std::isfinite(value))
        return std::nullopt;
    return value;
}

// Reads the attributes of one element into typed fields, collecting the
// first error.
class Attrs
{
  public:
    Attrs(const xmlNode *node, std::string &error) : m_node(node), m_error(error) {}

    void number(const char *name, double &out, double lo, double hi)
    {
        if (auto text = attr(m_node, name)) {
            if (auto value = parseNumber(*text))
                out = std::clamp(*value, lo, hi);
            else
                fail(name, *text);
        }
    }
    template <typename Int> void integer(const char *name, Int &out, Int lo, Int hi)
    {
        if (auto text = attr(m_node, name)) {
            long long value = 0;
            const char *end = text->data() + text->size();
            auto [ptr, ec] = std::from_chars(text->data(), end, value);
            if (ec != std::errc() || ptr != end)
                fail(name, *text);
            else
                out = static_cast<Int>(std::clamp<long long>(value, lo, hi));
        }
    }
    void colour(const char *name, Rgba &out)
    {
        if (auto text = attr(m_node, name)) {
            if (auto value = parseColor(*text))
                out = *value;
            else
                fail(name, *text);
        }
    }
    void flag(const char *name, bool &out)
    {
        if (auto text = attr(m_node, name)) {
            if (*text == "1" || *text == "true")
                out = true;
            else if (*text == "0" || *text == "false")
                out = false;
            else
                fail(name, *text);
        }
    }
    void fail(const char *name, const std::string &text)
    {
        if (m_error.empty())
            m_error = std::string("<") + reinterpret_cast<const char *>(m_node->name) + "> " + name + "=\"" + text +
                      "\" isn't valid (line " + std::to_string(xmlGetLineNo(m_node)) + ")";
    }

  private:
    const xmlNode *m_node;
    std::string &m_error;
};

std::string textContent(const xmlNode *node)
{
    xmlChar *content = xmlNodeGetContent(node);
    if (!content)
        return {};
    std::string out(reinterpret_cast<const char *>(content));
    xmlFree(content);
    return out;
}

void readFill(const xmlNode *node, Fill &fill, std::string &error)
{
    Attrs a(node, error);
    const std::string gradient = attr(node, "gradient").value_or("");
    const std::string kind = attr(node, "kind").value_or("");
    if (kind == "none")
        fill.kind = FillKind::None;
    else if (gradient == "linear")
        fill.kind = FillKind::Linear;
    else if (gradient == "radial")
        fill.kind = FillKind::Radial;
    else if (gradient.empty() && (kind.empty() || kind == "solid"))
        fill.kind = FillKind::Solid;
    else
        a.fail(gradient.empty() ? "kind" : "gradient", gradient.empty() ? kind : gradient);
    a.colour("color", fill.color);
    a.colour("from", fill.from);
    a.colour("to", fill.to);
    if (auto via = attr(node, "via")) {
        if (auto parsed = parseColor(*via))
            fill.via = *parsed;
        else
            a.fail("via", *via);
    }
    a.number("angle", fill.angle, -3600.0, 3600.0);
    a.number("opacity", fill.opacity, 0.0, 1.0);
}

void readAnimation(const xmlNode *node, Layer &layer, std::string &error, std::set<std::string> &warnings)
{
    const std::string name = attr(node, "property").value_or("");
    const std::optional<Property> property = propertyFromName(name);
    if (!property) {
        warnings.insert("animation of \"" + name + "\" isn't supported");
        return;
    }
    PropertyTrack track{*property, {}};
    for (const xmlNode *child = node->children; child; child = child->next) {
        if (!is(child, "key"))
            continue;
        if (track.keys.size() >= kMaxKeys) {
            error = "too many keyframes in one <animate>";
            return;
        }
        TitleKey key;
        Attrs a(child, error);
        a.integer<core::FrameIndex>("at", key.key.at, 0, kMaxZoneFrames);
        a.number("value", key.key.value, -kMaxCoordinate, kMaxCoordinate);
        const std::string zone = attr(child, "zone").value_or("intro");
        if (zone == "intro")
            key.zone = Zone::Intro;
        else if (zone == "hold")
            key.zone = Zone::Hold;
        else if (zone == "outro")
            key.zone = Zone::Outro;
        else
            a.fail("zone", zone);
        if (auto easing = attr(child, "easing")) {
            if (auto parsed = core::easingFromName(*easing))
                key.key.easing = *parsed;
            else
                a.fail("easing", *easing);
        }
        track.keys.push_back(key);
    }
    // Sorted by where they fall in the title; zones order before offsets.
    std::stable_sort(track.keys.begin(), track.keys.end(), [](const TitleKey &x, const TitleKey &y) {
        return x.zone != y.zone ? x.zone < y.zone : x.key.at < y.key.at;
    });
    layer.animation.push_back(std::move(track));
}

template <typename Enum, size_t N>
bool readChoice(const xmlNode *node, const char *name, const std::array<const char *, N> &names, Enum &out, Attrs &a)
{
    auto text = attr(node, name);
    if (!text)
        return true;
    for (size_t i = 0; i < N; ++i)
        if (*text == names[i]) {
            out = static_cast<Enum>(i);
            return true;
        }
    a.fail(name, *text);
    return false;
}

constexpr std::array<const char *, 3> kUnits = {"character", "word", "line"};
constexpr std::array<const char *, 4> kOrders = {"forward", "reverse", "centre-out", "random"};
constexpr std::array<const char *, 3> kZones = {"intro", "hold", "outro"};
constexpr std::array<const char *, 3> kSlots = {"in", "out", "loop"};

void readAnimator(const xmlNode *node, Layer &layer, std::string &error)
{
    Animator animator;
    Attrs a(node, error);
    readChoice(node, "unit", kUnits, animator.unit, a);
    readChoice(node, "order", kOrders, animator.order, a);
    readChoice(node, "zone", kZones, animator.zone, a);
    a.integer<uint32_t>("seed", animator.seed, 0, 0xffffffffu);
    a.number("stagger", animator.stagger, 0.0, 1000.0);
    a.number("spread", animator.spread, 0.0, 100000.0);
    a.flag("alternate", animator.alternate);
    a.integer<core::FrameIndex>("at", animator.at, 0, kMaxZoneFrames);
    for (const xmlNode *child = node->children; child; child = child->next) {
        if (!is(child, "key"))
            continue;
        if (animator.keys.size() >= kMaxKeys) {
            error = "too many keys in one <animator>";
            return;
        }
        AnimatorKey key;
        Attrs k(child, error);
        k.integer<core::FrameIndex>("at", key.at, 0, kMaxZoneFrames);
        k.number("dx", key.dx, -kMaxCoordinate, kMaxCoordinate);
        k.number("dy", key.dy, -kMaxCoordinate, kMaxCoordinate);
        k.number("scale", key.scale, 0.0, 100.0);
        k.number("rotation", key.rotation, -36000.0, 36000.0);
        k.number("opacity", key.opacity, 0.0, 1.0);
        k.number("blur", key.blur, 0.0, 500.0);
        if (auto easing = attr(child, "easing")) {
            if (auto parsed = core::easingFromName(*easing))
                key.easing = *parsed;
            else
                k.fail("easing", *easing);
        }
        animator.keys.push_back(key);
    }
    std::stable_sort(animator.keys.begin(), animator.keys.end(),
                     [](const AnimatorKey &x, const AnimatorKey &y) { return x.at < y.at; });
    layer.animators.push_back(std::move(animator));
}

void readBehavior(const xmlNode *node, Layer &layer, std::string &error)
{
    Behavior behavior;
    Attrs a(node, error);
    readChoice(node, "slot", kSlots, behavior.slot, a);
    behavior.id = attr(node, "id").value_or("");
    a.integer<core::FrameIndex>("duration", behavior.duration, 1, kMaxZoneFrames);
    a.integer<uint32_t>("seed", behavior.seed, 0, 0xffffffffu);
    a.number("amount", behavior.amount, 0.0, 100.0);
    if (auto easing = attr(node, "easing")) {
        if (auto parsed = core::easingFromName(*easing))
            behavior.easing = *parsed;
        else
            a.fail("easing", *easing);
    }
    layer.behaviors.push_back(std::move(behavior));
}

std::optional<Layer> readLayer(const xmlNode *node, std::string &error, std::set<std::string> &warnings)
{
    Layer layer;
    layer.id = attr(node, "id").value_or("");
    const std::string kind = attr(node, "kind").value_or("text");
    Attrs a(node, error);
    if (kind == "text") {
        layer.kind = LayerKind::Text;
    } else if (kind == "shape") {
        layer.kind = LayerKind::Shape;
    } else if (kind == "image") {
        layer.kind = LayerKind::Image;
        layer.src = attr(node, "src").value_or("");
    } else if (kind == "lottie") {
        layer.kind = LayerKind::Lottie;
        layer.src = attr(node, "src").value_or("");
        if (auto loop = attr(node, "loop")) {
            if (*loop == "once")
                layer.loop = false;
            else if (*loop != "loop")
                a.fail("loop", *loop);
        }
        a.number("speed", layer.speed, 0.25, 4.0);
    } else {
        warnings.insert("\"" + kind + "\" layers aren't supported yet");
        return std::nullopt;
    }
    a.flag("visible", layer.visible);
    a.flag("locked", layer.locked);
    a.number("x", layer.x, -kMaxCoordinate, kMaxCoordinate);
    a.number("y", layer.y, -kMaxCoordinate, kMaxCoordinate);
    a.number("w", layer.w, 0.0, kMaxCoordinate);
    a.number("h", layer.h, 0.0, kMaxCoordinate);
    a.number("opacity", layer.opacity, 0.0, 1.0);
    a.number("scale", layer.scale, 0.0, 100.0);
    a.number("rotation", layer.rotation, -36000.0, 36000.0);
    a.number("blur", layer.blur, 0.0, 500.0);
    a.number("radius", layer.radius, 0.0, kMaxCoordinate);
    if (auto align = attr(node, "align")) {
        if (*align == "left")
            layer.align = Align::Left;
        else if (*align == "center")
            layer.align = Align::Center;
        else if (*align == "right")
            layer.align = Align::Right;
        else
            a.fail("align", *align);
    }
    if (auto tags = attr(node, "tags")) {
        if (*tags == "basic")
            layer.basicTags = true;
        else if (*tags != "none")
            a.fail("tags", *tags);
    }
    if (auto fit = attr(node, "fit")) {
        if (*fit == "none")
            layer.fit = Fit::None;
        else if (*fit == "wrap")
            layer.fit = Fit::Wrap;
        else if (*fit == "shrink")
            layer.fit = Fit::Shrink;
        else
            a.fail("fit", *fit);
    }
    if (auto shape = attr(node, "shape")) {
        if (*shape == "rect")
            layer.shape = ShapeKind::Rect;
        else if (*shape == "rounded-rect")
            layer.shape = ShapeKind::RoundedRect;
        else if (*shape == "ellipse")
            layer.shape = ShapeKind::Ellipse;
        else if (*shape == "line")
            layer.shape = ShapeKind::Line;
        else
            warnings.insert("\"" + *shape + "\" shapes aren't supported yet");
    }

    for (const xmlNode *child = node->children; child; child = child->next) {
        if (child->type != XML_ELEMENT_NODE)
            continue;
        if (is(child, "text")) {
            layer.text = textContent(child);
        } else if (is(child, "font")) {
            Attrs f(child, error);
            if (auto family = attr(child, "family"); family && !family->empty())
                layer.font.family = *family;
            f.integer("weight", layer.font.weight, 100, 1000);
            f.number("size", layer.font.size, 1.0, 4000.0);
            f.number("tracking", layer.font.tracking, -1.0, 10.0);
            f.number("line-height", layer.font.lineHeight, 0.1, 10.0);
            layer.font.italic = attr(child, "style").value_or("") == "italic";
        } else if (is(child, "fill")) {
            readFill(child, layer.fill, error);
        } else if (is(child, "stroke")) {
            Attrs s(child, error);
            s.colour("color", layer.stroke.color);
            s.number("width", layer.stroke.width, 0.0, 1000.0);
            s.number("opacity", layer.stroke.opacity, 0.0, 1.0);
        } else if (is(child, "shadow")) {
            layer.shadow.enabled = true;
            Attrs s(child, error);
            s.number("dx", layer.shadow.dx, -kMaxCoordinate, kMaxCoordinate);
            s.number("dy", layer.shadow.dy, -kMaxCoordinate, kMaxCoordinate);
            s.number("blur", layer.shadow.blur, 0.0, 500.0);
            s.colour("color", layer.shadow.color);
            s.number("opacity", layer.shadow.opacity, 0.0, 1.0);
        } else if (is(child, "animate")) {
            readAnimation(child, layer, error, warnings);
        } else if (is(child, "animator")) {
            readAnimator(child, layer, error);
        } else if (is(child, "behavior")) {
            readBehavior(child, layer, error);
        } else {
            warnings.insert(std::string("<") + reinterpret_cast<const char *>(child->name) + "> isn't supported yet");
        }
    }
    return layer;
}

// %.17g-free, locale-free shortest round trip.
std::string num(double value)
{
    return core::formatDouble(value);
}

void setAttr(xmlNode *node, const char *name, const std::string &value)
{
    xmlNewProp(node, BAD_CAST name, BAD_CAST value.c_str());
}

const char *zoneName(Zone zone)
{
    switch (zone) {
    case Zone::Intro:
        return "intro";
    case Zone::Hold:
        return "hold";
    case Zone::Outro:
        return "outro";
    }
    return "intro";
}

void writeFill(xmlNode *parent, const Fill &fill, const char *element = "fill")
{
    xmlNode *node = xmlNewChild(parent, nullptr, BAD_CAST element, nullptr);
    switch (fill.kind) {
    case FillKind::None:
        setAttr(node, "kind", "none");
        break;
    case FillKind::Solid:
        setAttr(node, "color", formatColor(fill.color));
        break;
    case FillKind::Linear:
    case FillKind::Radial:
        setAttr(node, "gradient", fill.kind == FillKind::Linear ? "linear" : "radial");
        setAttr(node, "from", formatColor(fill.from));
        setAttr(node, "to", formatColor(fill.to));
        if (fill.via)
            setAttr(node, "via", formatColor(*fill.via));
        if (fill.kind == FillKind::Linear)
            setAttr(node, "angle", num(fill.angle));
        break;
    }
    if (fill.opacity != 1.0)
        setAttr(node, "opacity", num(fill.opacity));
}

} // namespace

bool isTitleFile(std::string_view path)
{
    constexpr std::string_view kExtension = ".ustitle";
    if (path.size() < kExtension.size())
        return false;
    const std::string_view tail = path.substr(path.size() - kExtension.size());
    return std::equal(tail.begin(), tail.end(), kExtension.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == static_cast<unsigned char>(b);
    });
}

std::expected<ReadResult, std::string> parseTitle(std::string_view xml)
{
    if (xml.size() > kMaxFileBytes)
        return std::unexpected("the title file is too large");
    // No network, no entity substitution (libxml2's default), no DTD load.
    std::unique_ptr<xmlDoc, DocFree> doc(xmlReadMemory(xml.data(), static_cast<int>(xml.size()), "title.ustitle",
                                                       nullptr, XML_PARSE_NONET | XML_PARSE_NOBLANKS));
    if (!doc)
        return std::unexpected("not well-formed XML");
    const xmlNode *root = xmlDocGetRootElement(doc.get());
    if (!root || !is(root, "ustitle"))
        return std::unexpected("not a title file (no <ustitle> root)");

    std::string error;
    std::set<std::string> warnings;
    ReadResult result;
    TitleDocument &title = result.document;
    Attrs a(root, error);
    int version = 0;
    a.integer("version", version, 0, 1'000'000);
    if (version < 1)
        return std::unexpected("the title file has no version");
    if (version > kTitleFormatVersion)
        return std::unexpected("the title was saved by a newer version (format " + std::to_string(version) + ")");
    a.integer("width", title.width, 16, kMaxCanvas);
    a.integer("height", title.height, 16, kMaxCanvas);
    if (auto name = attr(root, "name"))
        title.name = *name;
    if (auto category = attr(root, "category"))
        title.category = *category;
    if (auto ref = attr(root, "template"))
        title.templateRef = *ref;
    if (auto revision = attr(root, "template-revision"))
        title.templateRevision = *revision;
    if (auto fps = attr(root, "fps")) {
        const size_t slash = fps->find('/');
        Attrs dummy(root, error);
        long long num = 0, den = 1;
        const std::string numText = fps->substr(0, slash);
        const std::string denText = slash == std::string::npos ? "1" : fps->substr(slash + 1);
        auto n = std::from_chars(numText.data(), numText.data() + numText.size(), num);
        auto d = std::from_chars(denText.data(), denText.data() + denText.size(), den);
        if (n.ec != std::errc() || d.ec != std::errc() || n.ptr != numText.data() + numText.size() ||
            d.ptr != denText.data() + denText.size() || num <= 0 || den <= 0 || num > 1'000'000 || den > 1'000'000)
            dummy.fail("fps", *fps);
        else {
            title.fpsNum = static_cast<int>(num);
            title.fpsDen = static_cast<int>(den);
        }
    }

    for (const xmlNode *child = root->children; child && error.empty(); child = child->next) {
        if (child->type != XML_ELEMENT_NODE)
            continue;
        if (is(child, "timing")) {
            Attrs t(child, error);
            t.integer<int64_t>("intro", title.timing.intro, 0, kMaxZoneFrames);
            t.integer<int64_t>("hold", title.timing.hold, 0, kMaxZoneFrames);
            t.integer<int64_t>("outro", title.timing.outro, 0, kMaxZoneFrames);
            if (auto mode = attr(child, "hold-mode"); mode && *mode != "elastic")
                warnings.insert("hold-mode \"" + *mode + "\" isn't supported; the hold is elastic");
        } else if (is(child, "background")) {
            readFill(child, title.background, error);
        } else if (is(child, "field")) {
            Field field{attr(child, "name").value_or(""), attr(child, "label").value_or(""),
                        attr(child, "default").value_or("")};
            if (field.name.empty() || field.name.find_first_of("{}") != std::string::npos) {
                error = "a <field> needs a name without braces (line " + std::to_string(xmlGetLineNo(child)) + ")";
                break;
            }
            if (field.label.empty())
                field.label = field.name;
            title.fields.push_back(std::move(field));
        } else if (is(child, "layer")) {
            if (title.layers.size() >= kMaxLayers) {
                error = "too many layers";
                break;
            }
            if (std::optional<Layer> layer = readLayer(child, error, warnings))
                title.layers.push_back(std::move(*layer));
        } else {
            warnings.insert(std::string("<") + reinterpret_cast<const char *>(child->name) + "> isn't supported yet");
        }
    }
    if (!error.empty())
        return std::unexpected(error);
    result.warnings.assign(warnings.begin(), warnings.end());
    return result;
}

std::expected<ReadResult, std::string> readTitle(const std::string &path)
{
    std::error_code ec;
    const std::filesystem::path file = core::pathFromUtf8(path);
    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec)
        return std::unexpected("can't open " + path + " (" + ec.message() + ")");
    if (size > kMaxFileBytes)
        return std::unexpected(path + " is too large for a title");
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::unexpected("can't open " + path);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    auto result = parseTitle(buffer.str());
    if (!result)
        return std::unexpected(path + ": " + result.error());
    result->document.baseDirectory = core::utf8String(file.parent_path());
    return result;
}

std::string writeTitle(const TitleDocument &title)
{
    std::unique_ptr<xmlDoc, DocFree> doc(xmlNewDoc(BAD_CAST "1.0"));
    xmlNode *root = xmlNewNode(nullptr, BAD_CAST "ustitle");
    xmlDocSetRootElement(doc.get(), root);
    const bool animated = std::any_of(title.layers.begin(), title.layers.end(),
                                      [](const Layer &layer) { return layer.kind == LayerKind::Lottie; });
    setAttr(root, "version", animated ? "2" : "1");
    if (!title.name.empty())
        setAttr(root, "name", title.name);
    if (!title.category.empty())
        setAttr(root, "category", title.category);
    if (!title.templateRef.empty()) {
        setAttr(root, "template", title.templateRef);
        setAttr(root, "template-revision", title.templateRevision);
    }
    setAttr(root, "width", std::to_string(title.width));
    setAttr(root, "height", std::to_string(title.height));
    setAttr(root, "fps", std::to_string(title.fpsNum) + "/" + std::to_string(title.fpsDen));

    xmlNode *timing = xmlNewChild(root, nullptr, BAD_CAST "timing", nullptr);
    setAttr(timing, "intro", std::to_string(title.timing.intro));
    setAttr(timing, "hold", std::to_string(title.timing.hold));
    setAttr(timing, "outro", std::to_string(title.timing.outro));
    setAttr(timing, "hold-mode", "elastic");

    if (title.background.kind != FillKind::None)
        writeFill(root, title.background, "background");

    for (const Field &field : title.fields) {
        xmlNode *node = xmlNewChild(root, nullptr, BAD_CAST "field", nullptr);
        setAttr(node, "name", field.name);
        setAttr(node, "label", field.label);
        setAttr(node, "default", field.defaultValue);
    }

    static constexpr const char *kShapes[] = {"rect", "rounded-rect", "ellipse", "line"};
    static constexpr const char *kAligns[] = {"left", "center", "right"};
    static constexpr const char *kFits[] = {"none", "wrap", "shrink"};
    const Layer defaults;
    for (const Layer &layer : title.layers) {
        xmlNode *node = xmlNewChild(root, nullptr, BAD_CAST "layer", nullptr);
        setAttr(node, "id", layer.id);
        static constexpr const char *kKinds[] = {"text", "shape", "image", "lottie"};
        setAttr(node, "kind", kKinds[static_cast<size_t>(layer.kind)]);
        if (layer.kind == LayerKind::Shape)
            setAttr(node, "shape", kShapes[static_cast<size_t>(layer.shape)]);
        if (layer.kind == LayerKind::Image || layer.kind == LayerKind::Lottie)
            setAttr(node, "src", layer.src);
        if (layer.kind == LayerKind::Lottie) {
            if (!layer.loop)
                setAttr(node, "loop", "once");
            if (layer.speed != 1.0)
                setAttr(node, "speed", num(layer.speed));
        }
        setAttr(node, "x", num(layer.x));
        setAttr(node, "y", num(layer.y));
        setAttr(node, "w", num(layer.w));
        setAttr(node, "h", num(layer.h));
        if (layer.kind == LayerKind::Shape && layer.radius != 0.0)
            setAttr(node, "radius", num(layer.radius));
        if (!layer.visible)
            setAttr(node, "visible", "0");
        if (layer.locked)
            setAttr(node, "locked", "1");
        if (layer.opacity != defaults.opacity)
            setAttr(node, "opacity", num(layer.opacity));
        if (layer.scale != defaults.scale)
            setAttr(node, "scale", num(layer.scale));
        if (layer.rotation != defaults.rotation)
            setAttr(node, "rotation", num(layer.rotation));
        if (layer.blur != 0.0)
            setAttr(node, "blur", num(layer.blur));
        if (layer.kind == LayerKind::Text) {
            setAttr(node, "align", kAligns[static_cast<size_t>(layer.align)]);
            setAttr(node, "fit", kFits[static_cast<size_t>(layer.fit)]);
            if (layer.basicTags)
                setAttr(node, "tags", "basic");
            xmlNode *text = xmlNewChild(node, nullptr, BAD_CAST "text", nullptr);
            xmlNodeAddContent(text, BAD_CAST layer.text.c_str()); // escapes &, <, >
            xmlNode *font = xmlNewChild(node, nullptr, BAD_CAST "font", nullptr);
            setAttr(font, "family", layer.font.family);
            setAttr(font, "weight", std::to_string(layer.font.weight));
            setAttr(font, "size", num(layer.font.size));
            if (layer.font.italic)
                setAttr(font, "style", "italic");
            if (layer.font.tracking != 0.0)
                setAttr(font, "tracking", num(layer.font.tracking));
            if (layer.font.lineHeight != 1.0)
                setAttr(font, "line-height", num(layer.font.lineHeight));
        }
        if (layer.kind != LayerKind::Image && layer.kind != LayerKind::Lottie)
            writeFill(node, layer.fill);
        if (layer.stroke.width > 0.0) {
            xmlNode *stroke = xmlNewChild(node, nullptr, BAD_CAST "stroke", nullptr);
            setAttr(stroke, "color", formatColor(layer.stroke.color));
            setAttr(stroke, "width", num(layer.stroke.width));
            if (layer.stroke.opacity != 1.0)
                setAttr(stroke, "opacity", num(layer.stroke.opacity));
        }
        if (layer.shadow.enabled) {
            xmlNode *shadow = xmlNewChild(node, nullptr, BAD_CAST "shadow", nullptr);
            setAttr(shadow, "dx", num(layer.shadow.dx));
            setAttr(shadow, "dy", num(layer.shadow.dy));
            setAttr(shadow, "blur", num(layer.shadow.blur));
            setAttr(shadow, "color", formatColor(layer.shadow.color));
            setAttr(shadow, "opacity", num(layer.shadow.opacity));
        }
        for (const Behavior &behavior : layer.behaviors) {
            xmlNode *b = xmlNewChild(node, nullptr, BAD_CAST "behavior", nullptr);
            setAttr(b, "slot", kSlots[static_cast<size_t>(behavior.slot)]);
            setAttr(b, "id", behavior.id);
            setAttr(b, "duration", std::to_string(behavior.duration));
            setAttr(b, "easing", core::easingName(behavior.easing));
            if (behavior.seed != 1)
                setAttr(b, "seed", std::to_string(behavior.seed));
            if (behavior.amount != 1.0)
                setAttr(b, "amount", num(behavior.amount));
        }
        for (const Animator &animator : layer.animators) {
            xmlNode *a = xmlNewChild(node, nullptr, BAD_CAST "animator", nullptr);
            setAttr(a, "unit", kUnits[static_cast<size_t>(animator.unit)]);
            setAttr(a, "order", kOrders[static_cast<size_t>(animator.order)]);
            setAttr(a, "stagger", num(animator.stagger));
            if (animator.spread > 0.0)
                setAttr(a, "spread", num(animator.spread));
            if (animator.seed != 1)
                setAttr(a, "seed", std::to_string(animator.seed));
            if (animator.alternate)
                setAttr(a, "alternate", "1");
            if (animator.zone != Zone::Intro)
                setAttr(a, "zone", kZones[static_cast<size_t>(animator.zone)]);
            if (animator.at != 0)
                setAttr(a, "at", std::to_string(animator.at));
            const AnimatorKey identity;
            for (const AnimatorKey &key : animator.keys) {
                xmlNode *k = xmlNewChild(a, nullptr, BAD_CAST "key", nullptr);
                setAttr(k, "at", std::to_string(key.at));
                if (key.dx != identity.dx)
                    setAttr(k, "dx", num(key.dx));
                if (key.dy != identity.dy)
                    setAttr(k, "dy", num(key.dy));
                if (key.scale != identity.scale)
                    setAttr(k, "scale", num(key.scale));
                if (key.rotation != identity.rotation)
                    setAttr(k, "rotation", num(key.rotation));
                if (key.opacity != identity.opacity)
                    setAttr(k, "opacity", num(key.opacity));
                if (key.blur != identity.blur)
                    setAttr(k, "blur", num(key.blur));
                if (key.easing != core::Easing::Linear)
                    setAttr(k, "easing", core::easingName(key.easing));
            }
        }
        for (const PropertyTrack &track : layer.animation) {
            xmlNode *animate = xmlNewChild(node, nullptr, BAD_CAST "animate", nullptr);
            setAttr(animate, "property", propertyName(track.property));
            for (const TitleKey &key : track.keys) {
                xmlNode *k = xmlNewChild(animate, nullptr, BAD_CAST "key", nullptr);
                setAttr(k, "at", std::to_string(key.key.at));
                if (key.zone != Zone::Intro)
                    setAttr(k, "zone", zoneName(key.zone));
                setAttr(k, "value", num(key.key.value));
                if (key.key.easing != core::Easing::Linear)
                    setAttr(k, "easing", core::easingName(key.key.easing));
            }
        }
    }

    xmlChar *buffer = nullptr;
    int size = 0;
    xmlDocDumpFormatMemoryEnc(doc.get(), &buffer, &size, "UTF-8", 1);
    std::string out(reinterpret_cast<const char *>(buffer), static_cast<size_t>(size));
    xmlFree(buffer);
    return out;
}

std::string saveTitle(const TitleDocument &doc, const std::string &path)
{
    const std::filesystem::path target = core::pathFromUtf8(path);
    std::filesystem::path temp = target;
    temp += ".part";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
            return "can't write " + core::utf8String(temp);
        const std::string xml = writeTitle(doc);
        out.write(xml.data(), static_cast<std::streamsize>(xml.size()));
        if (!out.flush())
            return "can't write " + core::utf8String(temp);
    }
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return "can't replace " + path + " (" + ec.message() + ")";
    }
    return {};
}

} // namespace ustudio::titles

#pragma GCC diagnostic pop
