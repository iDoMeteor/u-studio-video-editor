#include "thumbnail_cache.h"

#include "core/log.h"

#include <mlt++/Mlt.h>

#include <algorithm>
#include <memory>

namespace ustudio::engine {

namespace Log = ustudio::core::Log;

namespace {
// A media-browser row icon, not a preview -- kept small on purpose so a
// project with dozens of imports doesn't decode/hold full-resolution
// frames just to draw a list of thumbnails. Height varies with the
// source's own aspect ratio (computed per job below); callers letterbox
// to a fixed row height the same way m_preview already does
// (GTK_CONTENT_FIT_CONTAIN), rather than this cache forcing a fixed
// aspect itself.
constexpr int kThumbnailWidth = 120;
} // namespace

ThumbnailCache::ThumbnailCache(core::concurrency::ThreadPool &pool, std::function<void()> onReady, size_t maxJobs,
                               core::concurrency::Priority priority)
    : m_pool(pool), m_maxJobs(std::max<size_t>(1, maxJobs)), m_priority(priority), m_onReady(std::move(onReady))
{}

ThumbnailCache::~ThumbnailCache()
{
    shutdown();
}

void ThumbnailCache::shutdown()
{
    std::vector<core::concurrency::JobHandle> jobs;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_quit = true;
        m_readyBatches.clear();
        jobs.swap(m_jobs);
    }
    for (core::concurrency::JobHandle &job : jobs)
        job.cancel(); // queued ones never start; running ones see their stop token
    for (core::concurrency::JobHandle &job : jobs)
        job.wait();
}

const ThumbnailCache::Data *ThumbnailCache::thumbnailFor(const std::string &resource)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_cache.find(resource);
    if (it != m_cache.end())
        return &it->second;

    if (!m_quit && m_inFlight.insert(resource).second) {
        enqueue(Job{resource, -1, 0, 1, resource}, /*newest=*/false);
        schedule();
    }
    return nullptr;
}

const ThumbnailCache::Data *ThumbnailCache::frameThumbnail(const std::string &resource, int frame, int fpsNum,
                                                           int fpsDen)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    // Evicted here, on the main thread, never by a job: a pointer this
    // returned is used before the next call, so eviction can't pull an entry
    // out from under a caller.
    while (m_frameOrder.size() > kMaxFrameThumbnails) {
        m_cache.erase(m_frameOrder.front());
        m_frameOrder.pop_front();
    }

    std::string key =
        resource + '\n' + std::to_string(frame) + '@' + std::to_string(fpsNum) + '/' + std::to_string(fpsDen);
    auto it = m_cache.find(key);
    if (it != m_cache.end())
        return &it->second;
    m_requestedIn[key] = m_generation;
    if (!m_quit && m_inFlight.insert(key).second) {
        enqueue(Job{resource, frame, fpsNum, fpsDen, key}, /*newest=*/true);
        schedule();
    }
    return nullptr;
}

void ThumbnailCache::newFrameGeneration()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_generation;
}

size_t ThumbnailCache::frameThumbnailsDecoded() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_framesDecoded;
}

size_t ThumbnailCache::peakConcurrentJobs() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_peakRunning;
}

namespace {
std::string batchKeyFor(const std::string &resource, int fpsNum, int fpsDen)
{
    return resource + '\n' + std::to_string(fpsNum) + '/' + std::to_string(fpsDen);
}
} // namespace

void ThumbnailCache::enqueue(Job job, bool newest)
{
    std::string batchKey = batchKeyFor(job.resource, job.fpsNum, job.fpsDen);
    Batch &batch = m_batches[batchKey];
    if (newest)
        batch.pending.push_front(std::move(job));
    else
        batch.pending.push_back(std::move(job));
    // Newest-asked file first, as the old single queue served newest first.
    std::erase(m_readyBatches, batchKey);
    if (!batch.running)
        m_readyBatches.push_front(batchKey);
}

bool ThumbnailCache::isStale(const Job &job)
{
    if (job.frame < 0)
        return false;
    auto requested = m_requestedIn.find(job.key);
    bool stale = requested == m_requestedIn.end() || requested->second + 1 < m_generation;
    if (requested != m_requestedIn.end())
        m_requestedIn.erase(requested);
    return stale;
}

void ThumbnailCache::schedule()
{
    // Finished handles are dropped as new ones go in.
    std::erase_if(m_jobs, [](const core::concurrency::JobHandle &job) {
        auto state = job.state();
        return state != core::concurrency::JobState::Pending && state != core::concurrency::JobState::Running;
    });
    while (!m_quit && m_running < m_maxJobs && !m_readyBatches.empty()) {
        std::string batchKey = m_readyBatches.front();
        m_readyBatches.pop_front();
        m_batches[batchKey].running = true;
        ++m_running;
        m_peakRunning = std::max(m_peakRunning, m_running);
        m_jobs.push_back(
            m_pool.submit([this, batchKey](std::stop_token stop) { runBatch(batchKey, stop); }, m_priority));
    }
}

