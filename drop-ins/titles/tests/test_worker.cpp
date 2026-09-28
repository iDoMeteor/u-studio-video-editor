// The titles app's render worker (app/render_worker): renders off the main
// thread, delivers on the main loop, newest request wins, and never calls
// back after it's gone. Run under TSan and ASan with the rest.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/render_worker.h"
#include "core/title_edit.h"

#include <glib.h>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using namespace ustudio::titles;
using ustudio::titles::app::RenderWorker;

namespace {
TitleDocument documentWith(int layers)
{
    TitleDocument doc;
    for (int i = 0; i < layers; ++i)
        addLayer(doc, makeShapeLayer(doc, ShapeKind::Rect));
    return doc;
}

template <typename Done> void spinUntil(Done done, int limitMs = 5000)
{
    const auto start = std::chrono::steady_clock::now();
    while (!done() && std::chrono::steady_clock::now() - start < std::chrono::milliseconds(limitMs))
        if (!g_main_context_iteration(nullptr, FALSE))
            g_usleep(1000);
}
} // namespace

TEST_CASE("the newest request is delivered, on the main thread")
{
    const std::thread::id mainThread = std::this_thread::get_id();
    std::vector<uint64_t> delivered;
    std::vector<int> widths;
    bool onMain = true;
    RenderWorker worker([&](RenderResult result, uint64_t generation) {
        onMain = onMain && std::this_thread::get_id() == mainThread;
        delivered.push_back(generation);
        widths.push_back(result.frame.width);
    });
    uint64_t last = 0;
    for (int i = 1; i <= 20; ++i)
        last = worker.request(documentWith(3), 0, {}, 100 + i, 50);
    spinUntil([&] { return !delivered.empty() && delivered.back() == last; });
    REQUIRE_FALSE(delivered.empty());
    CHECK(delivered.back() == last);
    CHECK(widths.back() == 120);
    CHECK(onMain);
    CHECK(delivered.size() < 20); // superseded requests were skipped
    for (size_t i = 1; i < delivered.size(); ++i)
        CHECK(delivered[i] > delivered[i - 1]);
}

TEST_CASE("nothing is delivered after the worker is destroyed")
{
    auto calls = std::make_shared<int>(0);
    {
        RenderWorker worker([calls](RenderResult, uint64_t) { ++*calls; });
        for (int i = 0; i < 5; ++i)
            worker.request(documentWith(20), 0, {}, 1920, 1080);
        std::this_thread::sleep_for(std::chrono::milliseconds(30)); // some finish, some post
    }
    const int before = *calls;
    spinUntil([] { return false; }, 300); // drain anything already posted
    CHECK(*calls == before);
}

#include "app/behavior_drawer.h"

TEST_CASE("the behaviour drawer's thumbnails: the layer alone, cropped to it, over the behaviour's window")
{
    TitleDocument doc;
    doc.timing = {20, 60, 20};
    Layer bar = makeShapeLayer(doc, ShapeKind::Rect);
    bar.x = 100;
    bar.y = 800;
    bar.w = 600;
    bar.h = 150;
    bar.animation.push_back({Property::X, {{Zone::Intro, {0, 50.0, ustudio::core::Easing::Linear}}}});
    addLayer(doc, bar);
    addLayer(doc, makeTextLayer(doc, "Other"));
    const Behavior rise{BehaviorSlot::In, "rise", 15, ustudio::core::Easing::CubicOut, 1, 1.0};
    const TitleDocument thumb = ustudio::titles::app::thumbnailDocument(doc, doc.layers[0], rise);
    REQUIRE(thumb.layers.size() == 1);
    CHECK(thumb.layers[0].behaviors.size() == 1);
    CHECK(std::abs(static_cast<double>(thumb.width) / thumb.height - 16.0 / 9.0) < 0.02);
    CHECK(thumb.width < 1920);
    // The layer sits inside the cropped canvas, its keys moved with it.
    CHECK(thumb.layers[0].x > 0);
    CHECK(thumb.layers[0].x + 600 < thumb.width);
    CHECK(thumb.layers[0].animation[0].keys[0].key.value == doctest::Approx(50 - (100 - thumb.layers[0].x)));
    // Frames: in from 0, out ending at the end, loops in the hold.
    const std::vector<double> in = ustudio::titles::app::thumbnailFrames(doc.timing, rise, 5);
    CHECK(in.front() == 0);
    CHECK(in.back() == 21);
    const Behavior fade{BehaviorSlot::Out, "fade", 12, ustudio::core::Easing::Linear, 1, 1.0};
    CHECK(ustudio::titles::app::thumbnailFrames(doc.timing, fade, 5).back() == 100);
    const Behavior pulse{BehaviorSlot::Loop, "pulse", 45, ustudio::core::Easing::Linear, 1, 1.0};
    CHECK(ustudio::titles::app::thumbnailFrames(doc.timing, pulse, 5).front() == 28);
}
