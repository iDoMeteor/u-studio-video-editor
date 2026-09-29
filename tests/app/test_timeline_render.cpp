#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "app/timeline/timeline_renderer.h"
#include "core/model/model.h"

#include <pango/pangocairo.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

using namespace ustudio::core;
using namespace ustudio::app::timeline;

namespace {

// doc 12, M3: "Snapshot <= 4 ms with 10 tracks x 500 clips on screen".
// 10 tracks of 500 clips each, gaps between them, all fitted into a
// 1920 px view.
struct Scene
{
    Model model = Model::createEmpty();
    Viewport viewport;
    TimelineController controller;
    std::vector<float> peaks = std::vector<float>(4096, 0.5f);

    Scene()
    {
        Asset asset;
        asset.path = "color:red";
        asset.info.hasVideo = true;
        asset.info.lengthInSequenceFrames = 10'000'000;
        AssetId id = model.addAsset(asset);
        for (int t = 0; t < 10; ++t) {
            TrackId track = model.addTrack(Track::Kind::Video, static_cast<size_t>(t), "V" + std::to_string(t + 1));
            for (int c = 0; c < 500; ++c)
                model.insertClip(track, id, c * 60 + (t % 3) * 5, 0, 49);
        }
        viewport.setOriginX(22.0);
        viewport.setVisibleWidth(1920.0 - 22.0);
        viewport.setSequenceLength(500 * 60 + 60, 1);
    }
};

// Median snapshot time in milliseconds over `runs`.
double measure(Scene &scene, bool waveforms, int runs)
{
    PangoFontMap *fontMap = pango_cairo_font_map_get_default();
    PangoContext *context = pango_font_map_create_context(fontMap);
    PangoLayout *layout = pango_layout_new(context);
    PangoFontDescription *font = pango_font_description_from_string("Sans 8");
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);

    TimelineScene timeline{.model = scene.model,
                           .viewport = scene.viewport,
                           .controller = scene.controller,
                           .layout = RowLayout{},
                           .handleWidth = 22.0,
                           .activeRow = 0,
                           .nameEditRow = -1,
                           .waveformFor = nullptr,
                           .thumbnailFor = nullptr,
                           .overlays = {},
                           .labelLayout = layout};
    if (waveforms)
        timeline.waveformFor = [&](const Clip &) { return &scene.peaks; };

