// The titles drop-in's core/: the .ustitle format, elastic timing, keyframe
// evaluation and fields (doc 16), without Pango or MLT.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/evaluate.h"
#include "core/title_xml.h"

#include <filesystem>
#include <string>

using namespace ustudio;
using namespace ustudio::titles;

namespace {

// Doc 16's example, as T1 reads it (animators are T3).
constexpr const char *kLowerThird = R"(<?xml version="1.0"?>
<ustitle version="1" width="1920" height="1080" fps="30/1">
  <timing intro="18" hold="60" outro="15" hold-mode="elastic"/>
  <field name="name" label="Name" default="Jay Doe"/>
  <field name="role" label="Role" default="Host"/>
  <layer id="bar" kind="shape" shape="rounded-rect" x="96" y="820" w="620" h="140" radius="12">
    <fill color="#1b1230" opacity="0.92"/>
    <animate property="opacity">
      <key at="0" value="0" easing="cubic_out"/>
      <key at="12" value="1"/>
      <key at="0" zone="outro" value="1" easing="cubic_in"/>
      <key at="15" zone="outro" value="0"/>
    </animate>
  </layer>
  <layer id="name" kind="text" x="128" y="838" w="560" fit="shrink">
    <text>{{name}} &amp; {{role}}</text>
    <font family="Space Grotesk" weight="700" size="64" tracking="0.02"/>
    <fill gradient="linear" from="#ff3cc7" to="#19e3ff" angle="0"/>
    <stroke color="#000000cc" width="3"/>
    <shadow dx="0" dy="4" blur="12" color="#000000" opacity="0.5"/>
    <animator unit="character" order="forward" stagger="1"/>
  </layer>
</ustitle>
)";

TitleDocument lowerThird()
{
    auto result = parseTitle(kLowerThird);
    REQUIRE(result.has_value());
    return result->document;
}

} // namespace

TEST_CASE("the doc 16 example reads")
{
    auto result = parseTitle(kLowerThird);
    REQUIRE(result.has_value());
    const TitleDocument &doc = result->document;
    CHECK(doc.width == 1920);
    CHECK(doc.fpsNum == 30);
    CHECK(doc.timing == Timing{18, 60, 15});
    REQUIRE(doc.fields.size() == 2);
    CHECK(doc.fields[1].defaultValue == "Host");
    REQUIRE(doc.layers.size() == 2);
    const Layer &bar = doc.layers[0];
    CHECK(bar.kind == LayerKind::Shape);
    CHECK(bar.shape == ShapeKind::RoundedRect);
    CHECK(bar.fill.opacity == doctest::Approx(0.92));
    REQUIRE(bar.animation.size() == 1);
    CHECK(bar.animation[0].keys.size() == 4);
    CHECK(bar.animation[0].keys[2].zone == Zone::Outro);
    CHECK(bar.animation[0].keys[0].key.easing == core::Easing::CubicOut);
    const Layer &name = doc.layers[1];
    CHECK(name.text == "{{name}} & {{role}}");
    CHECK(name.fit == Fit::Shrink);
    CHECK(name.font.family == "Space Grotesk");
    CHECK(name.font.weight == 700);
    CHECK(name.fill.kind == FillKind::Linear);
    CHECK(name.stroke.color.a == doctest::Approx(0xcc / 255.0));
    CHECK(name.shadow.enabled);
    CHECK(result->warnings.empty());
    REQUIRE(name.animators.size() == 1);
    CHECK(name.animators[0].unit == AnimatorUnit::Character);
}

TEST_CASE("write then read gives the same document")
{
    const TitleDocument doc = lowerThird();
    const std::string xml = writeTitle(doc);
    auto again = parseTitle(xml);
    REQUIRE(again.has_value());
    CHECK(again->document == doc);
    CHECK(again->warnings.empty());
    CHECK(writeTitle(again->document) == xml);
}

TEST_CASE("saveTitle writes atomically and readTitle reads it back")
{
    const auto dir = std::filesystem::temp_directory_path() / "ustudio-titles-test-core";
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "lower third.ustitle").string();
    CHECK(saveTitle(lowerThird(), path).empty());
    CHECK_FALSE(std::filesystem::exists(path + ".part"));
    auto read = readTitle(path);
    REQUIRE(read.has_value());
    CHECK(read->document.baseDirectory == dir.string()); // where its images resolve
    read->document.baseDirectory.clear();
    CHECK(read->document == lowerThird());
    std::filesystem::remove_all(dir);
    CHECK_FALSE(readTitle(path).has_value());
}

