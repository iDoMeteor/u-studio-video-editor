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

ThumbnailCache::ThumbnailCache(std::function<void()> onReady) : m_onReady(std::move(onReady))
{
    m_worker = std::thread([this] { workerMain(); });
}

ThumbnailCache::~ThumbnailCache()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_quit.store(true);
    }
    m_cv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

const ThumbnailCache::Data *ThumbnailCache::thumbnailFor(const std::string &resource)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_cache.find(resource);
    if (it != m_cache.end())
        return &it->second;

    if (m_inFlight.find(resource) == m_inFlight.end()) {
        m_inFlight.insert(resource);
        m_queue.push_back(Job{resource});
        m_cv.notify_one();
    }
    return nullptr;
}

void ThumbnailCache::workerMain()
{
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_quit.load() || !m_queue.empty(); });
            if (m_queue.empty()) {
                if (m_quit.load())
                    return;
                continue;
            }
            job = m_queue.front();
            m_queue.pop_front();
        }

        Data data;
        Log::ScopedTimer timer("[thumbnail] job " + job.resource);

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
        Mlt::Profile profile;
        Mlt::Producer producer(profile, job.resource.c_str());
        if (producer.is_valid()) {
            std::unique_ptr<Mlt::Frame> primeFrame(producer.get_frame());
            int metaWidth = producer.get_int("meta.media.width");
            int metaHeight = producer.get_int("meta.media.height");
            if (metaWidth > 0 && metaHeight > 0) {
                profile.set_width(metaWidth);
                profile.set_height(metaHeight);
                profile.set_sample_aspect(1, 1);
            }

            int length = producer.get_length();
            // A representative frame, not necessarily the first: many
            // real clips fade in from or open on black, which makes a
            // frame-0 thumbnail useless for telling clips apart at a
            // glance. 10% in is a cheap, reasonable stand-in for "past
            // the open" without needing real content-aware selection.
            int targetFrame = std::clamp(length / 10, 0, std::max(length - 1, 0));
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
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cache.emplace(job.resource, std::move(data));
            m_inFlight.erase(job.resource);
        }

        // Through MainThreadDispatcher rather than a raw g_idle_add(): one
        // hand-off mechanism for every worker-to-main-thread post, with a
        // lifetime guard, and the one place the TSan annotations for that
        // hand-off live (sanitizer report S5).
        MainThreadDispatcher::post(m_lifetime, m_onReady);
    }
}

} // namespace ustudio::engine
