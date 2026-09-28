#include "behavior_drawer.h"

#include "render/title_renderer.h"

#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>

// GLib's main-context lock is invisible to ThreadSanitizer: annotate the
// hand-off, as MainThreadDispatcher does (src/engine/dispatcher.cpp).
#ifdef __SANITIZE_THREAD__
#include <sanitizer/tsan_interface.h>
#define TITLES_TSAN_RELEASE(addr) __tsan_release(addr)
#define TITLES_TSAN_ACQUIRE(addr) __tsan_acquire(addr)
#else
#define TITLES_TSAN_RELEASE(addr) ((void)(addr))
#define TITLES_TSAN_ACQUIRE(addr) ((void)(addr))
#endif

namespace ustudio::titles::app {

namespace {

constexpr int kThumbWidth = 176, kThumbHeight = 99; // 16:9
constexpr int kFrames = 14;
constexpr guint kFrameMs = 90;

// One thumbnail: its picture and the frames it cycles through.
struct Thumb
{
    GtkWidget *picture = nullptr;
    std::vector<GdkTexture *> frames;
};

// The drawer's state, owned by its popover (freed when that goes).
struct Drawer
{
    std::vector<Thumb> thumbs;
    std::vector<TitleDocument> docs;
    std::vector<std::vector<double>> times;
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
    std::thread worker;
    guint tick = 0;
    size_t step = 0;

