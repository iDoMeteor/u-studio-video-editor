// titlerender (drop-ins/titles/render): what a title looks like in pixels,
// probed at known points, and that it's the same on every thread and
// every run (the editor, export and the titles app all draw with it).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/title_xml.h"
#include "render/blur.h"
#include "render/title_renderer.h"

#include <cstdlib>
#include <filesystem>
#include <thread>
#include <vector>

using namespace ustudio::titles;

namespace {

struct Pixel
{
    int r, g, b, a;
};

struct Straight
{
    int width = 0, height = 0;
    std::vector<uint8_t> bytes;

    Pixel at(int x, int y) const
    {
        const uint8_t *p =
            bytes.data() + (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
        return {p[0], p[1], p[2], p[3]};
    }
    // The bounding box of pixels with any alpha: {x0, y0, x1, y1}, or all -1.
    std::array<int, 4> inkBounds() const
    {
        std::array<int, 4> box{-1, -1, -1, -1};
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                if (at(x, y).a > 0) {
                    if (box[0] < 0 || x < box[0])
                        box[0] = x;
                    if (box[1] < 0)
                        box[1] = y;
                    box[2] = std::max(box[2], x);
                    box[3] = y;
                }
        return box;
    }
};

Straight straight(const RenderResult &result)
{
    Straight out{result.frame.width, result.frame.height, {}};
    out.bytes.resize(result.frame.pixels.size() * 4);
    toStraightRgba(result.frame, out.bytes.data());
    return out;
}

TitleDocument parse(const char *xml)
{
    auto result = parseTitle(xml);
    REQUIRE(result.has_value());
    return result->document;
}

Straight render(const TitleDocument &doc, double frame = 0, int width = 0, int height = 0,
                const std::map<std::string, std::string> &fields = {})
{
    return straight(renderTitle(doc, frame, fields, width ? width : doc.width, height ? height : doc.height));
}

// Doc 16's lower third, shadow and all, in the editor's own brand colours.
constexpr const char *kLowerThird = R"(<ustitle version="1" width="1920" height="1080" fps="30/1">
  <timing intro="18" hold="60" outro="15"/>
  <field name="name" label="Name" default="Jay Doe"/>
  <layer id="bar" kind="shape" shape="rounded-rect" x="96" y="820" w="620" h="140" radius="12">
    <fill color="#1b1230" opacity="0.92"/>
  </layer>
  <layer id="name" kind="text" x="128" y="838" w="560" fit="shrink">
    <text>{{name}}</text>
    <font family="Sans" weight="700" size="64"/>
    <fill gradient="linear" from="#ff3cc7" to="#19e3ff" angle="0"/>
    <shadow dx="0" dy="4" blur="12" color="#000000" opacity="0.5"/>
  </layer>
</ustitle>)";

} // namespace

TEST_CASE("a translucent bar reads back in straight alpha (T0's check)")
{
    const TitleDocument doc = parse(R"(<ustitle version="1" width="320" height="180">
      <layer kind="shape" x="40" y="40" w="100" h="50"><fill color="#fc3cba" opacity="0.6"/></layer>
    </ustitle>)");
    const Straight image = render(doc);
    const Pixel inside = image.at(90, 65);
    CHECK(inside.a == 153);
    CHECK(std::abs(inside.r - 0xfc) <= 1);
    CHECK(std::abs(inside.g - 0x3c) <= 1);
    CHECK(std::abs(inside.b - 0xba) <= 1);
    CHECK(image.at(20, 20).a == 0);
    CHECK(image.at(141, 65).a == 0);
    CHECK(image.inkBounds() == std::array<int, 4>{40, 40, 139, 89});
}

TEST_CASE("toStraightRgba un-premultiplies and reorders")
{
    RenderedFrame frame{2, 1, {0x80402010u, 0x00000000u}}; // A=0x80 R=0x40 G=0x20 B=0x10, premultiplied
    uint8_t out[8];
    toStraightRgba(frame, out);
    CHECK(int(out[0]) == 0x80);
    CHECK(int(out[1]) == 0x40);
    CHECK(int(out[2]) == 0x20);
    CHECK(int(out[3]) == 0x80);
    CHECK(int(out[7]) == 0);
}

