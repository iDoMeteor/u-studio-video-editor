// The titles app's document editing (core/title_edit, core/snapping):
// undo history, layer operations, guides, snapping and hit testing.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "core/snapping.h"
#include "core/title_edit.h"

using namespace ustudio::titles;

TEST_CASE("history: apply, undo, redo, labels and the dirty mark")
{
    TitleHistory history;
    CHECK_FALSE(history.canUndo());
    CHECK_FALSE(history.isDirty());
    Layer text = makeTextLayer(history.document(), "Hello");
    CHECK(history.apply("Add text", [&](TitleDocument &doc) { return addLayer(doc, text); }));
    CHECK(history.isDirty());
    CHECK(history.undoLabel() == "Add text");
    CHECK(history.document().layers.size() == 1);
    CHECK(history.undo());
    CHECK(history.document().layers.empty());
    CHECK_FALSE(history.isDirty());
    CHECK(history.redoLabel() == "Add text");
    CHECK(history.redo());
    CHECK(history.document().layers.size() == 1);
    history.markSaved();
    CHECK_FALSE(history.isDirty());
    // A refused or empty edit is no step.
    CHECK_FALSE(history.apply("Nothing", [](TitleDocument &) { return true; }));
    CHECK_FALSE(history.apply("Refused", [](TitleDocument &) { return false; }));
    CHECK(history.undoLabel() == "Add text");
    // A new edit clears redo.
    CHECK(history.undo());
    CHECK(
        history.apply("Other", [](TitleDocument &doc) { return addLayer(doc, makeShapeLayer(doc, ShapeKind::Rect)); }));
    CHECK_FALSE(history.canRedo());
}

TEST_CASE("history: a drag is one step; a new drag another")
{
    TitleHistory history;
    history.apply("Add", [](TitleDocument &doc) { return addLayer(doc, makeShapeLayer(doc, ShapeKind::Rect)); });
    const std::string id = history.document().layers[0].id;
    const auto moveBy = [&](double dx) {
        return history.apply(
            "Move layer", [&](TitleDocument &doc) { return updateLayer(doc, id, [&](Layer &l) { l.x += dx; }); },
            "move:" + id);
    };
    const double x0 = history.document().layers[0].x;
    for (int i = 0; i < 20; ++i)
        CHECK(moveBy(1));
    history.closeStep();
    CHECK(moveBy(5));
    CHECK(history.document().layers[0].x == x0 + 25);
    history.undo();
    CHECK(history.document().layers[0].x == x0 + 20);
    history.undo();
    CHECK(history.document().layers[0].x == x0);
    CHECK(history.undoLabel() == "Add");
}

TEST_CASE("layer operations")
{
    TitleDocument doc;
    CHECK(addLayer(doc, makeShapeLayer(doc, ShapeKind::Rect)));
    CHECK(addLayer(doc, makeShapeLayer(doc, ShapeKind::Rect)));
    CHECK(addLayer(doc, makeTextLayer(doc, "Name")));
    CHECK(doc.layers[0].id == "shape");
    CHECK(doc.layers[1].id == "shape-2");
    CHECK(doc.layers[2].id == "text");
    CHECK_FALSE(addLayer(doc, doc.layers[0])); // duplicate id
    CHECK(moveLayer(doc, "text", 0));
    CHECK(doc.layers[0].id == "text");
    CHECK_FALSE(moveLayer(doc, "text", 3));
    CHECK_FALSE(moveLayer(doc, "nope", 0));
    CHECK(removeLayer(doc, "shape"));
    CHECK(doc.layers.size() == 2);
    CHECK_FALSE(removeLayer(doc, "shape"));
    CHECK(updateLayer(doc, "shape-2", [](Layer &l) { l.visible = false; }));
    CHECK_FALSE(doc.layers[1].visible);
    CHECK(addLayer(doc, makeShapeLayer(doc, ShapeKind::Line), 0));
    CHECK(doc.layers[0].id == "line");
    CHECK(doc.layers[0].stroke.width > 0);
}

