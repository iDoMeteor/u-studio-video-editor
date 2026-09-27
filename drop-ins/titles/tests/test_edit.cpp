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