TEST_CASE("the same frame is byte-identical on every thread and every run")
{
    const TitleDocument doc = parse(kLowerThird);
    const RenderResult first = renderTitle(doc, 30, {{"name", "Ada Lovelace"}}, 1920, 1080);
    const RenderResult again = renderTitle(doc, 30, {{"name", "Ada Lovelace"}}, 1920, 1080);
    CHECK(first.frame.pixels == again.frame.pixels);
    std::vector<RenderResult> fromThreads(4);
    std::vector<std::thread> threads;
    for (RenderResult &slot : fromThreads)
        threads.emplace_back([&] { slot = renderTitle(doc, 30, {{"name", "Ada Lovelace"}}, 1920, 1080); });
    for (std::thread &t : threads)
        t.join();
    for (const RenderResult &result : fromThreads)
        CHECK(result.frame.pixels == first.frame.pixels);
    // The bar's left edge; the text's shadow may spill a little above it.
    const auto box = straight(first).inkBounds();
    CHECK(box[0] == 96);
    CHECK(box[1] <= 820);
    CHECK(box[1] >= 790);
    CHECK(first.warnings.empty());
}

TEST_CASE("a smaller output is the same picture, scaled")
{
    const TitleDocument doc = parse(R"(<ustitle version="1" width="1920" height="1080">
      <layer kind="text" x="200" y="300"><text>Scale me</text><font family="Sans" size="120"/></layer>
    </ustitle>)");
    const auto full = render(doc).inkBounds();
    const auto half = render(doc, 0, 960, 540).inkBounds();
    REQUIRE(full[0] >= 0);
    for (size_t i = 0; i < 4; ++i) {
        CAPTURE(i);
        CHECK(std::abs(half[i] * 2 - full[i]) <= 3);
    }
}

TEST_CASE("fields reach the text, and a longer name shrinks to fit")
{
    // The text box is 128..688 wide.
    const TitleDocument textOnly = parse(R"(<ustitle version="1" width="1920" height="1080">
      <field name="name" default="x"/>
      <layer kind="text" x="128" y="838" w="560" fit="shrink"><text>{{name}}</text><font family="Sans" size="64"/></layer>
    </ustitle>)");
    const auto fitted =
        render(textOnly, 0, 0, 0, {{"name", "A Considerably Longer Guest Name Than Fits On The Bar"}}).inkBounds();
    CHECK(fitted[0] >= 126);
    CHECK(fitted[2] <= 690);
    const auto small = render(textOnly, 0, 0, 0, {{"name", "Al"}}).inkBounds();
    CHECK(small[3] - small[1] > fitted[3] - fitted[1]); // only the long one shrank
}

TEST_CASE("alignment without a box: x is the anchor")
{
    const char *pattern = R"(<ustitle version="1" width="800" height="200">
      <layer kind="text" x="400" y="50" align="%s"><text>Centre</text><font family="Sans" size="60"/></layer>
    </ustitle>)";
    char xml[512];
    std::snprintf(xml, sizeof xml, pattern, "center");
    const auto centre = render(parse(xml)).inkBounds();
    CHECK(std::abs((centre[0] + centre[2]) / 2 - 400) <= 3);
    std::snprintf(xml, sizeof xml, pattern, "right");
    CHECK(std::abs(render(parse(xml)).inkBounds()[2] - 400) <= 6);
    std::snprintf(xml, sizeof xml, pattern, "left");
    CHECK(std::abs(render(parse(xml)).inkBounds()[0] - 400) <= 6);
}

TEST_CASE("rotation and scale turn about the box's centre")
{
    const TitleDocument doc = parse(R"(<ustitle version="1" width="400" height="400">
      <layer kind="shape" x="100" y="180" w="200" h="40" rotation="90"><fill color="#ffffff"/></layer>
    </ustitle>)");
    const auto box = render(doc).inkBounds();
    CHECK(std::abs(box[0] - 180) <= 1);
    CHECK(std::abs(box[1] - 100) <= 1);
    CHECK(std::abs(box[2] - 219) <= 1);
    CHECK(std::abs(box[3] - 299) <= 1);
    const TitleDocument scaled = parse(R"(<ustitle version="1" width="400" height="400">
      <layer kind="shape" x="100" y="100" w="200" h="200" scale="0.5"><fill color="#ffffff"/></layer>
    </ustitle>)");
    CHECK(render(scaled).inkBounds() == std::array<int, 4>{150, 150, 249, 249});
}

TEST_CASE("keyframes animate the picture")
{
    const TitleDocument doc = parse(R"(<ustitle version="1" width="200" height="100" fps="30/1">
      <timing intro="10" hold="10" outro="0"/>
      <layer kind="shape" x="0" y="0" w="50" h="50"><fill color="#ffffff"/>
        <animate property="opacity"><key at="0" value="0"/><key at="10" value="1"/></animate>
        <animate property="x"><key at="0" value="0"/><key at="10" value="100"/></animate>
      </layer>
    </ustitle>)");
    CHECK(render(doc, 0).inkBounds()[0] == -1); // invisible
    const Straight mid = render(doc, 5);
    CHECK(mid.inkBounds()[0] == 50);
    CHECK(std::abs(mid.at(60, 10).a - 128) <= 1);
    CHECK(render(doc, 15).inkBounds()[0] == 100);
}