void ThumbnailCache::runBatch(const std::string &batchKey, std::stop_token stop)
{
    // Opened once for the batch: every job in it is the same file at the
    // same rate.
    std::unique_ptr<Mlt::Profile> openProfile;
    std::unique_ptr<Mlt::Producer> openProducer;

    while (true) {
        Job job;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            Batch &batch = m_batches[batchKey];
            // Stale requests go without decoding; if wanted later they are
            // simply requested again.
            while (!batch.pending.empty() && isStale(batch.pending.front())) {
                m_inFlight.erase(batch.pending.front().key);
                batch.pending.pop_front();
            }
            if (batch.pending.empty() || stop.stop_requested() || m_quit) {
                if (stop.stop_requested() || m_quit) {
                    for (const Job &left : batch.pending)
                        m_inFlight.erase(left.key);
                    batch.pending.clear();
                }
                batch.running = false;
                if (batch.pending.empty())
                    m_batches.erase(batchKey);
                --m_running;
                schedule();
                return;
            }
            job = std::move(batch.pending.front());
            batch.pending.pop_front();
        }

        Data data;
        Log::ScopedTimer timer("[thumbnail] job " + job.key);

        // Independent Profile/Producer per job, same as WaveformCache --
        // never touches EngineSync's own objects or the live
        // playback/editing state. A thumbnail isn't frame-accurate
        // against any sequence's fps, so the default profile's fps is
        // fine -- but its 720x576, 16:15-sample-aspect *frame size*
        // (dv_pal, MLT's own default) is not: the loader's normalising
        // filters scale and pad every decoded frame to the profile,
        // which get_image() then returns as 720x576 with black bars and
        // squashed pixels regardless of the source's real 16:9 shape
        // (audit E2, verified 2026-09-22 and again in a standalone
        // repro here: a 1920x1080 red clip decoded to 720x576 with a
        // black bar at rows 0-2 and 573-575). meta.media.width/height
        // only populate after a frame has been pulled at least once
        // (this session's own earlier finding, engine_sync.cpp's
        // probeMedia), so prime with one throwaway get_frame() first,
        // then reconfigure the SAME profile object's width/height/
        // sample-aspect before decoding the real thumbnail frame --
        // confirmed empirically that this is enough on its own; the
        // producer does not need to be reopened.
        if (!openProducer) {
            openProducer.reset();
            openProfile = std::make_unique<Mlt::Profile>();
            // A frame job counts frames at the sequence's rate; set it
            // before opening, so the producer's positions use it.
            if (job.frame >= 0 && job.fpsNum > 0 && job.fpsDen > 0)
                openProfile->set_frame_rate(job.fpsNum, job.fpsDen);
            openProducer = std::make_unique<Mlt::Producer>(*openProfile, job.resource.c_str());
            if (openProducer->is_valid()) {
                std::unique_ptr<Mlt::Frame> primeFrame(openProducer->get_frame());
                int metaWidth = openProducer->get_int("meta.media.width");
                int metaHeight = openProducer->get_int("meta.media.height");
                if (metaWidth > 0 && metaHeight > 0) {
                    openProfile->set_width(metaWidth);
                    openProfile->set_height(metaHeight);
                    openProfile->set_sample_aspect(1, 1);
                }
            }
        }
        Mlt::Producer &producer = *openProducer;
        if (producer.is_valid()) {
            int length = producer.get_length();
            // A representative frame, not necessarily the first: many
            // real clips fade in from or open on black, which makes a
            // frame-0 thumbnail useless for telling clips apart at a
            // glance. 10% in is a cheap, reasonable stand-in for "past
            // the open" without needing real content-aware selection.
            int targetFrame = job.frame >= 0 ? std::clamp(job.frame, 0, std::max(length - 1, 0))
                                             : std::clamp(length / 10, 0, std::max(length - 1, 0));
            producer.seek(targetFrame);

            std::unique_ptr<Mlt::Frame> frame(producer.get_frame());
            if (frame && frame->is_valid()) {
                mlt_image_format format = mlt_image_rgba;
                int srcWidth = 0;
                int srcHeight = 0;
                uint8_t *image = frame->get_image(format, srcWidth, srcHeight);
                if (image && srcWidth > 0 && srcHeight > 0) {
                    int dstWidth = kThumbnailWidth;
                    int dstHeight = std::max(1, dstWidth * srcHeight / srcWidth);
                    data.rgba.resize(static_cast<size_t>(dstWidth) * static_cast<size_t>(dstHeight) * 4);
                    // Nearest-neighbour downsample: a thumbnail is small
                    // and decoded once per asset then cached forever, so
                    // decode/box-filter quality isn't worth the extra
                    // code -- this is legible at list-row size.
                    for (int y = 0; y < dstHeight; ++y) {
                        int srcY = std::min(srcHeight - 1, y * srcHeight / dstHeight);
                        for (int x = 0; x < dstWidth; ++x) {
                            int srcX = std::min(srcWidth - 1, x * srcWidth / dstWidth);
                            const uint8_t *srcPixel = image + (static_cast<size_t>(srcY) * srcWidth + srcX) * 4;
                            uint8_t *dstPixel = data.rgba.data() + (static_cast<size_t>(y) * dstWidth + x) * 4;
                            dstPixel[0] = srcPixel[0];
                            dstPixel[1] = srcPixel[1];
                            dstPixel[2] = srcPixel[2];
                            dstPixel[3] = srcPixel[3];
                        }
                    }
                    data.width = dstWidth;
                    data.height = dstHeight;
                }
            }
        } else {
            Log::warn("[thumbnail] could not open " + job.resource);
            openProducer.reset(); // try again next time
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cache.emplace(job.key, std::move(data));
            m_inFlight.erase(job.key);
            if (job.frame >= 0) {
                m_frameOrder.push_back(job.key);
                ++m_framesDecoded;
            }
        }

        // Through MainThreadDispatcher rather than a raw g_idle_add(): one
        // hand-off mechanism for every worker-to-main-thread post, with a
        // lifetime guard, and the one place the TSan annotations for that
        // hand-off live (sanitizer report S5).
        MainThreadDispatcher::post(m_lifetime, m_onReady);
    }
}

} // namespace ustudio::engine
