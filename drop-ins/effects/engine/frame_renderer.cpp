#include "engine/frame_renderer.h"

#include "core/log.h"
#include "core/model/animation.h"
#include "core/model/effect_native.h"
#include "platform/process.h"
#include "engine/dispatcher.h"
#include "engine/engine_extension.h"
#include "engine/producer_open.h"

#include <glib.h>
#include <mlt++/Mlt.h>

#include <algorithm>
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

RenderedFrame render(OpenMedia &media, const FrameRequest &request)
{
    RenderedFrame out;
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
            auto filter = std::make_unique<Mlt::Filter>(profile, native.service.c_str());
            if (!filter->is_valid())
                break;
            for (const auto &[name, value] : native.properties)
                filter->set(name.c_str(), value.c_str());
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