TEST_CASE("a shadow falls where it's offset, blurred")
{
    const TitleDocument sharp = parse(R"(<ustitle version="1" width="200" height="200">
      <layer kind="shape" x="50" y="50" w="100" h="40"><fill color="#ffffff"/>
        <shadow dx="0" dy="60" blur="0" color="#ff0000" opacity="1"/></layer>
    </ustitle>)");
    const Straight image = render(sharp);
    const Pixel shadow = image.at(100, 120);
    CHECK(shadow.a == 255);
    CHECK(shadow.r == 255);
    CHECK(shadow.g == 0);
    CHECK(image.at(100, 70).g == 255); // the layer over it
    const TitleDocument soft = parse(R"(<ustitle version="1" width="200" height="200">
      <layer kind="shape" x="50" y="50" w="100" h="40"><fill color="#ffffff"/>
        <shadow dx="0" dy="60" blur="6" color="#ff0000" opacity="1"/></layer>
    </ustitle>)");
    const Straight blurred = render(soft);
    CHECK(blurred.at(100, 130).a == 255); // the middle (110..149) stays solid
    CHECK(blurred.at(100, 150).a > 0);    // it spreads past the edge (149)...
    CHECK(blurred.at(100, 150).a < 255);  // ...softly
    CHECK(blurred.at(100, 175).a == 0);   // and fades out
}

TEST_CASE("stroke outlines outside the fill")
{
    const TitleDocument doc = parse(R"(<ustitle version="1" width="200" height="200">
      <layer kind="shape" x="50" y="50" w="100" h="100"><fill color="#ffffff"/><stroke color="#0000ff" width="10"/></layer>
    </ustitle>)");
    const Straight image = render(doc);
    CHECK(image.at(45, 100).b == 255);
    CHECK(image.at(45, 100).r == 0);
    CHECK(image.at(55, 100).r == 255); // inside is fill, not stroke
    CHECK(image.inkBounds() == std::array<int, 4>{40, 40, 159, 159});
}

TEST_CASE("a missing font is named, with what was used instead; generic names aren't")
{
    const TitleDocument doc = parse(R"(<ustitle version="1" width="400" height="100">
      <layer kind="text" x="10" y="10"><text>Hello</text><font family="No Such Font Family 7Q" size="40"/></layer>
      <layer kind="text" x="10" y="50"><text>Hello</text><font family="monospace" size="40"/></layer>
    </ustitle>)");
    const RenderResult result = renderTitle(doc, 0, {}, 400, 100);
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings[0].starts_with("No Such Font Family 7Q isn't installed; using "));
    CHECK(straight(result).inkBounds()[0] >= 0); // it still draws, in the fallback
}

TEST_CASE("addFontDirectory takes a real directory only")
{
    CHECK_FALSE(addFontDirectory("/nonexistent/ustudio/fonts"));
    const auto dir = std::filesystem::path(TITLES_TEST_FONT_DIR);
    std::filesystem::create_directories(dir);
    CHECK(addFontDirectory(dir.string()));
    // Renders after it still work (each thread's font map is rebuilt).
    const TitleDocument doc = parse(R"(<ustitle version="1" width="200" height="100">
      <layer kind="text" x="10" y="10"><text>Hi</text><font family="Sans" size="40"/></layer></ustitle>)");
    CHECK(render(doc).inkBounds()[0] >= 0);
}

TEST_CASE("blurAlpha: a no-op below half a pixel; spreads and conserves otherwise")
{
    std::vector<uint8_t> image(21 * 21, 0);
    image[10 * 21 + 10] = 255;
    std::vector<uint8_t> copy = image;
    blurAlpha(copy.data(), 21, 21, 21, 0.3);
    CHECK(copy == image);
    for (int y = 0; y < 21; ++y)
        for (int x = 0; x < 21; ++x)
            image[static_cast<size_t>(y * 21 + x)] = (x >= 5 && x < 16 && y >= 5 && y < 16) ? 255 : 0;
    long before = 0, after = 0;
    for (uint8_t v : image)
        before += v;
    blurAlpha(image.data(), 21, 21, 21, 1.0); // three passes of radius 1: 3 px of spread
    for (uint8_t v : image)
        after += v;
    CHECK(std::abs(after - before) < before / 50);
    CHECK(image[10 * 21 + 10] == 255);
    CHECK(image[10 * 21 + 3] > 0);
}

