#include "engine/frame_renderer.h"

#include "core/log.h"
#include "core/model/animation.h"
#include "core/model/effect_native.h"
#include "core/model/transition_native.h"
#include "platform/files.h"
#include "platform/process.h"
#include "engine/dispatcher.h"
#include "engine/engine_extension.h"
#include "engine/producer_open.h"

#include <glib.h>
#include <mlt++/Mlt.h>

#include <algorithm>
#include <filesystem>
#include <cstring>
#include <set>

namespace ustudio::effects {

namespace {

std::string effectKey(const core::Effect &effect)
{
    std::string key = effect.service + (effect.enabled ? "+" : "-");
    for (const core::NativeFilter &native : core::nativeFilters(effect, 0, 1'000'000))
        for (const auto &[name, value] : native.properties)
            key += "|" + name + "=" + value;
    return key;
}

// A profile like the sequence's (its rate and shape), at the size asked.
std::unique_ptr<Mlt::Profile> profileFor(const FrameRequest &request)
{
    auto profile = std::make_unique<Mlt::Profile>();
    profile->set_width(request.width);
    profile->set_height(request.height);
    profile->set_frame_rate(std::max(1, request.profile.fps.num), std::max(1, request.profile.fps.den));
    profile->set_sample_aspect(1, 1);
    profile->set_display_aspect(request.width, request.height);
    profile->set_progressive(1);
    return profile;
}

} // namespace

std::string FrameRequest::key() const
{
    std::string key = resource + "#" + std::to_string(sourceFrame) + "@" + std::to_string(clipIn) + "-" +
                      std::to_string(clipOut) + " " + std::to_string(width) + "x" + std::to_string(height) + " " +
                      std::to_string(profile.fps.num) + "/" + std::to_string(profile.fps.den);
    for (const core::Effect &effect : effects)
        key += "\n" + effectKey(effect);
    if (transition) {
        key += "\ntransition " + transition->resource + "#" + std::to_string(transition->in) + "-" +
               std::to_string(transition->out) + "@" + std::to_string(transition->position);
        for (const core::Param &param : transition->params)
            key += "|" + param.name + "=" + core::nativeValue(param.value);
    }
    return key;
}

namespace {
// Every renderer alive, for stopAll().
std::mutex g_liveMutex;
std::set<FrameRenderer *> g_live;
} // namespace

FrameRenderer::FrameRenderer(size_t cacheEntries) : m_capacity(cacheEntries)
{
    m_worker = std::thread([this] { run(); });
    std::lock_guard lock(g_liveMutex);
    g_live.insert(this);
}

FrameRenderer::~FrameRenderer()
{
    stop();
    std::lock_guard lock(g_liveMutex);
    g_live.erase(this);
}

void FrameRenderer::stopAll()
{
    std::set<FrameRenderer *> live;
    {
        std::lock_guard lock(g_liveMutex);
        live = g_live;
    }
    for (FrameRenderer *renderer : live)
        renderer->stop();
}

void FrameRenderer::stop()
{
    {
        std::lock_guard lock(m_mutex);
        m_stopping = true;
        m_jobs.clear();
        m_token.reset(); // results already posted are dropped
    }
    m_wake.notify_all();
    if (m_worker.joinable())
        m_worker.join();
    m_holdsMedia = false; // closed with the worker's thread
}

const RenderedFrame *FrameRenderer::cached(const FrameRequest &request) const
{
    auto it = m_cache.find(request.key());
    return it == m_cache.end() ? nullptr : &it->second.first;
}

void FrameRenderer::request(FrameRequest request, int lane, uint64_t generation, Done done)
{
    if (const RenderedFrame *frame = cached(request)) {
        done(*frame);
        return;
    }
    {
        std::lock_guard lock(m_mutex);
        if (m_stopping)
            return;
        uint64_t &newest = m_newest[lane];
        newest = std::max(newest, generation);
        m_jobs.push_back({std::move(request), lane, generation, std::move(done)});
    }
    m_wake.notify_one();
}

namespace {

// The media, opened once per resource and size on the worker thread and
// reused for every effect asked of it: opening costs far more than a frame.
struct OpenMedia
{
    std::string key;
    std::unique_ptr<Mlt::Profile> profile;
    std::unique_ptr<Mlt::Producer> media;
};

bool open(OpenMedia &open, const FrameRequest &request)
{
    const std::string key = request.resource + " " + std::to_string(request.width) + "x" +
                            std::to_string(request.height) + " " + std::to_string(request.profile.fps.num) + "/" +
                            std::to_string(request.profile.fps.den);
    // A clip that doesn't open stays unopened for its key: asked again, the
    // invalid producer must not be cut and seeked (a crash in the demo tour,
    // a title whose template wasn't picked yet, 2026-09-29).
    if (open.key == key)
        return open.media && open.media->is_valid();
    open.media.reset();
    open.profile = profileFor(request);
    open.media = engine::openProducer(*open.profile, request.resource, engine::ProducerUse::Worker);
    open.key = key;
    core::Log::debug("[effects] frame renderer: opened " + key);
    return open.media && open.media->is_valid();
}

// A filter for this thread's graph, or null when the service is missing.
std::unique_ptr<Mlt::Filter> workerFilter(Mlt::Profile &profile, const core::NativeFilter &native)
{
    auto filter = std::make_unique<Mlt::Filter>(profile, native.service.c_str());
    if (!filter->is_valid())
        return nullptr;
    for (const auto &[name, value] : native.properties)
        filter->set(name.c_str(), value.c_str());
    if (native.service == "affine") {
        // affine draws onto a background it opens through the default
        // loader, which gives movit's normalisers while the GPU pipeline's
        // glsl.manager exists: a movit frame on this GL-less thread (a
        // crash in the effects smoke, 2026-09-29). A worker producer
        // instead, as EngineSync's transforms do (docs/developer/notes/gpu.md).
        std::unique_ptr<Mlt::Producer> canvas = engine::openProducer(profile, "colour:0", engine::ProducerUse::Worker);
        if (canvas && canvas->is_valid()) {
            canvas->inc_ref();
            filter->set(
                "producer", canvas->get_producer(), 0,
                +[](void *producer) { mlt_producer_close(static_cast<mlt_producer>(producer)); });
        }
    }
    return filter;
}

// A transition's frame: the outgoing tail and the incoming head in a
// two-track tractor with the recipe's services, as EngineSync builds a
// dissolve (every transition's in/out the tractor's [0, length-1]).
RenderedFrame renderTransition(OpenMedia &media, const FrameRequest &request)
{
    RenderedFrame out;
    const FrameRequest::Transition &spec = *request.transition;
    if (!open(media, request))
        return out;
    Mlt::Profile &profile = *media.profile;
    std::unique_ptr<Mlt::Producer> incoming = engine::openProducer(profile, spec.resource, engine::ProducerUse::Worker);
    if (!incoming || !incoming->is_valid())
        return out;
    const core::FrameIndex length = request.clipOut - request.clipIn + 1;
    std::unique_ptr<Mlt::Producer> tail(media.media->cut(
        static_cast<int>(std::max<core::FrameIndex>(request.clipIn, 0)), static_cast<int>(request.clipOut)));
    std::unique_ptr<Mlt::Producer> head(
        incoming->cut(static_cast<int>(std::max<core::FrameIndex>(spec.in, 0)), static_cast<int>(spec.out)));
    core::Transition model;
    model.length = length;
    model.params = spec.params;
    const core::NativeTransition native = core::nativeTransition(model);
    std::vector<std::unique_ptr<Mlt::Filter>> filters;
    for (const auto &[specs, cut] : {std::pair{&native.tailFilters, tail.get()}, {&native.headFilters, head.get()}})
        for (const core::NativeFilter &nf : *specs) {
            std::unique_ptr<Mlt::Filter> filter = workerFilter(profile, nf);
            if (!filter)
                continue;
            engine::attachToCut(*cut, *filter);
            filters.push_back(std::move(filter));
        }
    Mlt::Tractor tractor(profile);
    tractor.set_track(*tail, 0);
    tractor.set_track(*head, 1);
    std::unique_ptr<Mlt::Field> field(tractor.field());
    Mlt::Transition video(profile, native.video.service.c_str());
    if (!video.is_valid())
        return out;
    for (const auto &[name, value] : native.video.properties)
        video.set(name.c_str(), value.c_str());
    if (!native.luma.empty() && native.video.service == "luma") {
        // The same generated maps the editor uses (core::lumaMapPath()).
        const std::filesystem::path cache = platform::userCacheDirectory();
        const std::filesystem::path map = core::lumaMapPath(cache / "ustudio" / "luma", native.luma);
        if (!cache.empty() && core::writeLumaMap(native.luma, map))
            video.set("resource", map.string().c_str());
    }
    video.set_in_and_out(0, static_cast<int>(length - 1));
    field->plant_transition(video, 0, 1);
    tractor.seek(static_cast<int>(std::clamp<core::FrameIndex>(spec.position, 0, length - 1)));
    std::unique_ptr<Mlt::Frame> frame(tractor.get_frame());
    if (!frame)
        return out;
    mlt_image_format format = mlt_image_rgba;
    int width = request.width, height = request.height;
    const uint8_t *image = frame->get_image(format, width, height);
    if (!image || format != mlt_image_rgba || width <= 0 || height <= 0)
        return out;
    out.width = width;
    out.height = height;
    out.rgba.assign(image, image + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    return out;
}

RenderedFrame render(OpenMedia &media, const FrameRequest &request)
{
    RenderedFrame out;
    if (request.transition)
        return renderTransition(media, request);
    if (!open(media, request))
        return out;
    Mlt::Profile &profile = *media.profile;
    const int in = static_cast<int>(std::max<core::FrameIndex>(request.clipIn, 0));
    const int outPoint = static_cast<int>(std::max(request.clipOut, request.clipIn));
    std::unique_ptr<Mlt::Producer> cut(media.media->cut(in, outPoint));
    std::vector<std::unique_ptr<Mlt::Filter>> filters;
    const core::FrameIndex length = request.clipOut - request.clipIn + 1;
    for (const core::Effect &effect : request.effects) {
        if (!effect.enabled)
            continue;
        for (const core::NativeFilter &native : core::nativeFilters(effect, 0, length)) {
            std::unique_ptr<Mlt::Filter> filter = workerFilter(profile, native);
            if (!filter)
                break;
            engine::attachToCut(*cut, *filter);
            filters.push_back(std::move(filter));
        }
    }
    cut->seek(static_cast<int>(request.sourceFrame - request.clipIn));
    std::unique_ptr<Mlt::Frame> frame(cut->get_frame());
    if (!frame)
        return out;
    mlt_image_format format = mlt_image_rgba;
    int width = request.width, height = request.height;
    const uint8_t *image = frame->get_image(format, width, height);
    if (!image || format != mlt_image_rgba || width <= 0 || height <= 0)
        return out;
    out.width = width;
    out.height = height;
    out.rgba.assign(image, image + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    return out;
}

} // namespace

RenderedFrame FrameRenderer::renderNow(const FrameRequest &request)
{
    OpenMedia media;
    return render(media, request);
}

void FrameRenderer::run()
{
    OpenMedia media; // this thread's alone
    while (true) {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            const auto ready = [this] { return m_stopping || !m_jobs.empty(); };
            if (!media.media) {
                m_wake.wait(lock, ready);
            } else if (!m_wake.wait_for(lock, kReleaseIdleMedia, ready)) {
                // Idle: close the media. An open 1080p H.264 decoder holds
                // about 180 MB (a frame thread a core, each with its
                // buffers), which a burst of tiles needs for a moment and
                // an editor playing for an hour doesn't (measured
                // 2026-09-29: the GPU soak's "leak" was this one step).
                lock.unlock();
                core::Log::debug("[effects] frame renderer: closed " + media.key + " (idle)");
                media = OpenMedia{};
                m_holdsMedia = false;
                platform::releaseFreeMemory(); // or RSS stays at the peak
                continue;
            }
            if (m_stopping)
                return;
            // The lowest lane first (the audition before the tiles), newest
            // first within it; stale generations are dropped.
            std::erase_if(m_jobs, [this](const Job &j) { return j.generation < m_newest[j.lane]; });
            if (m_jobs.empty())
                continue;
            auto best = m_jobs.begin();
            for (auto it = m_jobs.begin(); it != m_jobs.end(); ++it)
                if (it->lane < best->lane)
                    best = it;
            job = std::move(*best);
            m_jobs.erase(best);
        }
        const std::string key = job.request.key();
        RenderedFrame frame = render(media, job.request);
        m_holdsMedia = media.media != nullptr;
        std::weak_ptr<void> token;
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping)
                return;
            token = m_token;
        }
        // Not g_idle_add() by hand: the dispatcher annotates the hand-off
        // for ThreadSanitizer (GLib's lock is invisible to it).
        engine::MainThreadDispatcher::post(token,
                                           [this, key, frame = std::move(frame), done = std::move(job.done)]() mutable {
                                               deliver(key, std::move(frame), std::move(done));
                                           });
    }
}

void FrameRenderer::deliver(const std::string &key, RenderedFrame frame, Done done)
{
    if (!frame.rgba.empty()) {
        auto it = m_cache.find(key);
        if (it == m_cache.end()) {
            m_order.push_front(key);
            it = m_cache.emplace(key, std::make_pair(std::move(frame), m_order.begin())).first;
            while (m_cache.size() > m_capacity) {
                m_cache.erase(m_order.back());
                m_order.pop_back();
            }
        }
        done(it->second.first);
        return;
    }
    done(frame); // empty: the caller shows its placeholder
}

} // namespace ustudio::effects