TEST_CASE("guides and safe areas")
{
    TitleDocument doc; // 1920 x 1080
    CHECK(actionSafe(doc) == Rect{96, 54, 1728, 972});
    CHECK(titleSafe(doc) == Rect{192, 108, 1536, 864});
    const SnapGuides guides = snapGuides(doc, {{100, 200, 50, 20}});
    CHECK(std::find(guides.xs.begin(), guides.xs.end(), 960.0) != guides.xs.end());
    CHECK(std::find(guides.xs.begin(), guides.xs.end(), 125.0) != guides.xs.end());
    CHECK(std::find(guides.ys.begin(), guides.ys.end(), 220.0) != guides.ys.end());
}

TEST_CASE("snapping: nearest edge or centre, per axis, within the threshold")
{
    TitleDocument doc;
    const SnapGuides guides = snapGuides(doc, {});
    // A box whose centre is 3 px right of the canvas centre and whose top is
    // 2 px under title safe.
    Snap snap = snapBox({863, 110, 200, 100}, guides, 8);
    CHECK(snap.dx == doctest::Approx(-3));
    REQUIRE(snap.guideX.has_value());
    CHECK(*snap.guideX == 960);
    CHECK(snap.dy == doctest::Approx(-2));
    CHECK(*snap.guideY == 108);
    // Too far: no snap.
    snap = snapBox({700, 400, 30, 30}, guides, 8);
    CHECK(snap.dx == 0);
    CHECK_FALSE(snap.guideX.has_value());
    CHECK_FALSE(snap.guideY.has_value());
}

TEST_CASE("hit testing follows rotation and scale")
{
    const Rect box{100, 180, 200, 40}; // centre (200, 200)
    CHECK(hitsBox(box, 0, 1, 110, 190));
    CHECK_FALSE(hitsBox(box, 0, 1, 200, 250));
    // Turned 90 degrees it's tall: (200, 250) is on it, (110, 200) isn't.
    CHECK(hitsBox(box, 90, 1, 200, 250));
    CHECK_FALSE(hitsBox(box, 90, 1, 110, 200));
    // Half size about the centre.
    CHECK(hitsBox(box, 0, 0.5, 160, 200));
    CHECK_FALSE(hitsBox(box, 0, 0.5, 110, 200));
    CHECK_FALSE(hitsBox(box, 0, 0, 200, 200));
}

#include "core/brand_kit.h"
#include "core/title_xml.h"

#include <fstream>
#include <sstream>

namespace {
BrandKit shippedKit()
{
    std::ifstream in(TITLES_BRAND_XML);
    std::stringstream xml;
    xml << in.rdbuf();
    auto kit = parseBrandKit(xml.str());
    REQUIRE(kit.has_value());
    return *kit;
}
} // namespace

TEST_CASE("the shipped brand kit parses, with the design system's values")
{
    const BrandKit kit = shippedKit();
    CHECK(kit.name == "Unicorn Tears");
    REQUIRE(kit.colour("Magenta"));
    CHECK(formatColor(kit.colour("Magenta")->value) == "#ff2bd6");
    CHECK(formatColor(kit.colour("Cyan")->value) == "#19e3ff");
    REQUIRE(kit.gradients.size() >= 1);
    CHECK(kit.gradients[0].name == "Tears");
    REQUIRE(kit.gradients[0].fill.via.has_value());
    CHECK(formatColor(*kit.gradients[0].fill.via) == "#9d4eff");
    CHECK(kit.displayFont == "Anton");
    CHECK(kit.sansFont == "Space Grotesk");
    CHECK_FALSE(parseBrandKit("<nope/>").has_value());
    CHECK_FALSE(parseBrandKit(R"(<brand><colour name="x" value="red"/></brand>)").has_value());
}