TEST_CASE("the background: none leaves alpha, a colour fills the frame under the layers")
{
    const char *layers = R"(<layer kind="shape" x="10" y="10" w="20" h="20"><fill color="#ffffff"/></layer>)";
    std::string none = std::string(R"(<ustitle version="1" width="100" height="50">)") + layers + "</ustitle>";
    std::string colour = std::string(R"(<ustitle version="1" width="100" height="50"><background color="#19e3ff"/>)") +
                         layers + "</ustitle>";
    const Straight clear = render(parse(none.c_str()));
    CHECK(clear.at(80, 40).a == 0);
    const Straight filled = render(parse(colour.c_str()), 0, 200, 100); // scaled output too
    CHECK(filled.at(160, 80).a == 255);
    CHECK(filled.at(160, 80).g == 0xe3);
    CHECK(filled.at(199, 99).a == 255);
    CHECK(filled.at(40, 40).r == 255); // the layer over it
    // It round-trips through the file.
    auto again = parseTitle(writeTitle(parse(colour.c_str())));
    REQUIRE(again.has_value());
    CHECK(again->document.background.kind == FillKind::Solid);
    CHECK(formatColor(again->document.background.color) == "#19e3ff");
}

TEST_CASE("measureLayers: each layer's box, in canvas pixels")
{
    const TitleDocument doc = parse(R"(<ustitle version="1" width="800" height="200">
      <layer id="bar" kind="shape" x="10" y="20" w="300" h="40" rotation="15"><fill color="#ffffff"/></layer>
      <layer id="name" kind="text" x="400" y="50" align="center"><text>Name</text><font family="Sans" size="40"/></layer>
    </ustitle>)");
    const std::vector<LayerGeometry> geometry = measureLayers(doc, 0, {});
    REQUIRE(geometry.size() == 2);
    CHECK(geometry[0].id == "bar");
    CHECK(geometry[0].box == Rect{10, 20, 300, 40});
    CHECK(geometry[0].rotation == 15);
    CHECK(geometry[1].box.y == 50);
    CHECK(geometry[1].box.h > 30);
    CHECK(std::abs(geometry[1].box.x + geometry[1].box.w / 2 - 400) < 1); // centred on x
}

#include <cairo.h>

TEST_CASE("image layers: a PNG at its own size, scaled keeping its aspect, and missing ones named")
{
    const auto dir = std::filesystem::temp_directory_path() / "ustudio-titles-test-image";
    std::filesystem::create_directories(dir);
    // A 40 x 20 picture: red left half, blue right half.
    cairo_surface_t *png = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 40, 20);
    cairo_t *cr = cairo_create(png);
    cairo_set_source_rgb(cr, 1, 0, 0);
    cairo_rectangle(cr, 0, 0, 20, 20);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0, 0, 1);
    cairo_rectangle(cr, 20, 0, 20, 20);
    cairo_fill(cr);
    cairo_destroy(cr);
    REQUIRE(cairo_surface_write_to_png(png, (dir / "logo.png").string().c_str()) == CAIRO_STATUS_SUCCESS);
    cairo_surface_destroy(png);

    TitleDocument doc = parse(R"(<ustitle version="1" width="200" height="100">
      <layer id="own" kind="image" src="logo.png" x="10" y="10"/>
      <layer id="wide" kind="image" src="logo.png" x="100" y="10" w="80"/>
    </ustitle>)");
    doc.baseDirectory = dir.string();
    const RenderResult result = renderTitle(doc, 0, {}, 200, 100);
    CHECK(result.warnings.empty());
    const Straight image = straight(result);
    CHECK(image.at(15, 20).r == 255); // its own size: 40 x 20 at (10, 10)
    CHECK(image.at(45, 20).b == 255);
    CHECK(image.at(55, 20).a == 0);
    CHECK(image.at(110, 40).r == 255); // w 80: h follows, 40
    CHECK(image.at(170, 40).b == 255);
    CHECK(image.at(140, 55).a == 0);
    const std::vector<LayerGeometry> geometry = measureLayers(doc, 0, {});
    CHECK(geometry[0].box == Rect{10, 10, 40, 20});
    CHECK(geometry[1].box == Rect{100, 10, 80, 40});

    // It round-trips, and a missing picture is named (the rest still draws).
    auto again = parseTitle(writeTitle(doc));
    REQUIRE(again.has_value());
    CHECK(again->document.layers[1].src == "logo.png");
    std::filesystem::remove(dir / "logo.png");
    const RenderResult missing = renderTitle(doc, 0, {}, 200, 100);
    REQUIRE(missing.warnings.size() == 1);
    CHECK(missing.warnings[0] == "the picture logo.png isn't there");
    std::filesystem::remove_all(dir);
}
