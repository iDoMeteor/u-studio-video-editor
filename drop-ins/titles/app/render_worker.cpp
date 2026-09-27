#include "render_worker.h"

#include <glib.h>

// GLib's main-context lock is futex-based and invisible to ThreadSanitizer:
// annotate the hand-off, as MainThreadDispatcher does (src/engine/
// dispatcher.cpp, sanitizer report S5). Only in -Db_sanitize=thread builds.
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
struct Posted
{
    std::function<void()> deliver;
};

gboolean deliverOnMain(gpointer data)
{
    TITLES_TSAN_ACQUIRE(data);
    std::unique_ptr<Posted> posted(static_cast<Posted *>(data));
    posted->deliver();
    return G_SOURCE_REMOVE;
}
} // namespace

RenderWorker::RenderWorker(Done done) : m_link(std::make_shared<Link>())
{
    m_link->done = std::move(done);
    m_thread = std::thread([this] { run(); });
}

RenderWorker::~RenderWorker()
{
    {
        std::lock_guard lock(m_link->mutex);
        m_link->done = nullptr;
    }
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_one();
    m_thread.join();
}

uint64_t RenderWorker::request(const TitleDocument &doc, double titleFrame, std::map<std::string, std::string> fields,
                               int width, int height)
{
    uint64_t generation = 0;
    {
        std::lock_guard lock(m_mutex);
        generation = ++m_generation;
        m_pending = Job{doc, titleFrame, std::move(fields), width, height, generation};
    }
    m_wake.notify_one();
    return generation;
}

void RenderWorker::run()
{
    for (;;) {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stop || m_pending.has_value(); });
            if (m_stop)
                return;
            job = std::move(*m_pending);
            m_pending.reset();
        }
        auto result =
            std::make_shared<RenderResult>(renderTitle(job.doc, job.titleFrame, job.fields, job.width, job.height));
        std::weak_ptr<Link> link = m_link;
        const uint64_t generation = job.generation;
        auto *posted = new Posted{[link, result, generation] {
            std::shared_ptr<Link> alive = link.lock();
            if (!alive)
                return;
            std::lock_guard lock(alive->mutex);
            if (alive->done)
                alive->done(std::move(*result), generation);
        }};
        TITLES_TSAN_RELEASE(posted);
        g_idle_add_full(G_PRIORITY_DEFAULT, &deliverOnMain, posted, nullptr);
    }
}

} // namespace ustudio::titles::app