TEST_CASE("Apply brand restyles, keeping the layout")
{
    TitleDocument doc;
    Layer bar = makeShapeLayer(doc, ShapeKind::RoundedRect);
    bar.stroke.width = 2;
    addLayer(doc, bar);
    Layer name = makeTextLayer(doc, "Name");
    name.font.size = 96;
    addLayer(doc, name);
    Layer role = makeTextLayer(doc, "Role");
    role.font.size = 32;
    addLayer(doc, role);
    const TitleDocument before = doc;
    CHECK(applyBrand(doc, shippedKit()));
    CHECK(doc.layers[0].fill.color == *parseColor("#1b1230"));
    CHECK(doc.layers[0].fill.opacity == doctest::Approx(0.92));
    CHECK(doc.layers[0].stroke.color == *parseColor("#19e3ff"));
    CHECK(doc.layers[1].font.family == "Anton");
    CHECK(doc.layers[1].fill.kind == FillKind::Linear);
    CHECK(doc.layers[2].font.family == "Space Grotesk");
    CHECK(doc.layers[2].fill.color == *parseColor("#ffeffb"));
    for (size_t i = 0; i < doc.layers.size(); ++i) {
        CHECK(doc.layers[i].x == before.layers[i].x);
        CHECK(doc.layers[i].font.size == before.layers[i].font.size);
    }
    CHECK_FALSE(applyBrand(doc, shippedKit())); // already branded
}

TEST_CASE("a gradient's middle stop and a layer's lock round-trip")
{
    TitleDocument doc;
    Layer layer = makeShapeLayer(doc, ShapeKind::Rect);
    layer.fill = shippedKit().gradients[0].fill;
    layer.locked = true;
    addLayer(doc, layer);
    auto again = parseTitle(writeTitle(doc));
    REQUIRE(again.has_value());
    CHECK(again->document == doc);
}

TEST_CASE("keys at the playhead: in its zone, set, replaced, removed")
{
    const Timing timing{10, 40, 10};
    Layer layer = makeShapeLayer(TitleDocument{}, ShapeKind::Rect);
    CHECK(keyAtFrame(timing, 5, 1).zone == Zone::Intro);
    CHECK(keyAtFrame(timing, 20, 1).zone == Zone::Hold);
    CHECK(keyAtFrame(timing, 20, 1).key.at == 10);
    CHECK(keyAtFrame(timing, 55, 1).zone == Zone::Outro);
    CHECK(keyAtFrame(timing, 55, 1).key.at == 5);
    CHECK_FALSE(isAnimated(layer, Property::Opacity));
    setKey(layer, timing, Property::Opacity, 55, 0.0);
    setKey(layer, timing, Property::Opacity, 5, 0.0, ustudio::core::Easing::CubicOut);
    setKey(layer, timing, Property::Opacity, 20, 1.0);
    CHECK(isAnimated(layer, Property::Opacity));
    REQUIRE(layer.animation.size() == 1);
    CHECK(layer.animation[0].keys.size() == 3);
    CHECK(layer.animation[0].keys[0].zone == Zone::Intro); // sorted by place
    CHECK(layer.animation[0].keys[2].zone == Zone::Outro);
    setKey(layer, timing, Property::Opacity, 20, 0.5); // replaces, keeps easing
    CHECK(layer.animation[0].keys.size() == 3);
    REQUIRE(findKey(layer, timing, Property::Opacity, 20));
    CHECK(findKey(layer, timing, Property::Opacity, 20)->key.value == 0.5);
    CHECK(findKey(layer, timing, Property::Opacity, 5)->key.easing == ustudio::core::Easing::CubicOut);
    CHECK(findKey(layer, timing, Property::Opacity, 21) == nullptr);
    CHECK(removeKey(layer, timing, Property::Opacity, 20));
    CHECK_FALSE(removeKey(layer, timing, Property::Opacity, 20));
    CHECK(removeKey(layer, timing, Property::Opacity, 5));
    CHECK(removeKey(layer, timing, Property::Opacity, 55));
    CHECK(layer.animation.empty()); // an empty track goes
    setBaseValue(layer, Property::Rotation, 12);
    CHECK(baseValue(layer, Property::Rotation) == 12);
    CHECK(baseValue(layer, Property::Reveal) == 1.0);
}