    std::vector<double> times;
    for (int i = 0; i < runs; ++i) {
        GtkSnapshot *snapshot = gtk_snapshot_new();
        auto start = std::chrono::steady_clock::now();
        snapshotTimeline(snapshot, timeline, 1920.0, 600.0);
        GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
        auto end = std::chrono::steady_clock::now();
        if (node)
            gsk_render_node_unref(node);
        times.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    g_object_unref(layout);
    g_object_unref(context);
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

} // namespace

TEST_CASE("Timeline snapshot: 10 tracks x 500 clips on screen")
{
    Scene scene;
    measure(scene, true, 3); // warm up fonts and caches
    double all = measure(scene, true, 30);
    std::printf("[render] 10 x 500 clips, fitted (%.2f px each): median %.2f ms\n", 50 * scene.viewport.pxPerFrame(),
                all);

    // Zoomed in to about 40 clips across: labels, borders and waveforms all draw.
    scene.viewport.zoomAround(22.0, 12.0);
    double zoomed = measure(scene, true, 30);
    std::printf("[render] zoomed without waveforms: median %.2f ms\n", measure(scene, false, 30));
    std::printf("[render] 10 tracks zoomed in (%.0f px clips): median %.2f ms\n", 50 * scene.viewport.pxPerFrame(),
                zoomed);

    // The 4 ms budget is for an optimised build (measure there: meson setup
    // -Dbuildtype=release). Debug and sanitiser builds, often run in parallel
    // with everything else, only get a wide guard that still catches a real
    // regression (the first version of this renderer took 116 ms here).
    // Under ASan or TSan the times say nothing about the product (ASan took
    // 254 ms here), so they're printed, not checked: otherwise `just asan`
    // is always red and a real report there gets overlooked (post-M3 audit
    // P9). GCC defines these macros for -fsanitize=address / thread.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    MESSAGE("sanitizer build: snapshot times reported, not checked");
    (void)all;
    (void)zoomed;
#else
#ifdef NDEBUG
    constexpr double kBudgetMs = 4.0;
#else
    constexpr double kBudgetMs = 60.0;
#endif
    CHECK(all < kBudgetMs);
    CHECK(zoomed < kBudgetMs);
#endif
}

namespace {

// Every texture-scale and fill node under `node`, in order.
void collect(GskRenderNode *node, std::vector<GdkTexture *> &textures, int &fills)
{
    switch (gsk_render_node_get_node_type(node)) {
    case GSK_TEXTURE_SCALE_NODE:
        textures.push_back(gsk_texture_scale_node_get_texture(node));
        return;
    case GSK_FILL_NODE:
        ++fills;
        return;
    case GSK_CONTAINER_NODE:
        for (guint i = 0; i < gsk_container_node_get_n_children(node); ++i)
            collect(gsk_container_node_get_child(node, i), textures, fills);
        return;
    case GSK_CLIP_NODE:
        collect(gsk_clip_node_get_child(node), textures, fills);
        return;
    case GSK_ROUNDED_CLIP_NODE:
        collect(gsk_rounded_clip_node_get_child(node), textures, fills);
        return;
    case GSK_TRANSFORM_NODE:
        collect(gsk_transform_node_get_child(node), textures, fills);
        return;
    case GSK_OPACITY_NODE:
        collect(gsk_opacity_node_get_child(node), textures, fills);
        return;
    default:
        return;
    }
}

} // namespace

TEST_CASE("Timeline waveforms are cached textures, not paths GTK re-rasterises every frame")
{
    // GTK's GPU renderer rasterised a waveform's filled path with cairo on
    // every repaint (10 ms and more a frame for a long clip on screen,
    // 2026-09-29). With a cache they're textures, made once per content.
    Scene scene;
    scene.viewport.zoomAround(22.0, 12.0);
    TextureCache cache(64);
    PangoFontMap *fontMap = pango_cairo_font_map_get_default();
    PangoContext *context = pango_font_map_create_context(fontMap);
    PangoLayout *layout = pango_layout_new(context);
    TimelineScene timeline{.model = scene.model,
                           .viewport = scene.viewport,
                           .controller = scene.controller,
                           .layout = RowLayout{},
                           .handleWidth = 22.0,
                           .activeRow = 0,
                           .nameEditRow = -1,
                           .waveformFor = [&](const Clip &) { return &scene.peaks; },
                           .thumbnailFor = nullptr,
                           .overlays = {},
                           .waveformTextures = &cache,
                           .scaleFactor = 2,
                           .labelLayout = layout};
    auto snapshot = [&](std::vector<GdkTexture *> &textures, int &fills) {
        GtkSnapshot *s = gtk_snapshot_new();
        snapshotTimeline(s, timeline, 1920.0, 600.0);
        GskRenderNode *node = gtk_snapshot_free_to_node(s);
        REQUIRE(node);
        collect(node, textures, fills);
        gsk_render_node_unref(node);
    };
    std::vector<GdkTexture *> first, second;
    int fills = 0;
    snapshot(first, fills);
    CHECK(fills == 0);
    REQUIRE(!first.empty());
    // At the scale factor: 2 device px per logical px each way.
    CHECK(gdk_texture_get_height(first.front()) >= 2 * 10);
    snapshot(second, fills);
    CHECK(second == first); // the same textures again: nothing re-made or re-uploaded
    // Equal waveforms share one texture.
    CHECK(std::count(first.begin(), first.end(), first.front()) > 1);
    g_object_unref(layout);
    g_object_unref(context);
}
