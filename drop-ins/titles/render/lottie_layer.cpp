#include "lottie_layer.h"

#include "core/media/utf8_path.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <utility>
#include <vector>

#ifdef TITLES_HAVE_THORVG
#include <thorvg_capi.h>
#endif

namespace ustudio::titles {

namespace {

#ifdef TITLES_HAVE_THORVG

// ThorVG isn't safe on concurrent threads, even with separate canvases and
// animations (slice 1 repro: 6 crashes in 10 runs; 0 in 10 behind one lock).
// The lock is this library's, so a process must hold one copy of titlerender
// that draws animations: the editor and u-studio-render have the MLT
// module's, the designer its own (the editor's side of the drop-in doesn't
// link titlerender). Linking a second copy into a process that already
// loads the module would give ThorVG two locks, which is none.
std::mutex &thorvgLock()
{
    static std::mutex lock;
    return lock;
}

// Once per process, under the lock: no threads of ThorVG's own, since
// titlerender already runs on the callers' threads.
void initThorvg()
{
    static bool started = false;
    if (!started) {
        tvg_engine_init(0);
        started = true;
    }
}

// A checked, loaded animation and the canvas it's drawn on, owned by one
// thread. One canvas for the animation's life: a picture moved to a fresh
// canvas drew nothing after its first (ThorVG 1.0.6, slice 3); a canvas
// can be pointed at a new buffer each frame.
struct Loaded
{
    std::string path;
    std::filesystem::file_time_type modified;
    lottie::Facts facts;
    Tvg_Animation animation = nullptr;
    Tvg_Canvas canvas = nullptr;

    Loaded() = default;
    Loaded(const Loaded &) = delete;
    Loaded &operator=(const Loaded &) = delete;
    Loaded(Loaded &&other) noexcept
        : path(std::move(other.path)), modified(other.modified), facts(other.facts),
          animation(std::exchange(other.animation, nullptr)), canvas(std::exchange(other.canvas, nullptr))
    {}
    Loaded &operator=(Loaded &&other) noexcept
    {
        std::swap(path, other.path);
        std::swap(modified, other.modified);
        std::swap(facts, other.facts);
        std::swap(animation, other.animation);
        std::swap(canvas, other.canvas);
        return *this;
    }
    ~Loaded()
    {
        if (animation || canvas) {
            std::lock_guard guard(thorvgLock());
            // The canvas releases its reference to the picture; the
            // animation owns it and goes last.
            if (canvas)
                tvg_canvas_destroy(canvas);
            if (animation)
                tvg_animation_del(animation);
        }
    }
};

#else

struct Loaded
{
    std::string path;
    std::filesystem::file_time_type modified;
    lottie::Facts facts;
};

#endif

// The thread's loaded animations, the most recently used last.
Loaded *loaded(const std::string &path, const std::string &shownName, std::set<std::string> &warnings)
{
    thread_local std::vector<Loaded> cache;
    const std::filesystem::path file = core::pathFromUtf8(path);
    std::error_code ec;
    const auto modified = std::filesystem::last_write_time(file, ec);
    if (ec) {
        warnings.insert("the animation " + shownName + " isn't there");
        return nullptr;
    }
    for (auto it = cache.begin(); it != cache.end(); ++it)
        if (it->path == path && it->modified == modified) {
            std::rotate(it, it + 1, cache.end());
            return &cache.back();
        }
    const auto size = std::filesystem::file_size(file, ec);
    if (ec || size > lottie::kMaxBytes) {
        warnings.insert("the animation " + shownName + " can't be used: " +
                        (ec ? "it doesn't read" : "it's over " + std::to_string(lottie::kMaxBytes >> 20) + " MB"));
        return nullptr;
    }
    std::ifstream in(file, std::ios::binary);
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto facts = lottie::check(json);
    if (!facts) {
        warnings.insert("the animation " + shownName + " can't be used: " + facts.error());
        return nullptr;
    }
    Loaded entry;
    entry.path = path;
    entry.modified = modified;
    entry.facts = *facts;
#ifdef TITLES_HAVE_THORVG
    {
        std::lock_guard guard(thorvgLock());
        initThorvg();
        entry.animation = tvg_animation_new();
        // From memory, copied (copy=false caches by address, process-wide),
        // with no resource path: the check refused any external asset.
        Tvg_Paint picture = tvg_animation_get_picture(entry.animation);
        if (tvg_picture_load_data(picture, json.data(), static_cast<uint32_t>(json.size()), "lottie+json", "", true) ==
            TVG_RESULT_SUCCESS) {
            entry.canvas = tvg_swcanvas_create(TVG_ENGINE_OPTION_DEFAULT);
            tvg_canvas_add(entry.canvas, picture);
        } else {
            tvg_animation_del(entry.animation);
            entry.animation = nullptr;
        }
    }
    if (!entry.animation) {
        warnings.insert("the animation " + shownName + " doesn't load");
        return nullptr;
    }
#endif
    constexpr size_t kCached = 8;
    std::erase_if(cache, [&](const Loaded &c) { return c.path == path; });
    if (cache.size() >= kCached)
        cache.erase(cache.begin());
    cache.push_back(std::move(entry));
    return &cache.back();
}

} // namespace

std::optional<lottie::Facts> lottieFacts(const std::string &path, const std::string &shownName,
                                         std::set<std::string> &warnings)
{
    if (const Loaded *entry = loaded(path, shownName, warnings))
        return entry->facts;
    return std::nullopt;
}

cairo_surface_t *lottieFrame(const std::string &path, const std::string &shownName, double frame, int width, int height,
                             std::set<std::string> &warnings)
{
#ifdef TITLES_HAVE_THORVG
    Loaded *entry = loaded(path, shownName, warnings);
    if (!entry || width < 1 || height < 1)
        return nullptr;
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return nullptr;
    }
    cairo_surface_flush(surface);
    // Cairo's ARGB32 is ThorVG's premultiplied ARGB8888; the stride in pixels.
    auto *pixels = reinterpret_cast<uint32_t *>(cairo_image_surface_get_data(surface));
    const auto stride = static_cast<uint32_t>(cairo_image_surface_get_stride(surface) / 4);
    {
        std::lock_guard guard(thorvgLock());
        Tvg_Paint picture = tvg_animation_get_picture(entry->animation);
        // set_size stretches to exactly this; the caller asked for the
        // animation's own aspect (slice 1).
        tvg_picture_set_size(picture, static_cast<float>(width), static_cast<float>(height));
        tvg_animation_set_frame(entry->animation, static_cast<float>(frame));
        tvg_swcanvas_set_target(entry->canvas, pixels, stride, static_cast<uint32_t>(width),
                                static_cast<uint32_t>(height), TVG_COLORSPACE_ARGB8888);
        tvg_canvas_update(entry->canvas);
        tvg_canvas_draw(entry->canvas, true);
        tvg_canvas_sync(entry->canvas);
    }
    cairo_surface_mark_dirty(surface);
    return surface;
#else
    (void)path;
    (void)frame;
    (void)width;
    (void)height;
    warnings.insert("the animation " + shownName + " isn't drawn: this build has no ThorVG");
    return nullptr;
#endif
}

} // namespace ustudio::titles