TEST_CASE("untrusted input: refused cleanly or clamped, never trusted")
{
    CHECK_FALSE(parseTitle("").has_value());
    CHECK_FALSE(parseTitle("<ustitle").has_value());
    CHECK_FALSE(parseTitle("<mlt/>").has_value());
    CHECK_FALSE(parseTitle(R"(<ustitle width="100"/>)").has_value()); // no version
    CHECK(parseTitle(R"(<ustitle version="2"/>)").error().find("newer") != std::string::npos);
    CHECK_FALSE(parseTitle(R"(<ustitle version="1" width="wide"/>)").has_value());
    CHECK_FALSE(parseTitle(R"(<ustitle version="1" fps="0/1"/>)").has_value());
    CHECK_FALSE(parseTitle(R"(<ustitle version="1"><layer><fill color="red"/></layer></ustitle>)").has_value());
    CHECK_FALSE(parseTitle(R"(<ustitle version="1"><field name="{{x}}"/></ustitle>)").has_value());
    CHECK_FALSE(
        parseTitle(
            R"(<ustitle version="1"><layer><animate property="x"><key at="0" easing="wobbly"/></animate></layer></ustitle>)")
            .has_value());
    // Out-of-range numbers are clamped.
    auto big =
        parseTitle(R"(<ustitle version="1" width="99999" height="1"><layer><font size="1e9"/></layer></ustitle>)");
    REQUIRE(big.has_value());
    CHECK(big->document.width == 8192);
    CHECK(big->document.height == 16);
    CHECK(big->document.layers[0].font.size == 4000.0);
    // An external entity is never fetched or expanded.
    auto entity = parseTitle(R"(<?xml version="1.0"?>
<!DOCTYPE ustitle [<!ENTITY secret SYSTEM "file:///etc/passwd">]>
<ustitle version="1"><layer><text>&secret;</text></layer></ustitle>)");
    if (entity)
        CHECK(entity->document.layers[0].text.find("root") == std::string::npos);
    // Unknown layer kinds are skipped with a warning.
    auto image = parseTitle(R"(<ustitle version="1"><layer kind="video"/></ustitle>)");
    REQUIRE(image.has_value());
    CHECK(image->document.layers.empty());
    CHECK(image->warnings.size() == 1);
}

TEST_CASE("colours")
{
    CHECK(parseColor("#fff") == Rgba{1, 1, 1, 1});
    CHECK(parseColor("#ff000080")->a == doctest::Approx(128 / 255.0));
    CHECK_FALSE(parseColor("ff0000").has_value());
    CHECK_FALSE(parseColor("#ff00").has_value());
    CHECK_FALSE(parseColor("#gg0000").has_value());
    CHECK(formatColor(*parseColor("#1B1230")) == "#1b1230");
    CHECK(formatColor(*parseColor("#1b123080")) == "#1b123080");
}

TEST_CASE("elastic timing: stretching the clip changes only the hold")
{
    const TitleDocument doc = lowerThird(); // 18 + 60 + 15 at 30 fps
    for (double length : {93.0, 120.0, 600.0, 36000.0}) {
        CAPTURE(length);
        // The intro plays from the start, at its own speed.
        for (double f = 0; f < 18; ++f)
            CHECK(titleFrame(doc, length, f, 30.0) == doctest::Approx(f));
        // The outro ends at the end, at its own speed.
        for (double f = 0; f < 15; ++f)
            CHECK(titleFrame(doc, length, length - 15 + f, 30.0) == doctest::Approx(78 + f));
        // The hold spans what's between.
        CHECK(titleFrame(doc, length, 18, 30.0) == doctest::Approx(18));
        const double mid = titleFrame(doc, length, 18 + (length - 33) / 2, 30.0);
        CHECK(mid == doctest::Approx(48));
    }
    // Shorter than intro + outro: both play, squeezed, and no hold.
    CHECK(titleFrame(doc, 33.0 / 2, 0, 30.0) == 0.0);
    CHECK(titleFrame(doc, 33.0 / 2, 9, 30.0) == doctest::Approx(78));
    CHECK(titleFrame(doc, 33.0 / 2, 4.5, 30.0) == doctest::Approx(9));
    // A 60 fps project plays a 30 fps title at its designed speed.
    CHECK(titleFrame(doc, 400, 36, 60.0) == doctest::Approx(18));
    CHECK(titleFrame(doc, 400, 399, 60.0) == doctest::Approx(78 + 14.5));
    CHECK(titleFrame(doc, 0, 0, 30.0) == 0.0);
}

TEST_CASE("keyframes by zone: outro keys follow the outro when the hold changes")
{
    TitleDocument doc = lowerThird();
    const Layer &bar = doc.layers[0];
    CHECK(evaluateLayer(bar, doc.timing, 0).opacity == 0.0);
    CHECK(evaluateLayer(bar, doc.timing, 12).opacity == 1.0);
    CHECK(evaluateLayer(bar, doc.timing, 50).opacity == 1.0);
    CHECK(evaluateLayer(bar, doc.timing, 78).opacity == 1.0);
    CHECK(evaluateLayer(bar, doc.timing, 93).opacity == 0.0);
    // cubic_out: past halfway at the midpoint of the fade-in.
    CHECK(evaluateLayer(bar, doc.timing, 6).opacity > 0.5);
    doc.timing.hold = 200;
    CHECK(evaluateLayer(bar, doc.timing, 93).opacity == 1.0);
    CHECK(evaluateLayer(bar, doc.timing, 218 + 15).opacity == 0.0);
    // Unanimated properties keep the layer's own values.
    CHECK(evaluateLayer(bar, doc.timing, 5).x == 96.0);
}

TEST_CASE("fields: clip values, then defaults; unknown names stay visible")
{
    const TitleDocument doc = lowerThird();
    CHECK(substituteFields("{{name}} - {{role}}", doc.fields, {}) == "Jay Doe - Host");
    CHECK(substituteFields("{{name}} - {{role}}", doc.fields, {{"name", "Ada"}}) == "Ada - Host");
    CHECK(substituteFields("{{nmae}}", doc.fields, {}) == "{{nmae}}");
    CHECK(substituteFields("{{name", doc.fields, {}) == "{{name");
    CHECK(substituteFields("a {{name}}}", doc.fields, {{"name", "{{role}}"}}) == "a {{role}}}");
}
