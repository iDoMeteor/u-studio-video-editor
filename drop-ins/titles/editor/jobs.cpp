#include "jobs.h"

#include <gio/gio.h>

#include <list>
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

namespace ustudio::titles {

namespace {

std::atomic<bool> g_cancelled{false};

class Jobs
{
  public:
    static Jobs &instance()
    {
        static Jobs jobs;
        return jobs;
    }

    void start(std::function<void()> work)
    {
        std::lock_guard lock(m_mutex);
        // Finished ones go first.
        for (auto it = m_jobs.begin(); it != m_jobs.end();) {
            if (it->done->load()) {
                it->thread.join();
                it = m_jobs.erase(it);
            } else {
                ++it;
            }
        }
        if (!m_hooked) {
            if (GApplication *app = g_application_get_default())
                g_signal_connect(app, "shutdown", G_CALLBACK(onShutdown), this);
            m_hooked = true;
        }
        auto done = std::make_shared<std::atomic<bool>>(false);
        m_jobs.push_back({std::thread([work = std::move(work), done] {
                              work();
                              done->store(true);
                          }),
                          done});
    }

    // Without an app to shut down (a test), at exit.
    ~Jobs()
    {
        joinAll();
    }

  private:
    struct Job
    {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    void joinAll()
    {
        g_cancelled.store(true);
        std::lock_guard lock(m_mutex);
        for (Job &job : m_jobs)
            job.thread.join();
        m_jobs.clear();
    }

    std::mutex m_mutex;
    std::list<Job> m_jobs;
    bool m_hooked = false;

    // --- GLib trampolines --------------------------------------------------
    static void onShutdown(GApplication *, gpointer self)
    {
        static_cast<Jobs *>(self)->joinAll();
    }
};

struct Posted
{
    std::function<void()> fn;
};

gboolean runPosted(gpointer data)
{
    TITLES_TSAN_ACQUIRE(data);
    std::unique_ptr<Posted> posted(static_cast<Posted *>(data));
    posted->fn();
    return G_SOURCE_REMOVE;
}

} // namespace

void startJob(std::function<void()> work)
{
    Jobs::instance().start(std::move(work));
}

const std::atomic<bool> &jobsCancelled()
{
    return g_cancelled;
}

void postToMain(std::function<void()> fn)
{
    auto *posted = new Posted{std::move(fn)};
    TITLES_TSAN_RELEASE(posted);
    g_idle_add_full(G_PRIORITY_DEFAULT, &runPosted, posted, nullptr);
}

} // namespace ustudio::titles