    ~Drawer()
    {
        cancelled->store(true);
        if (worker.joinable())
            worker.join();
        if (tick)
            g_source_remove(tick);
        for (Thumb &thumb : thumbs)
            for (GdkTexture *texture : thumb.frames)
                g_object_unref(texture);
    }
};

// A rendered frame for thumbnail `index`, back on the main thread.
struct Posted
{
    std::weak_ptr<std::atomic<bool>> alive; // expired or set: the drawer is gone
    Drawer *drawer;
    size_t index;
    RenderedFrame frame;
};

gboolean deliver(gpointer data)
{
    TITLES_TSAN_ACQUIRE(data);
    std::unique_ptr<Posted> posted(static_cast<Posted *>(data));
    auto cancelled = posted->alive.lock();
    if (!cancelled || cancelled->load())
        return G_SOURCE_REMOVE;
    const RenderedFrame &frame = posted->frame;
    GBytes *bytes = g_bytes_new(frame.pixels.data(), frame.pixels.size() * sizeof(uint32_t));
    GdkTexture *texture = gdk_memory_texture_new(frame.width, frame.height, GDK_MEMORY_DEFAULT, bytes,
                                                 static_cast<gsize>(frame.width) * 4);
    g_bytes_unref(bytes);
    Thumb &thumb = posted->drawer->thumbs[posted->index];
    thumb.frames.push_back(texture);
    if (thumb.frames.size() == 1)
        gtk_picture_set_paintable(GTK_PICTURE(thumb.picture), GDK_PAINTABLE(texture));
    return G_SOURCE_REMOVE;
}

gboolean advance(gpointer data)
{
    auto *drawer = static_cast<Drawer *>(data);
    ++drawer->step;
    for (Thumb &thumb : drawer->thumbs)
        if (!thumb.frames.empty())
            gtk_picture_set_paintable(GTK_PICTURE(thumb.picture),
                                      GDK_PAINTABLE(thumb.frames[drawer->step % thumb.frames.size()]));
    return G_SOURCE_CONTINUE;
}

struct PickHandler
{
    std::function<void(const BehaviorInfo &)> pick;
    const BehaviorInfo *info;
    GtkWidget *popover;
};

void onPicked(GtkButton *, gpointer data)
{
    auto *handler = static_cast<PickHandler *>(data);
    const BehaviorInfo *info = handler->info;
    auto pick = handler->pick;
    gtk_popover_popdown(GTK_POPOVER(handler->popover));
    pick(*info);
}

} // namespace

TitleDocument thumbnailDocument(const TitleDocument &doc, const Layer &layer, const Behavior &behavior)
{
    TitleDocument out = doc;
    Layer only = layer;
    std::erase_if(only.behaviors, [&](const Behavior &b) { return b.slot == behavior.slot; });
    only.behaviors.push_back(behavior);
    // Crop the canvas to the layer at rest, with room around it to move in.
    TitleDocument alone = doc;
    alone.layers = {layer};
    const std::vector<LayerGeometry> geometry = measureLayers(alone, static_cast<double>(doc.timing.intro), {});
    Rect box = geometry.empty() ? Rect{0, 0, static_cast<double>(doc.width), static_cast<double>(doc.height)}
                                : geometry.front().box;
    const double pad = std::max(box.w, box.h) * 0.25 + 24;
    box = {box.x - pad, box.y - pad, box.w + 2 * pad, box.h + 2 * pad};
    // 16:9 around it.
    if (box.w / box.h > 16.0 / 9.0) {
        const double h = box.w * 9.0 / 16.0;
        box.y -= (h - box.h) / 2;
        box.h = h;
    } else {
        const double w = box.h * 16.0 / 9.0;
        box.x -= (w - box.w) / 2;
        box.w = w;
    }
    only.x -= box.x;
    only.y -= box.y;
    for (PropertyTrack &track : only.animation)
        for (TitleKey &key : track.keys) {
            if (track.property == Property::X)
                key.key.value -= box.x;
            if (track.property == Property::Y)
                key.key.value -= box.y;
        }
    out.width = std::max(16, static_cast<int>(std::lround(box.w)));
    out.height = std::max(16, static_cast<int>(std::lround(box.h)));
    out.layers = {only};
    return out;
}

std::vector<double> thumbnailFrames(const Timing &timing, const Behavior &behavior, int count)
{
    const auto d = static_cast<double>(behavior.duration);
    const auto end = static_cast<double>(timing.length());
    double from = 0, to = d + 6; // in: the behaviour and a moment after
    if (behavior.slot == BehaviorSlot::Out) {
        from = std::max(0.0, end - d - 6);
        to = end;
    } else if (behavior.slot == BehaviorSlot::Loop) {
        from = static_cast<double>(timing.intro) + 8; // past the loop's ramp
        to = from + d;
    }
    std::vector<double> frames;
    for (int i = 0; i < count; ++i)
        frames.push_back(std::floor(from + (to - from) * i / std::max(1, count - 1)));
    return frames;
}

GtkWidget *makeBehaviorDrawer(const TitleDocument &doc, const Layer &layer, BehaviorSlot slot,
                              std::function<void(const BehaviorInfo &)> pick)
{
    GtkWidget *popover = gtk_popover_new();
    auto *drawer = new Drawer;
    g_object_set_data_full(G_OBJECT(popover), "drawer", drawer, [](gpointer d) { delete static_cast<Drawer *>(d); });

    GtkWidget *flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 4);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 2);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 8);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 8);
    for (const BehaviorInfo &info : behaviorCatalogue()) {
        const bool fits = (slot == BehaviorSlot::In && info.in) || (slot == BehaviorSlot::Out && info.out) ||
                          (slot == BehaviorSlot::Loop && info.loop);
        if (!fits || (info.textOnly && layer.kind != LayerKind::Text))
            continue;
        const Behavior behavior{slot, info.id, info.duration, info.easing, 1, 1.0};
        drawer->docs.push_back(thumbnailDocument(doc, layer, behavior));
        drawer->times.push_back(thumbnailFrames(doc.timing, behavior, kFrames));

        GtkWidget *button = gtk_button_new();
        gtk_widget_add_css_class(button, "flat");
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget *picture = gtk_picture_new();
        gtk_widget_set_size_request(picture, kThumbWidth, kThumbHeight);
        gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
        gtk_widget_add_css_class(picture, "card");
        gtk_box_append(GTK_BOX(box), picture);
        GtkWidget *name = gtk_label_new(info.label);
        gtk_box_append(GTK_BOX(box), name);
        gtk_button_set_child(GTK_BUTTON(button), box);
        gtk_widget_set_tooltip_text(button, (std::string("Add ") + info.label).c_str());
        g_signal_connect_data(
            button, "clicked", G_CALLBACK(onPicked), new PickHandler{pick, &info, popover},
            [](gpointer data, GClosure *) { delete static_cast<PickHandler *>(data); }, G_CONNECT_DEFAULT);
        gtk_flow_box_append(GTK_FLOW_BOX(flow), button);
        drawer->thumbs.push_back({picture, {}});
    }
    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(scroller), 4 * (kThumbWidth + 16));
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroller), 480);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), flow);
    gtk_popover_set_child(GTK_POPOVER(popover), scroller);

    // Frames render on a worker, thumbnail by thumbnail, and cycle as they come.
    std::weak_ptr<std::atomic<bool>> alive = drawer->cancelled;
    drawer->worker =
        std::thread([docs = drawer->docs, times = drawer->times, cancelled = drawer->cancelled, alive, drawer] {
            for (size_t i = 0; i < docs.size(); ++i)
                for (double t : times[i]) {
                    if (cancelled->load())
                        return;
                    auto *posted =
                        new Posted{alive, drawer, i, renderTitle(docs[i], t, {}, kThumbWidth, kThumbHeight).frame};
                    TITLES_TSAN_RELEASE(posted);
                    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, &deliver, posted, nullptr);
                }
        });
    drawer->tick = g_timeout_add(kFrameMs, &advance, drawer);
    return popover;
}

} // namespace ustudio::titles::app
